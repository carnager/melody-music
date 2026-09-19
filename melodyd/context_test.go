package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"io"
	"log"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// newContextApp builds a queue-state app with four library tracks and two
// stored playlists, so context switches have somewhere to go and a queue to
// displace.
func newContextApp(t *testing.T) (*app, *webTarget) {
	t.Helper()
	a, wt, _ := newQueueStateTestApp(t)

	artistID, err := a.db.upsertArtist("Context Artist")
	if err != nil {
		t.Fatalf("upsertArtist: %v", err)
	}
	albumID, err := a.db.upsertAlbum(artistID, "Context Album", "2024")
	if err != nil {
		t.Fatalf("upsertAlbum: %v", err)
	}
	for index, title := range []string{"alpha", "beta", "gamma", "delta"} {
		if _, err := a.db.upsertTrack(&trackMeta{
			AlbumID:     albumID,
			Artist:      "Context Artist",
			Title:       title,
			TrackNumber: index + 1,
			Duration:    100,
			Path:        filepath.Join(a.cfg.Library.MusicDir, title+".flac"),
			albumArtist: "Context Artist",
			album:       "Context Album",
			date:        "2024",
		}); err != nil {
			t.Fatalf("upsertTrack: %v", err)
		}
	}
	add := func(playlist, title string) {
		if err := dispatchError(t, a, "playlistadd \""+playlist+"\" \""+title+".flac\""); err != nil {
			t.Fatalf("playlistadd %s/%s: %s", playlist, title, err.Error())
		}
	}
	add("Road", "alpha")
	add("Road", "beta")
	add("Calm", "gamma")
	add("Calm", "delta")
	return a, wt
}

// queueTitles reports the current queue as track titles.
func queueTitles(t *testing.T, a *app) []string {
	t.Helper()
	a.playQueueMu.Lock()
	songIDs := append([]string{}, a.playQueue...)
	a.playQueueMu.Unlock()
	var titles []string
	for _, songID := range songIDs {
		id, err := strconv.ParseInt(songID, 10, 64)
		if err != nil {
			t.Fatalf("bad song id %q", songID)
		}
		track, err := a.db.trackByID(id)
		if err != nil {
			t.Fatalf("trackByID: %v", err)
		}
		titles = append(titles, stringify(track["title"]))
	}
	return titles
}

func contextName(t *testing.T, a *app) string {
	t.Helper()
	out := dispatchCapture(t, a, "melody_context")
	for _, line := range strings.Split(out, "\n") {
		if strings.HasPrefix(line, "context: ") {
			return strings.TrimPrefix(line, "context: ")
		}
	}
	t.Fatalf("no context line in %q", out)
	return ""
}

func TestContextDefaultsToTheQueue(t *testing.T) {
	a, _ := newContextApp(t)
	if got := contextName(t, a); got != "" {
		t.Fatalf("default context = %q, want the queue context", got)
	}
}

func TestContextPlayMaterializesAndStashes(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `add "one.flac"`)
	dispatchCapture(t, a, `add "two.flac"`)
	beforeVersion := a.queueVersion

	dispatchCapture(t, a, `melody_context play "Road"`)
	if got := contextName(t, a); got != "Road" {
		t.Fatalf("active context = %q, want Road", got)
	}
	if got := strings.Join(queueTitles(t, a), ","); got != "alpha,beta" {
		t.Fatalf("materialized queue = %q, want alpha,beta", got)
	}
	if a.queueVersion <= beforeVersion {
		t.Fatalf("queue version must bump on a context switch")
	}
	a.playQueueMu.Lock()
	stashed := len(a.ctxStash.Songs)
	a.playQueueMu.Unlock()
	if stashed != 2 {
		t.Fatalf("stash holds %d songs, want the 2 displaced queue tracks", stashed)
	}
}

