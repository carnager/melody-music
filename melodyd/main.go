package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math/rand"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"syscall"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/BurntSushi/toml"
	"github.com/carnager/melody/internal/shared"
	"golang.org/x/net/websocket"
)

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

type config struct {
	Server struct {
		Name          string   `toml:"name"` // display name for the local output device
		BindToAddress []string `toml:"bind_to_address"`
		APISecret     string   `toml:"api_secret"`
		BaseURL       string   `toml:"base_url"` // externally reachable URL for stream URLs sent to remote devices
		WebSecret     string   `toml:"web_secret"`
	} `toml:"server"`
	Library struct {
		MusicDir string `toml:"music_dir"`
	} `toml:"library"`
	MPV struct {
		Socket     string `toml:"socket"`
		Executable string `toml:"executable"`
		ReplayGain string `toml:"replaygain"` // "off", "track", "album"
	} `toml:"mpv"`
	Random struct {
		Tracks int `toml:"tracks"`
	} `toml:"random"`
	MPD struct {
		Port int `toml:"port"`
	} `toml:"mpd"`
}

type paths struct {
	DataDir           string
	ConfigPath        string
	DBFile            string
	ActiveDeviceFile  string
	PlayQueueFile     string
	PlayStateFile     string
	TranscodeCacheDir string
}

// ---------------------------------------------------------------------------
// mpv IPC
// ---------------------------------------------------------------------------

type mpvClient struct {
	socketPath string
	executable string
	replaygain string
	mu         sync.Mutex
	process    *exec.Cmd
	reqID      int
	conn       net.Conn
	scanner    *bufio.Scanner
}

type mpvRequest struct {
	Command   []any `json:"command"`
	RequestID int   `json:"request_id"`
}

type mpvResponse struct {
	Data      any    `json:"data"`
	Error     string `json:"error"`
	RequestID int    `json:"request_id"`
}

// ---------------------------------------------------------------------------
// Playback target interface
// ---------------------------------------------------------------------------

type playbackTarget interface {
	loadFile(url, mode string, meta map[string]any) error
	loadFileBatch(urls []string, mode string) error
	playlistClear() error
	playlistRemove(index int) error
	playlistMove(from, to int) error
	getProperty(name string) (any, error)
	setProperty(name string, value any) error
	command(args ...any) (*mpvResponse, error)
	commandBatch(cmds [][]any) ([]*mpvResponse, error)
	isRunning() bool
}

// remoteTarget is now agentTarget in mpd.go — agents connect via MPD protocol.

// ---------------------------------------------------------------------------
// Device management
// ---------------------------------------------------------------------------

type device struct {
	ID         string    `json:"id"`
	Name       string    `json:"name"`
	Address    string    `json:"address"`
	IsLocal    bool      `json:"is_local"`
	Type       string    `json:"type"` // "local", "agent"
	Format     string    `json:"format"`
	MaxBitRate    int       `json:"max_bitrate"`
	ReplayGain    string    `json:"replaygain,omitempty"` // "off", "track", "album"
	LastSeen      time.Time `json:"last_seen"`
}

// ---------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------

type app struct {
	cfg     config
	paths   paths
	logger  *log.Logger
	mpv     *mpvClient
	db      *musicDB
	scanner *scanner
	// playQueue tracks song IDs (SQLite track IDs as strings) in current mpv playlist order
	playQueue      []string
	playQueueMu    sync.Mutex
	queueVersion   int   // incremented on every queue change, used by MPD plchanges
	queueIDs       []int // parallel to playQueue, unique MPD songid per entry
	queueIDCounter int   // monotonically incrementing counter for MPD songids
	// playback state
	curQueuePos    int // authoritative current position in playQueue
	pendingNextPos int // queue position preloaded at target slot 1 (-1 if none)
	// playback modes
	modeRepeat  bool // loop the queue
	modeRandom  bool // random track order
	modeSingle  bool // stop after current track (or repeat it if repeat is on)
	modeConsume bool // remove tracks from queue after playing
	// MPD notification hub
	mpdHub *notifyHub
	// device management
	devices      map[string]*device
	agentTargets map[string]*agentTarget // keyed by device ID
	webTargets   map[string]*webTarget   // keyed by device ID
	devicesMu    sync.RWMutex
	activeDevice string // device ID, "" = local
}

func main() {
	logger := log.New(os.Stdout, "melodyd: ", log.LstdFlags)
	cfg, pathCfg, err := loadConfig()
	if err != nil {
		logger.Fatalf("load config: %v", err)
	}

	if cfg.Library.MusicDir == "" {
		logger.Fatalf("library.music_dir is required in config")
	}

	db, err := openMusicDB(pathCfg.DBFile)
	if err != nil {
		logger.Fatalf("open database: %v", err)
	}

	a := &app{
		cfg:    cfg,
		paths:  pathCfg,
		logger: logger,
		db:     db,
		mpv: &mpvClient{
			socketPath: cfg.MPV.Socket,
			executable: cfg.MPV.Executable,
			replaygain: cfg.MPV.ReplayGain,
		},
		scanner:   newScanner(cfg.Library.MusicDir, db, logger, pathCfg.TranscodeCacheDir),
		playQueue: []string{},
		devices: map[string]*device{
			"local": {
				ID:       "local",
				Name:     cfg.Server.Name,
				IsLocal:  true,
				Type:     "local",
				LastSeen: time.Now(),
			},
		},
		agentTargets: make(map[string]*agentTarget),
		webTargets:   make(map[string]*webTarget),
		activeDevice: "local",
		mpdHub:       newNotifyHub(),
	}

	a.scanner.onScanComplete = func() {
		db.invalidateCache()
		if err := db.rebuildFTS(); err != nil {
			logger.Printf("warning: FTS rebuild after scan failed: %v", err)
		}
		db.warmCache()
		a.mpdHub.notify(SubDatabase)
	}

	// Pre-warm expensive query caches at startup
	db.warmCache()

	a.restorePlayQueue()
	a.restoreActiveDevice()

	// Assign MPD queue IDs for restored queue
	a.playQueueMu.Lock()
	for range a.playQueue {
		a.queueIDCounter++
		a.queueIDs = append(a.queueIDs, a.queueIDCounter)
	}
	if len(a.playQueue) > 0 {
		a.queueVersion = 1
	}
	a.playQueueMu.Unlock()

	// Initial library scan
	go func() {
		if err := a.scanner.fullScan(); err != nil {
			logger.Printf("initial scan error: %v", err)
		}
	}()
	go a.scanner.watchForChanges()

	go a.ensureMPV()
	go a.watchMPVEvents()
	go a.watchPlayState()
	go a.deviceCleanup()
	if a.cfg.MPD.Port > 0 {
		go func() {
			if err := a.serveMPD(); err != nil {
				logger.Printf("mpd server error: %v", err)
			}
		}()
	}
	// Graceful shutdown on SIGINT/SIGTERM: save state, close agents, exit.
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sigCh
		logger.Println("shutting down...")
		a.savePlayState()
		a.devicesMu.Lock()
		for _, at := range a.agentTargets {
			at.close()
		}
		a.devicesMu.Unlock()
		os.Exit(0)
	}()

	if err := a.serve(); err != nil {
		logger.Fatalf("listen and serve: %v", err)
	}
}

