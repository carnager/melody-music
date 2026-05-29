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
	"github.com/coder/websocket"
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
		MusicDir    string `toml:"music_dir"`
		EmbedLyrics bool   `toml:"embed_lyrics"`
		SaveLRC     bool   `toml:"save_lrc"`
	} `toml:"library"`
	Player struct {
		ReplayGain string `toml:"replaygain"` // "off", "track", "album"
	} `toml:"player"`
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
	isRunning() bool
}

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
	cfg    config
	paths  paths
	logger *log.Logger
	db     *musicDB
	scanner *scanner
	// playQueue tracks song IDs (SQLite track IDs as strings) in current mpv playlist order
	playQueue      []string
	playQueueMu    sync.Mutex
	queueVersion   int   // incremented on every queue change, used by MPD plchanges
	queueIDs       []int // parallel to playQueue, unique MPD songid per entry
	queuePriority  []int // parallel to playQueue, 0=normal, 10=low, 20=medium, 30=high
	queueIDCounter int   // monotonically incrementing counter for MPD songids
	// playback state
	curQueuePos    int // authoritative current position in playQueue
	pendingNextPos int // queue position preloaded at target slot 1 (-1 if none)
	prioReturnPos  int // position to resume after priority tracks are consumed (-1 = none)
	// shuffle state for random mode
	shuffleOrder []int // permutation of queue indices, walked sequentially
	shufflePos   int   // current position within shuffleOrder
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
		cfg:           cfg,
		paths:         pathCfg,
		logger:        logger,
		db:            db,
		scanner:       newScanner(cfg.Library.MusicDir, db, logger, pathCfg.TranscodeCacheDir),
		playQueue:     []string{},
		devices:       make(map[string]*device),
		agentTargets:  make(map[string]*agentTarget),
		webTargets:    make(map[string]*webTarget),
		prioReturnPos: -1,
		mpdHub:        newNotifyHub(),
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

	go a.startLocalAgent()
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
	playerSection, _ := raw["player"].(map[string]any)
	random, _ := raw["random"].(map[string]any)
	cfg.Server.Name = stringify(server["name"])
	cfg.Server.BindToAddress = stringSlice(server["bind_to_address"])
	cfg.Server.APISecret = stringify(server["api_secret"])
	cfg.Server.BaseURL = stringify(server["base_url"])
	cfg.Server.WebSecret = stringify(server["web_secret"])
	cfg.Library.MusicDir = stringify(library["music_dir"])
	cfg.Library.EmbedLyrics = boolFromAny(library["embed_lyrics"], false)
	cfg.Library.SaveLRC = boolFromAny(library["save_lrc"], false)
	cfg.Player.ReplayGain = stringify(playerSection["replaygain"])
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

[player]
replaygain = ""

[random]
tracks = 20