func TestContextPlayAtPositionAndBadArguments(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `melody_context play "Road" 1`)
	a.playQueueMu.Lock()
	pos := a.curQueuePos
	a.playQueueMu.Unlock()
	if pos != 1 {
		t.Fatalf("curQueuePos = %d, want the requested row 1", pos)
	}
	if err := dispatchError(t, a, `melody_context play "Nope"`); err == nil {
		t.Fatalf("unknown playlist must ACK")
	}
	if err := dispatchError(t, a, `melody_context play "Road" -1`); err == nil {
		t.Fatalf("negative position must ACK")
	}
	if err := dispatchError(t, a, `melody_context bogus`); err == nil {
		t.Fatalf("unknown subcommand must ACK")
	}
}

func TestContextPlayClearsPriorityBookkeeping(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `add "one.flac"`)
	dispatchCapture(t, a, `add "two.flac"`)
	a.playQueueMu.Lock()
	a.prioReturnPos = 1
	a.prioPlayedIDs = map[int]bool{7: true}
	a.pendingNextPos = 1
	a.playQueueMu.Unlock()

	dispatchCapture(t, a, `melody_context play "Road"`)
	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()
	// Stale bookkeeping describes positions in the discarded queue: the
	// return cursor and played-memory must be gone, and the preloaded slot
	// must point inside the new queue (planSyncTarget recomputes it).
	if a.prioReturnPos != -1 {
		t.Fatalf("priority return position survived: %d", a.prioReturnPos)
	}
	if len(a.prioPlayedIDs) != 0 {
		t.Fatalf("played-through-priority memory survived: %v", a.prioPlayedIDs)
	}
	if a.pendingNextPos >= len(a.playQueue) {
		t.Fatalf("preloaded slot %d is outside the new queue of %d", a.pendingNextPos,
			len(a.playQueue))
	}
}

func TestContextSwitchBetweenPlaylistsRemembersPositions(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `melody_context play "Road" 1`)
	dispatchCapture(t, a, `melody_context play "Calm" 1`)
	if got := strings.Join(queueTitles(t, a), ","); got != "gamma,delta" {
		t.Fatalf("second context = %q, want gamma,delta", got)
	}
	a.playQueueMu.Lock()
	remembered := a.ctxPositions["Road"].Pos
	stashSongs := len(a.ctxStash.Songs)
	a.playQueueMu.Unlock()
	if remembered != 1 {
		t.Fatalf("Road resume position = %d, want 1", remembered)
	}
	if stashSongs != 0 {
		t.Fatalf("playlist→playlist must not re-stash; stash now holds %d", stashSongs)
	}

	// Returning resumes where the playlist was left.
	dispatchCapture(t, a, `melody_context play "Road"`)
	a.playQueueMu.Lock()
	pos := a.curQueuePos
	a.playQueueMu.Unlock()
	if pos != 1 {
		t.Fatalf("resumed Road at %d, want its remembered row 1", pos)
	}
}

func TestContextQueueRestoresTheStash(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `add "one.flac"`)
	dispatchCapture(t, a, `add "two.flac"`)
	a.playQueueMu.Lock()
	a.curQueuePos = 1
	a.playQueueMu.Unlock()

	dispatchCapture(t, a, `melody_context play "Road"`)
	dispatchCapture(t, a, `melody_context queue`)
	if got := contextName(t, a); got != "" {
		t.Fatalf("context after restore = %q, want the queue context", got)
	}
	if got := strings.Join(queueTitles(t, a), ","); got != "one.flac,two.flac" {
		t.Fatalf("restored queue = %q, want the original tracks", got)
	}
	a.playQueueMu.Lock()
	pos, stash, remembered := a.curQueuePos, a.ctxStash, a.ctxPositions["Road"].Pos
	a.playQueueMu.Unlock()
	if pos != 1 {
		t.Fatalf("restored position = %d, want the stashed 1", pos)
	}
	if stash != nil {
		t.Fatalf("stash must be consumed by the restore")
	}
	if remembered != 0 {
		t.Fatalf("Road position = %d, want its left-at row recorded", remembered)
	}
}

