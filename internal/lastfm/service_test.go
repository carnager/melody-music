// SPDX-License-Identifier: GPL-3.0-only
package lastfm

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestAccounting(t *testing.T) {
	s := New("")
	s.state.Enabled = true
	s.state.Session = "session"
	track := Track{Artist: "Artist", Title: "Song", Duration: 40}
	base := time.Unix(100000, 0)
	sample := func(id string, position float64, playing bool, seconds int) {
		s.Observe(id, track, position, playing, base.Add(time.Duration(seconds)*time.Second))
	}
	sample("one", 0, true, 0)
	for n := 1; n <= 10; n++ {
		sample("one", float64(n), true, n)
	}
	sample("one", 10, false, 11)
	sample("one", 10, false, 21)
	sample("one", 10, true, 22)
	sample("one", 35, true, 23) // seek earns no time
	sample("one", 0, true, 24)  // backward seek earns no time
	if s.listened != 10 {
		t.Fatalf("pause or seek counted: %f", s.listened)
	}
	for n := 1; n <= 10; n++ {
		sample("one", float64(n), true, 24+n)
	}
	if len(s.state.Pending) != 1 || s.state.Pending[0].Timestamp != 100000 {
		t.Fatal(s.state.Pending)
	}
	for n := 11; n <= 20; n++ {
		sample("one", float64(n), true, 24+n)
	}
	if len(s.state.Pending) != 1 {
		t.Fatal("duplicate submission")
	}
	sample("two", 0, true, 45)
	for n := 1; n <= 20; n++ {
		sample("two", float64(n), true, 45+n)
	}
	if len(s.state.Pending) != 2 {
		t.Fatal("duplicate source occurrence lost")
	}
	track.Duration = 30
	sample("short", 0, true, 66)
	for n := 1; n <= 30; n++ {
		sample("short", float64(n), true, 66+n)
	}
	if len(s.state.Pending) != 2 {
		t.Fatal("short track submitted")
	}
}
func TestAuthLovePersistenceAndRetry(t *testing.T) {
	var calls []url.Values
	fail := true
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if err := r.ParseForm(); err != nil {
			t.Error(err)
		}
		p := r.Form
		calls = append(calls, p)
		if p.Get("method") == "track.getInfo" {
			if r.Method != http.MethodGet || p.Has("sk") || p.Has("api_sig") || p.Get("username") != "listener" {
				t.Error("track info must be an unsigned GET with username")
			}
		} else if r.Method != http.MethodPost || p.Get("api_sig") != Signature(p, strings.Repeat("b", 32)) {
			t.Error("expected signed POST")
		}
		w.Header().Set("Content-Type", "application/json")
		switch p.Get("method") {
		case "auth.getToken":
			w.Write([]byte(`{"token":"TOKEN"}`))
		case "auth.getSession":
			w.Write([]byte(`{"session":{"name":"listener","key":"SESSION"}}`))
		case "track.love", "track.unlove":
			w.Write([]byte(`{}`))
		case "track.getInfo":
			w.Write([]byte(`{"track":{"userloved":"1"}}`))
		case "track.scrobble":
			if fail {
				w.Write([]byte(`{"error":11}`))
			} else {
				w.Write([]byte(`{"scrobbles":{"@attr":{"accepted":"1","ignored":"0"}}}`))
			}
		}
	}))
	defer server.Close()
	path := filepath.Join(t.TempDir(), "state.json")
	s := New(path)
	s.endpoint = server.URL
	state, err := s.Execute("begin", []string{strings.Repeat("a", 32), strings.Repeat("b", 32)})
	if err != nil || !strings.Contains(state["url"].(string), "TOKEN") {
		t.Fatal(state, err)
	}
	if _, err = s.Execute("finish", nil); err != nil {
		t.Fatal(err)
	}
	if _, err = s.Execute("enable", []string{"1"}); err != nil {
		t.Fatal(err)
	}
	for _, op := range []string{"love", "unlove", "info"} {
		state, err = s.Execute(op, []string{"Björk & A", "A + B"})
		if err != nil {
			t.Fatal(err)
		}
	}
	if state["loved"] != true {
		t.Fatal(state)
	}
	status, _ := s.Execute("status", nil)
	b, _ := json.Marshal(status)
	if strings.Contains(string(b), "SESSION") || strings.Contains(string(b), strings.Repeat("b", 32)) {
		t.Fatal("credentials exposed")
	}
	info, _ := os.Stat(path)
	if info.Mode().Perm() != 0600 {
		t.Fatal(info.Mode())
	}
	s.state.Pending = []Submission{{Track: Track{Artist: "Björk", Title: "Song", Duration: 300}, Timestamp: 10000}}
	if err = s.save(); err != nil {
		t.Fatal(err)
	}
	restored := New(path)
	restored.endpoint = server.URL
	now := time.Now()
	restored.Flush(now)
	if len(restored.state.Pending) != 1 {
		t.Fatal("retry lost")
	}
	count := len(calls)
	restored.Flush(now)
	if len(calls) != count {
		t.Fatal("backoff ignored")
	}
	fail = false
	restored.Flush(now.Add(time.Hour))
	if len(restored.state.Pending) != 0 {
		t.Fatal("not acknowledged")
	}
	if len(New(path).state.Pending) != 0 {
		t.Fatal("ack not persisted")
	}
	if _, err = restored.Execute("disconnect", nil); err != nil {
		t.Fatal(err)
	}
	if New(path).state.Session != "" {
		t.Fatal("disconnect not persisted")
	}
}
func TestFourMinuteThresholdAndDisabled(t *testing.T) {
	s := New("")
	track := Track{Artist: "A", Title: "T", Duration: 1000}
	now := time.Unix(12345, 0)
	for n := 0; n <= 500; n++ {
		s.Observe("x", track, float64(n), true, now.Add(time.Duration(n)*time.Second))
	}
	if len(s.state.Pending) != 0 {
		t.Fatal("disabled submitted")
	}
	s.state.Enabled = true
	s.state.Session = "s"
	for n := 0; n < 240; n++ {
		s.Observe("x", track, float64(n), true, now.Add(time.Duration(n)*time.Second))
	}
	if len(s.state.Pending) != 0 {
		t.Fatal("early")
	}
	s.Observe("x", track, 240, true, now.Add(240*time.Second))
	if len(s.state.Pending) != 1 {
		t.Fatal("missing four-minute scrobble")
	}
}

