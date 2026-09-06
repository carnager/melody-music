package main

import "testing"

// TestReloadTransportDecision verifies how a (re)connecting enabled agent
// joins playback. The regression this guards: a fresh registration (no resume
// stash, e.g. Android restarted the app process or the daemon restarted) with
// no other output playing must stay silent instead of starting audio.
func TestReloadTransportDecision(t *testing.T) {
	playing := &agentResume{state: "play", pos: 3, elapsed: 12.5}
	paused := &agentResume{state: "pause", pos: 3, elapsed: 12.5}

	cases := []struct {
		name          string
		othersPlaying bool
		othersPaused  bool
		resume        *agentResume
		wantLoad      bool
		wantPaused    bool
	}{
		{"fresh registration, nothing anywhere", false, false, nil, false, false},
		{"fresh registration, sibling paused", false, true, nil, true, true},
		{"fresh registration, sibling playing", true, false, nil, true, false},
		{"own stash playing, no siblings", false, false, playing, true, false},
		{"own stash paused, no siblings", false, false, paused, true, true},
		{"own stash paused, sibling playing joins live", true, false, paused, true, false},
		{"own stash playing, sibling paused", false, true, playing, true, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			load, startPaused := reloadTransportDecision(tc.othersPlaying, tc.othersPaused, tc.resume)
			if load != tc.wantLoad || startPaused != tc.wantPaused {
				t.Fatalf("got load=%v paused=%v, want load=%v paused=%v",
					load, startPaused, tc.wantLoad, tc.wantPaused)
			}
		})
	}
}
