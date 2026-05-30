package player

import (
	"fmt"
	"strings"
	"testing"
	"time"
)

type fakeMPV struct {
	slots    []int64
	nextID   int64
	commands []string
}

func newTestPlayer(f *fakeMPV) *Player {
	return &Player{
		state:       "stop",
		volume:      100,
		replayGain:  "no",
		pending:     make(map[int]chan commandResult),
		eventCh:     make(chan mpvEvent, 64),
		commandHook: f.command,
	}
}

func (f *fakeMPV) command(args ...any) (mpvResponse, error) {
	parts := make([]string, 0, len(args))
	for _, arg := range args {
		parts = append(parts, fmt.Sprint(arg))
	}
	f.commands = append(f.commands, strings.Join(parts, " "))

	if len(args) == 0 {
		return mpvResponse{}, nil
	}
	switch args[0] {
	case "playlist-clear":
		f.slots = nil
	case "loadfile":
		if len(args) < 3 {
			return mpvResponse{}, fmt.Errorf("bad loadfile")
		}
		f.nextID++
		mode := fmt.Sprint(args[2])
		if mode == "replace" {
			f.slots = []int64{f.nextID}
		} else {
			f.slots = append(f.slots, f.nextID)
		}
	case "playlist-remove":
		index := int(args[1].(int))
		if index >= 0 && index < len(f.slots) {
			f.slots = append(f.slots[:index], f.slots[index+1:]...)
		}
	case "get_property":
		name := fmt.Sprint(args[1])
		switch name {
		case "playlist-count":
			return mpvResponse{Data: float64(len(f.slots))}, nil
		case "playlist/0/id":
			return mpvResponse{Data: float64(f.slots[0])}, nil
		case "playlist/1/id":
			return mpvResponse{Data: float64(f.slots[1])}, nil
		case "pause":
			return mpvResponse{Data: false}, nil
		case "time-pos", "duration":
			return mpvResponse{Data: 0.0}, nil
		case "volume":
			return mpvResponse{Data: 100.0}, nil
		}
	}
	return mpvResponse{Error: "success"}, nil
}

func TestPlayPairLoadsCurrentAndNext(t *testing.T) {
	f := &fakeMPV{}
	p := newTestPlayer(f)

	err := p.PlayPair(TrackSpec{Path: "current.flac"}, &TrackSpec{Path: "next.flac"}, -1)
	if err != nil {
		t.Fatalf("PlayPair: %v", err)
	}
	if len(f.slots) != 2 {
		t.Fatalf("slots = %v, want two entries", f.slots)
	}
	if !p.currentLoaded || !p.nextLoaded {
		t.Fatalf("loaded flags current=%v next=%v", p.currentLoaded, p.nextLoaded)
	}
	if p.currentEntryID != f.slots[0] || p.nextEntryID != f.slots[1] {
		t.Fatalf("entry ids current=%d next=%d slots=%v", p.currentEntryID, p.nextEntryID, f.slots)
	}
}

func TestPlayPairSeekLoadsPausedThenUnpauses(t *testing.T) {
	f := &fakeMPV{}
	p := newTestPlayer(f)

	if err := p.PlayPair(TrackSpec{Path: "current.flac"}, nil, 42.5); err != nil {
		t.Fatalf("PlayPair: %v", err)
	}

	joined := strings.Join(f.commands, "\n")
	for _, want := range []string{
		"set_property pause true",
		"loadfile current.flac replace",
		"seek 42.5 absolute exact",
		"set_property pause false",
	} {
		if !strings.Contains(joined, want) {
			t.Fatalf("commands missing %q:\n%s", want, joined)
		}
	}
	if strings.Index(joined, "set_property pause true") > strings.Index(joined, "loadfile current.flac replace") {
		t.Fatalf("pause true should happen before loadfile:\n%s", joined)
	}
	if strings.Index(joined, "seek 42.5 absolute exact") > strings.Index(joined, "set_property pause false") {
		t.Fatalf("seek should happen before unpause:\n%s", joined)
	}
}

func TestPreloadReplacesOnlySlotOne(t *testing.T) {
	f := &fakeMPV{}
	p := newTestPlayer(f)
	if err := p.PlayPair(TrackSpec{Path: "current.flac"}, &TrackSpec{Path: "old-next.flac"}, -1); err != nil {
		t.Fatal(err)
	}
	currentID := f.slots[0]

	if err := p.Preload("new-next.flac", "", 0, 0); err != nil {
		t.Fatalf("Preload: %v", err)
	}
	if len(f.slots) != 2 {
		t.Fatalf("slots = %v, want two entries", f.slots)
	}
	if f.slots[0] != currentID {
		t.Fatalf("current slot changed from %d to %d", currentID, f.slots[0])
	}
	if p.nextEntryID != f.slots[1] || p.nextPath != "new-next.flac" {
		t.Fatalf("next state id=%d path=%q slots=%v", p.nextEntryID, p.nextPath, f.slots)
	}
}

