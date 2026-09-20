// SPDX-License-Identifier: GPL-3.0-only

// Package lastfm owns account state, listen accounting, and a durable outbox.
package lastfm

import (
	"bytes"
	"crypto/md5"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"sync"
	"time"
)

type Track struct {
	Artist, Title, Album string
	Duration             float64
}
type Submission struct {
	Track
	Timestamp int64
}
type diskState struct {
	Version                    int
	Key, Secret, Session, User string
	Enabled                    bool
	Pending                    []Submission
}
type Service struct {
	mu                             sync.Mutex
	network                        sync.Mutex
	state                          diskState
	path, token, message           string
	client                         *http.Client
	endpoint                       string
	retry                          time.Time
	failures                       int
	identity                       string
	track                          Track
	started                        int64
	listened, position, uncredited float64
	last                           time.Time
	playing, submitted             bool
	nowPlaying                     *Track
	blocked                        bool
	authFailed                     bool
}

func New(path string) *Service {
	s := &Service{path: path, client: &http.Client{Timeout: 8 * time.Second}, endpoint: "https://ws.audioscrobbler.com/2.0/"}
	s.state.Version = 1
	if b, e := os.ReadFile(path); e == nil {
		if json.Unmarshal(b, &s.state) != nil || s.state.Version != 1 {
			s.state = diskState{Version: 1}
			s.blocked = true
			s.message = "Unreadable Last.fm state; preserve or remove the state file before setup"
		}
	} else if !os.IsNotExist(e) {
		s.blocked = true
		s.message = "Could not read Last.fm state"
	}
	return s
}
func (s *Service) save() error {
	if s.blocked {
		return errors.New(s.message)
	}
	if s.path == "" {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(s.path), 0700); err != nil {
		return err
	}
	b, err := json.Marshal(s.state)
	if err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(s.path), ".lastfm-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if _, err = f.Write(b); err != nil {
		f.Close()
		return err
	}
	if err = f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	if err := os.Rename(f.Name(), s.path); err != nil {
		return err
	}
	directory, err := os.Open(filepath.Dir(s.path))
	if err != nil {
		return err
	}
	defer directory.Close()
	return directory.Sync()
}
func Signature(params url.Values, secret string) string {
	keys := make([]string, 0, len(params))
	for k := range params {
		if k != "format" && k != "callback" && k != "api_sig" {
			keys = append(keys, k)
		}
	}
	sort.Strings(keys)
	var b bytes.Buffer
	for _, k := range keys {
		b.WriteString(k)
		b.WriteString(params.Get(k))
	}
	b.WriteString(secret)
	sum := md5.Sum(b.Bytes())
	return hex.EncodeToString(sum[:])
}

type apiError struct {
	code  int
	retry bool
}

func (e apiError) Error() string { return fmt.Sprintf("Last.fm request failed (code %d)", e.code) }
func (s *Service) call(method string, args url.Values) (map[string]json.RawMessage, error) {
	s.mu.Lock()
	state := s.state
	s.mu.Unlock()
	args.Set("method", method)
	args.Set("api_key", state.Key)
	readOnly := method == "track.getInfo"
	if !readOnly {
		if method != "auth.getToken" && method != "auth.getSession" {
			args.Set("sk", state.Session)
		}
		args.Set("api_sig", Signature(args, state.Secret))
	}
	args.Set("format", "json")
	httpMethod, endpoint := http.MethodPost, s.endpoint
	var body io.Reader = bytes.NewBufferString(args.Encode())
	if readOnly {
		u, err := url.Parse(endpoint)
		if err != nil {
			return nil, err
		}
		u.RawQuery = args.Encode()
		endpoint, httpMethod, body = u.String(), http.MethodGet, nil
	}
	req, err := http.NewRequest(httpMethod, endpoint, body)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	resp, err := s.client.Do(req)
	if err != nil {
		return nil, apiError{0, true}
	}
	defer resp.Body.Close()
	b, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return nil, apiError{0, true}
	}
	var result map[string]json.RawMessage
	if json.Unmarshal(b, &result) != nil {
		return nil, apiError{resp.StatusCode, true}
	}
	var code int
	_ = json.Unmarshal(result["error"], &code)
	if code != 0 {
		return nil, apiError{code, code == 11 || code == 16 || code == 29}
	}
	if resp.StatusCode != 200 {
		return nil, apiError{resp.StatusCode, resp.StatusCode >= 500 || resp.StatusCode == 429}
	}
	return result, nil
}
func (s *Service) status() map[string]any {
	return map[string]any{"connected": s.state.Session != "", "user": s.state.User, "enabled": s.state.Enabled, "pending": len(s.state.Pending), "message": s.message, "credentials_saved": len(s.state.Key) == 32 && len(s.state.Secret) == 32, "authorization_pending": s.token != ""}
}

