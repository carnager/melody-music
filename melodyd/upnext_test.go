// SPDX-License-Identifier: GPL-3.0-only
package main

import (
	"fmt"
	"io"
	"log"
	"slices"
	"strings"
	"testing"
)

func TestRequestQueueReturnsToNormalOccurrence(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `melody_context play "Road" 0`)
	base := slices.Clone(a.queueIDs)
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext append %d gamma.flac delta.flac`, a.queueVersion))
	requests := slices.Clone(a.upNext.Pending)
	if len(requests) != 2 {
		t.Fatal(a.upNext)
	}
	if got := a.nextQueuePos(); got != a.requestPosition(requests[0]) {
		t.Fatal(got)
	}
	if !a.advanceRequest(a.nextQueuePos()) || a.upNext.Active != requests[0] {
		t.Fatal(a.upNext)
	}
	if !a.advanceRequest(a.nextQueuePos()) || a.upNext.Active != requests[1] {
		t.Fatal(a.upNext)
	}
	if !a.advanceRequest(a.nextQueuePos()) || a.upNext.Active != 0 {
		t.Fatal(a.upNext)
	}
	if !slices.Equal(a.queueIDs, base) || a.queueIDs[a.curQueuePos] != base[1] {
		t.Fatal(a.queueIDs, a.curQueuePos)
	}
}

func TestRequestQueuePrependDuplicatesAndConflict(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `melody_context play "Road" 0`)
	rev := a.queueVersion
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext append %d beta.flac beta.flac`, rev))
	first := slices.Clone(a.upNext.Pending)
	if first[0] == first[1] {
		t.Fatal("duplicate occurrences share identity")
	}
	if dispatchError(t, a, fmt.Sprintf(`melody_upnext clear %d`, rev)) == nil {
		t.Fatal("stale edit accepted")
	}
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext prepend %d gamma.flac`, a.queueVersion))
	if !slices.Equal(a.upNext.Pending[1:], first) {
		t.Fatal(a.upNext)
	}
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext move %d %d 0`, a.queueVersion, first[1]))
	if a.upNext.Pending[0] != first[1] {
		t.Fatal(a.upNext)
	}
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext clear %d`, a.queueVersion))
	if len(a.queueIDs) != 2 || len(a.upNext.Pending) != 0 {
		t.Fatal(a.upNext, a.queueIDs)
	}
}

func TestRequestQueueShuffleAndClearWhilePlaying(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `melody_context play "Road" 0`)
	a.modeRandom = true
	a.generateShuffle()
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext append %d gamma.flac delta.flac`, a.queueVersion))
	if !a.advanceRequest(a.nextQueuePos()) {
		t.Fatal("no request transition")
	}
	active := a.upNext.Active
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext clear %d`, a.queueVersion))
	if a.upNext.Active != active || len(a.upNext.Pending) != 0 {
		t.Fatal(a.upNext)
	}
	if !a.advanceRequest(a.nextQueuePos()) || a.curQueuePos != 1 {
		t.Fatal(a.upNext, a.curQueuePos)
	}
}

func TestRequestQueueNaturalAdvanceAndExternalDelete(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `melody_context play "Road" 0`)
	base := slices.Clone(a.queueIDs)
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext append %d beta.flac delta.flac`, a.queueVersion))
	requests := slices.Clone(a.upNext.Pending)
	a.advanceTrack()
	if a.upNext.Active != requests[0] {
		t.Fatal(a.upNext)
	}
	current := dispatchCapture(t, a, "currentsong")
	if !strings.Contains(current, fmt.Sprintf("Id: %d\n", requests[0])) {
		t.Fatal(current)
	}
	dispatchCapture(t, a, fmt.Sprintf("deleteid %d", requests[0]))
	if a.upNext.Active != requests[1] {
		t.Fatal(a.upNext)
	}
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext return %d`, a.queueVersion))
	if a.upNext.Active != 0 || !slices.Equal(a.queueIDs, base) || a.curQueuePos != 1 {
		t.Fatal(a.upNext, a.queueIDs, a.curQueuePos)
	}
}

func TestRequestQueueStoredPlaylistEditPreservesDetour(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `melody_context play "Road" 0`)
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext append %d gamma.flac delta.flac`, a.queueVersion))
	a.advanceRequest(a.nextQueuePos())
	active := a.upNext.Active
	pending := slices.Clone(a.upNext.Pending)
	dispatchCapture(t, a, `playlistadd "Road" "gamma.flac"`)
	if a.upNext.Active != active || !slices.Equal(a.upNext.Pending, pending) {
		t.Fatal(a.upNext)
	}
	a.advanceRequest(a.nextQueuePos())
	a.advanceRequest(a.nextQueuePos())
	if a.curQueuePos != 1 || len(a.playQueue) != 3 {
		t.Fatal(a.curQueuePos, a.playQueue)
	}
}

