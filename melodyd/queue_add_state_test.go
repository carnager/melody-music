package main

import (
	"bufio"
	"fmt"
	"log"
	"sync"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// newQueueStateTestApp builds an app with a real music DB containing two
// tracks and a single enabled web output, then serves the MPD protocol over
// a net.Pipe. Web targets double as a cheap non-agent output implementation.
// newTestLibraryDB creates a music DB under tmp with two tracks and returns
// it along with the music dir the track paths live in.
func newTestLibraryDB(t *testing.T, tmp string) (*musicDB, string) {
	t.Helper()
	db, err := openMusicDB(filepath.Join(tmp, "music.db"))
	if err != nil {
		t.Fatalf("openMusicDB: %v", err)
	}
	t.Cleanup(func() { _ = db.close() })

	musicDir := filepath.Join(tmp, "music")
	artistID, err := db.upsertArtist("Artist")
	if err != nil {
		t.Fatalf("upsertArtist: %v", err)
	}
	albumID, err := db.upsertAlbum(artistID, "Album", "2020")
	if err != nil {
		t.Fatalf("upsertAlbum: %v", err)
	}
	for i, name := range []string{"one.flac", "two.flac"} {
		_, err := db.upsertTrack(&trackMeta{
			AlbumID:     albumID,
			Artist:      "Artist",
			Title:       name,
			TrackNumber: i + 1,
			Duration:    100,
			Path:        filepath.Join(musicDir, name),
		})
		if err != nil {
			t.Fatalf("upsertTrack(%s): %v", name, err)
		}
	}
	return db, musicDir
}

func newQueueStateTestApp(t *testing.T) (*app, *webTarget, *bufio.ReadWriter) {
	t.Helper()

	tmp := t.TempDir()
	db, musicDir := newTestLibraryDB(t, tmp)

	wt := newWebTarget()
	a := &app{
		logger:         log.New(os.Stdout, "test: ", 0),
		db:             db,
		scanner:        &scanner{},
		devices:        map[string]*device{"web-a": {ID: "web-a", Name: "A", Type: "web"}},
		webTargets:     map[string]*webTarget{"web-a": wt},
		agentTargets:   make(map[string]*agentTarget),
		enabledOutputs: map[string]bool{"web-a": true},
		primaryOutput:  "web-a",
		agentResumes:   make(map[string]*agentResume),
		mpdHub:         newNotifyHub(),
		pendingNextPos: -1,
	}
	a.cfg.Library.MusicDir = musicDir
	a.paths.PlayQueueFile = filepath.Join(tmp, "play_queue.json")
	a.paths.PlayStateFile = filepath.Join(tmp, "play_state.json")

	server, client := net.Pipe()
	c := &mpdConn{
		conn:   server,
		reader: bufio.NewReader(server),
		writer: bufio.NewWriter(server),
		app:    a,
	}
	go c.serve()
	t.Cleanup(func() {
		_ = client.Close()
		_ = server.Close()
	})

	rw := bufio.NewReadWriter(bufio.NewReader(client), bufio.NewWriter(client))
	if got := readLine(t, rw); !strings.HasPrefix(got, "OK MPD") {
		t.Fatalf("greeting = %q", got)
	}
	return a, wt, rw
}

// runOK sends an MPD command and reads lines until OK, returning the
// key: value pairs seen. Fails the test on ACK.
func runOK(t *testing.T, rw *bufio.ReadWriter, cmd string) map[string]string {
	t.Helper()
	writeLine(t, rw, cmd)
	kv := make(map[string]string)
	for {
		line := readLine(t, rw)
		if line == "OK" {
			return kv
		}
		if strings.HasPrefix(line, "ACK") {
			t.Fatalf("%q returned %q", cmd, line)
		}
		if k, v, ok := strings.Cut(line, ": "); ok {
			kv[k] = v
		}
	}
}

func TestAddToEmptyQueueKeepsPlaybackStopped(t *testing.T) {
	_, wt, rw := newQueueStateTestApp(t)

	st := runOK(t, rw, "status")
	if st["state"] != "stop" || st["playlistlength"] != "0" {
		t.Fatalf("initial state=%q playlistlength=%q, want stop/0", st["state"], st["playlistlength"])
	}

	resp := runOK(t, rw, `addid "one.flac"`)
	if resp["Id"] == "" || resp["Id"] == "0" {
		t.Fatalf("addid Id = %q, want a real song id", resp["Id"])
	}

	st = runOK(t, rw, "status")
	if st["playlistlength"] != "1" {
		t.Fatalf("playlistlength = %q after addid, want 1", st["playlistlength"])
	}
	if st["state"] != "stop" {
		t.Fatalf("state = %q after addid to empty queue, want stop", st["state"])
	}

	// The output must not have started playing: nothing loaded, still paused.
	wt.mu.Lock()
	loaded, paused := len(wt.playlist), wt.paused
	wt.mu.Unlock()
	if loaded != 0 {
		t.Fatalf("output has %d tracks loaded after addid, want 0", loaded)
	}
	if !paused {
		t.Fatal("output unpaused by addid to empty queue")
	}
}

func TestAddWhilePausedKeepsPaused(t *testing.T) {
	_, wt, rw := newQueueStateTestApp(t)

	runOK(t, rw, `addid "one.flac"`)
	runOK(t, rw, "play")
	runOK(t, rw, "pause 1")

	st := runOK(t, rw, "status")
	if st["state"] != "pause" {
		t.Fatalf("state = %q after pause, want pause", st["state"])
	}

	runOK(t, rw, `add "two.flac"`)

	st = runOK(t, rw, "status")
	if st["playlistlength"] != "2" {
		t.Fatalf("playlistlength = %q after add, want 2", st["playlistlength"])
	}
	if st["state"] != "pause" {
		t.Fatalf("state = %q after add while paused, want pause", st["state"])
	}
	wt.mu.Lock()
	paused := wt.paused
	wt.mu.Unlock()
	if !paused {
		t.Fatal("output unpaused by add while paused")
	}
}

func TestExplicitPlayAfterAddStartsPlayback(t *testing.T) {
	_, wt, rw := newQueueStateTestApp(t)

	runOK(t, rw, `addid "one.flac"`)
	runOK(t, rw, "play")

	st := runOK(t, rw, "status")
	if st["state"] != "play" {
		t.Fatalf("state = %q after explicit play, want play", st["state"])
	}
	wt.mu.Lock()
	loaded, paused := len(wt.playlist), wt.paused
	wt.mu.Unlock()
	if loaded == 0 {
		t.Fatal("play after add loaded nothing into the output")
	}
	if paused {
		t.Fatal("output still paused after explicit play")
	}
}

// pausedSetup brings the app into "track 0 loaded, paused" with both library
// tracks queued.
func pausedSetup(t *testing.T, rw *bufio.ReadWriter) {
	t.Helper()
	runOK(t, rw, `add "one.flac"`)
	runOK(t, rw, `add "two.flac"`)
	runOK(t, rw, "play")
	runOK(t, rw, "pause 1")
	if st := runOK(t, rw, "status"); st["state"] != "pause" {
		t.Fatalf("setup state = %q, want pause", st["state"])
	}
}

func TestNextWhilePausedStaysPaused(t *testing.T) {
	_, wt, rw := newQueueStateTestApp(t)
	pausedSetup(t, rw)

	runOK(t, rw, "next")

	st := runOK(t, rw, "status")
	if st["state"] != "pause" || st["song"] != "1" {
		t.Fatalf("state=%q song=%q after next while paused, want pause/1", st["state"], st["song"])
	}
	wt.mu.Lock()
	paused := wt.paused
	wt.mu.Unlock()
	if !paused {
		t.Fatal("output unpaused by next while paused")
	}
}

func TestPreviousWhilePausedStaysPaused(t *testing.T) {
	_, wt, rw := newQueueStateTestApp(t)
	pausedSetup(t, rw)
	runOK(t, rw, "next")

	runOK(t, rw, "previous")

	st := runOK(t, rw, "status")
	if st["state"] != "pause" || st["song"] != "0" {
		t.Fatalf("state=%q song=%q after previous while paused, want pause/0", st["state"], st["song"])
	}
	wt.mu.Lock()
	paused := wt.paused
	wt.mu.Unlock()
	if !paused {
		t.Fatal("output unpaused by previous while paused")
	}
}

func TestNextWhileStoppedStaysStopped(t *testing.T) {
	_, wt, rw := newQueueStateTestApp(t)
	runOK(t, rw, `add "one.flac"`)
	runOK(t, rw, `add "two.flac"`)

	runOK(t, rw, "next")

	st := runOK(t, rw, "status")
	if st["state"] != "stop" || st["song"] != "1" {
		t.Fatalf("state=%q song=%q after next while stopped, want stop/1", st["state"], st["song"])
	}
	wt.mu.Lock()
	loaded := len(wt.playlist)
	wt.mu.Unlock()
	if loaded != 0 {
		t.Fatalf("output has %d tracks loaded after next while stopped, want 0", loaded)
	}
}

func TestInsertWhilePausedStaysPaused(t *testing.T) {
	_, wt, rw := newQueueStateTestApp(t)
	runOK(t, rw, `add "one.flac"`)
	runOK(t, rw, "play")
	runOK(t, rw, "pause 1")

	runOK(t, rw, `enqueue insert artist Artist`)

	st := runOK(t, rw, "status")
	if st["state"] != "pause" {
		t.Fatalf("state = %q after insert while paused, want pause", st["state"])
	}
	if st["playlistlength"] != "3" {
		t.Fatalf("playlistlength = %q after insert, want 3", st["playlistlength"])
	}
	wt.mu.Lock()
	paused := wt.paused
	wt.mu.Unlock()
	if !paused {
		t.Fatal("output unpaused by insert while paused")
	}
}

func TestInsertIntoEmptyQueueKeepsStopped(t *testing.T) {
	_, wt, rw := newQueueStateTestApp(t)

	runOK(t, rw, `enqueue insert artist Artist`)

	st := runOK(t, rw, "status")
	if st["state"] != "stop" || st["playlistlength"] != "2" {
		t.Fatalf("state=%q playlistlength=%q after insert into empty queue, want stop/2", st["state"], st["playlistlength"])
	}
	wt.mu.Lock()
	loaded, paused := len(wt.playlist), wt.paused
	wt.mu.Unlock()
	if loaded != 0 || !paused {
		t.Fatalf("output loaded=%d paused=%v after insert into empty queue, want 0/true", loaded, paused)
	}
}

func TestSeekToOtherSongWhilePausedStaysPaused(t *testing.T) {
	_, wt, rw := newQueueStateTestApp(t)
	pausedSetup(t, rw)

	runOK(t, rw, "seek 1 30")

	st := runOK(t, rw, "status")
	if st["state"] != "pause" || st["song"] != "1" {
		t.Fatalf("state=%q song=%q after seek while paused, want pause/1", st["state"], st["song"])
	}
	wt.mu.Lock()
	paused := wt.paused
	wt.mu.Unlock()
	if !paused {
		t.Fatal("output unpaused by seek to another song while paused")
	}
}

func TestDeleteCurrentWhilePausedStaysPaused(t *testing.T) {
	_, wt, rw := newQueueStateTestApp(t)
	pausedSetup(t, rw)

	runOK(t, rw, "delete 0")

	st := runOK(t, rw, "status")
	if st["state"] != "pause" || st["playlistlength"] != "1" {
		t.Fatalf("state=%q playlistlength=%q after deleting current while paused, want pause/1", st["state"], st["playlistlength"])
	}
	wt.mu.Lock()
	paused := wt.paused
	wt.mu.Unlock()
	if !paused {
		t.Fatal("output unpaused by deleting the current track while paused")
	}
}

func TestShufflePreservesPlaybackAndCurrentTrack(t *testing.T) {
	_, wt, rw := newQueueStateTestApp(t)
	pausedSetup(t, rw)

	// A full resync would reload slot 0 and reset progress; shuffle must not.
	wt.mu.Lock()
	wt.timePos = 33
	wt.mu.Unlock()

	runOK(t, rw, "shuffle")

	st := runOK(t, rw, "status")
	if st["state"] != "pause" {
		t.Fatalf("state = %q after shuffle while paused, want pause", st["state"])
	}
	wt.mu.Lock()
	paused, timePos := wt.paused, wt.timePos
	wt.mu.Unlock()
	if !paused {
		t.Fatal("output unpaused by shuffle")
	}
	if timePos != 33 {
		t.Fatalf("current track was reloaded by shuffle (timePos reset to %v)", timePos)
	}
}

// TestMelodyVersionAdvertised checks that the server identifies itself:
// clients gate add/addid playback-state workarounds on this command's
// presence in `commands`.
func TestMelodyVersionAdvertised(t *testing.T) {
	_, _, rw := newQueueStateTestApp(t)

	resp := runOK(t, rw, "melody_version")
	if resp["version"] != melodyVersion {
		t.Fatalf("melody_version = %q, want %q", resp["version"], melodyVersion)
	}

	writeLine(t, rw, "commands")
	found := false
	for {
		line := readLine(t, rw)
		if line == "OK" {
			break
		}
		if line == "command: melody_version" {
			found = true
		}
	}
	if !found {
		t.Fatal("commands list does not advertise melody_version")
	}
}

// TestStopReallyStopsAndPlayRestartsTrack: stop must unload (state stop, not
// pause) and a following play restarts the current track from the beginning.
func TestStopReallyStopsAndPlayRestartsTrack(t *testing.T) {
	_, wt, rw := newQueueStateTestApp(t)
	runOK(t, rw, `add "one.flac"`)
	runOK(t, rw, `add "two.flac"`)
	runOK(t, rw, "play")

	// Simulate mid-track progress before stopping.
	wt.mu.Lock()
	wt.timePos = 50
	wt.mu.Unlock()

	runOK(t, rw, "stop")

	st := runOK(t, rw, "status")
	if st["state"] != "stop" || st["song"] != "0" {
		t.Fatalf("state=%q song=%q after stop, want stop/0", st["state"], st["song"])
	}
	wt.mu.Lock()
	loaded := len(wt.playlist)
	wt.mu.Unlock()
	if loaded != 0 {
		t.Fatalf("output still has %d tracks loaded after stop, want 0", loaded)
	}

	runOK(t, rw, "play")

	st = runOK(t, rw, "status")
	if st["state"] != "play" || st["song"] != "0" {
		t.Fatalf("state=%q song=%q after play, want play/0", st["state"], st["song"])
	}
	wt.mu.Lock()
	loaded, timePos, paused := len(wt.playlist), wt.timePos, wt.paused
	wt.mu.Unlock()
	if loaded == 0 || paused {
		t.Fatalf("loaded=%d paused=%v after play from stop, want reloaded and playing", loaded, paused)
	}
	if timePos != 0 {
		t.Fatalf("timePos = %v after play from stop, want restart from 0", timePos)
	}
}

// agentCommandLog records every protocol line a fake agent receives.
type agentCommandLog struct {
	mu   sync.Mutex
	cmds []string
}

func (l *agentCommandLog) list() []string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return append([]string(nil), l.cmds...)
}