func TestClearPreloadKeepsCurrent(t *testing.T) {
	f := &fakeMPV{}
	p := newTestPlayer(f)
	if err := p.PlayPair(TrackSpec{Path: "current.flac"}, &TrackSpec{Path: "next.flac"}, -1); err != nil {
		t.Fatal(err)
	}
	currentID := f.slots[0]

	if err := p.ClearPreload(); err != nil {
		t.Fatalf("ClearPreload: %v", err)
	}
	if len(f.slots) != 1 || f.slots[0] != currentID {
		t.Fatalf("slots = %v, want only current id %d", f.slots, currentID)
	}
	if p.nextLoaded {
		t.Fatalf("nextLoaded = true after ClearPreload")
	}
}

func TestEOFWithNextAdvancesAndCallsOnce(t *testing.T) {
	f := &fakeMPV{}
	p := newTestPlayer(f)
	if err := p.PlayPair(TrackSpec{Path: "current.flac"}, &TrackSpec{Path: "next.flac"}, -1); err != nil {
		t.Fatal(err)
	}
	oldID := p.currentEntryID
	nextID := p.nextEntryID
	calls := make(chan struct{}, 2)
	p.OnTrackEnd = func() { calls <- struct{}{} }

	p.handleEvent(mpvEvent{Event: "end-file", Reason: "eof", PlaylistEntryID: oldID})

	expectCall(t, calls)
	expectNoCall(t, calls)
	if len(f.slots) != 1 || f.slots[0] != nextID {
		t.Fatalf("slots = %v, want advanced next id %d", f.slots, nextID)
	}
	if p.currentEntryID != nextID || p.nextLoaded {
		t.Fatalf("current=%d nextLoaded=%v", p.currentEntryID, p.nextLoaded)
	}
}

func TestEOFWithoutNextStopsAndCallsOnce(t *testing.T) {
	f := &fakeMPV{}
	p := newTestPlayer(f)
	if err := p.PlayPair(TrackSpec{Path: "current.flac"}, nil, -1); err != nil {
		t.Fatal(err)
	}
	oldID := p.currentEntryID
	calls := make(chan struct{}, 2)
	p.OnTrackEnd = func() { calls <- struct{}{} }

	p.handleEvent(mpvEvent{Event: "end-file", Reason: "eof", PlaylistEntryID: oldID})

	expectCall(t, calls)
	expectNoCall(t, calls)
	if p.currentLoaded || p.state != "stop" {
		t.Fatalf("currentLoaded=%v state=%q", p.currentLoaded, p.state)
	}
}

func TestEndFileReasonsAndStaleEOFDoNotAdvance(t *testing.T) {
	f := &fakeMPV{}
	p := newTestPlayer(f)
	if err := p.PlayPair(TrackSpec{Path: "current.flac"}, nil, -1); err != nil {
		t.Fatal(err)
	}
	oldID := p.currentEntryID
	calls := make(chan struct{}, 2)
	p.OnTrackEnd = func() { calls <- struct{}{} }

	p.handleEvent(mpvEvent{Event: "end-file", Reason: "stop", PlaylistEntryID: oldID})
	expectNoCall(t, calls)

	if err := p.PlayPair(TrackSpec{Path: "new.flac"}, nil, -1); err != nil {
		t.Fatal(err)
	}
	p.handleEvent(mpvEvent{Event: "end-file", Reason: "eof", PlaylistEntryID: oldID})
	expectNoCall(t, calls)

	p.Stop()
	p.handleEvent(mpvEvent{Event: "end-file", Reason: "eof", PlaylistEntryID: p.currentEntryID})
	expectNoCall(t, calls)
}

func TestControlsIssueExpectedCommands(t *testing.T) {
	f := &fakeMPV{}
	p := newTestPlayer(f)
	if err := p.PlayPair(TrackSpec{Path: "current.flac"}, nil, -1); err != nil {
		t.Fatal(err)
	}
	f.commands = nil

	p.Pause()
	p.Resume()
	if err := p.Seek(12.5); err != nil {
		t.Fatalf("Seek: %v", err)
	}
	p.SetVolume(42)
	p.SetReplayGain("album")

	joined := strings.Join(f.commands, "\n")
	for _, want := range []string{
		"set_property pause true",
		"set_property pause false",
		"seek 12.5 absolute exact",
		"set_property volume 42",
		"set_property replaygain album",
	} {
		if !strings.Contains(joined, want) {
			t.Fatalf("commands missing %q:\n%s", want, joined)
		}
	}
}

func expectCall(t *testing.T, calls <-chan struct{}) {
	t.Helper()
	select {
	case <-calls:
	case <-time.After(time.Second):
		t.Fatal("expected OnTrackEnd call")
	}
}

func expectNoCall(t *testing.T, calls <-chan struct{}) {
	t.Helper()
	select {
	case <-calls:
		t.Fatal("unexpected OnTrackEnd call")
	case <-time.After(20 * time.Millisecond):
	}
}