// Execute is serialized independently of listen sampling; no audio locks are held.
func (s *Service) Execute(op string, args []string) (map[string]any, error) {
	if op == "status" {
		s.mu.Lock()
		defer s.mu.Unlock()
		return s.status(), nil
	}
	if !s.network.TryLock() {
		return nil, errors.New("Last.fm is busy; try again shortly")
	}
	defer s.network.Unlock()
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.blocked {
		return nil, errors.New(s.message)
	}
	result := map[string]any{}
	call := func(method string, p url.Values) (map[string]json.RawMessage, error) {
		s.mu.Unlock()
		r, e := s.call(method, p)
		s.mu.Lock()
		return r, e
	}
	switch op {
	case "status":
	case "begin":
		if len(args) == 0 {
			args = []string{s.state.Key, s.state.Secret}
		}
		if len(args) != 2 || len(args[0]) != 32 || len(args[1]) != 32 {
			return nil, errors.New("provide the 32-character API key and shared secret")
		}
		if s.state.Session != "" && (s.state.Key != args[0] || s.state.Secret != args[1]) {
			return nil, errors.New("disconnect before changing API credentials")
		}
		s.token = ""
		s.state.Key = args[0]
		s.state.Secret = args[1]
		r, e := call("auth.getToken", url.Values{})
		if e != nil {
			return nil, e
		}
		if json.Unmarshal(r["token"], &s.token) != nil || s.token == "" {
			return nil, errors.New("Last.fm returned no token")
		}
		result["url"] = "https://www.last.fm/api/auth/?" + url.Values{"api_key": {s.state.Key}, "token": {s.token}}.Encode()
	case "finish":
		if s.token == "" {
			return nil, errors.New("start authorization first")
		}
		r, e := call("auth.getSession", url.Values{"token": {s.token}})
		if e != nil {
			var apiErr apiError
			if errors.As(e, &apiErr) && apiErr.code == 14 {
				return s.status(), nil
			}
			return nil, e
		}
		var session struct{ Key, Name string }
		if json.Unmarshal(r["session"], &session) != nil || session.Key == "" {
			return nil, errors.New("Last.fm returned no session")
		}
		if s.state.User == "" {
			s.state.Enabled = true
		}
		if s.state.User != "" && s.state.User != session.Name {
			s.state.Enabled = false
			s.state.Pending = nil
		}
		s.identity = ""
		s.nowPlaying = nil
		s.state.Session = session.Key
		s.state.User = session.Name
		s.token = ""
		s.retry = time.Time{}
		s.message = "Connected"
		s.authFailed = false
	case "disconnect":
		s.state = diskState{Version: 1}
		s.token = ""
		s.identity = ""
		s.nowPlaying = nil
		s.message = "Disconnected; pending scrobbles cleared"
	case "enable":
		if len(args) != 1 || (args[0] != "0" && args[0] != "1") {
			return nil, errors.New("enable expects 0 or 1")
		}
		if args[0] == "1" && s.state.Session == "" {
			return nil, errors.New("connect Last.fm first")
		}
		s.state.Enabled = args[0] == "1"
		s.identity = ""
		s.nowPlaying = nil
	case "love", "unlove", "info":
		if s.state.Session == "" {
			return nil, errors.New("connect Last.fm first")
		}
		if len(args) != 2 || args[0] == "" || args[1] == "" {
			return nil, errors.New("artist and title are required")
		}
		p := url.Values{"artist": {args[0]}, "track": {args[1]}}
		method := "track." + op
		if op == "info" {
			method = "track.getInfo"
			p.Set("username", s.state.User)
			p.Set("autocorrect", "0")
		}
		r, e := call(method, p)
		if e != nil {
			return nil, e
		}
		if op == "info" {
			var t struct {
				Loved string `json:"userloved"`
			}
			if json.Unmarshal(r["track"], &t) != nil {
				return nil, errors.New("invalid track response")
			}
			result["loved"] = t.Loved == "1"
		} else {
			result["loved"] = op == "love"
			s.message = "Last.fm updated"
		}
		result["artist"] = args[0]
		result["title"] = args[1]
	default:
		return nil, errors.New("unknown Last.fm operation")
	}
	if op != "status" && op != "info" {
		if e := s.save(); e != nil {
			s.message = "Last.fm state could not be saved"
			return nil, errors.New(s.message)
		}
	}
	for k, v := range s.status() {
		result[k] = v
	}
	return result, nil
}