// newAgentQueueTestApp is like newQueueStateTestApp but the only output is a
// piped agent target whose commands are recorded and answered with OK.
func newAgentQueueTestApp(t *testing.T) (*app, *agentCommandLog, *bufio.ReadWriter) {
	t.Helper()

	tmp := t.TempDir()
	db, musicDir := newTestLibraryDB(t, tmp)

	a := &app{
		logger:         log.New(os.Stdout, "test: ", 0),
		db:             db,
		scanner:        &scanner{},
		devices:        map[string]*device{"sat": {ID: "sat", Name: "Sat", Type: "agent"}},
		webTargets:     make(map[string]*webTarget),
		enabledOutputs: map[string]bool{"sat": true},
		primaryOutput:  "sat",
		agentResumes:   make(map[string]*agentResume),
		mpdHub:         newNotifyHub(),
		pendingNextPos: -1,
	}
	a.cfg.Library.MusicDir = musicDir
	a.paths.PlayQueueFile = filepath.Join(tmp, "play_queue.json")
	a.paths.PlayStateFile = filepath.Join(tmp, "play_state.json")

	agentConn, serverSide := net.Pipe()
	at := &agentTarget{
		writer:  bufio.NewWriter(agentConn),
		conn:    agentConn,
		alive:   true,
		done:    make(chan struct{}),
		app:     a,
		respCh:  make(chan agentResp, 1),
		agState: "stop",
	}
	a.agentTargets = map[string]*agentTarget{"sat": at}
	go at.readLoop(bufio.NewReader(agentConn))

	alog := &agentCommandLog{}
	go func() {
		sc := bufio.NewScanner(serverSide)
		for sc.Scan() {
			line := strings.TrimSpace(sc.Text())
			if line == "" {
				continue
			}
			alog.mu.Lock()
			alog.cmds = append(alog.cmds, line)
			alog.mu.Unlock()
			fmt.Fprintln(serverSide, "OK")
		}
	}()
	t.Cleanup(func() {
		_ = agentConn.Close()
		_ = serverSide.Close()
	})

	server, client := net.Pipe()
	c := &mpdConn{
		conn:   server,
		reader: bufio.NewReader(server),
		writer: bufio.NewWriter(server),
		app:    a,
	}
	go c.serve()
	t.Cleanup(func() {
		_ = client.Close()
		_ = server.Close()
	})

	rw := bufio.NewReadWriter(bufio.NewReader(client), bufio.NewWriter(client))
	if got := readLine(t, rw); !strings.HasPrefix(got, "OK MPD") {
		t.Fatalf("greeting = %q", got)
	}
	return a, alog, rw
}

