package main

import (
	"bufio"
	"bytes"
	"log"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// TestUpdateCommandReportsJobAndStats drives the MPD `update` command through a
// real scanner and verifies it returns an update job id, that `status` exposes
// updating_db while a scan runs, and that `stats` reports a db_update timestamp
// once a scan has completed.
func TestUpdateCommandReportsJobAndStats(t *testing.T) {
	musicDir := t.TempDir()
	writeFile(t, filepath.Join(musicDir, "Artist", "Album", "01.flac"))

	db, err := openMusicDB(filepath.Join(t.TempDir(), "music.db"))
	if err != nil {
		t.Fatalf("openMusicDB: %v", err)
	}
	a := &app{
		mpdHub:  newNotifyHub(),
		db:      db,
		scanner: newScanner(musicDir, db, log.New(os.Stderr, "", 0), ""),
	}
	a.cfg.Library.MusicDir = musicDir

	// `update` returns "updating_db: <job>".
	out := dispatchCapture(t, a, "update")
	if !strings.Contains(out, "updating_db: 1") {
		t.Fatalf("update response missing job id, got:\n%s", out)
	}

	// Wait for the background scan to finish and ingest the file.
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if n, _ := db.trackCount(); n == 1 && a.scanner.currentJob() == 0 {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if n, _ := db.trackCount(); n != 1 {
		t.Fatalf("background scan did not ingest file, tracks=%d", n)
	}

	// stats now reports a non-zero db_update timestamp.
	stats := dispatchCapture(t, a, "stats")
	if strings.Contains(stats, "db_update: 0\n") {
		t.Fatalf("stats still reports db_update: 0 after a scan:\n%s", stats)
	}
}

// dispatchCapture runs a single MPD command against app a and returns everything
// written to the connection (handler output + trailing OK).
func dispatchCapture(t *testing.T, a *app, line string) string {
	t.Helper()
	var buf bytes.Buffer
	c := &mpdConn{
		writer: bufio.NewWriter(&buf),
		app:    a,
		logger: log.New(os.Stderr, "", 0),
	}
	cmd, args := parseCommand(line)
	if err := c.dispatch(cmd, args); err != nil {
		t.Fatalf("dispatch %q: %s", line, err.Error())
	}
	c.writer.Flush()
	return buf.String()
}