// ---------------------------------------------------------------------------
// Config loading
// ---------------------------------------------------------------------------

func loadConfig() (config, paths, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return config{}, paths{}, err
	}
	xdgData := getenvDefault("XDG_DATA_HOME", filepath.Join(home, ".local", "share"))
	xdgConfig := getenvDefault("XDG_CONFIG_HOME", filepath.Join(home, ".config"))

	pathCfg := paths{
		DataDir:           filepath.Join(xdgData, "melody"),
		ConfigPath:        filepath.Join(xdgConfig, "melody", "melodyd.toml"),
		DBFile:            filepath.Join(xdgData, "melody", "melody.db"),
		ActiveDeviceFile:  filepath.Join(xdgData, "melody", "active_device"),
		PlayQueueFile:     filepath.Join(xdgData, "melody", "playqueue.json"),
		PlayStateFile:     filepath.Join(xdgData, "melody", "playstate.json"),
		TranscodeCacheDir: filepath.Join(xdgData, "melody", "transcode_cache"),
	}

	if err := os.MkdirAll(pathCfg.DataDir, 0o755); err != nil {
		return config{}, paths{}, err
	}
	if err := os.MkdirAll(pathCfg.TranscodeCacheDir, 0o755); err != nil {
		return config{}, paths{}, err
	}
	if err := os.MkdirAll(filepath.Dir(pathCfg.ConfigPath), 0o755); err != nil {
		return config{}, paths{}, err
	}

	if _, err := os.Stat(pathCfg.ConfigPath); errors.Is(err, os.ErrNotExist) {
		if err := os.WriteFile(pathCfg.ConfigPath, []byte(defaultDaemonConfig()), 0o644); err != nil {
			return config{}, paths{}, err
		}
	}

	var raw map[string]any
	if _, err := toml.DecodeFile(pathCfg.ConfigPath, &raw); err != nil {
		return config{}, paths{}, err
	}
	var cfg config
	server, _ := raw["server"].(map[string]any)
	library, _ := raw["library"].(map[string]any)
	mpvSection, _ := raw["mpv"].(map[string]any)
	random, _ := raw["random"].(map[string]any)
	cfg.Server.Name = stringify(server["name"])
	cfg.Server.BindToAddress = stringSlice(server["bind_to_address"])
	cfg.Server.APISecret = stringify(server["api_secret"])
	cfg.Server.BaseURL = stringify(server["base_url"])
	cfg.Server.WebSecret = stringify(server["web_secret"])
	cfg.Library.MusicDir = stringify(library["music_dir"])
	cfg.MPV.Socket = stringify(mpvSection["socket"])
	cfg.MPV.Executable = stringify(mpvSection["executable"])
	cfg.MPV.ReplayGain = stringify(mpvSection["replaygain"])
	cfg.Random.Tracks = intFromAny(random["tracks"], 20)
	mpdSection, _ := raw["mpd"].(map[string]any)
	cfg.MPD.Port = intFromAny(mpdSection["port"], 6600)
	applyDefaults(&cfg)
	return cfg, pathCfg, nil
}

func defaultDaemonConfig() string {
	return `[server]
bind_to_address = ["0.0.0.0:6701", "` + shared.DefaultSocketPath() + `"]

[library]
music_dir = ""

[mpv]
socket = "` + defaultMPVSocket() + `"
executable = "mpv"
replaygain = ""

[random]
tracks = 20

`
}

func defaultMPVSocket() string {
	runtimeDir := shared.Getenv("XDG_RUNTIME_DIR", filepath.Join(os.TempDir(), fmt.Sprintf("melody-%d", os.Getuid())))
	return filepath.Join(runtimeDir, "melody", "mpv.sock")
}