func TestRequestQueueEmptyBaseExplicitStart(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, "clear")
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext append %d gamma.flac delta.flac`, a.queueVersion))
	first := a.upNext.Pending[0]
	dispatchCapture(t, a, "play")
	if a.upNext.Active != first || len(a.upNext.Pending) != 1 {
		t.Fatal(a.upNext)
	}
	a.advanceRequest(a.nextQueuePos())
	a.advanceRequest(a.nextQueuePos())
	if len(a.playQueue) != 0 || a.upNext.Active != 0 {
		t.Fatal(a.upNext, a.playQueue)
	}
}

func TestRequestQueueUndoAndInsertion(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `melody_context play "Road" 0`)
	base := slices.Clone(a.queueIDs)
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext append %d gamma.flac delta.flac`, a.queueVersion))
	first := slices.Clone(a.upNext.Pending)
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext insert %d 1 beta.flac`, a.queueVersion))
	if a.upNext.Pending[0] != first[0] || a.upNext.Pending[2] != first[1] {
		t.Fatal(a.upNext)
	}
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext undo %d`, a.queueVersion))
	if !slices.Equal(a.upNext.Pending, first) {
		t.Fatal(a.upNext)
	}
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext clear %d`, a.queueVersion))
	if !slices.Equal(a.queueIDs, base) {
		t.Fatal(a.queueIDs)
	}
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext undo %d`, a.queueVersion))
	if !slices.Equal(a.upNext.Pending, first) {
		t.Fatal(a.upNext)
	}
	a.advanceRequest(a.nextQueuePos())
	if dispatchError(t, a, fmt.Sprintf(`melody_upnext undo %d`, a.queueVersion)) == nil {
		t.Fatal("undo crossed playback boundary")
	}
}

func TestRequestQueuePersistenceKeepsOccurrenceAndReturnIDs(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `melody_context play "Road" 0`)
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext append %d beta.flac beta.flac`, a.queueVersion))
	a.advanceRequest(a.nextQueuePos())
	restored := &app{db: a.db, mpdHub: newNotifyHub(), pendingNextPos: -1, logger: log.New(io.Discard, "", 0)}
	restored.paths.PlayQueueFile = a.paths.PlayQueueFile
	restored.restorePlayQueue()
	if restored.upNext.Active != a.upNext.Active || !slices.Equal(restored.upNext.Pending, a.upNext.Pending) || !slices.Equal(restored.upNext.Return, a.upNext.Return) || !slices.Equal(restored.queueIDs, a.queueIDs) {
		t.Fatal(restored.upNext, restored.queueIDs)
	}
	if restored.queueVersion <= a.queueVersion {
		t.Fatal("restart must invalidate old revisions")
	}
	for _, id := range restored.queueIDs {
		if id > restored.queueIDCounter {
			t.Fatal("restored counter can reuse an occurrence ID")
		}
	}
}

func TestRequestQueueModesAndMissingReturn(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `melody_context play "Road" 0`)
	base := slices.Clone(a.queueIDs)
	a.modeConsume = true
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext append %d gamma.flac delta.flac`, a.queueVersion))
	a.advanceRequest(a.nextQueuePos())
	if a.requestPosition(base[0]) >= 0 {
		t.Fatal("consume must remove the completed base occurrence")
	}
	a.modeSingle = true
	a.modeRepeat = true
	if a.nextQueuePos() != -1 {
		t.Fatal("single must stop at the request boundary")
	}
	active := a.upNext.Active
	a.advanceTrack()
	if a.upNext.Active != active || len(a.upNext.Pending) != 1 {
		t.Fatal(a.upNext)
	}
	a.modeSingle = false
	a.advanceRequest(a.nextQueuePos())
	a.eraseRequest(base[1])
	if a.nextQueuePos() != -1 {
		t.Fatal("deleted continuation must not choose an unrelated track")
	}
	a.advanceRequest(-1)
	if a.upNext.Active != 0 || a.curQueuePos != -1 {
		t.Fatal(a.upNext, a.curQueuePos)
	}
}

func TestStockQueueEditsDoNotPersistRequests(t *testing.T) {
	a, _ := newContextApp(t)
	dispatchCapture(t, a, `melody_context play "Road" 0`)
	dispatchCapture(t, a, fmt.Sprintf(`melody_upnext append %d gamma.flac`, a.queueVersion))
	dispatchCapture(t, a, `add "delta.flac"`)
	stored := dispatchCapture(t, a, `listplaylistinfo "Road"`)
	if strings.Contains(stored, "Title: gamma") || !strings.Contains(stored, "Title: delta") {
		t.Fatalf("requests leaked into stored list or ordinary edit lost: %s", stored)
	}
}
