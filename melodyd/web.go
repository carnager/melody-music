package main

import (
	"crypto/rand"
	"crypto/subtle"
	"embed"
	"encoding/hex"
	"fmt"
	"io/fs"
	"net/http"
	"sync"
	"time"
)

// ---------------------------------------------------------------------------
// webTarget — virtual playback target for browser clients
// ---------------------------------------------------------------------------
// The browser plays audio via <audio> element streaming from /api/v1/stream/{id}.
// This target just tracks state so the server knows what the browser is "playing".

type webTarget struct {
	mu          sync.Mutex
	playlistPos int
	timePos     float64
	paused      bool
	volume      float64
	alive       bool
	playlist    []string // URLs loaded via loadFile
}

func newWebTarget() *webTarget {
	return &webTarget{
		playlistPos: -1,
		paused:      true,
		volume:      100,
		alive:       true,
	}
}

func (wt *webTarget) loadFile(url, mode string, meta map[string]any) error {
	wt.mu.Lock()
	defer wt.mu.Unlock()
	switch mode {
	case "replace":
		wt.playlist = []string{url}
		wt.playlistPos = 0
		wt.timePos = 0
	case "append":
		wt.playlist = append(wt.playlist, url)
	}
	return nil
}

func (wt *webTarget) loadFileBatch(urls []string, mode string) error {
	for i, url := range urls {
		m := mode
		if i > 0 && mode == "replace" {
			m = "append"
		}
		if err := wt.loadFile(url, m, nil); err != nil {
			return err
		}
	}
	return nil
}

func (wt *webTarget) playlistClear() error {
	wt.mu.Lock()
	defer wt.mu.Unlock()
	wt.playlist = nil
	wt.playlistPos = -1
	wt.timePos = 0
	return nil
}

func (wt *webTarget) playlistRemove(index int) error {
	wt.mu.Lock()
	defer wt.mu.Unlock()
	if index >= 0 && index < len(wt.playlist) {
		wt.playlist = append(wt.playlist[:index], wt.playlist[index+1:]...)
		if wt.playlistPos >= len(wt.playlist) {
			wt.playlistPos = len(wt.playlist) - 1
		}
	}
	return nil
}

func (wt *webTarget) playlistMove(from, to int) error {
	wt.mu.Lock()
	defer wt.mu.Unlock()
	if from >= 0 && from < len(wt.playlist) && to >= 0 && to <= len(wt.playlist) {
		item := wt.playlist[from]
		wt.playlist = append(wt.playlist[:from], wt.playlist[from+1:]...)
		if to > from {
			to--
		}
		wt.playlist = append(wt.playlist[:to], append([]string{item}, wt.playlist[to:]...)...)
	}
	return nil
}

func (wt *webTarget) getProperty(name string) (any, error) {
	wt.mu.Lock()
	defer wt.mu.Unlock()
	switch name {
	case "playlist-pos":
		return float64(wt.playlistPos), nil
	case "time-pos":
		return wt.timePos, nil
	case "pause":
		return wt.paused, nil
	case "volume":
		return wt.volume, nil
	case "duration":
		return 0.0, nil // browser tracks this itself
	case "playlist-count":
		return float64(len(wt.playlist)), nil
	default:
		return nil, nil
	}
}

func (wt *webTarget) setProperty(name string, value any) error {
	wt.mu.Lock()
	defer wt.mu.Unlock()
	switch name {
	case "playlist-pos":
		if f, ok := value.(float64); ok {
			wt.playlistPos = int(f)
			wt.timePos = 0
		} else if i, ok := value.(int); ok {
			wt.playlistPos = i
			wt.timePos = 0
		}
	case "time-pos":
		if f, ok := value.(float64); ok {
			wt.timePos = f
		}
	case "pause":
		if b, ok := value.(bool); ok {
			wt.paused = b
		}
	case "volume":
		if f, ok := value.(float64); ok {
			wt.volume = f
		}
	}
	return nil
}

func (wt *webTarget) command(args ...any) (*mpvResponse, error) {
	if len(args) == 0 {
		return nil, fmt.Errorf("empty command")
	}
	cmd := fmt.Sprintf("%v", args[0])
	switch cmd {
	case "playlist-next":
		wt.mu.Lock()
		if wt.playlistPos+1 < len(wt.playlist) {
			wt.playlistPos++
			wt.timePos = 0
		}
		wt.mu.Unlock()
		return &mpvResponse{}, nil
	case "playlist-prev":
		wt.mu.Lock()
		if wt.playlistPos > 0 {
			wt.playlistPos--
			wt.timePos = 0
		}
		wt.mu.Unlock()
		return &mpvResponse{}, nil
	case "playlist-clear":
		return &mpvResponse{}, wt.playlistClear()
	case "playlist-move":
		if len(args) >= 3 {
			return &mpvResponse{}, wt.playlistMove(intFromAny(args[1], 0), intFromAny(args[2], 0))
		}
		return &mpvResponse{}, nil
	default:
		return &mpvResponse{}, nil
	}
}