func applyDefaults(cfg *config) {
	if cfg.Server.Name == "" {
		hostname, _ := os.Hostname()
		if hostname != "" {
			cfg.Server.Name = hostname
		} else {
			cfg.Server.Name = "Server"
		}
	}
	if cfg.MPV.Socket == "" {
		cfg.MPV.Socket = defaultMPVSocket()
	}
	if cfg.MPV.Executable == "" {
		cfg.MPV.Executable = "mpv"
	}
	if cfg.Random.Tracks <= 0 {
		cfg.Random.Tracks = 20
	}
	if envBind := os.Getenv("MELODYD_BIND_TO_ADDRESS"); envBind != "" {
		cfg.Server.BindToAddress = splitAndTrim(envBind, ",")
	}
	if len(cfg.Server.BindToAddress) == 0 {
		cfg.Server.BindToAddress = defaultBindToAddress()
	}
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

func (a *app) serve() error {
	handler := a.routes()
	listeners, err := a.listenConfigured()
	if err != nil {
		return err
	}

	// Log web UI availability for each TCP listener
	for _, l := range listeners {
		if addr, ok := l.Addr().(*net.TCPAddr); ok {
			host := addr.IP.String()
			if host == "0.0.0.0" {
				host = "localhost"
			}
			a.logger.Printf("web UI available at http://%s:%d/web/", host, addr.Port)
		}
	}

	errCh := make(chan error, len(listeners))
	for _, listener := range listeners {
		l := listener
		go func() {
			errCh <- http.Serve(l, handler)
		}()
	}

	err = <-errCh
	for _, listener := range listeners {
		_ = listener.Close()
	}
	return err
}

func (a *app) listenConfigured() ([]net.Listener, error) {
	listeners := make([]net.Listener, 0, len(a.cfg.Server.BindToAddress))
	for _, bind := range a.cfg.Server.BindToAddress {
		listener, err := a.listenAddress(bind)
		if err != nil {
			for _, existing := range listeners {
				_ = existing.Close()
			}
			return nil, err
		}
		listeners = append(listeners, listener)
	}
	return listeners, nil
}

func (a *app) listenAddress(bind string) (net.Listener, error) {
	bind = strings.TrimSpace(bind)
	if bind == "" {
		return nil, fmt.Errorf("empty bind_to_address entry")
	}
	if isUnixBindAddress(bind) {
		listener, err := listenUnixSocket(bind)
		if err != nil {
			return nil, err
		}
		a.logger.Printf("serving unix socket on %s", bind)
		return listener, nil
	}
	listener, err := net.Listen("tcp", bind)
	if err != nil {
		return nil, err
	}
	a.logger.Printf("serving tcp on %s", bind)
	return listener, nil
}

func listenUnixSocket(socketPath string) (net.Listener, error) {
	if socketPath == "" {
		return nil, fmt.Errorf("empty socket path")
	}
	if err := os.MkdirAll(filepath.Dir(socketPath), 0o755); err != nil {
		return nil, err
	}
	if err := os.Remove(socketPath); err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	listener, err := net.Listen("unix", socketPath)
	if err != nil {
		return nil, err
	}
	if err := os.Chmod(socketPath, 0o600); err != nil {
		_ = listener.Close()
		return nil, err
	}
	return listener, nil
}

func isUnixBindAddress(bind string) bool {
	return strings.Contains(bind, "/")
}

func defaultBindToAddress() []string {
	return []string{
		"0.0.0.0:6701",
		shared.DefaultSocketPath(),
	}
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

func (a *app) routes() http.Handler {
	mux := http.NewServeMux()

	// Audio streaming
	mux.Handle("GET /api/v1/stream/{id}", a.authMiddleware(http.HandlerFunc(a.handleStream)))

	// Cover art
	mux.Handle("GET /api/v1/cover/{id}", a.authMiddleware(http.HandlerFunc(a.handleCoverArt)))

	// WebSocket MPD transport
	mux.Handle("GET /mpd", a.authMiddleware(websocket.Server{
		Handler:   a.serveMPDWebSocket,
		Handshake: func(*websocket.Config, *http.Request) error { return nil },
	}))

	// Web UI auth (no middleware — this IS the login endpoint)
	mux.HandleFunc("POST /web/auth", a.handleWebAuth)

	// Web UI: redirect /web to /web/
	mux.HandleFunc("GET /web", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/web/", http.StatusMovedPermanently)
	})

	// Web UI static files (no auth — the JS handles login flow)
	mux.Handle("/web/", http.StripPrefix("/web/", a.webHandler()))

	return mux
}

// serveMPDWebSocket handles a WebSocket connection using the MPD protocol.
// Same protocol as TCP, just over WebSocket for HTTP reverse proxy compatibility.
func (a *app) serveMPDWebSocket(ws *websocket.Conn) {
	c := &mpdConn{
		conn:   ws,
		reader: bufio.NewReader(ws),
		writer: bufio.NewWriter(ws),
		app:    a,
		logger: a.logger,
	}
	c.serve()
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Stream URL helpers
// ---------------------------------------------------------------------------

// streamURL returns a URL for streaming the given track.
// For local mpv, returns the file path directly.
func (a *app) streamURL(songID string) string {
	id, err := strconv.ParseInt(songID, 10, 64)
	if err != nil {
		return ""
	}
	path, err := a.db.trackPathByID(id)
	if err != nil {
		return ""
	}
	// For local playback, use the file path directly
	dev := a.activeDeviceInfo()
	if dev == nil || dev.IsLocal {
		return path
	}
	return a.buildStreamURL(songID, "", 0)
}

// streamURLForDevice builds an HTTP stream URL for remote devices.
func (a *app) streamURLForDevice(songID, format string, maxBitRate int, _ string) string {
	// For local playback target, use file path directly
	id, err := strconv.ParseInt(songID, 10, 64)
	if err != nil {
		return ""
	}
	_, err = a.db.trackPathByID(id)
	if err != nil {
		return ""
	}
	// For remote devices, always use HTTP URL
	if format == "" && maxBitRate == 0 {
		return a.buildStreamURL(songID, "", 0)
	}
	return a.buildStreamURL(songID, format, maxBitRate)
}

func (a *app) streamURLForActiveDevice(songID string) string {
	dev := a.activeDeviceInfo()
	if dev == nil || dev.IsLocal {
		// Local mpv: use file path directly
		id, err := strconv.ParseInt(songID, 10, 64)
		if err != nil {
			return ""
		}
		path, err := a.db.trackPathByID(id)
		if err != nil {
			return ""
		}
		return path
	}
	return a.streamURLForDevice(songID, dev.Format, dev.MaxBitRate, "")
}

// buildStreamURL constructs an HTTP URL to the server's stream endpoint.
func (a *app) buildStreamURL(songID, format string, maxBitRate int) string {
	baseURL := strings.TrimRight(a.cfg.Server.BaseURL, "/")
	if baseURL == "" {
		// Fall back to first TCP bind address
		for _, bind := range a.cfg.Server.BindToAddress {
			if !strings.Contains(bind, "/") { // Not a unix socket
				host, port, _ := net.SplitHostPort(bind)
				if host == "" || host == "0.0.0.0" {
					host = outboundIP()
				}
				baseURL = "http://" + net.JoinHostPort(host, port)
				break
			}
		}
	}
	if baseURL == "" {
		baseURL = "http://127.0.0.1:6701"
	}
	u := baseURL + "/api/v1/stream/" + songID
	params := url.Values{}
	if a.cfg.Server.APISecret != "" {
		params.Set("secret", a.cfg.Server.APISecret)
	}
	if format != "" {
		params.Set("format", format)
	}
	if maxBitRate > 0 {
		params.Set("max_bitrate", strconv.Itoa(maxBitRate))
	}
	if len(params) > 0 {
		u += "?" + params.Encode()
	}
	return u
}

func (a *app) restoreActiveDevice() {
	data, err := os.ReadFile(a.paths.ActiveDeviceFile)
	if err != nil {
		return
	}
	id := strings.TrimSpace(string(data))
	if id != "" {
		a.activeDevice = id
	}
}


// ---------------------------------------------------------------------------
// mpv management
// ---------------------------------------------------------------------------

func (a *app) ensureMPV() {
	for {
		if a.mpv.isRunning() {
			time.Sleep(5 * time.Second)
			continue
		}
		a.logger.Printf("mpv: starting idle instance")
		if err := a.mpv.start(); err != nil {
			a.logger.Printf("mpv: start failed: %v", err)
			time.Sleep(5 * time.Second)
			continue
		}
		a.logger.Printf("mpv: started, ipc at %s", a.mpv.socketPath)
		time.Sleep(5 * time.Second)
	}
}

// nextQueuePos returns the queue position that should follow the current one,
// applying playback modes. Returns -1 if there's no next track.
func (a *app) nextQueuePos() int {
	qLen := len(a.playQueue)
	if qLen == 0 {
		return -1
	}
	if a.modeSingle {
		if a.modeRepeat {
			return a.curQueuePos // loop same track
		}
		return -1 // no next in single mode
	}
	if a.modeRandom && qLen > 1 {
		next := rand.Intn(qLen)
		if next == a.curQueuePos {
			next = (next + 1) % qLen
		}
		return next
	}
	next := a.curQueuePos + 1
	if next >= qLen {
		if a.modeRepeat {
			return 0
		}
		return -1
	}
	return next
}

// syncTarget loads the current track and the preloaded next track into the
// target. The target always has at most 2 tracks: slot 0 = current, slot 1 = next.
// Must be called with playQueueMu held.
func (a *app) syncTarget() {
	t := a.target()
	qLen := len(a.playQueue)

	if qLen == 0 || a.curQueuePos < 0 || a.curQueuePos >= qLen {
		_ = t.playlistClear()
		a.pendingNextPos = -1
		return
	}

	// Load current track
	_ = t.playlistClear()
	url := a.streamURLForActiveDevice(a.playQueue[a.curQueuePos])
	a.logger.Printf("syncTarget: loading pos %d (songID=%s)", a.curQueuePos, a.playQueue[a.curQueuePos])
	if err := t.loadFile(url, "replace", a.replayGainMeta(a.playQueue[a.curQueuePos])); err != nil {
		a.logger.Printf("syncTarget: loadFile replace failed: %v", err)
	}

	// Preload next track for gapless playback
	a.pendingNextPos = a.nextQueuePos()
	if a.pendingNextPos >= 0 && a.pendingNextPos < qLen {
		nextURL := a.streamURLForActiveDevice(a.playQueue[a.pendingNextPos])
		a.logger.Printf("syncTarget: preloading pos %d (songID=%s)", a.pendingNextPos, a.playQueue[a.pendingNextPos])
		_ = t.loadFile(nextURL, "append", a.replayGainMeta(a.playQueue[a.pendingNextPos]))
	}

}

// syncNextTrack updates only the preloaded next track (slot 1) without
// restarting the current track. Used when playback modes change.
func (a *app) syncNextTrack() {
	t := a.target()
	qLen := len(a.playQueue)

	if qLen == 0 || a.curQueuePos < 0 || a.curQueuePos >= qLen {
		return
	}

	// Remove old preloaded track (slot 1) if present
	if a.pendingNextPos >= 0 {
		_ = t.playlistRemove(1)
	}

	// Preload new next track
	a.pendingNextPos = a.nextQueuePos()
	if a.pendingNextPos >= 0 && a.pendingNextPos < qLen {
		nextURL := a.streamURLForActiveDevice(a.playQueue[a.pendingNextPos])
		_ = t.loadFile(nextURL, "append", a.replayGainMeta(a.playQueue[a.pendingNextPos]))
	}
}

// advanceTrack is called when a track naturally ends (target moved from slot 0 to 1,
// or web/Android sent trackended). It applies playback modes and loads the next pair.
// IMPORTANT: at this point the preloaded track at slot 1 is already playing in mpv.
// We must NOT call syncTarget (which would clear+reload and restart the track).
// Instead: remove finished slot 0, let playing track slide to slot 0, append new slot 1.
func (a *app) advanceTrack() {
	t := a.target()
	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()

	qLen := len(a.playQueue)
	if qLen == 0 {
		return
	}

	// Single mode: stop or repeat the current track
	if a.modeSingle {
		if a.modeRepeat {
			// Loop the same track — reload it at slot 0
			a.syncTarget()
		} else {
			// Pause at end — go back to slot 0 (reload current track, paused)
			a.syncTarget()
			_ = t.setProperty("pause", true)
		}
		a.mpdHub.notify(SubPlayer)
		return
	}

	// Consume mode: remove the track that just finished from the server queue
	if a.modeConsume && a.curQueuePos >= 0 && a.curQueuePos < qLen {
		a.playQueue = append(a.playQueue[:a.curQueuePos], a.playQueue[a.curQueuePos+1:]...)
		a.queueIDs = append(a.queueIDs[:a.curQueuePos], a.queueIDs[a.curQueuePos+1:]...)
		a.queueVersion++
		a.savePlayQueue()
		qLen = len(a.playQueue)

		if qLen == 0 {
			a.curQueuePos = 0
			_ = t.playlistClear()
			a.pendingNextPos = -1
			a.mpdHub.notify(SubPlaylist, SubPlayer)
			return
		}

		// Adjust pendingNextPos for the removal
		if a.pendingNextPos > a.curQueuePos {
			a.pendingNextPos--
		}
		if a.pendingNextPos >= qLen {
			if a.modeRepeat {
				a.pendingNextPos = 0
			} else {
				a.pendingNextPos = -1
			}
		}

		// The preloaded track is now playing — use pendingNextPos as new curQueuePos
		if a.pendingNextPos >= 0 && a.pendingNextPos < qLen {
			a.curQueuePos = a.pendingNextPos
		} else {
			a.curQueuePos = 0
			_ = t.playlistClear()
			a.pendingNextPos = -1
			a.mpdHub.notify(SubPlaylist, SubPlayer)
			return
		}

		// Remove finished track from mpv (slot 0), playing track slides to slot 0
		_ = t.playlistRemove(0)
		// Preload new next track at slot 1
		a.pendingNextPos = a.nextQueuePos()
		if a.pendingNextPos >= 0 && a.pendingNextPos < qLen {
			nextURL := a.streamURLForActiveDevice(a.playQueue[a.pendingNextPos])
			_ = t.loadFile(nextURL, "append", a.replayGainMeta(a.playQueue[a.pendingNextPos]))
		}
		a.mpdHub.notify(SubPlaylist, SubPlayer)
		return
	}

	// Normal advance: the preloaded track at slot 1 is already playing
	if a.pendingNextPos >= 0 && a.pendingNextPos < qLen {
		a.curQueuePos = a.pendingNextPos
	} else {
		// No next track was preloaded — end of queue
		a.mpdHub.notify(SubPlayer)
		return
	}

	// Remove finished track from mpv (slot 0), playing track slides to slot 0
	_ = t.playlistRemove(0)

	// Preload new next track at slot 1
	a.pendingNextPos = a.nextQueuePos()
	if a.pendingNextPos >= 0 && a.pendingNextPos < qLen {
		nextURL := a.streamURLForActiveDevice(a.playQueue[a.pendingNextPos])
		_ = t.loadFile(nextURL, "append", a.replayGainMeta(a.playQueue[a.pendingNextPos]))
	}

	a.mpdHub.notify(SubPlayer)
}

// removeFromQueue removes a single track at pos from the server's queue.
// Does NOT touch the target playlist — caller must syncTarget if needed.
func (a *app) removeFromQueue(pos int) {
	// Caller must hold playQueueMu
	if pos < 0 || pos >= len(a.playQueue) {
		return
	}
	a.playQueue = append(a.playQueue[:pos], a.playQueue[pos+1:]...)
	a.queueIDs = append(a.queueIDs[:pos], a.queueIDs[pos+1:]...)
	a.queueVersion++
	a.savePlayQueue()
}

// watchMPVEvents opens a dedicated connection to mpv's IPC socket and listens
// for end-file events. When a track ends naturally (reason=eof), it calls
// advanceTrack. This replaces polling — no races, no skipInProgress flag needed.
// User-initiated actions (next, play, etc.) cause reason=stop which is ignored.
func (a *app) watchMPVEvents() {
	for {
		// Wait for mpv to be ready
		for !a.mpv.isRunning() {
			time.Sleep(1 * time.Second)
		}

		conn, err := net.DialTimeout("unix", a.mpv.socketPath, 2*time.Second)
		if err != nil {
			a.logger.Printf("mpv events: connect failed: %v", err)
			time.Sleep(2 * time.Second)
			continue
		}

		a.logger.Printf("mpv events: listening for end-file events")
		scanner := bufio.NewScanner(conn)
		for scanner.Scan() {
			var evt struct {
				Event string `json:"event"`
				Reason string `json:"reason"`
			}
			if err := json.Unmarshal(scanner.Bytes(), &evt); err != nil {
				continue
			}
			if evt.Event != "end-file" {
				continue
			}
			a.logger.Printf("mpv events: end-file reason=%s", evt.Reason)
			if evt.Reason == "eof" {
				a.advanceTrack()
			}
		}

		conn.Close()
		a.logger.Printf("mpv events: connection lost, reconnecting...")
		time.Sleep(1 * time.Second)
	}
}

func (m *mpvClient) isRunning() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.conn != nil {
		// Try a lightweight property get to confirm mpv is alive
		return true
	}
	conn, err := net.DialTimeout("unix", m.socketPath, 500*time.Millisecond)
	if err != nil {
		return false
	}
	m.conn = conn
	m.scanner = bufio.NewScanner(conn)
	return true
}

func (m *mpvClient) start() error {
	m.mu.Lock()
	defer m.mu.Unlock()

	if err := os.MkdirAll(filepath.Dir(m.socketPath), 0o755); err != nil {
		return err
	}
	_ = os.Remove(m.socketPath)

	args := []string{"--idle", "--no-video", "--no-terminal", "--input-ipc-server=" + m.socketPath}
	if m.replaygain != "" && m.replaygain != "off" {
		args = append(args, "--replaygain="+m.replaygain)
	}
	cmd := exec.Command(m.executable, args...)
	cmd.Stdout = nil
	cmd.Stderr = nil
	if err := cmd.Start(); err != nil {
		return err
	}
	m.process = cmd

	// Wait for socket to appear
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(m.socketPath); err == nil {
			return nil
		}
		time.Sleep(100 * time.Millisecond)
	}
	return fmt.Errorf("mpv socket did not appear at %s", m.socketPath)
}

