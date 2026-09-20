package main

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"strconv"
	"strings"
	"time"
)

const cliHelp = `Usage: melody-cli <command> [args...]

Library:
  find|search|findadd|searchadd <tag> <value> [tag value ...]
  find|search|findadd|searchadd <filter> [filter ...] [sort <tag>] [window <start:end>]
  searchalbums <filter> [sort <tag>] [window <start:end>]
  list <tag> [filters/options ...]    List tag values, with optional grouping
  current                           Show current song, ratings and audio format
  lyrics [uri]                      Show lyrics (defaults to current song)
  rate <songid> <0-10>               Rate track (0 clears rating)
  albumrate <artist> <album> <date> <0-10>
  getrating <songid>
  getalbumrating <artist> <album> <date>

Last.fm:
  lastfm [status]                    Show account and scrobbling status
  lastfm begin <api-key> <secret>    Get browser authorization URL
  lastfm finish                     Complete browser authorization
  lastfm enable <0|1>                Disable/enable scrobbling
  lastfm info|love|unlove [artist title]  Defaults to current song
  lastfm disconnect                 Disconnect and clear pending scrobbles

Playback contexts and working lists:
  context                           Show active playback context
  context play <playlist> [pos]      Play/resume a stored playlist
  context queue [pos]                Return to the saved queue
  context queueinfo                  Show the saved queue
  context queueadd <pos> <uri...>    Insert into saved queue (-1 appends)
  context queuereplace <pos> <uri...> Replace queue and play
  context queuedelete <pos...>
  context queuemove <from> <to>
  scratch [playlist <0|1>]           List, promote (0), or mark (1) working lists
  playlistadd <playlist> <uri> [pos] Add one track to a stored playlist
  playlistappend <playlist> <uri...> Append multiple tracks

Up Next:
  upnext                            Show pending requests and revision
  upnext append|prepend <uri...>
  upnext insert <pos> <uri...>
  upnext remove|play <request-id>
  upnext move <request-id> <pos>
  upnext clear|undo|return
  Edits fetch the current revision; concurrent changes fail without retrying.

Other MPD commands are passed through with shell arguments quoted:
  play, pause, stop, next, previous, seekcur, setvol, outputs,
  enableoutput, disableoutput, toggleoutput, listplaylists, rename,
  playlistdelete, playlistmove, playlistclear, update, tagtypes, ...
  raw <command>                     Send literal MPD protocol text
  help                              Show this help without connecting
`

func prepareCommand(command string, args []string) (string, []string, error) {
	for _, value := range append([]string{command}, args...) {
		if strings.ContainsAny(value, "\r\n") {
			return "", nil, fmt.Errorf("commands and arguments must not contain newlines")
		}
	}
	if strings.ContainsAny(command, " \t") {
		return "", nil, fmt.Errorf("use raw to send literal MPD protocol text")
	}
	valid := true
	usage := command
	switch command {
	case "current":
		valid = len(args) == 0
		command = "currentsong"
	case "lyrics":
		valid = len(args) <= 1
		usage += " [uri]"
	case "rate", "getrating", "albumrate", "getalbumrating":
		counts := map[string]int{"rate": 2, "getrating": 1, "albumrate": 4, "getalbumrating": 3}
		valid = len(args) == counts[command]
		usage = map[string]string{
			"rate": "rate <songid> <0-10>", "getrating": "getrating <songid>",
			"albumrate":      "albumrate <artist> <album> <date> <0-10>",
			"getalbumrating": "getalbumrating <artist> <album> <date>",
		}[command]
	case "find", "search", "findadd", "searchadd", "searchalbums":
		valid = len(args) > 0
		usage += " <filter> [sort <tag>] [window <start:end>] or <tag> <value> [...]"
		if valid && !strings.HasPrefix(strings.TrimSpace(args[0]), "(") {
			valid = len(args)%2 == 0
		}
	case "lastfm":
		if len(args) == 0 {
			args = []string{"status"}
		}
		usage += " status|begin <api-key> <secret>|finish|enable <0|1>|info|love|unlove [artist title]|disconnect"
		switch args[0] {
		case "status", "finish", "disconnect":
			valid = len(args) == 1
		case "begin":
			valid = len(args) == 3
		case "enable":
			valid = len(args) == 2 && (args[1] == "0" || args[1] == "1")
		case "info", "love", "unlove":
			valid = len(args) == 1 || len(args) == 3
		default:
			valid = false
		}
	case "context":
		usage += " [play|queue|queueinfo|queueadd|queuereplace|queuedelete|queuemove ...] (see help)"
		if len(args) > 0 {
			switch args[0] {
			case "play":
				valid = len(args) == 2 || len(args) == 3
			case "queue":
				valid = len(args) <= 2
			case "queueinfo":
				valid = len(args) == 1
			case "queueadd", "queuereplace":
				valid = len(args) >= 3
			case "queuedelete":
				valid = len(args) >= 2
			case "queuemove":
				valid = len(args) == 3
			default:
				valid = false
			}
		}
		command = "melody_context"
	case "scratch":
		valid = len(args) == 0 || (len(args) == 2 && (args[1] == "0" || args[1] == "1"))
		usage += " [playlist <0|1>]"
		command = "melody_scratch"
	case "playlistappend":
		valid = len(args) >= 2
		usage += " <playlist> <uri...>"
		command = "melody_playlistadd"
	case "upnext":
		usage += " [append|prepend|insert|remove|move|play|clear|undo|return ...] (see help)"
		if len(args) > 0 {
			switch args[0] {
			case "append", "prepend":
				valid = len(args) >= 2
			case "insert":
				valid = len(args) >= 3
			case "remove", "play":
				valid = len(args) == 2
			case "move":
				valid = len(args) == 3
			case "clear", "undo", "return":
				valid = len(args) == 1
			default:
				valid = false
			}
		}
	case "raw":
		valid = len(args) > 0
		usage += " <command>"
	}
	if !valid {
		return "", nil, fmt.Errorf("usage: melody-cli %s", usage)
	}
	return command, args, nil
}

