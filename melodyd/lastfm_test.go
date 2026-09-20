// SPDX-License-Identifier: GPL-3.0-only
package main

import (
	"encoding/json"
	"github.com/carnager/melody/internal/lastfm"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestLastFMCountsPrimaryOnceAndSurvivesClientQueries(t *testing.T) {
	a, primary := newContextApp(t)
	path := filepath.Join(t.TempDir(), "lastfm.json")
	// A fake authorized account: sampling and protocol status perform no HTTP.
	if err := os.WriteFile(path, []byte(`{"Version":1,"Enabled":true,"Key":"key","Secret":"secret","Session":"session","User":"listener"}`), 0600); err != nil {
		t.Fatal(err)
	}
	a.lastfm = lastfm.New(path)
	dispatchCapture(t, a, `melody_context play "Road" 0`)
	other := newWebTarget()
	other.playlist = []string{"other"}
	other.playlistPos = 0
	other.paused = false
	a.devicesMu.Lock()
	a.webTargets["other"] = other
	a.enabledOutputs["other"] = true
	a.devicesMu.Unlock()
	start := time.Unix(12345000, 0)
	for n := 0; n <= 50; n++ {
		primary.mu.Lock()
		primary.timePos = float64(n)
		primary.paused = false
		primary.mu.Unlock()
		a.sampleLastFM(start.Add(time.Duration(n) * time.Second))
		// Reading from multiple clients must not feed the accounting service.
		for range 2 {
			status := dispatchCapture(t, a, `melody_lastfm status`)
			if strings.Contains(status, "secret") || strings.Contains(status, "session\"") {
				t.Fatal("credentials leaked")
			}
		}
	}
	status, err := a.lastfm.Execute("status", nil)
	if err != nil || status["pending"] != 1 {
		t.Fatal(status, err)
	}
	restored := lastfm.New(path)
	state, _ := restored.Execute("status", nil)
	if state["pending"] != 1 {
		t.Fatal(state)
	}
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var disk struct{ Pending []lastfm.Submission }
	if json.Unmarshal(b, &disk) != nil || len(disk.Pending) != 1 || disk.Pending[0].Title != "alpha" {
		t.Fatal("wrong submission")
	}
	primary.mu.Lock()
	primary.paused = true
	primary.mu.Unlock()
	for n := 51; n <= 120; n++ {
		a.sampleLastFM(start.Add(time.Duration(n) * time.Second))
	}
	status, _ = a.lastfm.Execute("status", nil)
	if status["pending"] != 1 {
		t.Fatal("paused or second output duplicated scrobble")
	}
}