func (m *mpvClient) connect() error {
	if m.conn != nil {
		m.conn.Close()
		m.conn = nil
		m.scanner = nil
	}
	conn, err := net.DialTimeout("unix", m.socketPath, 2*time.Second)
	if err != nil {
		return fmt.Errorf("mpv connect: %w", err)
	}
	m.conn = conn
	m.scanner = bufio.NewScanner(conn)
	return nil
}

func (m *mpvClient) command(args ...any) (*mpvResponse, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	m.reqID++
	reqID := m.reqID

	// Try on existing connection, reconnect once on failure
	for attempt := 0; attempt < 2; attempt++ {
		if m.conn == nil {
			if err := m.connect(); err != nil {
				return nil, err
			}
		}

		m.conn.SetDeadline(time.Now().Add(5 * time.Second))

		req := mpvRequest{Command: args, RequestID: reqID}
		data, err := json.Marshal(req)
		if err != nil {
			return nil, err
		}
		data = append(data, '\n')
		if _, err := m.conn.Write(data); err != nil {
			// Connection broken, reconnect
			m.conn.Close()
			m.conn = nil
			m.scanner = nil
			continue
		}

		for m.scanner.Scan() {
			var resp mpvResponse
			if err := json.Unmarshal(m.scanner.Bytes(), &resp); err != nil {
				continue
			}
			if resp.RequestID == reqID {
				if resp.Error != "" && resp.Error != "success" {
					return nil, fmt.Errorf("mpv: %s", resp.Error)
				}
				return &resp, nil
			}
		}
		// Scanner exhausted — connection broken
		m.conn.Close()
		m.conn = nil
		m.scanner = nil
	}
	return nil, fmt.Errorf("mpv: no response")
}