func TestContextQueueWithoutStash(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `add "one.flac"`)
	dispatchCapture(t, a, `add "two.flac"`)
	// No stash: a bare switch is a no-op, a positional one plays that row.
	dispatchCapture(t, a, `melody_context queue`)
	if got := strings.Join(queueTitles(t, a), ","); got != "one.flac,two.flac" {
		t.Fatalf("no-op switch changed the queue: %q", got)
	}
	dispatchCapture(t, a, `melody_context queue 1`)
	a.playQueueMu.Lock()
	pos := a.curQueuePos
	a.playQueueMu.Unlock()
	if pos != 1 {
		t.Fatalf("positional switch = %d, want row 1", pos)
	}
	if err := dispatchError(t, a, `melody_context queue 9`); err == nil {
		t.Fatalf("out-of-range position must ACK")
	}
}

func TestContextQueueInfoListsStashElseLiveQueue(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `add "one.flac"`)
	live := fileOrder(dispatchCapture(t, a, `melody_context queueinfo`))
	if len(live) != 1 || !strings.HasSuffix(live[0], "one") {
		t.Fatalf("queueinfo without a stash = %v, want the live queue", live)
	}
	dispatchCapture(t, a, `melody_context play "Road"`)
	stashed := fileOrder(dispatchCapture(t, a, `melody_context queueinfo`))
	if len(stashed) != 1 || !strings.HasSuffix(stashed[0], "one") {
		t.Fatalf("queueinfo with a stash = %v, want the displaced queue", stashed)
	}
	if got := strings.Join(queueTitles(t, a), ","); got != "alpha,beta" {
		t.Fatalf("queueinfo must not disturb the materialized queue: %q", got)
	}
}

func TestContextQueueEditsAreNotWrittenBack(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `melody_context play "Road"`)
	dispatchCapture(t, a, `add "one.flac"`)
	if got := strings.Join(queueTitles(t, a), ","); got != "alpha,beta,one.flac" {
		t.Fatalf("queue edit = %q, want the added track in the materialization", got)
	}
	// The playlist itself is untouched, so switching away and back drops it.
	dispatchCapture(t, a, `melody_context play "Calm"`)
	dispatchCapture(t, a, `melody_context play "Road"`)
	if got := strings.Join(queueTitles(t, a), ","); got != "alpha,beta" {
		t.Fatalf("re-materialized = %q, want the stored playlist contents", got)
	}
}

func TestContextActivePlaylistEditsMirrorIntoTheQueue(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `melody_context play "Road"`)

	dispatchCapture(t, a, `playlistadd "Road" "gamma.flac"`)
	if got := strings.Join(queueTitles(t, a), ","); got != "alpha,beta,gamma" {
		t.Fatalf("after playlistadd = %q, want the row in the queue too", got)
	}
	dispatchCapture(t, a, `playlistmove "Road" 2 0`)
	if got := strings.Join(queueTitles(t, a), ","); got != "gamma,alpha,beta" {
		t.Fatalf("after playlistmove = %q", got)
	}
	dispatchCapture(t, a, `playlistdelete "Road" 0`)
	if got := strings.Join(queueTitles(t, a), ","); got != "alpha,beta" {
		t.Fatalf("after playlistdelete = %q", got)
	}

	// A non-active playlist's edits never touch the queue.
	dispatchCapture(t, a, `playlistadd "Calm" "one.flac"`)
	if got := strings.Join(queueTitles(t, a), ","); got != "alpha,beta" {
		t.Fatalf("editing another playlist changed the queue: %q", got)
	}
}

func TestContextFollowsRenameAndSurvivesRemoval(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `add "one.flac"`)
	dispatchCapture(t, a, `melody_context play "Road"`)
	dispatchCapture(t, a, `rename "Road" "Roadtrip"`)
	if got := contextName(t, a); got != "Roadtrip" {
		t.Fatalf("context after rename = %q, want Roadtrip", got)
	}

	// Removing the active playlist releases the name but keeps playing, and
	// the displaced queue is still restorable.
	dispatchCapture(t, a, `rm "Roadtrip"`)
	if got := contextName(t, a); got != "" {
		t.Fatalf("context after rm = %q, want the queue context", got)
	}
	if got := strings.Join(queueTitles(t, a), ","); got != "alpha,beta" {
		t.Fatalf("rm must not disturb playback: %q", got)
	}
	dispatchCapture(t, a, `melody_context queue`)
	if got := strings.Join(queueTitles(t, a), ","); got != "one.flac" {
		t.Fatalf("stash after rm = %q, want the displaced queue", got)
	}
}