// TestReplaceSequenceResyncsFreshlyStoppedAgent reproduces the TUI's
// replace-queue gesture (clear + add + play fired back to back) against an
// agent output. The stop sent by clear must be reflected in the server's
// cached agent state immediately — play has to do a full resync instead of
// resuming whatever stale track the agent still held.
func TestReplaceSequenceResyncsFreshlyStoppedAgent(t *testing.T) {
	_, alog, rw := newAgentQueueTestApp(t)

	runOK(t, rw, `add "one.flac"`)
	runOK(t, rw, `add "two.flac"`)
	runOK(t, rw, "play")
	runOK(t, rw, "pause 1")

	runOK(t, rw, "clear")
	runOK(t, rw, `add "one.flac"`)
	runOK(t, rw, `add "two.flac"`)
	runOK(t, rw, "play")

	cmds := alog.list()
	lastStop := -1
	for i, c := range cmds {
		if c == "stop" {
			lastStop = i
		}
	}
	if lastStop < 0 {
		t.Fatalf("agent never received stop; commands: %v", cmds)
	}
	resynced := false
	for _, c := range cmds[lastStop:] {
		if strings.HasPrefix(c, "play 0") {
			resynced = true
		}
	}
	if !resynced {
		t.Fatalf("play after clear+add resumed instead of resyncing; commands after stop: %v", cmds[lastStop:])
	}

	if st := runOK(t, rw, "status"); st["state"] != "play" {
		t.Fatalf("state = %q after replace sequence, want play", st["state"])
	}
}