// commandBatch sends multiple commands in a single write and reads all responses.
// Much faster than sequential command() calls due to reduced IPC round-trips.
func (m *mpvClient) commandBatch(cmds [][]any) ([]*mpvResponse, error) {
	if len(cmds) == 0 {
		return nil, nil
	}

	m.mu.Lock()
	defer m.mu.Unlock()

	startID := m.reqID + 1
	var allData []byte
	for _, args := range cmds {
		m.reqID++
		req := mpvRequest{Command: args, RequestID: m.reqID}
		data, err := json.Marshal(req)
		if err != nil {
			return nil, err
		}
		allData = append(allData, data...)
		allData = append(allData, '\n')
	}
	endID := m.reqID

	for attempt := 0; attempt < 2; attempt++ {
		if m.conn == nil {
			if err := m.connect(); err != nil {
				return nil, err
			}
		}

		m.conn.SetDeadline(time.Now().Add(30 * time.Second))
		if _, err := m.conn.Write(allData); err != nil {
			m.conn.Close()
			m.conn = nil
			m.scanner = nil
			continue
		}

		responses := make([]*mpvResponse, len(cmds))
		got := 0
		for m.scanner.Scan() && got < len(cmds) {
			var resp mpvResponse
			if err := json.Unmarshal(m.scanner.Bytes(), &resp); err != nil {
				continue
			}
			if resp.RequestID >= startID && resp.RequestID <= endID {
				idx := resp.RequestID - startID
				responses[idx] = &resp
				got++
			}
		}
		if got == len(cmds) {
			return responses, nil
		}
		m.conn.Close()
		m.conn = nil
		m.scanner = nil
	}
	return nil, fmt.Errorf("mpv: batch failed")
}

func (m *mpvClient) getProperty(name string) (any, error) {
	resp, err := m.command("get_property", name)
	if err != nil {
		return nil, err
	}
	return resp.Data, nil
}

func (m *mpvClient) setProperty(name string, value any) error {
	// mpv uses "no" instead of "off" for its replaygain property
	if name == "replaygain" {
		if s, ok := value.(string); ok && s == "off" {
			value = "no"
		}
	}
	_, err := m.command("set_property", name, value)
	return err
}

func (m *mpvClient) loadFile(url string, mode string, meta map[string]any) error {
	_, err := m.command("loadfile", url, mode)
	return err
}

func (m *mpvClient) loadFileBatch(urls []string, mode string) error {
	if len(urls) == 0 {
		return nil
	}
	cmds := make([][]any, len(urls))
	for i, url := range urls {
		m := mode
		if i > 0 && mode == "replace" {
			m = "append"
		}
		cmds[i] = []any{"loadfile", url, m}
	}
	_, err := m.commandBatch(cmds)
	return err
}

func (m *mpvClient) playlistClear() error {
	_, err := m.command("playlist-clear")
	return err
}

func (m *mpvClient) playlistRemove(index int) error {
	_, err := m.command("playlist-remove", index)
	return err
}

func (m *mpvClient) playlistMove(from, to int) error {
	_, err := m.command("playlist-move", from, to)
	return err
}

// ---------------------------------------------------------------------------
// Playback target / device helpers
// ---------------------------------------------------------------------------

func (a *app) target() playbackTarget {
	a.devicesMu.RLock()
	defer a.devicesMu.RUnlock()
	if a.activeDevice == "" || a.activeDevice == "local" {
		return a.mpv
	}
	if at, ok := a.agentTargets[a.activeDevice]; ok && at.isRunning() {
		return at
	}
	if wt, ok := a.webTargets[a.activeDevice]; ok && wt.isRunning() {
		return wt
	}
	return a.mpv
}