func TestContextPersistenceRoundTripAndLegacyFile(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `add "one.flac"`)
	dispatchCapture(t, a, `melody_context play "Road" 1`)

	data, err := os.ReadFile(a.paths.PlayQueueFile)
	if err != nil {
		t.Fatalf("read play queue: %v", err)
	}
	var saved savedQueue
	if err := json.Unmarshal(data, &saved); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if saved.ActiveContext != "Road" || saved.Stash == nil || len(saved.Stash.Songs) != 1 {
		t.Fatalf("persisted context state = %+v", saved)
	}

	restored := &app{db: a.db, mpdHub: newNotifyHub(), pendingNextPos: -1,
		logger: log.New(io.Discard, "", 0)}
	restored.paths.PlayQueueFile = a.paths.PlayQueueFile
	restored.restorePlayQueue()
	if restored.activeContext != "Road" || restored.ctxStash == nil {
		t.Fatalf("restore lost the context: %q stash=%v", restored.activeContext,
			restored.ctxStash)
	}

	// A file from before contexts still loads, as no context state.
	legacy := filepath.Join(t.TempDir(), "legacy.json")
	if err := os.WriteFile(legacy, []byte(`{"songs":["1","2"],"version":3}`), 0o644); err != nil {
		t.Fatalf("write legacy: %v", err)
	}
	old := &app{db: a.db, mpdHub: newNotifyHub(), pendingNextPos: -1,
		logger: log.New(io.Discard, "", 0)}
	old.paths.PlayQueueFile = legacy
	old.restorePlayQueue()
	if len(old.playQueue) != 2 || old.activeContext != "" || old.ctxStash != nil {
		t.Fatalf("legacy restore = %d songs, context %q", len(old.playQueue), old.activeContext)
	}
}

func TestContextResumePositionClampedAfterShrink(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `melody_context play "Road" 1`)
	dispatchCapture(t, a, `melody_context play "Calm"`)
	// Road remembers row 1; shrink it to a single track and come back.
	dispatchCapture(t, a, `playlistdelete "Road" 1`)
	dispatchCapture(t, a, `melody_context play "Road"`)
	a.playQueueMu.Lock()
	pos, length := a.curQueuePos, len(a.playQueue)
	a.playQueueMu.Unlock()
	if length != 1 || pos != 0 {
		t.Fatalf("resume after shrink: pos %d of %d, want clamped to 0 of 1", pos, length)
	}
}

// stashTitles reports the displaced queue as track titles.
func stashTitles(t *testing.T, a *app) []string {
	t.Helper()
	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()
	if a.ctxStash == nil {
		return nil
	}
	var titles []string
	for _, songID := range a.ctxStash.Songs {
		id, err := strconv.ParseInt(songID, 10, 64)
		if err != nil {
			t.Fatalf("bad song id %q", songID)
		}
		track, err := a.db.trackByID(id)
		if err != nil {
			t.Fatalf("trackByID: %v", err)
		}
		titles = append(titles, stringify(track["title"]))
	}
	return titles
}

func TestContextQueueEditsReachTheStash(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `add "alpha.flac"`)
	dispatchCapture(t, a, `add "beta.flac"`)
	dispatchCapture(t, a, `melody_context play "Calm"`)

	if err := dispatchError(t, a, `melody_context queueadd -1 "gamma.flac"`); err != nil {
		t.Fatalf("queueadd: %s", err.Error())
	}
	if got := strings.Join(stashTitles(t, a), ","); got != "alpha,beta,gamma" {
		t.Fatalf("after queueadd stash = %q", got)
	}
	// The list that is actually playing stays exactly as it was.
	if got := strings.Join(queueTitles(t, a), ","); got != "gamma,delta" {
		t.Fatalf("queue edit leaked into the active context: %q", got)
	}

	if err := dispatchError(t, a, `melody_context queuemove 2 0`); err != nil {
		t.Fatalf("queuemove: %s", err.Error())
	}
	if got := strings.Join(stashTitles(t, a), ","); got != "gamma,alpha,beta" {
		t.Fatalf("after queuemove stash = %q", got)
	}
	if err := dispatchError(t, a, `melody_context queuedelete 0 2`); err != nil {
		t.Fatalf("queuedelete: %s", err.Error())
	}
	if got := strings.Join(stashTitles(t, a), ","); got != "alpha" {
		t.Fatalf("after queuedelete stash = %q", got)
	}
}

