package main

import (
	"io"
	"log"
	"reflect"
	"testing"
)

// TestNextSpendsPriorityWithoutConsuming verifies that skipping past a
// prioritized track resets its priority to zero but keeps the track in the
// queue (no auto-consume), and that playback resumes after the saved return
// position.
func TestNextSpendsPriorityWithoutConsuming(t *testing.T) {
	a := newQueueTestApp("a", "b", "c")
	a.logger = log.New(io.Discard, "", 0)
	a.curQueuePos = 0
	a.pendingNextPos = -1
	a.prioReturnPos = -1
	a.queuePriority[2] = 192

	// First next jumps to the prioritized track and saves the return position.
	dispatchCapture(t, a, "next")
	if a.curQueuePos != 2 {
		t.Fatalf("after first next: curQueuePos = %d, want 2 (priority jump)", a.curQueuePos)
	}
	if a.prioReturnPos != 0 {
		t.Fatalf("after first next: prioReturnPos = %d, want 0", a.prioReturnPos)
	}

	// Second next leaves the prioritized track behind: it stays in the queue
	// with its priority spent, and playback resumes after the saved position.
	dispatchCapture(t, a, "next")
	if want := []string{"a", "b", "c"}; !reflect.DeepEqual(a.playQueue, want) {
		t.Fatalf("queue = %v, want %v (no auto-consume)", a.playQueue, want)
	}
	if want := []int{0, 0, 0}; !reflect.DeepEqual(a.queuePriority, want) {
		t.Fatalf("priorities = %v, want %v (spent on play)", a.queuePriority, want)
	}
	if a.curQueuePos != 1 {
		t.Fatalf("after second next: curQueuePos = %d, want 1 (resume after saved position)",
			a.curQueuePos)
	}
	if a.prioReturnPos != -1 {
		t.Fatalf("after second next: prioReturnPos = %d, want -1 (cleared)", a.prioReturnPos)
	}
}

// TestSequentialAdvanceSkipsPrioPlayedTracks verifies the played memory: a
// track that played through a priority jump is skipped when the natural
// sequence reaches it, and a repeat wraparound starts a fresh cycle where it
// plays again.
func TestSequentialAdvanceSkipsPrioPlayedTracks(t *testing.T) {
	a := newQueueTestApp("a", "b", "c", "d")
	a.logger = log.New(io.Discard, "", 0)
	a.curQueuePos = 0
	a.pendingNextPos = -1
	a.prioReturnPos = -1
	a.queuePriority[2] = 192

	dispatchCapture(t, a, "next") // priority jump to c
	dispatchCapture(t, a, "next") // spend priority, resume at b
	if a.curQueuePos != 1 {
		t.Fatalf("after resume: curQueuePos = %d, want 1", a.curQueuePos)
	}

	// The natural sequence skips the already-played c and lands on d.
	dispatchCapture(t, a, "next")
	if a.curQueuePos != 3 {
		t.Fatalf("after skip: curQueuePos = %d, want 3 (c was already played)", a.curQueuePos)
	}
	if want := []string{"a", "b", "c", "d"}; !reflect.DeepEqual(a.playQueue, want) {
		t.Fatalf("queue = %v, want %v (no removal)", a.playQueue, want)
	}

	// A repeat wraparound is a new cycle: the memory clears and c plays again.
	a.modeRepeat = true
	dispatchCapture(t, a, "next") // wrap to a, memory cleared
	if a.curQueuePos != 0 {
		t.Fatalf("after wrap: curQueuePos = %d, want 0", a.curQueuePos)
	}
	if len(a.prioPlayedIDs) != 0 {
		t.Fatalf("after wrap: prioPlayedIDs = %v, want empty (new cycle)", a.prioPlayedIDs)
	}
	dispatchCapture(t, a, "next") // b
	dispatchCapture(t, a, "next") // c plays again this cycle
	if a.curQueuePos != 2 {
		t.Fatalf("new cycle: curQueuePos = %d, want 2 (c eligible again)", a.curQueuePos)
	}
}

// TestExplicitPlayClearsPrioPlayedMark verifies that explicitly playing a
// track removes its played-this-cycle mark.
func TestExplicitPlayClearsPrioPlayedMark(t *testing.T) {
	a := newQueueTestApp("a", "b", "c")
	a.logger = log.New(io.Discard, "", 0)
	a.curQueuePos = 0
	a.pendingNextPos = -1
	a.prioReturnPos = -1
	a.queuePriority[2] = 192

	dispatchCapture(t, a, "next") // jump to c
	dispatchCapture(t, a, "next") // spend priority, mark c, resume at b
	if !a.prioPlayedAt(2) {
		t.Fatalf("expected c to be marked as priority-played")
	}
	dispatchCapture(t, a, "play 2")
	if a.prioPlayedAt(2) {
		t.Fatalf("explicit play should clear the priority-played mark")
	}
	if a.curQueuePos != 2 {
		t.Fatalf("after play 2: curQueuePos = %d, want 2", a.curQueuePos)
	}
}