// sortedDevices returns devices in stable order: "local" first, then agents sorted by ID.
// Caller must hold devicesMu.
func (a *app) sortedDevices() []*device {
	devs := make([]*device, 0, len(a.devices))
	// Local first
	if d, ok := a.devices["local"]; ok {
		devs = append(devs, d)
	}
	// Agents sorted by ID
	ids := make([]string, 0, len(a.devices))
	for id := range a.devices {
		if id != "local" {
			ids = append(ids, id)
		}
	}
	sort.Strings(ids)
	for _, id := range ids {
		devs = append(devs, a.devices[id])
	}
	return devs
}

func (a *app) activeDeviceInfo() *device {
	a.devicesMu.RLock()
	defer a.devicesMu.RUnlock()
	if a.activeDevice == "" || a.activeDevice == "local" {
		if d, ok := a.devices["local"]; ok {
			return d
		}
		return &device{ID: "local", Name: a.cfg.Server.Name, IsLocal: true, LastSeen: time.Now()}
	}
	return a.devices[a.activeDevice]
}

// ---------------------------------------------------------------------------
// Playback helpers
// ---------------------------------------------------------------------------

func (a *app) replayGainMeta(songID string) map[string]any {
	meta := map[string]any{"song_id": songID}
	if track := a.findTrackBySongID(songID); track != nil {
		if rg, ok := track["replay_gain"].(map[string]any); ok {
			meta["replay_gain"] = rg
		}
	}
	return meta
}

func (a *app) findTrackBySongID(songID string) map[string]any {
	track, err := a.db.trackBySongID(songID)
	if err != nil {
		return nil
	}
	return track
}

func (a *app) addSongsToPlaylist(songIDs []string, mode string) error {
	if len(songIDs) == 0 {
		return nil
	}

	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()

	switch mode {
	case "replace":
		a.playQueue = nil
		a.queueIDs = nil
		a.playQueue = append(a.playQueue, songIDs...)
		for range songIDs {
			a.queueIDCounter++
			a.queueIDs = append(a.queueIDs, a.queueIDCounter)
		}
		a.queueVersion++
		a.curQueuePos = 0
		a.savePlayQueue()
		a.syncTarget()
		return a.target().setProperty("pause", false)

	case "insert":
		pos := a.curQueuePos + 1
		var newIDs []int
		for range songIDs {
			a.queueIDCounter++
			newIDs = append(newIDs, a.queueIDCounter)
		}
		newQueue := make([]string, 0, len(a.playQueue)+len(songIDs))
		newQueue = append(newQueue, a.playQueue[:pos]...)
		newQueue = append(newQueue, songIDs...)
		if pos < len(a.playQueue) {
			newQueue = append(newQueue, a.playQueue[pos:]...)
		}
		a.playQueue = newQueue
		newQueueIDs := make([]int, 0, len(a.queueIDs)+len(newIDs))
		newQueueIDs = append(newQueueIDs, a.queueIDs[:pos]...)
		newQueueIDs = append(newQueueIDs, newIDs...)
		if pos < len(a.queueIDs) {
			newQueueIDs = append(newQueueIDs, a.queueIDs[pos:]...)
		}
		a.queueIDs = newQueueIDs
		a.queueVersion++
		a.savePlayQueue()
		// Resync preloaded next track since insert may change it
		a.syncNextTrack()
		return a.target().setProperty("pause", false)

	default: // "add"
		wasEmpty := len(a.playQueue) == 0
		a.playQueue = append(a.playQueue, songIDs...)
		for range songIDs {
			a.queueIDCounter++
			a.queueIDs = append(a.queueIDs, a.queueIDCounter)
		}
		a.queueVersion++
		a.savePlayQueue()
		if wasEmpty {
			a.curQueuePos = 0
			a.syncTarget()
		} else {
			// Resync preloaded next track in case it changed
			a.syncNextTrack()
		}
		return nil
	}
}

// queuePosByMPDID finds the queue position for the given MPD song ID.
func (a *app) queuePosByMPDID(mpdID int) int {
	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()
	for i, id := range a.queueIDs {
		if id == mpdID {
			return i
		}
	}
	return -1
}

// savePlayQueue persists the current play queue to disk (caller must hold playQueueMu or be safe).
func (a *app) savePlayQueue() {
	data, _ := json.Marshal(a.playQueue)
	_ = os.WriteFile(a.paths.PlayQueueFile, data, 0o644)
}

// restorePlayQueue loads the saved play queue from disk and reloads into
// the local mpv target so that clients see a consistent queue on reconnect.
func (a *app) restorePlayQueue() {
	data, err := os.ReadFile(a.paths.PlayQueueFile)
	if err != nil {
		return
	}
	var queue []string
	if json.Unmarshal(data, &queue) != nil || len(queue) == 0 {
		return
	}
	a.playQueue = queue
	a.logger.Printf("restored play queue: %d tracks", len(queue))

	// For local target, reload into mpv (paused) so clients can browse/click
	if a.activeDevice == "" || a.activeDevice == "local" {
		go a.reloadQueueIntoTarget()
	}
}

// reloadQueueIntoTarget loads the 2-track window into the current target (paused).
func (a *app) reloadQueueIntoTarget() {
	// Wait for mpv to be ready
	for i := 0; i < 20; i++ {
		if a.mpv.isRunning() {
			break
		}
		time.Sleep(500 * time.Millisecond)
	}
	if !a.mpv.isRunning() {
		a.logger.Printf("restore: mpv not ready, skipping queue reload")
		return
	}

	a.playQueueMu.Lock()
	qLen := len(a.playQueue)
	a.playQueueMu.Unlock()

	if qLen == 0 {
		return
	}

	// Restore curQueuePos from saved play state before syncing
	a.restorePlayStatePos()

	a.playQueueMu.Lock()
	a.syncTarget()
	a.playQueueMu.Unlock()

	_ = a.target().setProperty("pause", true)
	a.logger.Printf("restored 2-track window at pos %d (paused)", a.curQueuePos)

	a.restorePlayState()
}

// playState represents saved playback position for resume across restarts.
type playState struct {
	SongPos int     `json:"song_pos"`
	TimePos float64 `json:"time_pos"`
	Playing bool    `json:"playing"`
	Repeat  bool    `json:"repeat"`
	Random  bool    `json:"random"`
	Single  bool    `json:"single"`
	Consume bool    `json:"consume"`
}

func (a *app) savePlayState() {
	t := a.target()
	if t == nil || !t.isRunning() {
		return
	}
	var ps playState
	ps.SongPos = a.curQueuePos
	ps.Repeat = a.modeRepeat
	ps.Random = a.modeRandom
	ps.Single = a.modeSingle
	ps.Consume = a.modeConsume
	if tpRaw, err := t.getProperty("time-pos"); err == nil {
		if f, ok := tpRaw.(float64); ok {
			ps.TimePos = f
		}
	}
	if pauseRaw, err := t.getProperty("pause"); err == nil {
		if p, ok := pauseRaw.(bool); ok {
			ps.Playing = !p
		}
	}
	data, _ := json.Marshal(ps)
	_ = os.WriteFile(a.paths.PlayStateFile, data, 0o644)
}