func TestCoarsePositionReports(t *testing.T) {
	s := New("")
	s.state.Enabled = true
	s.state.Session = "s"
	now := time.Unix(1000, 0)
	for n := 0; n <= 20; n++ {
		s.Observe("web", Track{Artist: "A", Title: "T", Duration: 40}, float64((n/5)*5), true, now.Add(time.Duration(n)*time.Second))
	}
	if len(s.state.Pending) != 1 {
		t.Fatal("coarse progress was mistaken for seeking")
	}
}

func TestReconnectUsesSavedCredentials(t *testing.T) {
	key, secret := strings.Repeat("a", 32), strings.Repeat("b", 32)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = r.ParseForm()
		if r.Form.Get("api_key") != key || r.Form.Get("api_sig") != Signature(r.Form, secret) {
			t.Error("saved credentials not used")
		}
		w.Write([]byte(`{"token":"TOKEN"}`))
	}))
	defer server.Close()
	path := filepath.Join(t.TempDir(), "state.json")
	s := New(path)
	s.endpoint = server.URL
	if _, err := s.Execute("begin", nil); err == nil {
		t.Fatal("missing credentials accepted")
	}
	if _, err := s.Execute("begin", []string{key, secret}); err != nil {
		t.Fatal(err)
	}
	restored := New(path)
	restored.endpoint = server.URL
	state, err := restored.Execute("begin", nil)
	if err != nil {
		t.Fatal(err)
	}
	if state["credentials_saved"] != true || state["authorization_pending"] != true {
		t.Fatal(state)
	}
	data, _ := json.Marshal(state)
	if strings.Contains(string(data), secret) {
		t.Fatal("secret exposed")
	}
	if _, err := restored.Execute("disconnect", nil); err != nil {
		t.Fatal(err)
	}
	if _, err := restored.Execute("begin", nil); err == nil {
		t.Fatal("disconnected credentials retained")
	}
}

func TestAuthorizationPendingAndScrobblingDefault(t *testing.T) {
	var approved atomic.Bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = r.ParseForm()
		if r.Form.Get("method") == "auth.getToken" {
			w.Write([]byte(`{"token":"TOKEN"}`))
			return
		}
		if !approved.Load() {
			w.Write([]byte(`{"error":14}`))
			return
		}
		w.Write([]byte(`{"session":{"name":"listener","key":"SESSION"}}`))
	}))
	defer server.Close()
	s := New(filepath.Join(t.TempDir(), "state.json"))
	s.endpoint = server.URL
	if _, err := s.Execute("begin", []string{strings.Repeat("a", 32), strings.Repeat("b", 32)}); err != nil {
		t.Fatal(err)
	}
	pending, err := s.Execute("finish", nil)
	if err != nil || pending["authorization_pending"] != true || s.state.Enabled {
		t.Fatalf("unexpected pending state: %v %v", pending, err)
	}
	approved.Store(true)
	if _, err := s.Execute("finish", nil); err != nil {
		t.Fatal(err)
	}
	if !s.state.Enabled {
		t.Fatal("first connection did not enable scrobbling")
	}
	if _, err := s.Execute("enable", []string{"0"}); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Execute("begin", nil); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Execute("finish", nil); err != nil {
		t.Fatal(err)
	}
	if s.state.Enabled {
		t.Fatal("reconnection overwrote disabled preference")
	}
	restored := New(s.path)
	if restored.state.Enabled || restored.state.Session == "" {
		t.Fatal("preference not persisted")
	}
}