// TestStopThenPlayResyncsAgent: after a stop, play must resync the agent
// (full play command from the track start) immediately, without waiting for
// the agent's next periodic state report.
func TestStopThenPlayResyncsAgent(t *testing.T) {
	_, alog, rw := newAgentQueueTestApp(t)
	runOK(t, rw, `add "one.flac"`)
	runOK(t, rw, `add "two.flac"`)
	runOK(t, rw, "play")

	runOK(t, rw, "stop")
	runOK(t, rw, "play")

	cmds := alog.list()
	lastStop := -1
	for i, c := range cmds {
		if c == "stop" {
			lastStop = i
		}
	}
	if lastStop < 0 {
		t.Fatalf("agent never received stop; commands: %v", cmds)
	}
	resynced := false
	for _, c := range cmds[lastStop:] {
		if strings.HasPrefix(c, "play 0") {
			resynced = true
		}
	}
	if !resynced {
		t.Fatalf("play after stop did not restart the track; commands after stop: %v", cmds[lastStop:])
	}
	if st := runOK(t, rw, "status"); st["state"] != "play" {
		t.Fatalf("state = %q after stop+play, want play", st["state"])
	}
}

// TestAddToEmptyQueueSendsNoAgentCommands verifies the agent-target path:
// populating an empty queue must not send any playback command to an agent.
func TestAddToEmptyQueueSendsNoAgentCommands(t *testing.T) {
	clientConn, serverConn := net.Pipe()
	t.Cleanup(func() {
		_ = clientConn.Close()
		_ = serverConn.Close()
	})

	a := &app{
		logger:         log.New(os.Stdout, "test: ", 0),
		devices:        map[string]*device{"sat": {ID: "sat", Name: "Sat", Type: "agent"}},
		webTargets:     make(map[string]*webTarget),
		enabledOutputs: map[string]bool{"sat": true},
		primaryOutput:  "sat",
		agentResumes:   make(map[string]*agentResume),
		mpdHub:         newNotifyHub(),
		pendingNextPos: -1,
	}
	a.paths.PlayQueueFile = filepath.Join(t.TempDir(), "play_queue.json")
	at := &agentTarget{
		writer:  bufio.NewWriter(clientConn),
		conn:    clientConn,
		alive:   true,
		done:    make(chan struct{}),
		app:     a,
		respCh:  make(chan agentResp, 1),
		agState: "stop",
	}
	a.agentTargets = map[string]*agentTarget{"sat": at}

	if err := a.addSongsToPlaylist([]string{"1"}, "add"); err != nil {
		t.Fatalf("addSongsToPlaylist: %v", err)
	}

	_ = serverConn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	buf := make([]byte, 256)
	n, err := serverConn.Read(buf)
	if err == nil || n > 0 {
		t.Fatalf("agent received %q after add to empty queue, want no commands", string(buf[:n]))
	}
	if ne, ok := err.(net.Error); !ok || !ne.Timeout() {
		t.Fatalf("agent read ended with %v, want timeout (no data)", err)
	}
}
