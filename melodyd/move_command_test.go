package main

import (
	"reflect"
	"testing"
)

// newQueueTestApp builds a minimal app with a populated play queue. Song IDs
// are non-numeric so no code path touches the (nil) database.
func newQueueTestApp(songs ...string) *app {
	a := &app{
		mpdHub:      newNotifyHub(),
		playQueue:   append([]string{}, songs...),
		curQueuePos: -1,
	}
	a.queueIDs = make([]int, len(songs))
	for i := range songs {
		a.queueIDs[i] = i + 1
	}
	a.queuePriority = make([]int, len(songs))
	return a
}

// TestMoveCommandSemantics verifies MPD `move FROM TO` semantics: TO is the
// moved song's position in the resulting queue. Downward moves used to be
// off by one (move X X+1 was a no-op).
func TestMoveCommandSemantics(t *testing.T) {
	cases := []struct {
		name string
		cmd  string
		want []string
	}{
		{"down one", "move 0 1", []string{"b", "a", "c"}},
		{"down to end", "move 0 2", []string{"b", "c", "a"}},
		{"up one", "move 2 1", []string{"a", "c", "b"}},
		{"up to front", "move 2 0", []string{"c", "a", "b"}},
		{"same position", "move 1 1", []string{"a", "b", "c"}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			a := newQueueTestApp("a", "b", "c")
			dispatchCapture(t, a, tc.cmd)
			if !reflect.DeepEqual(a.playQueue, tc.want) {
				t.Fatalf("%s: queue = %v, want %v", tc.cmd, a.playQueue, tc.want)
			}
		})
	}
}

// TestMoveCommandTracksCurrentSong verifies curQueuePos follows the moved
// song and shifts correctly for bystanders.
func TestMoveCommandTracksCurrentSong(t *testing.T) {
	// Current song is the one being moved: it ends up at TO.
	a := newQueueTestApp("a", "b", "c")
	a.curQueuePos = 0
	dispatchCapture(t, a, "move 0 2")
	if a.curQueuePos != 2 {
		t.Fatalf("moved current song: curQueuePos = %d, want 2", a.curQueuePos)
	}

	// Current song is a bystander that slides up when a song above it moves down.
	a = newQueueTestApp("a", "b", "c")
	a.curQueuePos = 1 // "b"
	dispatchCapture(t, a, "move 0 2")
	if a.playQueue[a.curQueuePos] != "b" {
		t.Fatalf("bystander: curQueuePos %d points at %q, want \"b\" (queue %v)",
			a.curQueuePos, a.playQueue[a.curQueuePos], a.playQueue)
	}

	// Bystander slides down when a song below it moves above it.
	a = newQueueTestApp("a", "b", "c")
	a.curQueuePos = 0 // "a"
	dispatchCapture(t, a, "move 2 0")
	if a.playQueue[a.curQueuePos] != "a" {
		t.Fatalf("bystander: curQueuePos %d points at %q, want \"a\" (queue %v)",
			a.curQueuePos, a.playQueue[a.curQueuePos], a.playQueue)
	}
}

// TestMoveCommandMovesPriority verifies the priority value travels with the song.
func TestMoveCommandMovesPriority(t *testing.T) {
	a := newQueueTestApp("a", "b", "c")
	a.queuePriority[0] = 30
	dispatchCapture(t, a, "move 0 2")
	if want := []int{0, 0, 30}; !reflect.DeepEqual(a.queuePriority, want) {
		t.Fatalf("priorities = %v, want %v", a.queuePriority, want)
	}
}

// TestMoveIDCommand verifies moveid resolves the song ID and delegates to move.
func TestMoveIDCommand(t *testing.T) {
	a := newQueueTestApp("a", "b", "c") // MPD ids 1, 2, 3
	dispatchCapture(t, a, "moveid 1 2")
	if want := []string{"b", "c", "a"}; !reflect.DeepEqual(a.playQueue, want) {
		t.Fatalf("queue = %v, want %v", a.playQueue, want)
	}
}

// TestShuffleShortRangeReleasesLock is a regression test: `shuffle` with a
// sub-2-element range used to return without unlocking playQueueMu, deadlocking
// every later queue command.
func TestShuffleShortRangeReleasesLock(t *testing.T) {
	a := newQueueTestApp("a", "b", "c")
	dispatchCapture(t, a, "shuffle 0:1")
	// Deadlocks here (test timeout) if the lock leaked.
	dispatchCapture(t, a, "move 0 1")
	if want := []string{"b", "a", "c"}; !reflect.DeepEqual(a.playQueue, want) {
		t.Fatalf("queue = %v, want %v", a.playQueue, want)
	}
}