func TestContextQueueReplaceSwitchesBackAndPlays(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `add "alpha.flac"`)
	dispatchCapture(t, a, `melody_context play "Calm"`)

	if err := dispatchError(t, a, `melody_context queuereplace 0 "beta.flac" "gamma.flac"`); err != nil {
		t.Fatalf("queuereplace: %s", err.Error())
	}
	if got := contextName(t, a); got != "" {
		t.Fatalf("after queuereplace context = %q, want the queue context", got)
	}
	if got := strings.Join(queueTitles(t, a), ","); got != "beta,gamma" {
		t.Fatalf("after queuereplace queue = %q", got)
	}
	if a.hasQueueStash() {
		t.Fatalf("replacing the queue must leave nothing displaced")
	}
}

func TestContextQueueEditWithoutStashIsRejected(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `add "alpha.flac"`)
	if err := dispatchError(t, a, `melody_context queueadd -1 "beta.flac"`); err == nil {
		t.Fatalf("editing the queue context without a stash must ACK: plain add applies")
	}
}

// dispatchOnOneConn runs several commands over a single connection, the way a
// real client stages a list across lines before consuming it.
func dispatchOnOneConn(t *testing.T, a *app, lines ...string) *mpdError {
	t.Helper()
	c := &mpdConn{
		writer: bufio.NewWriter(&bytes.Buffer{}),
		app:    a,
		logger: log.New(os.Stderr, "", 0),
	}
	for _, line := range lines {
		cmd, args := parseCommand(line)
		if err := c.dispatch(cmd, args); err != nil {
			return err
		}
	}
	return nil
}

func TestContextStageClearsAndFeedsQueueEdits(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `add "alpha.flac"`)
	dispatchCapture(t, a, `melody_context play "Calm"`)

	if err := dispatchOnOneConn(t, a, `melody_context stage "beta.flac"`,
		`melody_context stage`, // bare stage discards
		`melody_context stage "gamma.flac"`,
		`melody_context queueadd -1`); err != nil {
		t.Fatalf("queueadd from staging: %s", err.Error())
	}
	if got := strings.Join(stashTitles(t, a), ","); got != "alpha,gamma" {
		t.Fatalf("staged queue edit = %q", got)
	}
}

func TestScratchPlaylistsAreFlaggedAndListed(t *testing.T) {
	a, _ := newContextApp(t)
	if err := dispatchError(t, a, `melody_scratch "Road" 1`); err != nil {
		t.Fatalf("melody_scratch: %s", err.Error())
	}
	out := dispatchCapture(t, a, "melody_scratch")
	if !strings.Contains(out, "scratch: Road") || strings.Contains(out, "scratch: Calm") {
		t.Fatalf("scratch listing = %q, want only Road", out)
	}
	// A scratch list is a stored playlist like any other: still listed,
	// still playable as a context.
	if !strings.Contains(dispatchCapture(t, a, "listplaylists"), "playlist: Road") {
		t.Fatalf("a scratch list must stay an ordinary stored playlist")
	}
	if err := dispatchError(t, a, `melody_context play "Road"`); err != nil {
		t.Fatalf("play scratch context: %s", err.Error())
	}
	if got := contextName(t, a); got != "Road" {
		t.Fatalf("context = %q, want Road", got)
	}
	// Promotion clears the flag.
	if err := dispatchError(t, a, `melody_scratch "Road" 0`); err != nil {
		t.Fatalf("promote: %s", err.Error())
	}
	if strings.Contains(dispatchCapture(t, a, "melody_scratch"), "Road") {
		t.Fatalf("promoted list must no longer be scratch")
	}
	if err := dispatchError(t, a, `melody_scratch "Nope" 1`); err == nil {
		t.Fatalf("flagging a missing playlist must ACK")
	}
}
