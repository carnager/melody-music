package main

// Playback contexts (docs/protocol.md). A context is a stored playlist
// materialized into the one MPD queue: switching to it stashes the queue it
// displaces and remembers where each playlist was left, so clients can move
// between independently playable lists without losing anything. Stock
// clients only ever see an ordinary queue — every switch is a normal queue
// replacement plus a player event.

import (
	"errors"
	"fmt"
	"strconv"
)

// captureTransport reads the current position and pause state from the
// primary output. Must be called WITHOUT playQueueMu: the IPC round trip
// would otherwise hold the queue lock across the network.
func (a *app) captureTransport() (elapsed float64, paused bool) {
	elapsed, paused, _ = captureOutputState(a.primaryTarget())
	return elapsed, paused
}

// startEnabledOutputsAt restarts every enabled output on the current queue
// window at elapsed, honoring paused. This is the per-switch analogue of
// enableOutput's single-target startOutputAt.
func (a *app) startEnabledOutputsAt(elapsed float64, paused bool) {
	for _, ti := range a.enabledTargetInfos() {
		a.startOutputAt(ti, elapsed, paused, -1)
	}
}

// rememberContextPositionLocked records where the active context was left.
// Caller holds playQueueMu.
func (a *app) rememberContextPositionLocked(elapsed float64) {
	if a.activeContext == "" {
		return
	}
	if a.ctxPositions == nil {
		a.ctxPositions = map[string]contextPos{}
	}
	a.ctxPositions[a.activeContext] = contextPos{Pos: a.curQueuePos, Elapsed: elapsed}
}

// switchToPlaylistContext materializes a stored playlist into the queue and
// starts playing it. With hasPos the playback starts at that row from the
// beginning; without it the playlist resumes where it was last left.
func (a *app) switchToPlaylistContext(name string, pos int, hasPos bool) error {
	id, err := a.db.playlistIDByName(name)
	if err != nil {
		return fmt.Errorf("playlist not found: %s", name)
	}
	songIDs, err := a.db.playlistTrackSongIDs(id)
	if err != nil {
		return err
	}
	elapsed, _ := a.captureTransport()

	a.playQueueMu.Lock()
	// Leaving a playlist records its resume point; leaving the queue
	// context stashes the queue itself. An orphan stash (the previous
	// active playlist was removed while playing) is kept as-is — the queue
	// it holds is still the one the client expects to come back to.
	if a.activeContext != "" {
		a.rememberContextPositionLocked(elapsed)
	} else if a.ctxStash == nil {
		a.ctxStash = &queueStash{
			Songs:      append([]string{}, a.playQueue...),
			Priorities: append([]int{}, a.queuePriority...),
			Pos:        a.curQueuePos,
			Elapsed:    elapsed,
		}
	}

	startPos, startElapsed := 0, 0.0
	if hasPos {
		startPos = pos
	} else if remembered, ok := a.ctxPositions[name]; ok {
		startPos = remembered.Pos
		startElapsed = remembered.Elapsed
	}
	if startPos < 0 || startPos >= len(songIDs) {
		startPos = 0
		startElapsed = 0
	}
	a.replaceQueueLocked(songIDs, make([]int, len(songIDs)), startPos)
	a.activeContext = name
	a.ctxLabel = ""
	a.savePlayQueue()
	a.playQueueMu.Unlock()

	// An explicit play always starts audio; the resume seek is best effort,
	// the same accuracy class as the periodic play-state snapshot.
	a.startEnabledOutputsAt(startElapsed, false)
	a.mpdHub.notify(SubPlaylist, SubPlayer, SubContext)
	return nil
}