// restorePlayStatePos reads saved play state and sets curQueuePos.
// Called before syncTarget during startup.
func (a *app) restorePlayStatePos() {
	data, err := os.ReadFile(a.paths.PlayStateFile)
	if err != nil {
		return
	}
	var ps playState
	if json.Unmarshal(data, &ps) != nil {
		return
	}
	if ps.SongPos >= 0 && ps.SongPos < len(a.playQueue) {
		a.curQueuePos = ps.SongPos
	}
	a.modeRepeat = ps.Repeat
	a.modeRandom = ps.Random
	a.modeSingle = ps.Single
	a.modeConsume = ps.Consume
	a.logger.Printf("restored modes: repeat=%v random=%v single=%v consume=%v", ps.Repeat, ps.Random, ps.Single, ps.Consume)
}

func (a *app) restorePlayState() {
	data, err := os.ReadFile(a.paths.PlayStateFile)
	if err != nil {
		return
	}
	var ps playState
	if json.Unmarshal(data, &ps) != nil {
		return
	}

	t := a.target()

	// Wait for track to load
	for i := 0; i < 40; i++ {
		if v, err := t.getProperty("duration"); err == nil {
			if d, ok := v.(float64); ok && d > 0 {
				break
			}
		}
		time.Sleep(100 * time.Millisecond)
	}

	if ps.TimePos > 0 {
		_ = t.setProperty("time-pos", ps.TimePos)
	}

	a.logger.Printf("restored position: track %d, %.1fs (paused)", ps.SongPos, ps.TimePos)
}

// watchPlayState periodically saves playback position to disk.
func (a *app) watchPlayState() {
	for {
		time.Sleep(5 * time.Second)
		a.savePlayState()
	}
}

func (a *app) currentPlayingSongID() string {
	a.playQueueMu.Lock()
	defer a.playQueueMu.Unlock()

	if a.curQueuePos < 0 || a.curQueuePos >= len(a.playQueue) {
		return ""
	}
	return a.playQueue[a.curQueuePos]
}


func (a *app) handleStream(w http.ResponseWriter, r *http.Request) {
	idStr := r.PathValue("id")
	id, err := strconv.ParseInt(idStr, 10, 64)
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	path, err := a.db.trackPathByID(id)
	if err != nil {
		http.Error(w, "track not found", http.StatusNotFound)
		return
	}

	format := r.URL.Query().Get("format")
	maxBitrate := intFromAny(r.URL.Query().Get("max_bitrate"), 0)
	startTime, _ := strconv.ParseFloat(r.URL.Query().Get("start"), 64)

	if format != "" || maxBitrate > 0 {
		a.streamTranscoded(w, r, idStr, path, format, maxBitrate, startTime)
		return
	}

	// Serve file directly with Range support
	f, err := os.Open(path)
	if err != nil {
		http.Error(w, "file not found", http.StatusNotFound)
		return
	}
	defer f.Close()

	info, err := f.Stat()
	if err != nil {
		http.Error(w, "stat error", http.StatusInternalServerError)
		return
	}

	ext := strings.ToLower(filepath.Ext(path))
	contentType := "application/octet-stream"
	switch ext {
	case ".flac":
		contentType = "audio/flac"
	case ".mp3":
		contentType = "audio/mpeg"
	case ".m4a":
		contentType = "audio/mp4"
	case ".ogg":
		contentType = "audio/ogg"
	case ".opus":
		contentType = "audio/opus"
	}
	w.Header().Set("Content-Type", contentType)
	http.ServeContent(w, r, filepath.Base(path), info.ModTime(), f)
}

func (a *app) transcodeCachePath(songID, format string, maxBitrate int) string {
	ext := format
	if ext == "" {
		ext = "mp3"
	}
	name := fmt.Sprintf("%s_%s_%d.%s", songID, ext, maxBitrate, ext)
	return filepath.Join(a.paths.TranscodeCacheDir, name)
}

func transcodeContentType(format string) string {
	switch format {
	case "mp3":
		return "audio/mpeg"
	case "opus":
		return "audio/opus"
	case "ogg":
		return "audio/ogg"
	default:
		return "application/octet-stream"
	}
}

func (a *app) streamTranscoded(w http.ResponseWriter, r *http.Request, songID, path, format string, maxBitrate int, startTime float64) {
	if format == "" {
		format = "mp3"
	}

	cachePath := a.transcodeCachePath(songID, format, maxBitrate)

	// Serve from cache if available and fresh (only for full-file requests)
	if startTime == 0 {
		if cInfo, err := os.Stat(cachePath); err == nil {
			if sInfo, err := os.Stat(path); err == nil && !sInfo.ModTime().After(cInfo.ModTime()) {
				w.Header().Set("Content-Type", transcodeContentType(format))
				http.ServeFile(w, r, cachePath)
				return
			}
		}
	}

	// Stream ffmpeg output directly to client, tee to cache for full-file requests
	var args []string
	if startTime > 0 {
		args = []string{"-ss", strconv.FormatFloat(startTime, 'f', 3, 64), "-i", path, "-v", "quiet", "-vn"}
	} else {
		args = []string{"-i", path, "-v", "quiet", "-vn"}
	}
	switch format {
	case "mp3":
		args = append(args, "-f", "mp3", "-codec:a", "libmp3lame")
	case "opus":
		args = append(args, "-f", "opus", "-codec:a", "libopus")
	case "ogg":
		args = append(args, "-f", "ogg", "-codec:a", "libvorbis")
	default:
		args = append(args, "-f", format)
	}
	if maxBitrate > 0 {
		args = append(args, "-b:a", strconv.Itoa(maxBitrate*1000))
	}
	args = append(args, "pipe:1")

	cmd := exec.Command("ffmpeg", args...)
	var stderrBuf bytes.Buffer
	cmd.Stderr = &stderrBuf
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		http.Error(w, "transcode error", http.StatusInternalServerError)
		return
	}
	if err := cmd.Start(); err != nil {
		http.Error(w, "transcode start error", http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", transcodeContentType(format))
	w.Header().Set("Transfer-Encoding", "chunked")

	// Tee to cache file for full-file requests (skip cache for offset seeks)
	tmpFile, tmpErr := os.CreateTemp(a.paths.TranscodeCacheDir, "transcode_*.tmp")
	if tmpErr == nil && startTime == 0 {
		reader := io.TeeReader(stdout, tmpFile)
		io.Copy(w, reader)
		tmpFile.Close()
		if err := cmd.Wait(); err == nil {
			os.Rename(tmpFile.Name(), cachePath)
		} else {
			os.Remove(tmpFile.Name())
			a.logger.Printf("transcode error for %s: %v — %s", songID, err, stderrBuf.String())
		}
	} else {
		if tmpFile != nil {
			tmpFile.Close()
			os.Remove(tmpFile.Name())
		}
		io.Copy(w, stdout)
		if err := cmd.Wait(); err != nil {
			a.logger.Printf("transcode error for %s: %v — %s", songID, err, stderrBuf.String())
		}
	}
}

func (a *app) handleCoverArt(w http.ResponseWriter, r *http.Request) {
	idStr := r.PathValue("id")
	if idStr == "" {
		http.Error(w, "missing id", http.StatusBadRequest)
		return
	}
	albumID, err := strconv.ParseInt(idStr, 10, 64)
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}

	// Get tracks for this album to find a file path
	tracks, err := a.db.tracksByAlbum(albumID)
	if err != nil || len(tracks) == 0 {
		http.Error(w, "no tracks for album", http.StatusNotFound)
		return
	}

	// Get the file path of the first track
	firstTrackID, _ := strconv.ParseInt(stringify(tracks[0]["song_id"]), 10, 64)
	trackPath, err := a.db.trackPathByID(firstTrackID)
	if err != nil {
		http.Error(w, "track not found", http.StatusNotFound)
		return
	}

	// Try embedded art first
	data, mimeType := extractCoverArt(trackPath)
	if data != nil {
		w.Header().Set("Content-Type", mimeType)
		w.Header().Set("Cache-Control", "public, max-age=86400")
		w.Header().Set("Content-Length", strconv.Itoa(len(data)))
		w.Write(data)
		return
	}

	// Try folder art
	dir := filepath.Dir(trackPath)
	artPath := findFolderArt(dir)
	if artPath != "" {
		w.Header().Set("Cache-Control", "public, max-age=86400")
		http.ServeFile(w, r, artPath)
		return
	}

	http.Error(w, "no cover art", http.StatusNotFound)
}


