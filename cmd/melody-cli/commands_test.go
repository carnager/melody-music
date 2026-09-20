package main

import (
	"bufio"
	"bytes"
	"fmt"
	"net"
	"strings"
	"testing"
)

func TestCommandEncoding(t *testing.T) {
	tests := []struct {
		name    string
		command string
		args    []string
		want    string
	}{
		{"tag pairs", "find", []string{"AlbumArtist", "Cocteau Twins"}, `find "AlbumArtist" "Cocteau Twins"`},
		{"expression", "find", []string{"((Genre == 'Jazz') OR (Genre == 'Blues'))", "sort", "-Added", "window", "0:50"}, `find "((Genre == 'Jazz') OR (Genre == 'Blues'))" "sort" "-Added" "window" "0:50"`},
		{"album search", "searchalbums", []string{"(albumrating >= 8)", "sort", "-rating"}, `searchalbums "(albumrating >= 8)" "sort" "-rating"`},
		{"playlist spaces", "rename", []string{"Road trip", "Long drive"}, `rename "Road trip" "Long drive"`},
		{"escaping", "add", []string{`Music\A "Song".flac`}, `add "Music\\A \"Song\".flac"`},
		{"empty date", "albumrate", []string{"Artist", "Album", "", "8"}, `albumrate "Artist" "Album" "" "8"`},
		{"context", "context", []string{"play", "Road trip"}, `melody_context "play" "Road trip"`},
		{"scratch", "scratch", []string{"Working list", "1"}, `melody_scratch "Working list" "1"`},
		{"batch playlist", "playlistappend", []string{"Road trip", "first song.flac", "second.flac"}, `melody_playlistadd "Road trip" "first song.flac" "second.flac"`},
		{"raw", "raw", []string{`find Album "Road trip"`}, `find Album "Road trip"`},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			command, args, err := prepareCommand(test.command, test.args)
			if err != nil {
				t.Fatal(err)
			}
			if got := encodeCommand(command, args); got != test.want {
				t.Fatalf("got %q, want %q", got, test.want)
			}
		})
	}
}

func TestInvalidCommands(t *testing.T) {
	for _, args := range [][]string{
		{"lastfm", "enable", "yes"}, {"lastfm", "finish", "extra"},
		{"lastfm", "love", "artist"}, {"lastfm", "unknown"},
		{"context", "play"}, {"context", "queueadd", "-1"},
		{"scratch", "name", "2"}, {"playlistappend", "name"},
		{"upnext", "move", "42"}, {"upnext", "clear", "extra"},
		{"find", "Artist"}, {"raw"}, {"current", "extra"},
		{"add", "song\nclear"}, {"raw", "status\r\nclear"},
		{"status\nclear"}, {"status extra"},
	} {
		if _, _, err := prepareCommand(args[0], args[1:]); err == nil {
			t.Errorf("accepted invalid args %q", args)
		}
	}
}

func TestClientWorkflows(t *testing.T) {
	type exchange struct {
		command string
		reply   string
	}
	tests := []struct {
		name      string
		args      []string
		exchanges []exchange
		want      string
		wantError string
	}{
		{
			name: "lastfm status", args: []string{"lastfm"},
			exchanges: []exchange{{`melody_lastfm "status"`, "lastfm: {\"connected\":false}\nOK\n"}},
			want:      "lastfm: {\"connected\":false}\n",
		},
		{
			name: "love current song", args: []string{"lastfm", "love"},
			exchanges: []exchange{
				{"currentsong", "file: album/song.flac\nArtist: Cocteau Twins\nTitle: Heaven or Las Vegas\nOK\n"},
				{`melody_lastfm "love" "Cocteau Twins" "Heaven or Las Vegas"`, "lastfm: {\"loved\":true}\nOK\n"},
			},
			want: "lastfm: {\"loved\":true}\n",
		},
		{
			name: "explicit track", args: []string{"lastfm", "info", "Artist", "Title"},
			exchanges: []exchange{{`melody_lastfm "info" "Artist" "Title"`, "lastfm: {\"loved\":false}\nOK\n"}},
			want:      "lastfm: {\"loved\":false}\n",
		},
		{
			name: "missing metadata", args: []string{"lastfm", "unlove"},
			exchanges: []exchange{{"currentsong", "file: song.flac\nTitle: Song\nOK\n"}},
			wantError: "needs artist and title",
		},
		{
			name: "lyrics", args: []string{"lyrics"},
			exchanges: []exchange{
				{"currentsong", "file: album/song.flac\nOK\n"},
				{`readlyrics "album/song.flac"`, "X-Lyrics-Type: plain\nX-Lyrics: first\\nsecond\nOK\n"},
			},
			want: "first\nsecond\n",
		},
		{
			name: "explicit lyrics", args: []string{"lyrics", "a song.flac"},
			exchanges: []exchange{{`readlyrics "a song.flac"`, "X-Lyrics: text\nOK\n"}},
			want:      "text\n",
		},
		{
			name: "append request", args: []string{"upnext", "append", "a song.flac"},
			exchanges: []exchange{
				{"melody_upnext", "revision: 12\nactive: 0\nOK\n"},
				{`melody_upnext "append" "12" "a song.flac"`, "OK\n"},
			},
		},
		{
			name: "revision conflict is not retried", args: []string{"upnext", "remove", "45"},
			exchanges: []exchange{
				{"melody_upnext", "revision: 12\nOK\n"},
				{`melody_upnext "remove" "12" "45"`, "ACK [2@0] {melody_upnext} request queue changed\n"},
			},
			wantError: "request queue changed",
		},
		{
			name: "missing revision", args: []string{"upnext", "clear"},
			exchanges: []exchange{{"melody_upnext", "OK\n"}},
			wantError: "no valid Up Next revision",
		},
		{
			name: "unsupported server", args: []string{"lastfm"},
			exchanges: []exchange{{`melody_lastfm "status"`, "ACK [5@0] {melody_lastfm} unknown command\n"}},
			wantError: "unknown command",
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			command, args, err := prepareCommand(test.args[0], test.args[1:])
			if err != nil {
				t.Fatal(err)
			}
			conn, server := net.Pipe()
			defer conn.Close()
			done := make(chan error, 1)
			go func() {
				defer server.Close()
				reader := bufio.NewReader(server)
				for _, step := range test.exchanges {
					line, err := reader.ReadString('\n')
					if err != nil {
						done <- err
						return
					}
					if line != step.command+"\n" {
						done <- fmt.Errorf("got command %q, want %q", line, step.command)
						return
					}
					if _, err := fmt.Fprint(server, step.reply); err != nil {
						done <- err
						return
					}
				}
				done <- nil
			}()
			client := mpdClient{conn: conn, reader: bufio.NewReader(conn)}
			var output bytes.Buffer
			err = client.run(command, args, &output)
			if test.wantError == "" && err != nil {
				t.Fatal(err)
			}
			if test.wantError != "" && (err == nil || !strings.Contains(err.Error(), test.wantError)) {
				t.Fatalf("got error %v, want %q", err, test.wantError)
			}
			conn.Close()
			if err := <-done; err != nil {
				t.Fatal(err)
			}
			if output.String() != test.want {
				t.Fatalf("got output %q, want %q", output.String(), test.want)
			}
		})
	}
}