func encodeCommand(command string, args []string) string {
	if command == "raw" {
		return strings.Join(args, " ")
	}
	parts := []string{command}
	for _, arg := range args {
		parts = append(parts, quote(arg))
	}
	return strings.Join(parts, " ")
}

type mpdClient struct {
	conn   net.Conn
	reader *bufio.Reader
}

func (client *mpdClient) request(command string, args []string) ([]string, error) {
	if err := client.conn.SetDeadline(time.Now().Add(15 * time.Second)); err != nil {
		return nil, err
	}
	if _, err := io.WriteString(client.conn, encodeCommand(command, args)+"\n"); err != nil {
		return nil, err
	}
	var lines []string
	for {
		line, err := client.reader.ReadString('\n')
		if err != nil {
			return nil, err
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "OK" {
			return lines, nil
		}
		if strings.HasPrefix(line, "ACK ") {
			return nil, fmt.Errorf("%s", line)
		}
		lines = append(lines, line)
	}
}

func responseValue(lines []string, key string) string {
	for _, line := range lines {
		if value, ok := strings.CutPrefix(line, key+": "); ok {
			return value
		}
	}
	return ""
}

func (client *mpdClient) run(command string, args []string, output io.Writer) error {
	isLyrics := command == "lyrics"
	if isLyrics && len(args) == 0 || command == "lastfm" && len(args) == 1 && (args[0] == "love" || args[0] == "unlove" || args[0] == "info") {
		song, err := client.request("currentsong", nil)
		if err != nil {
			return err
		}
		if isLyrics {
			file := responseValue(song, "file")
			if file == "" {
				return fmt.Errorf("no current song")
			}
			args = []string{file}
		} else {
			artist, title := responseValue(song, "Artist"), responseValue(song, "Title")
			if artist == "" || title == "" {
				return fmt.Errorf("current song needs artist and title; pass them explicitly")
			}
			args = append(append([]string{}, args...), artist, title)
		}
	}
	switch command {
	case "lyrics":
		command = "readlyrics"
	case "lastfm":
		command = "melody_lastfm"
	case "upnext":
		command = "melody_upnext"
		if len(args) > 0 {
			state, err := client.request(command, nil)
			if err != nil {
				return err
			}
			revision := responseValue(state, "revision")
			if number, err := strconv.Atoi(revision); err != nil || number < 0 {
				return fmt.Errorf("server returned no valid Up Next revision")
			}
			args = append([]string{args[0], revision}, args[1:]...)
		}
	}
	lines, err := client.request(command, args)
	if err != nil {
		return err
	}
	for _, line := range lines {
		if isLyrics {
			if strings.HasPrefix(line, "X-Lyrics-Type: ") {
				continue
			}
			if value, ok := strings.CutPrefix(line, "X-Lyrics: "); ok {
				line = unescapeLyrics(value)
			}
		}
		if _, err := fmt.Fprintln(output, line); err != nil {
			return err
		}
	}
	return nil
}