`
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
	mux.Handle("GET /mpd", a.authMiddleware(http.HandlerFunc(a.handleMPDWebSocket)))

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

// handleMPDWebSocket upgrades to WebSocket and handles the MPD protocol.
// Uses github.com/coder/websocket which properly handles ping/pong frames,
// keeping connections alive through NAT and mobile network proxies.
func (a *app) handleMPDWebSocket(w http.ResponseWriter, r *http.Request) {
	ws, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		InsecureSkipVerify: true, // allow any origin
	})
	if err != nil {
		a.logger.Printf("websocket accept: %v", err)
		return
	}
	defer ws.CloseNow()

	// NetConn gives us a net.Conn that handles ping/pong automatically
	// in the background. This lets mpdConn use it like any TCP connection.
	ctx := r.Context()
	conn := websocket.NetConn(ctx, ws, websocket.MessageText)

	c := &mpdConn{
		conn:   conn,
		reader: bufio.NewReader(conn),
		writer: bufio.NewWriter(conn),
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

// streamURL returns a URL or path for the given track.
// For local agents, returns the file path. For remote, returns HTTP URL.
func (a *app) streamURL(songID string) string {
	id, err := strconv.ParseInt(songID, 10, 64)
	if err != nil {
		return ""
	}
	path, err := a.db.trackPathByID(id)
	if err != nil {
		return ""
	}
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
	if dev == nil {
		// No active device — return file path as fallback
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
	if dev.IsLocal {
		// Local agent: return file path
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


// generateShuffle creates a shuffled order for the queue.
// The current track is placed at shufflePos (position 0), and only the
// remaining (unplayed) tracks after it are shuffled — matching MPD behavior
// when random is toggled on mid-playback.
// Must be called with playQueueMu held.
func (a *app) generateShuffle() {
	qLen := len(a.playQueue)
	if qLen == 0 {
		a.shuffleOrder = nil
		a.shufflePos = 0
		return
	}
	// Build list of all indices except current and prioritized tracks
	remaining := make([]int, 0, qLen-1)
	for i := 0; i < qLen; i++ {
		if i == a.curQueuePos {
			continue
		}
		if i < len(a.queuePriority) && a.queuePriority[i] > 0 {
			continue // prioritized tracks are played via priority override, not shuffle
		}
		remaining = append(remaining, i)
	}
	// Fisher-Yates shuffle the remaining
	for i := len(remaining) - 1; i > 0; i-- {
		j := rand.Intn(i + 1)
		remaining[i], remaining[j] = remaining[j], remaining[i]
	}
	// Current track first, then shuffled rest
	a.shuffleOrder = make([]int, 0, qLen)
	a.shuffleOrder = append(a.shuffleOrder, a.curQueuePos)
	a.shuffleOrder = append(a.shuffleOrder, remaining...)
	a.shufflePos = 0
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

	// Priority override: find highest-priority track (not current)
	bestPos := -1
	bestPrio := 0
	for i := 0; i < qLen; i++ {
		if i == a.curQueuePos {
			continue
		}
		if i < len(a.queuePriority) && a.queuePriority[i] > bestPrio {
			bestPrio = a.queuePriority[i]
			bestPos = i
		}
	}
	if bestPos >= 0 {
		return bestPos
	}

	// No priority tracks left — return to saved position if we were in priority mode
	if a.prioReturnPos >= 0 {
		ret := a.prioReturnPos
		// Adjust for queue bounds (tracks may have been removed)
		if ret >= qLen {
			ret = qLen - 1
		}
		if ret < 0 {
			return -1
		}
		// Next track after the saved position
		next := ret + 1
		if next >= qLen {
			if a.modeRepeat {
				return 0
			}
			return -1
		}
		return next
	}

	if a.modeRandom && qLen > 1 {
		next := a.shufflePos + 1
		if next >= len(a.shuffleOrder) {
			if a.modeRepeat {
				// Reshuffle for next pass
				a.generateShuffle()
				// Skip index 0 since that's the track we just finished
				if len(a.shuffleOrder) > 1 {
					return a.shuffleOrder[1]
				}
				return a.shuffleOrder[0]
			}
			return -1 // played all tracks
		}
		return a.shuffleOrder[next]
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

// ---------------------------------------------------------------------------
// Sync plan types — separate state computation (under lock) from IPC execution
// ---------------------------------------------------------------------------

// syncPlan describes IPC operations to load tracks into the playback target.
type syncPlan struct {
	doClear    bool
	currentURL string
	nextURL    string
	// for logging
	curPos     int
	curSongID  string
	nextPos    int
	nextSongID string
}

// nextTrackPlan describes IPC operations to update the preloaded next track.
type nextTrackPlan struct {
	removeOld bool
	nextURL   string
	nextPos   int // queue position for agent targets
}

// planSyncTarget computes what IPC calls are needed.
// Must be called with playQueueMu held.
func (a *app) planSyncTarget() syncPlan {
	qLen := len(a.playQueue)

	if qLen == 0 || a.curQueuePos < 0 || a.curQueuePos >= qLen {
		a.pendingNextPos = -1
		return syncPlan{doClear: true}
	}

	plan := syncPlan{
		doClear:   true,
		curPos:    a.curQueuePos,
		curSongID: a.playQueue[a.curQueuePos],
	}
	plan.currentURL = a.streamURLForActiveDevice(plan.curSongID)

	a.pendingNextPos = a.nextQueuePos()
	if a.pendingNextPos >= 0 && a.pendingNextPos < qLen {
		plan.nextPos = a.pendingNextPos
		plan.nextSongID = a.playQueue[a.pendingNextPos]
		plan.nextURL = a.streamURLForActiveDevice(plan.nextSongID)
	}

	return plan
}

// execSyncPlan executes the IPC calls described by the plan.
// Must NOT be called with playQueueMu held.
func (a *app) execSyncPlan(plan syncPlan) {
	t := a.target()

	// Agent targets use position-based commands
	if at, ok := t.(*agentTarget); ok {
		if plan.currentURL == "" {
			_ = at.playlistClear()
			return
		}
		a.logger.Printf("syncTarget(agent): play pos %d next=%d", plan.curPos, plan.nextPos)
		if err := at.agentPlay(plan.curPos, plan.nextPos); err != nil {
			a.logger.Printf("syncTarget(agent): play failed: %v", err)
		}
		return
	}

	if plan.doClear {
		_ = t.playlistClear()
	}
	if plan.currentURL == "" {
		return
	}

	a.logger.Printf("syncTarget: loading pos %d (songID=%s)", plan.curPos, plan.curSongID)
	if err := t.loadFile(plan.currentURL, "replace", nil); err != nil {
		a.logger.Printf("syncTarget: loadFile replace failed: %v", err)
	}

	if plan.nextURL != "" {
		a.logger.Printf("syncTarget: preloading pos %d (songID=%s)", plan.nextPos, plan.nextSongID)
		_ = t.loadFile(plan.nextURL, "append", nil)
	}
}

// planNextTrack computes IPC operations to update the preloaded next track.
// Must be called with playQueueMu held.
func (a *app) planNextTrack() nextTrackPlan {
	qLen := len(a.playQueue)

	if qLen == 0 || a.curQueuePos < 0 || a.curQueuePos >= qLen {
		return nextTrackPlan{nextPos: -1}
	}

	plan := nextTrackPlan{
		removeOld: a.pendingNextPos >= 0,
		nextPos:   -1,
	}

	a.pendingNextPos = a.nextQueuePos()
	if a.pendingNextPos >= 0 && a.pendingNextPos < qLen {
		plan.nextURL = a.streamURLForActiveDevice(a.playQueue[a.pendingNextPos])
		plan.nextPos = a.pendingNextPos
	}

	return plan
}

// execNextTrackPlan executes the next-track preload IPC.
// Must NOT be called with playQueueMu held.
func (a *app) execNextTrackPlan(plan nextTrackPlan) {
	t := a.target()

	// Agent targets use position-based preload
	if at, ok := t.(*agentTarget); ok {
		if plan.nextPos >= 0 {
			if err := at.agentPreload(plan.nextPos); err != nil {
				a.logger.Printf("execNextTrackPlan(agent): preload %d failed: %v", plan.nextPos, err)
			}
		}
		return
	}

	if plan.removeOld {
		_ = t.playlistRemove(1)
	}
	if plan.nextURL != "" {
		_ = t.loadFile(plan.nextURL, "append", nil)
	}
}

// advanceTrack is called when a track naturally ends (target moved from slot 0 to 1,
// or web/Android sent trackended). It applies playback modes and loads the next pair.
// IMPORTANT: at this point the preloaded track at slot 1 is already playing in mpv.
// We must NOT call syncTarget (which would clear+reload and restart the track).
// Instead: remove finished slot 0, let playing track slide to slot 0, append new slot 1.
//
// The lock is released before IPC calls to avoid blocking clients querying status.
func (a *app) advanceTrack() {
	a.playQueueMu.Lock()

	qLen := len(a.playQueue)
	if qLen == 0 {
		a.playQueueMu.Unlock()
		return
	}

	// Single mode: stop or repeat the current track
	if a.modeSingle {
		plan := a.planSyncTarget()
		paused := !a.modeRepeat
		a.playQueueMu.Unlock()
		a.execSyncPlan(plan)
		if paused {
			_ = a.target().setProperty("pause", true)
		}
		a.mpdHub.notify(SubPlayer)
		return
	}

	// Auto-consume prioritized tracks: if the track that just finished had priority > 0, remove it
	isPrioConsume := a.curQueuePos >= 0 && a.curQueuePos < len(a.queuePriority) && a.queuePriority[a.curQueuePos] > 0

	// Consume mode or priority auto-consume: remove the track that just finished
	if (a.modeConsume || isPrioConsume) && a.curQueuePos >= 0 && a.curQueuePos < qLen {
		// Adjust prioReturnPos for the removal
		if isPrioConsume && a.prioReturnPos > a.curQueuePos {
			a.prioReturnPos--
		}
		a.playQueue = append(a.playQueue[:a.curQueuePos], a.playQueue[a.curQueuePos+1:]...)
		a.queueIDs = append(a.queueIDs[:a.curQueuePos], a.queueIDs[a.curQueuePos+1:]...)
		if a.curQueuePos < len(a.queuePriority) {
			a.queuePriority = append(a.queuePriority[:a.curQueuePos], a.queuePriority[a.curQueuePos+1:]...)
		}
		a.queueVersion++
		a.savePlayQueue()
		qLen = len(a.playQueue)

		if qLen == 0 {
			a.curQueuePos = 0
			a.pendingNextPos = -1
			a.playQueueMu.Unlock()
			_ = a.target().playlistClear()
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
			if a.modeRandom {
				a.shufflePos++
			}
		} else {
			a.curQueuePos = 0
			a.pendingNextPos = -1
			a.playQueueMu.Unlock()
			_ = a.target().playlistClear()
			a.mpdHub.notify(SubPlaylist, SubPlayer)
			return
		}

		// Compute next preload under lock
		a.pendingNextPos = a.nextQueuePos()
		nextPreloadPos := a.pendingNextPos
		var nextURL string
		if a.pendingNextPos >= 0 && a.pendingNextPos < qLen {
			nextURL = a.streamURLForActiveDevice(a.playQueue[a.pendingNextPos])
		}
		a.playQueueMu.Unlock()

		// IPC calls outside lock
		t := a.target()
		if at, ok := t.(*agentTarget); ok {
			// Agent: just tell it about the next track to preload
			if nextPreloadPos >= 0 {
				_ = at.agentPreload(nextPreloadPos)
			}
		} else {
			_ = t.playlistRemove(0)
			if nextURL != "" {
				_ = t.loadFile(nextURL, "append", nil)
			}
		}
		a.mpdHub.notify(SubPlaylist, SubPlayer)
		return
	}

	// Normal advance: the preloaded track at slot 1 is already playing
	if a.pendingNextPos >= 0 && a.pendingNextPos < qLen {
		// Track prioReturnPos when entering/leaving priority mode
		pendingHasPrio := a.pendingNextPos < len(a.queuePriority) && a.queuePriority[a.pendingNextPos] > 0
		if pendingHasPrio && a.prioReturnPos < 0 {
			a.prioReturnPos = a.curQueuePos
		}
		if !pendingHasPrio && a.prioReturnPos >= 0 {
			a.prioReturnPos = -1
		}
		a.curQueuePos = a.pendingNextPos
		if a.modeRandom {
			a.shufflePos++
		}
	} else {
		// No next track was preloaded — end of queue, stop playback
		a.playQueueMu.Unlock()
		_ = a.target().playlistClear()
		a.mpdHub.notify(SubPlayer)
		return
	}

	// Compute next preload under lock
	a.pendingNextPos = a.nextQueuePos()
	nextPreloadPos := a.pendingNextPos
	var nextURL string
	if a.pendingNextPos >= 0 && a.pendingNextPos < qLen {
		nextURL = a.streamURLForActiveDevice(a.playQueue[a.pendingNextPos])
	}
	a.playQueueMu.Unlock()

	// IPC calls outside lock — clients can query status while these run
	t := a.target()
	if at, ok := t.(*agentTarget); ok {
		// Agent: just tell it about the next track to preload
		if nextPreloadPos >= 0 {
			_ = at.agentPreload(nextPreloadPos)
		}
	} else {
		_ = t.playlistRemove(0)
		if nextURL != "" {
			_ = t.loadFile(nextURL, "append", nil)
		}
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
	if pos < len(a.queuePriority) {
		a.queuePriority = append(a.queuePriority[:pos], a.queuePriority[pos+1:]...)
	}
	a.queueVersion++
	a.savePlayQueue()
}

// ---------------------------------------------------------------------------
// Playback target / device helpers
// ---------------------------------------------------------------------------

func (a *app) target() playbackTarget {
	a.devicesMu.RLock()
	defer a.devicesMu.RUnlock()
	devID := a.activeDevice
	if at, ok := a.agentTargets[devID]; ok && at.isRunning() {
		return at
	}
	if wt, ok := a.webTargets[devID]; ok && wt.isRunning() {
		return wt
	}
	return &noopTarget{}
}

// noopTarget is returned when no playback device is available.
type noopTarget struct{}

func (noopTarget) loadFile(string, string, map[string]any) error { return nil }
func (noopTarget) loadFileBatch([]string, string) error          { return nil }
func (noopTarget) playlistClear() error                          { return nil }
func (noopTarget) playlistRemove(int) error                      { return nil }
func (noopTarget) playlistMove(int, int) error                   { return nil }
func (noopTarget) getProperty(string) (any, error)               { return nil, fmt.Errorf("no device") }
func (noopTarget) setProperty(string, any) error                 { return nil }
func (noopTarget) isRunning() bool                               { return false }

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
	return a.addSongsWithPriority(songIDs, mode, 0)
}

func (a *app) addSongsWithPriority(songIDs []string, mode string, priority int) error {
	if len(songIDs) == 0 {
		return nil
	}

	a.playQueueMu.Lock()

	// Build priority slice for the new tracks
	prios := make([]int, len(songIDs))
	for i := range prios {
		prios[i] = priority
	}

	switch mode {
	case "replace":
		a.playQueue = nil
		a.queueIDs = nil
		a.queuePriority = nil
		a.playQueue = append(a.playQueue, songIDs...)
		a.queuePriority = append(a.queuePriority, prios...)
		for range songIDs {
			a.queueIDCounter++
			a.queueIDs = append(a.queueIDs, a.queueIDCounter)
		}
		a.queueVersion++
		a.curQueuePos = 0
		if a.modeRandom {
			a.generateShuffle()
		}
		a.savePlayQueue()
		plan := a.planSyncTarget()
		a.playQueueMu.Unlock()
		a.execSyncPlan(plan)
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
		newPrios := make([]int, 0, len(a.queuePriority)+len(prios))
		newPrios = append(newPrios, a.queuePriority[:pos]...)
		newPrios = append(newPrios, prios...)
		if pos < len(a.queuePriority) {
			newPrios = append(newPrios, a.queuePriority[pos:]...)
		}
		a.queuePriority = newPrios
		a.queueVersion++
		a.savePlayQueue()
		// Resync preloaded next track since insert may change it
		ntPlan := a.planNextTrack()
		a.playQueueMu.Unlock()
		a.execNextTrackPlan(ntPlan)
		return a.target().setProperty("pause", false)

	default: // "add"
		wasEmpty := len(a.playQueue) == 0
		oldNextPos := a.pendingNextPos
		a.playQueue = append(a.playQueue, songIDs...)
		a.queuePriority = append(a.queuePriority, prios...)
		for range songIDs {
			a.queueIDCounter++
			a.queueIDs = append(a.queueIDs, a.queueIDCounter)
		}
		a.queueVersion++
		a.savePlayQueue()
		if wasEmpty {
			a.curQueuePos = 0
			plan := a.planSyncTarget()
			a.playQueueMu.Unlock()
			a.execSyncPlan(plan)
		} else {
			// Only resync preload if the next track actually changed
			// (e.g. priority tracks were added). Appending to the end
			// of the queue never changes the next track.
			ntPlan := a.planNextTrack()
			a.playQueueMu.Unlock()
			if ntPlan.nextPos != oldNextPos {
				a.execNextTrackPlan(ntPlan)
			}
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

// savedQueue is the on-disk format for the play queue.
type savedQueue struct {
	Songs      []string `json:"songs"`
	Priorities []int    `json:"priorities,omitempty"`
}

// savePlayQueue persists the current play queue to disk (caller must hold playQueueMu or be safe).
func (a *app) savePlayQueue() {
	sq := savedQueue{Songs: a.playQueue, Priorities: a.queuePriority}
	data, _ := json.Marshal(sq)
	_ = os.WriteFile(a.paths.PlayQueueFile, data, 0o644)
}

// restorePlayQueue loads the saved play queue from disk and reloads into
// the local mpv target so that clients see a consistent queue on reconnect.
func (a *app) restorePlayQueue() {
	data, err := os.ReadFile(a.paths.PlayQueueFile)
	if err != nil {
		return
	}
	// Try new format first
	var sq savedQueue
	if json.Unmarshal(data, &sq) == nil && len(sq.Songs) > 0 {
		a.playQueue = sq.Songs
		a.queuePriority = sq.Priorities
		if len(a.queuePriority) < len(a.playQueue) {
			a.queuePriority = append(a.queuePriority, make([]int, len(a.playQueue)-len(a.queuePriority))...)
		}
	} else {
		// Fall back to old format (bare string array)
		var queue []string
		if json.Unmarshal(data, &queue) != nil || len(queue) == 0 {
			return
		}
		a.playQueue = queue
		a.queuePriority = make([]int, len(queue))
	}
	a.logger.Printf("restored play queue: %d tracks", len(a.playQueue))

	go a.reloadQueueIntoTarget()
}

// reloadQueueIntoTarget loads the 2-track window into the current target (paused).
func (a *app) reloadQueueIntoTarget() {
	// Wait for a playback target to be ready
	for i := 0; i < 30; i++ {
		if t := a.target(); t != nil && t.isRunning() {
			break
		}
		time.Sleep(500 * time.Millisecond)
	}
	if t := a.target(); t == nil || !t.isRunning() {
		a.logger.Printf("restore: no target ready, skipping queue reload")
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
	plan := a.planSyncTarget()
	a.playQueueMu.Unlock()
	a.execSyncPlan(plan)

	_ = a.target().setProperty("pause", true)
	a.logger.Printf("restored 2-track window at pos %d (paused)", a.curQueuePos)

	a.restorePlayState()
}

// playState represents saved playback position for resume across restarts.
type playState struct {
	SongPos    int     `json:"song_pos"`
	TimePos    float64 `json:"time_pos"`
	Playing    bool    `json:"playing"`
	Repeat     bool    `json:"repeat"`
	Random     bool    `json:"random"`
	Single     bool    `json:"single"`
	Consume    bool    `json:"consume"`
	ReplayGain string  `json:"replaygain,omitempty"`
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
	ps.ReplayGain = a.cfg.Player.ReplayGain
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
	if ps.ReplayGain != "" {
		a.cfg.Player.ReplayGain = ps.ReplayGain
	}
	if a.modeRandom {
		a.generateShuffle()
	}
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
func (a *app) switchDevice(newID string) error {
	a.devicesMu.Lock()
	_, exists := a.devices[newID]
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
	if at, ok := a.agentTargets[oldID]; ok && at.isRunning() {
		oldTarget = at
	} else if wt, ok := a.webTargets[oldID]; ok && wt.isRunning() {
		oldTarget = wt
	}

	// Build new target
	var newTarget playbackTarget
	if at, ok := a.agentTargets[newID]; ok && at.isRunning() {
		newTarget = at
	} else if wt, ok := a.webTargets[newID]; ok && wt.isRunning() {
		newTarget = wt
	} else {
		a.devicesMu.Unlock()
		return fmt.Errorf("device %s not connected", newID)
	}

	a.devicesMu.Unlock()

	// Capture state from old target
	var timePos float64
	var volume float64 = -1
	var wasPaused bool

	if oldTarget != nil {
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
		if volRaw, err := oldTarget.getProperty("volume"); err == nil {
			if f, ok := volRaw.(float64); ok {
				volume = f
			}
		}

		// Stop old target
		_ = oldTarget.setProperty("pause", true)
		_ = oldTarget.playlistClear()
	}

	// Update active device before syncing so target() returns the new one
	a.devicesMu.Lock()
	a.activeDevice = newID
	a.devicesMu.Unlock()
	_ = os.WriteFile(a.paths.ActiveDeviceFile, []byte(newID), 0o644)

	// Transfer volume and replaygain to new target
	if volume >= 0 {
		_ = newTarget.setProperty("volume", volume)
	}
	if a.cfg.Player.ReplayGain != "" {
		_ = newTarget.setProperty("replaygain", a.cfg.Player.ReplayGain)
	}

	// Load 2-track window into new target with seek position
	a.playQueueMu.Lock()
	qLen := len(a.playQueue)
	plan := a.planSyncTarget()
	a.playQueueMu.Unlock()

	if qLen > 0 {
		// For agent targets, use agentPlayAt to seek atomically before audio starts
		if at, ok := newTarget.(*agentTarget); ok {
			at.ensureQueueSync()
			if err := at.agentPlayAt(plan.curPos, plan.nextPos, timePos); err != nil {
				a.logger.Printf("switchDevice: agentPlayAt failed: %v", err)
			}
			if wasPaused {
				_ = newTarget.setProperty("pause", true)
			}
		} else {
			a.execSyncPlan(plan)
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
	}

	a.logger.Printf("active device switched: %s -> %s", oldID, newID)
	a.mpdHub.notify(SubOutput, SubPlayer, SubMixer)
	return nil
}


// reloadQueueIntoAgent loads the 2-track window into a reconnected agent.
// Called when an agent re-registers and was already the active device.
func (a *app) reloadQueueIntoAgent(at *agentTarget, dev *device) {
	// Apply replaygain setting
	if a.cfg.Player.ReplayGain != "" {
		_ = at.setProperty("replaygain", a.cfg.Player.ReplayGain)
	}

	a.playQueueMu.Lock()
	qLen := len(a.playQueue)
	if qLen == 0 {
		a.playQueueMu.Unlock()
		return
	}
	plan := a.planSyncTarget()
	a.playQueueMu.Unlock()
	a.execSyncPlan(plan)
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