func (wt *webTarget) commandBatch(cmds [][]any) ([]*mpvResponse, error) {
	var results []*mpvResponse
	for _, cmd := range cmds {
		resp, err := wt.command(cmd...)
		if err != nil {
			return results, err
		}
		results = append(results, resp)
	}
	return results, nil
}

func (wt *webTarget) isRunning() bool {
	wt.mu.Lock()
	defer wt.mu.Unlock()
	return wt.alive
}

func (wt *webTarget) close() {
	wt.mu.Lock()
	wt.alive = false
	wt.mu.Unlock()
}

// ---------------------------------------------------------------------------
// Embedded web UI assets
// ---------------------------------------------------------------------------

//go:embed web
var webFS embed.FS

// ---------------------------------------------------------------------------
// Session store
// ---------------------------------------------------------------------------

type sessionStore struct {
	mu     sync.Mutex
	tokens map[string]time.Time // token → expiry
}

func newSessionStore() *sessionStore {
	s := &sessionStore{
		tokens: make(map[string]time.Time),
	}
	go s.cleanup()
	return s
}

// create generates a cryptographically random session token and stores it
// with a 7-day expiry.
func (s *sessionStore) create() string {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		panic("crypto/rand failed: " + err.Error())
	}
	token := hex.EncodeToString(b)

	s.mu.Lock()
	s.tokens[token] = time.Now().Add(7 * 24 * time.Hour)
	s.mu.Unlock()

	return token
}

// valid reports whether token exists and has not expired.
func (s *sessionStore) valid(token string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	expiry, ok := s.tokens[token]
	if !ok {
		return false
	}
	return time.Now().Before(expiry)
}

// cleanup runs in a goroutine and removes expired tokens every hour.
func (s *sessionStore) cleanup() {
	ticker := time.NewTicker(1 * time.Hour)
	defer ticker.Stop()

	for range ticker.C {
		s.mu.Lock()
		now := time.Now()
		for tok, expiry := range s.tokens {
			if now.After(expiry) {
				delete(s.tokens, tok)
			}
		}
		s.mu.Unlock()
	}
}

// Package-level session store, ready to use.
var webSessions = newSessionStore()

// ---------------------------------------------------------------------------
// Auth handler
// ---------------------------------------------------------------------------

// handleWebAuth authenticates a web UI client by comparing the submitted
// secret against the configured WebSecret. On success it sets a session
// cookie.
func (a *app) handleWebAuth(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	secret := r.FormValue("secret")
	expected := a.cfg.Server.WebSecret

	if subtle.ConstantTimeCompare([]byte(secret), []byte(expected)) != 1 {
		http.Error(w, "invalid secret", http.StatusUnauthorized)
		return
	}

	token := webSessions.create()
	http.SetCookie(w, &http.Cookie{
		Name:     "melody_session",
		Value:    token,
		Path:     "/",
		HttpOnly: true,
		SameSite: http.SameSiteLaxMode,
		MaxAge:   7 * 24 * 60 * 60, // 7 days in seconds
	})

	w.WriteHeader(http.StatusOK)
}

// ---------------------------------------------------------------------------
// Auth middleware
// ---------------------------------------------------------------------------

// authMiddleware gates access to the wrapped handler behind a valid session
// cookie. If no WebSecret is configured, all requests pass through.
func (a *app) authMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if a.cfg.Server.WebSecret == "" {
			next.ServeHTTP(w, r)
			return
		}

		cookie, err := r.Cookie("melody_session")
		if err != nil || !webSessions.valid(cookie.Value) {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}

		next.ServeHTTP(w, r)
	})
}

// ---------------------------------------------------------------------------
// Static file handler
// ---------------------------------------------------------------------------

// webHandler returns an http.Handler that serves the embedded web UI assets.
func (a *app) webHandler() http.Handler {
	subFS, err := fs.Sub(webFS, "web")
	if err != nil {
		panic("failed to create sub-filesystem for web UI: " + err.Error())
	}
	fileServer := http.FileServer(http.FS(subFS))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-cache, must-revalidate")
		fileServer.ServeHTTP(w, r)
	})
}