// switchDevice performs a full device handoff: captures playback state from old device,
// loads the queue into the new device, seeks to the same position, and resumes.
// Returns ("already active", nil) if newID is already active, or an error string if device not found.
func (a *app) switchDevice(newID string) error {
	a.devicesMu.Lock()
	newDev, exists := a.devices[newID]
	if !exists {
		a.devicesMu.Unlock()
		return fmt.Errorf("device not found: %s", newID)
	}

	oldID := a.activeDevice
	if oldID == newID {
		a.devicesMu.Unlock()
		return nil
	}

	// Build old target while holding devicesMu
	var oldTarget playbackTarget
	if oldID == "" || oldID == "local" {
		oldTarget = a.mpv
	} else if at, ok := a.agentTargets[oldID]; ok && at.isRunning() {
		oldTarget = at
	} else if wt, ok := a.webTargets[oldID]; ok && wt.isRunning() {
		oldTarget = wt
	} else {
		oldTarget = a.mpv
	}

	// Build new target
	var newTarget playbackTarget
	if newDev.IsLocal {
		newTarget = a.mpv
	} else if at, ok := a.agentTargets[newDev.ID]; ok && at.isRunning() {
		newTarget = at
	} else if wt, ok := a.webTargets[newDev.ID]; ok && wt.isRunning() {
		newTarget = wt
	} else {
		a.devicesMu.Unlock()
		return fmt.Errorf("device %s not connected", newDev.ID)
	}

	a.devicesMu.Unlock()

	// Capture state from old target
	var timePos float64
	var wasPaused bool

	if tpRaw, err := oldTarget.getProperty("time-pos"); err == nil {
		if f, ok := tpRaw.(float64); ok {
			timePos = f
		}
	}
	if pauseRaw, err := oldTarget.getProperty("pause"); err == nil {
		if p, ok := pauseRaw.(bool); ok {
			wasPaused = p
		}
	}

	// Pause old target and clear its playlist
	_ = oldTarget.setProperty("pause", true)
	_ = oldTarget.playlistClear()

	// Update active device before syncing so target() returns the new one
	a.devicesMu.Lock()
	a.activeDevice = newID
	a.devicesMu.Unlock()
	_ = os.WriteFile(a.paths.ActiveDeviceFile, []byte(newID), 0o644)

	// Load 2-track window into new target
	a.playQueueMu.Lock()
	qLen := len(a.playQueue)
	a.syncTarget()
	a.playQueueMu.Unlock()

	if qLen > 0 {
		// Wait for track to load before seeking
		deadline := time.Now().Add(5 * time.Second)
		for time.Now().Before(deadline) {
			if v, err := newTarget.getProperty("duration"); err == nil {
				if d, ok := v.(float64); ok && d > 0 {
					break
				}
			}
			time.Sleep(50 * time.Millisecond)
		}
		if timePos > 0 {
			_ = newTarget.setProperty("time-pos", timePos)
		}
		if !wasPaused {
			_ = newTarget.setProperty("pause", false)
		}
	}

	a.logger.Printf("active device switched: %s -> %s", oldID, newID)
	a.mpdHub.notify(SubOutput, SubPlayer)
	return nil
}


// reloadQueueIntoAgent loads the 2-track window into a reconnected agent.
// Called when an agent re-registers and was already the active device.
func (a *app) reloadQueueIntoAgent(at *agentTarget, dev *device) {
	a.playQueueMu.Lock()
	qLen := len(a.playQueue)
	if qLen == 0 {
		a.playQueueMu.Unlock()
		return
	}
	a.syncTarget()
	a.playQueueMu.Unlock()
	a.logger.Printf("agent reload: loaded 2-track window into %s at pos %d", dev.Name, a.curQueuePos)
}

func (a *app) deviceCleanup() {
	// Agent connections are managed by handleAgentRegister — cleanup on disconnect
	// is automatic. This goroutine is kept for future non-agent device types.
	select {}
}


// ---------------------------------------------------------------------------
// Generic helpers
// ---------------------------------------------------------------------------

func matchesAll(text string, terms []string) bool {
	for _, term := range terms {
		if !strings.Contains(text, term) {
			return false
		}
	}
	return true
}

func stringSlice(value any) []string {
	switch v := value.(type) {
	case string:
		if strings.TrimSpace(v) == "" {
			return nil
		}
		return []string{v}
	case []string:
		return v
	case []any:
		out := make([]string, 0, len(v))
		for _, item := range v {
			if s := stringify(item); s != "" {
				out = append(out, s)
			}
		}
		return out
	default:
		return nil
	}
}


func stringify(value any) string {
	return shared.Stringify(value)
}

// outboundIP returns the preferred outbound IP of this machine.
func outboundIP() string {
	conn, err := net.Dial("udp", "192.0.2.1:80") // doesn't actually send anything
	if err != nil {
		return "127.0.0.1"
	}
	defer conn.Close()
	addr := conn.LocalAddr().(*net.UDPAddr)
	return addr.IP.String()
}

func getenvDefault(key, fallback string) string {
	return shared.Getenv(key, fallback)
}

func intFromAny(value any, fallback int) int {
	return shared.IntFromAny(value, fallback)
}

func boolFromAny(value any, fallback bool) bool {
	return shared.BoolFromAny(value, fallback)
}

func splitAndTrim(value, sep string) []string {
	parts := strings.Split(value, sep)
	out := make([]string, 0, len(parts))
	for _, part := range parts {
		part = strings.TrimSpace(part)
		if part != "" {
			out = append(out, part)
		}
	}
	return out
}

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		if value != "" {
			return value
		}
	}
	return ""
}