// Observe credits advancing audio only; seeking and long polling gaps earn nothing.
func (s *Service) Observe(identity string, t Track, position float64, playing bool, now time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.state.Enabled || s.state.Session == "" {
		s.identity = ""
		return
	}
	if identity != s.identity {
		s.identity = identity
		s.track = t
		s.started = 0
		s.listened = 0
		s.uncredited = 0
		s.submitted = false
		s.last = now
		s.position = position
		s.playing = playing
		s.nowPlaying = nil
		if identity != "" && playing && t.Artist != "" && t.Title != "" {
			s.started = now.Unix()
			copy := t
			s.nowPlaying = &copy
		}
		return
	}
	if s.started == 0 && identity != "" && playing && t.Artist != "" && t.Title != "" {
		s.started = now.Unix()
		copy := t
		s.nowPlaying = &copy
	}
	dt := now.Sub(s.last).Seconds()
	advance := position - s.position
	if playing && s.playing && dt > 0 && dt <= 5 {
		s.uncredited += dt
		if advance != 0 {
			if advance > 0 && s.uncredited <= 10 && advance <= s.uncredited+1 {
				s.listened += math.Min(s.uncredited, advance)
			}
			s.uncredited = 0
		} else if s.uncredited > 10 {
			s.uncredited = 0
		}
	} else {
		s.uncredited = 0
	}
	s.last = now
	s.position = position
	s.playing = playing
	if !s.submitted && identity != "" && t.Artist != "" && t.Title != "" && t.Duration > 30 && s.listened >= math.Min(t.Duration/2, 240) {
		if len(s.state.Pending) >= 1000 {
			s.message = "Scrobble outbox is full"
			return
		}
		s.state.Pending = append(s.state.Pending, Submission{Track: t, Timestamp: s.started})
		if e := s.save(); e != nil {
			s.state.Pending = s.state.Pending[:len(s.state.Pending)-1]
			s.message = "Could not save pending scrobble"
			return
		}
		s.submitted = true
	}
}
func trackParams(t Track) url.Values {
	p := url.Values{"artist": {t.Artist}, "track": {t.Title}, "duration": {strconv.Itoa(int(t.Duration))}}
	if t.Album != "" {
		p.Set("album", t.Album)
	}
	return p
}
func (s *Service) Flush(now time.Time) {
	s.network.Lock()
	defer s.network.Unlock()
	s.mu.Lock()
	if s.authFailed || !s.state.Enabled || s.state.Session == "" || now.Before(s.retry) {
		s.mu.Unlock()
		return
	}
	np := s.nowPlaying
	s.nowPlaying = nil
	var item *Submission
	if len(s.state.Pending) > 0 {
		copy := s.state.Pending[0]
		item = &copy
	}
	s.mu.Unlock()
	if np != nil {
		_, _ = s.call("track.updateNowPlaying", trackParams(*np))
	}
	if item == nil {
		return
	}
	p := trackParams(item.Track)
	p.Set("timestamp", strconv.FormatInt(item.Timestamp, 10))
	r, err := s.call("track.scrobble", p)
	s.mu.Lock()
	defer s.mu.Unlock()
	if err != nil {
		var e apiError
		errors.As(err, &e)
		s.message = err.Error()
		if e.code == 9 {
			s.authFailed = true
			s.message = "Last.fm session expired; reconnect"
			return
		}
		if e.retry {
			s.failures++
			delay := time.Duration(1<<min(s.failures, 8)) * time.Second
			s.retry = now.Add(delay)
			return
		}
	}
	if err == nil {
		var result struct {
			Attr map[string]json.RawMessage `json:"@attr"`
		}
		if json.Unmarshal(r["scrobbles"], &result) != nil || result.Attr == nil {
			s.message = "Invalid scrobble response"
			s.retry = now.Add(time.Minute)
			return
		}
		count := func(key string) int {
			raw := result.Attr[key]
			var str string
			if json.Unmarshal(raw, &str) == nil {
				n, _ := strconv.Atoi(str)
				return n
			}
			var n int
			_ = json.Unmarshal(raw, &n)
			return n
		}
		if count("accepted")+count("ignored") != 1 {
			s.message = "Invalid scrobble acknowledgement"
			s.retry = now.Add(time.Minute)
			return
		}
		s.message = "Scrobble submitted"
		if count("ignored") > 0 {
			s.message = "Last.fm ignored the scrobble (metadata or timestamp)"
		}
	}
	s.state.Pending = s.state.Pending[1:]
	s.failures = 0
	s.retry = time.Time{}
	if s.save() != nil {
		s.message = "Could not save scrobble acknowledgement"
	}
}