// switchToTrackListContext materializes an arbitrary list of tracks — a
// client's working tab — as the active context. It stashes the queue the
// same way a playlist switch does, so an ad-hoc list plays without
// destroying what was queued. The context carries no name: the client owns
// the list, the server only holds the stash to come back to.
func (a *app) switchToTrackListContext(songIDs []string, pos int) error {
	if len(songIDs) == 0 {
		return fmt.Errorf("no tracks to play")
	}
	elapsed, _ := a.captureTransport()

	a.playQueueMu.Lock()
	if a.activeContext != "" {
		a.rememberContextPositionLocked(elapsed)
	} else if a.ctxStash == nil {
		a.ctxStash = &queueStash{
			Songs:      append([]string{}, a.playQueue...),
			Priorities: append([]int{}, a.queuePriority...),
			Pos:        a.curQueuePos,
			Elapsed:    elapsed,
		}
	}
	if pos < 0 || pos >= len(songIDs) {
		pos = 0
	}
	a.replaceQueueLocked(songIDs, make([]int, len(songIDs)), pos)
	a.activeContext = ""
	a.ctxLabel = ""
	a.savePlayQueue()
	a.playQueueMu.Unlock()

	a.startEnabledOutputsAt(0, false)
	a.mpdHub.notify(SubPlaylist, SubPlayer, SubContext)
	return nil
}

// hasQueueStash reports whether a displaced queue is waiting to be restored.
func (a *app) hasQueueStash() bool {
	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()
	return a.ctxStash != nil
}

// switchToQueueContext restores the stashed queue. Without a position it
// resumes where the queue was left and preserves the current pause state —
// this is a switch back, not a play command.
func (a *app) switchToQueueContext(pos int, hasPos bool) error {
	elapsed, paused := a.captureTransport()

	a.playQueueMu.Lock()
	if a.ctxStash == nil {
		// Nothing displaced: a bare switch is a no-op, and one with a
		// position plays that row of the current queue.
		if !hasPos {
			a.playQueueMu.Unlock()
			return nil
		}
		if pos < 0 || pos >= len(a.playQueue) {
			a.playQueueMu.Unlock()
			return fmt.Errorf("position out of range: %d", pos)
		}
		a.curQueuePos = pos
		a.activeContext = ""
		a.savePlayQueue()
		a.playQueueMu.Unlock()
		a.startEnabledOutputsAt(0, false)
		a.mpdHub.notify(SubPlaylist, SubPlayer, SubContext)
		return nil
	}

	a.rememberContextPositionLocked(elapsed)
	stash := a.ctxStash
	startPos, startElapsed, startPaused := stash.Pos, stash.Elapsed, paused
	if hasPos {
		startPos, startElapsed, startPaused = pos, 0, false
	}
	priorities := stash.Priorities
	if len(priorities) != len(stash.Songs) {
		priorities = make([]int, len(stash.Songs))
	}
	a.replaceQueueLocked(stash.Songs, priorities, startPos)
	a.activeContext = ""
	a.ctxLabel = ""
	a.ctxStash = nil
	a.savePlayQueue()
	a.playQueueMu.Unlock()

	a.startEnabledOutputsAt(startElapsed, startPaused)
	a.mpdHub.notify(SubPlaylist, SubPlayer, SubContext)
	return nil
}

// contextQueueTracks lists the queue context's contents: the stash when a
// playlist is active, otherwise the live queue. Clients use it to show the
// queue tab while playing a list.
func (a *app) contextQueueTracks() ([]map[string]any, error) {
	a.playQueueMu.Lock()
	songIDs := append([]string{}, a.playQueue...)
	if a.ctxStash != nil {
		songIDs = append([]string{}, a.ctxStash.Songs...)
	}
	a.playQueueMu.Unlock()

	tracks := make([]map[string]any, 0, len(songIDs))
	for _, songID := range songIDs {
		id, err := strconv.ParseInt(songID, 10, 64)
		if err != nil {
			continue
		}
		track, err := a.db.trackByID(id)
		if err != nil || track == nil {
			continue
		}
		tracks = append(tracks, track)
	}
	return tracks, nil
}

// errNoQueueStash reports that nothing is displaced, so the queue context is
// the live queue and ordinary queue commands already address it.
var errNoQueueStash = errors.New("no stashed queue")

