package main

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/BurntSushi/toml"
)

type config struct {
	MPDHost string `toml:"mpd_host"`
	MPDPort int    `toml:"mpd_port"`
}

func loadConfig() config {
	home, _ := os.UserHomeDir()
	xdgConfig := os.Getenv("XDG_CONFIG_HOME")
	if xdgConfig == "" {
		xdgConfig = filepath.Join(home, ".config")
	}
	configPath := filepath.Join(xdgConfig, "melody", "melody-cli.toml")

	var c config
	toml.DecodeFile(configPath, &c)
	applyMPDEnv(&c)
	if c.MPDHost == "" {
		c.MPDHost = "localhost"
	}
	if c.MPDPort == 0 {
		c.MPDPort = 6600
	}
	return c
}

func applyMPDEnv(c *config) {
	if h := os.Getenv("MPD_HOST"); h != "" {
		if host, port, ok := strings.Cut(h, ":"); ok {
			c.MPDHost = host
			fmt.Sscanf(port, "%d", &c.MPDPort)
		} else {
			c.MPDHost = h
		}
	}
	if p := os.Getenv("MPD_PORT"); p != "" {
		fmt.Sscanf(p, "%d", &c.MPDPort)
	}
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "Usage: melody-cli <command> [args...]")
		fmt.Fprintln(os.Stderr, "")
		fmt.Fprintln(os.Stderr, "Commands:")
		fmt.Fprintln(os.Stderr, "  find <tag> <value> [tag value ...]   Find tracks (exact match)")
		fmt.Fprintln(os.Stderr, "  search <tag> <value> [tag value ...] Search tracks (case-insensitive)")
		fmt.Fprintln(os.Stderr, "  findadd <tag> <value> [...]          Find and add to queue")
		fmt.Fprintln(os.Stderr, "  searchadd <tag> <value> [...]        Search and add to queue")
		fmt.Fprintln(os.Stderr, "  rate <songid> <rating>               Rate track (0-10, 0=unrate)")
		fmt.Fprintln(os.Stderr, "  albumrate <artist> <album> <date> <rating>")
		fmt.Fprintln(os.Stderr, "  getrating <songid>                   Get track rating")
		fmt.Fprintln(os.Stderr, "  getalbumrating <artist> <album> <date>")
		fmt.Fprintln(os.Stderr, "  current                              Show current song with rating")
		fmt.Fprintln(os.Stderr, "  raw <command>                        Send raw MPD command")
		os.Exit(1)
	}

	cfg := loadConfig()
	conn, err := net.DialTimeout("tcp", fmt.Sprintf("%s:%d", cfg.MPDHost, cfg.MPDPort), 3*time.Second)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: cannot connect to %s:%d: %v\n", cfg.MPDHost, cfg.MPDPort, err)
		os.Exit(1)
	}
	defer conn.Close()

	r := bufio.NewReader(conn)
	w := bufio.NewWriter(conn)

	// Read greeting
	greeting, err := r.ReadString('\n')
	if err != nil || !strings.HasPrefix(greeting, "OK MPD") {
		fmt.Fprintln(os.Stderr, "Error: not an MPD server")
		os.Exit(1)
	}

	command := os.Args[1]
	args := os.Args[2:]

	var mpdCmd string
	switch command {
	case "find", "search", "findadd", "searchadd":
		mpdCmd = buildFindCmd(command, args)
	case "rate":
		if len(args) != 2 {
			fmt.Fprintln(os.Stderr, "Usage: melody-cli rate <songid> <rating>")
			os.Exit(1)
		}
		mpdCmd = fmt.Sprintf("rate %s %s", args[0], args[1])
	case "albumrate":
		if len(args) != 4 {
			fmt.Fprintln(os.Stderr, "Usage: melody-cli albumrate <artist> <album> <date> <rating>")
			os.Exit(1)
		}
		mpdCmd = fmt.Sprintf("albumrate %s %s %s %s", quote(args[0]), quote(args[1]), quote(args[2]), args[3])
	case "getrating":
		if len(args) != 1 {
			fmt.Fprintln(os.Stderr, "Usage: melody-cli getrating <songid>")
			os.Exit(1)
		}
		mpdCmd = "getrating " + args[0]
	case "getalbumrating":
		if len(args) != 3 {
			fmt.Fprintln(os.Stderr, "Usage: melody-cli getalbumrating <artist> <album> <date>")
			os.Exit(1)
		}
		mpdCmd = fmt.Sprintf("getalbumrating %s %s %s", quote(args[0]), quote(args[1]), quote(args[2]))
	case "current":
		mpdCmd = "currentsong"
	case "raw":
		if len(args) == 0 {
			fmt.Fprintln(os.Stderr, "Usage: melody-cli raw <command>")
			os.Exit(1)
		}
		mpdCmd = strings.Join(args, " ")
	default:
		// Pass through as raw command
		mpdCmd = command
		if len(args) > 0 {
			mpdCmd += " " + strings.Join(args, " ")
		}
	}

	conn.SetDeadline(time.Now().Add(10 * time.Second))
	w.WriteString(mpdCmd + "\n")
	w.Flush()

	for {
		line, err := r.ReadString('\n')
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "OK" {
			break
		}
		if strings.HasPrefix(line, "ACK ") {
			fmt.Fprintln(os.Stderr, line)
			os.Exit(1)
		}
		fmt.Println(line)
	}
}

func buildFindCmd(cmd string, args []string) string {
	if len(args) < 2 || len(args)%2 != 0 {
		fmt.Fprintf(os.Stderr, "Usage: melody-cli %s <tag> <value> [tag value ...]\n", cmd)
		os.Exit(1)
	}
	parts := []string{cmd}
	for i := 0; i < len(args); i += 2 {
		parts = append(parts, args[i], quote(args[i+1]))
	}
	return strings.Join(parts, " ")
}

func quote(s string) string {
	s = strings.ReplaceAll(s, `\`, `\\`)
	s = strings.ReplaceAll(s, `"`, `\"`)
	return `"` + s + `"`
}