// editQueueStash applies an edit to the displaced queue. While another list
// is the active queue, the queue context lives in the stash — that is the
// list the client still shows in its Queue tab, so edits aimed at the queue
// have to reach it. Without the stash they would land in the materialized
// list nobody is looking at.
func (a *app) editQueueStash(mutate func(stash *queueStash) error) error {
	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()
	if a.ctxStash == nil {
		return errNoQueueStash
	}
	if err := mutate(a.ctxStash); err != nil {
		return err
	}
	if len(a.ctxStash.Priorities) != len(a.ctxStash.Songs) {
		a.ctxStash.Priorities = make([]int, len(a.ctxStash.Songs))
	}
	if a.ctxStash.Pos >= len(a.ctxStash.Songs) {
		a.ctxStash.Pos = 0
		a.ctxStash.Elapsed = 0
	}
	a.savePlayQueue()
	a.mpdHub.notify(SubContext)
	return nil
}

// queueStashAdd inserts songs at pos, appending when pos is negative or past
// the end. The resume point follows the track it was on.
func (a *app) queueStashAdd(songIDs []string, pos int) error {
	return a.editQueueStash(func(stash *queueStash) error {
		if pos < 0 || pos > len(stash.Songs) {
			stash.Songs = append(stash.Songs, songIDs...)
			return nil
		}
		tail := append([]string{}, stash.Songs[pos:]...)
		stash.Songs = append(append(stash.Songs[:pos:pos], songIDs...), tail...)
		if stash.Pos >= pos {
			stash.Pos += len(songIDs)
		}
		return nil
	})
}

// queueStashDelete removes the given rows.
func (a *app) queueStashDelete(positions []int) error {
	return a.editQueueStash(func(stash *queueStash) error {
		drop := map[int]bool{}
		for _, position := range positions {
			drop[position] = true
		}
		kept := make([]string, 0, len(stash.Songs))
		for index, songID := range stash.Songs {
			if drop[index] {
				if index < stash.Pos {
					stash.Pos--
				}
				continue
			}
			kept = append(kept, songID)
		}
		stash.Songs = kept
		if stash.Pos < 0 {
			stash.Pos = 0
		}
		return nil
	})
}

// queueStashMove moves one row to another index, the granular step clients
// already compose multi-row reorders from.
func (a *app) queueStashMove(from, to int) error {
	return a.editQueueStash(func(stash *queueStash) error {
		if from < 0 || from >= len(stash.Songs) || to < 0 || to >= len(stash.Songs) {
			return fmt.Errorf("position out of range")
		}
		songID := stash.Songs[from]
		stash.Songs = append(stash.Songs[:from], stash.Songs[from+1:]...)
		tail := append([]string{}, stash.Songs[to:]...)
		stash.Songs = append(append(stash.Songs[:to:to], songID), tail...)
		switch {
		case stash.Pos == from:
			stash.Pos = to
		case from < stash.Pos && to >= stash.Pos:
			stash.Pos--
		case from > stash.Pos && to <= stash.Pos:
			stash.Pos++
		}
		return nil
	})
}

// queueStashReplace makes the given songs the queue context and switches back
// to it playing at pos: "replace the queue and play" means the queue is this
// list now, so it also stops being the displaced one.
func (a *app) queueStashReplace(songIDs []string, pos int) error {
	if len(songIDs) == 0 {
		return fmt.Errorf("no tracks")
	}
	if err := a.editQueueStash(func(stash *queueStash) error {
		stash.Songs = append([]string{}, songIDs...)
		stash.Priorities = nil
		stash.Pos = pos
		stash.Elapsed = 0
		return nil
	}); err != nil {
		return err
	}
	return a.switchToQueueContext(pos, true)
}

// resyncTrackListContext re-materializes a client's ad-hoc list after it was
// edited, keeping the playing track playing — the working-tab counterpart of
// resyncActiveContext. Editing the list you are listening to is a live edit,
// not a restart.
func (a *app) resyncTrackListContext(songIDs []string) error {
	if a.activeContextName() != "" {
		return fmt.Errorf("a playlist context is active")
	}
	if !a.hasQueueStash() {
		return errNoQueueStash
	}
	elapsed, paused := a.captureTransport()

	a.playQueueMu.Lock()
	playing := ""
	if a.curQueuePos >= 0 && a.curQueuePos < len(a.playQueue) {
		playing = a.playQueue[a.curQueuePos]
	}
	startPos := a.curQueuePos
	moved := false
	if playing != "" {
		for index, songID := range songIDs {
			if songID == playing {
				startPos = index
				moved = true
				break
			}
		}
	}
	if !moved {
		elapsed = 0
	}
	if startPos < 0 || startPos >= len(songIDs) {
		startPos = 0
		elapsed = 0
	}
	a.replaceQueueLocked(songIDs, make([]int, len(songIDs)), startPos)
	a.savePlayQueue()
	a.playQueueMu.Unlock()

	if len(songIDs) > 0 {
		a.startEnabledOutputsAt(elapsed, paused)
	}
	a.mpdHub.notify(SubPlaylist, SubPlayer, SubContext)
	return nil
}

// renameActiveContext follows a playlist rename, and dropActiveContext
// releases the name when the playlist stops existing (rm/clear) while its
// materialized content keeps playing. The stash stays restorable either way.
func (a *app) renameActiveContext(from, to string) {
	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()
	if position, ok := a.ctxPositions[from]; ok {
		if a.ctxPositions == nil {
			a.ctxPositions = map[string]contextPos{}
		}
		a.ctxPositions[to] = position
		delete(a.ctxPositions, from)
	}
	if a.activeContext == from {
		a.activeContext = to
		a.savePlayQueue()
	}
}

func (a *app) dropActiveContext(name string) {
	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()
	delete(a.ctxPositions, name)
	if a.activeContext == name {
		a.activeContext = ""
		a.savePlayQueue()
	}
}

// setContextLabel tags the active context with the client's own name for the
// list it materialized, so the client can tell its list is the live queue —
// and show the queue's contents there, including what other clients add.
func (a *app) setContextLabel(label string) {
	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()
	a.ctxLabel = label
	a.savePlayQueue()
}

func (a *app) activeContextLabel() string {
	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()
	return a.ctxLabel
}

// activeContextName reports the materialized playlist ("" = queue context).
func (a *app) activeContextName() string {
	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()
	return a.activeContext
}

// resyncActiveContext re-materializes the active playlist after it was
// edited, keeping the currently playing track playing where possible. This
// is what makes editing the list you are listening to feel live: rows
// appear, disappear, and move in the queue exactly as they do in the
// playlist.
func (a *app) resyncActiveContext(name string) {
	if a.activeContextName() != name {
		return
	}
	id, err := a.db.playlistIDByName(name)
	if err != nil {
		return
	}
	songIDs, err := a.db.playlistTrackSongIDs(id)
	if err != nil {
		return
	}
	elapsed, paused := a.captureTransport()

	a.playQueueMu.Lock()
	playing := ""
	if a.curQueuePos >= 0 && a.curQueuePos < len(a.playQueue) {
		playing = a.playQueue[a.curQueuePos]
	}
	// Follow the playing track to its new row; if it is gone, stay at the
	// same index so playback continues with whatever took its place.
	startPos := a.curQueuePos
	moved := false
	if playing != "" {
		for index, songID := range songIDs {
			if songID == playing {
				startPos = index
				moved = true
				break
			}
		}
	}
	if !moved {
		elapsed = 0
	}
	if startPos < 0 || startPos >= len(songIDs) {
		startPos = 0
		elapsed = 0
	}
	a.replaceQueueLocked(songIDs, make([]int, len(songIDs)), startPos)
	a.savePlayQueue()
	a.playQueueMu.Unlock()

	if len(songIDs) > 0 {
		a.startEnabledOutputsAt(elapsed, paused)
	}
	a.mpdHub.notify(SubPlaylist, SubPlayer)
}
