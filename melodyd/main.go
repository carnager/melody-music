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
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/BurntSushi/toml"
	"github.com/carnager/melody/internal/shared"
	"github.com/coder/websocket"
)

// melodyVersion is the melodyd release version reported by the
// `melody_version` MPD command. Bump it as part of the release flow
// (see RELEASE.md).
const melodyVersion = "1.4.0-dev"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

type config struct {
	Server struct {
		Name          string   `toml:"name"` // display name for the local output device
		BindToAddress []string `toml:"bind_to_address"`
		BaseURL       string   `toml:"base_url"` // externally reachable URL for stream URLs sent to remote devices
		WebSecret     string   `toml:"web_secret"`
	} `toml:"server"`
	Library struct {
		MusicDir       string   `toml:"music_dir"`
		RequiredMounts []string `toml:"required_mounts"`
		EmbedLyrics    bool     `toml:"embed_lyrics"`
		SaveLRC        bool     `toml:"save_lrc"`
	} `toml:"library"`
	Player struct {
		ReplayGain string  `toml:"replaygain"` // "off", "track", "album"
		Volume     float64 `toml:"volume"`
		MPVPath    string  `toml:"mpv_path"`
		MPVSocket  string  `toml:"mpv_socket"`
	} `toml:"player"`
	Random struct {
		Tracks int `toml:"tracks"`
	} `toml:"random"`
	MPD struct {
		Port int `toml:"port"`
	} `toml:"mpd"`
	Transcode struct {
		CacheMaxMB int `toml:"cache_max_mb"` // soft cap on the transcode cache; 0 = unlimited
	} `toml:"transcode"`
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

// stoppedTarget is an optional playbackTarget extension reporting whether the
// output has nothing loaded — "stop" in MPD terms, as opposed to paused.
type stoppedTarget interface {
	isStopped() bool
}

// targetStopped reports whether t has nothing loaded. Targets that don't
// implement stoppedTarget are assumed to have content loaded.
func targetStopped(t playbackTarget) bool {
	if st, ok := t.(stoppedTarget); ok {
		return st.isStopped()
	}
	return false
}

// transportState captures the primary output's transport so queue mutations
// can preserve it. Must NOT be called with playQueueMu held.
func (a *app) transportState() (stopped, paused bool) {
	t := a.target()
	if !t.isRunning() || targetStopped(t) {
		return true, false
	}
	if pRaw, err := t.getProperty("pause"); err == nil {
		if p, ok := pRaw.(bool); ok {
			paused = p
		}
	}
	return false, paused
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
	MaxBitRate int       `json:"max_bitrate"`
	ReplayGain string    `json:"replaygain,omitempty"` // "off", "track", "album"
	LastSeen   time.Time `json:"last_seen"`
}

// ---------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------

type app struct {
	cfg     config
	paths   paths
	logger  *log.Logger
	db      *musicDB
	scanner *scanner
	// playQueue tracks song IDs (SQLite track IDs as strings) in current mpv playlist order
	playQueue      []string
	playQueueMu    sync.Mutex
	queueVersion   int   // incremented on every queue change, used by MPD plchanges
	queueIDs       []int // parallel to playQueue, unique MPD songid per entry
	queuePriority  []int // parallel to playQueue, 0=normal, 10=low, 20=medium, 30=high
	queueIDCounter int   // monotonically incrementing counter for MPD songids
	// playback state
	curQueuePos    int          // authoritative current position in playQueue
	pendingNextPos int          // queue position preloaded at target slot 1 (-1 if none)
	prioReturnPos  int          // position to resume after priority tracks have played (-1 = none)
	prioPlayedIDs  map[int]bool // song IDs whose spent priority already played this cycle
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
	// enabledOutputs is the set of device IDs enabled for playback (MPD-style:
	// several may be enabled and play simultaneously, best-effort synced).
	// It is configuration, not connection state: an agent going offline never
	// removes it from the set, so the output comes back enabled when the agent
	// reconnects. Only an explicit disable takes an entry out.
	enabledOutputs map[string]bool
	// agentResumes holds the last known playback position of agents that went
	// offline while enabled, so a reconnect can pick up mid-track instead of
	// restarting it. Keyed by device ID, guarded by devicesMu.
	agentResumes map[string]*agentResume
	// primaryOutput is the enabled device that drives the playback clock and
	// queue advancement. "" = none (no output enabled/connected).
	primaryOutput string
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
		cfg:            cfg,
		paths:          pathCfg,
		logger:         logger,
		db:             db,
		scanner:        newScanner(cfg.Library.MusicDir, db, logger, pathCfg.TranscodeCacheDir),
		playQueue:      []string{},
		devices:        make(map[string]*device),
		agentTargets:   make(map[string]*agentTarget),
		webTargets:     make(map[string]*webTarget),
		enabledOutputs: make(map[string]bool),
		agentResumes:   make(map[string]*agentResume),
		prioReturnPos:  -1,
		mpdHub:         newNotifyHub(),
	}
	a.scanner.requiredMounts = cfg.Library.RequiredMounts
	a.logConfigWarnings()

	a.scanner.onScanComplete = func(fullRebuild bool) {
		db.invalidateCache()
		if fullRebuild {
			// Full scans can add/remove arbitrary rows, so rebuild the FTS index
			// and pre-warm caches. Targeted updates already maintain the FTS
			// per-track (upsertTrack / removeTracksUnderPrefixNotIn), so they skip
			// the expensive full rebuild and let caches rebuild lazily — keeping
			// each watcher-driven album update sub-second instead of re-indexing
			// the whole 66k-row library.
			if err := db.rebuildFTS(); err != nil {
				logger.Printf("warning: FTS rebuild after scan failed: %v", err)
			}
			db.warmCache()
		}
		a.mpdHub.notify(SubDatabase)
	}

	// Pre-warm expensive query caches at startup
	db.warmCache()

	a.restorePlayQueue()
	a.restoreOutputs()

	// Assign MPD queue IDs for restored queue
	a.playQueueMu.Lock()
	for range a.playQueue {
		a.queueIDCounter++
		a.queueIDs = append(a.queueIDs, a.queueIDCounter)
	}
	if len(a.playQueue) > 0 {
		a.ensureQueueVersionLocked()
	}
	a.playQueueMu.Unlock()

	// Initial library scan
	go func() {
		if err := a.scanner.fullScan(); err != nil {
			logger.Printf("initial scan error: %v", err)
		}
	}()
	// Trim the transcode cache to its size cap on startup.
	go a.maybeEvictTranscodeCache()
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
	cfg.Server.BaseURL = stringify(server["base_url"])
	cfg.Server.WebSecret = stringify(server["web_secret"])
	cfg.Library.MusicDir = stringify(library["music_dir"])
	cfg.Library.RequiredMounts = stringSlice(library["required_mounts"])
	cfg.Library.EmbedLyrics = boolFromAny(library["embed_lyrics"], false)
	cfg.Library.SaveLRC = boolFromAny(library["save_lrc"], false)
	cfg.Player.ReplayGain = stringify(playerSection["replaygain"])
	cfg.Player.Volume = floatFromAny(playerSection["volume"], 100)
	cfg.Player.MPVPath = stringify(playerSection["mpv_path"])
	cfg.Player.MPVSocket = stringify(playerSection["mpv_socket"])
	cfg.Random.Tracks = intFromAny(random["tracks"], 20)
	mpdSection, _ := raw["mpd"].(map[string]any)
	cfg.MPD.Port = intFromAny(mpdSection["port"], 6600)
	transcodeSection, _ := raw["transcode"].(map[string]any)
	cfg.Transcode.CacheMaxMB = intFromAny(transcodeSection["cache_max_mb"], 5120) // 5 GB default
	applyDefaults(&cfg)
	return cfg, pathCfg, nil
}

func defaultDaemonConfig() string {
	return `[server]
name = ""
bind_to_address = ["0.0.0.0:6701", "` + shared.DefaultSocketPath() + `"]
base_url = ""
web_secret = ""

[library]
music_dir = ""
required_mounts = []
embed_lyrics = false
save_lrc = false

[player]
replaygain = ""
volume = 100
mpv_path = "mpv"
mpv_socket = ""

[random]
tracks = 20

[mpd]
port = 6600

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
	if cfg.Player.Volume == 0 {
		cfg.Player.Volume = 100
	}
	if cfg.Player.MPVPath == "" {
		cfg.Player.MPVPath = "mpv"
	}
	if envBind := os.Getenv("MELODYD_BIND_TO_ADDRESS"); envBind != "" {
		cfg.Server.BindToAddress = splitAndTrim(envBind, ",")
	}
	if len(cfg.Server.BindToAddress) == 0 {
		cfg.Server.BindToAddress = defaultBindToAddress()
	}
}

func (a *app) logConfigWarnings() {
	if strings.TrimSpace(a.cfg.Server.WebSecret) == "" {
		a.logger.Printf("warning: server.web_secret is empty; /mpd, streams, cover art, and web API are unauthenticated")
	}

	if strings.TrimSpace(a.cfg.Server.BaseURL) == "" {
		fallback := fallbackStreamBaseURL(a.cfg.Server.BindToAddress)
		if fallback == "" {
			fallback = "http://127.0.0.1:6701"
		}
		a.logger.Printf("warning: server.base_url is empty; remote stream URLs will fall back to %s (set base_url for VPN/reverse proxy clients)", fallback)
		return
	}

	u, err := url.Parse(a.cfg.Server.BaseURL)
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
		a.logger.Printf("warning: server.base_url %q is not an absolute http(s) URL", a.cfg.Server.BaseURL)
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

// streamURLFor resolves what the given device should be told to play:
// a file path for the local device (or nil), a per-device transcode HTTP URL
// for remote devices.
func (a *app) streamURLFor(dev *device, songID string) string {
	id, err := strconv.ParseInt(songID, 10, 64)
	if err != nil {
		return ""
	}
	path, err := a.db.trackPathByID(id)
	if err != nil {
		return ""
	}
	if dev == nil || dev.IsLocal {
		return path
	}
	return a.buildStreamURL(songID, dev.Format, dev.MaxBitRate)
}

// buildStreamURL constructs an HTTP URL to the server's stream endpoint.
func (a *app) buildStreamURL(songID, format string, maxBitRate int) string {
	baseURL := strings.TrimRight(a.cfg.Server.BaseURL, "/")
	if baseURL == "" {
		baseURL = fallbackStreamBaseURL(a.cfg.Server.BindToAddress)
	}
	if baseURL == "" {
		baseURL = "http://127.0.0.1:6701"
	}
	u := baseURL + "/api/v1/stream/" + songID
	params := url.Values{}
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

func fallbackStreamBaseURL(bindAddresses []string) string {
	for _, bind := range bindAddresses {
		if strings.Contains(bind, "/") {
			continue
		}
		host, port, err := net.SplitHostPort(bind)
		if err != nil {
			continue
		}
		if host == "" || host == "0.0.0.0" || host == "::" {
			host = outboundIP()
		}
		return "http://" + net.JoinHostPort(host, port)
	}
	return ""
}

// savedOutputs is the on-disk format for the enabled-output set.
type savedOutputs struct {
	Enabled []string `json:"enabled"`
	Primary string   `json:"primary"`
}

// persistOutputs writes the enabled-output set and primary to disk.
func (a *app) persistOutputs() {
	a.devicesMu.RLock()
	so := savedOutputs{Primary: a.primaryOutput}
	for id := range a.enabledOutputs {
		so.Enabled = append(so.Enabled, id)
	}
	a.devicesMu.RUnlock()
	sort.Strings(so.Enabled)
	data, _ := json.Marshal(so)
	_ = os.WriteFile(a.paths.ActiveDeviceFile, data, 0o644)
}

// restoreOutputs loads the enabled-output set from disk. Older versions stored
// a single bare device ID; migrate that to a one-element set. Stale web device
// IDs are pruned — browsers register fresh on connect.
func (a *app) restoreOutputs() {
	data, err := os.ReadFile(a.paths.ActiveDeviceFile)
	if err != nil {
		return
	}
	content := strings.TrimSpace(string(data))
	if content == "" {
		return
	}
	var so savedOutputs
	if strings.HasPrefix(content, "{") {
		if json.Unmarshal([]byte(content), &so) != nil {
			return
		}
	} else {
		// Legacy format: single device ID
		so = savedOutputs{Enabled: []string{content}, Primary: content}
	}
	for _, id := range so.Enabled {
		if strings.HasPrefix(id, "web-") {
			continue
		}
		a.enabledOutputs[id] = true
		// List the output as offline until its agent connects, so it can be
		// seen and disabled in the meantime. The agent's own registration
		// replaces this placeholder with the real device record.
		if id != "local" && a.devices[id] == nil {
			a.devices[id] = &device{
				ID:   id,
				Name: strings.TrimPrefix(id, "agent-"),
				Type: "agent",
			}
		}
	}
	if a.enabledOutputs[so.Primary] {
		a.primaryOutput = so.Primary
	}
	if len(a.enabledOutputs) > 0 {
		a.logger.Printf("restored enabled outputs: %v (primary %q)", so.Enabled, a.primaryOutput)
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

// markPrioPlayed remembers that the song with the given queue ID played
// through a spent priority. Sequential advancement skips it until the cycle
// wraps, it is played explicitly, or it is prioritized again.
func (a *app) markPrioPlayed(songID int) {
	if a.prioPlayedIDs == nil {
		a.prioPlayedIDs = make(map[int]bool)
	}
	a.prioPlayedIDs[songID] = true
}

// prioPlayedAt reports whether the track at queue position pos already played
// through a spent priority in the current cycle.
func (a *app) prioPlayedAt(pos int) bool {
	if len(a.prioPlayedIDs) == 0 || pos < 0 || pos >= len(a.queueIDs) {
		return false
	}
	return a.prioPlayedIDs[a.queueIDs[pos]]
}

// nextSequentialPos returns the first position at or after from whose track
// has not already played through a priority jump. A repeat wraparound starts
// a new cycle: the played memory is cleared and every track is eligible again.
func (a *app) nextSequentialPos(from int) int {
	for pos := from; pos < len(a.playQueue); pos++ {
		if !a.prioPlayedAt(pos) {
			return pos
		}
	}
	if a.modeRepeat && len(a.playQueue) > 0 {
		a.prioPlayedIDs = nil
		return 0
	}
	return -1
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
		// Next track after the saved position, skipping tracks that already
		// played through a priority jump this cycle.
		return a.nextSequentialPos(ret + 1)
	}

	if a.modeRandom && qLen > 1 {
		next := a.shufflePos + 1
		if next >= len(a.shuffleOrder) {
			if a.modeRepeat {
				// Reshuffle for next pass — a new cycle, so the priority
				// played-memory is cleared as well
				a.generateShuffle()
				a.prioPlayedIDs = nil
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
	// Sequential advance, skipping tracks that already played through a
	// priority jump this cycle.
	return a.nextSequentialPos(a.curQueuePos + 1)
}

// ---------------------------------------------------------------------------
// Sync plan types — separate state computation (under lock) from IPC execution
// ---------------------------------------------------------------------------

// syncPlan describes IPC operations to load tracks into the playback targets.
// It is device-independent: per-device stream URLs are resolved at exec time.
type syncPlan struct {
	doClear    bool
	hasCurrent bool
	curPos     int
	curSongID  string
	nextPos    int // -1 = none
	nextSongID string
	// startPaused loads the current track without starting audio, preserving
	// a paused transport across track switches. Set by callers after
	// planSyncTarget based on the transport state they observed.
	startPaused bool
}

// nextTrackPlan describes IPC operations to update the preloaded next track.
type nextTrackPlan struct {
	removeOld  bool
	nextPos    int // queue position, -1 = none
	nextSongID string
}

// planSyncTarget computes what IPC calls are needed.
// Must be called with playQueueMu held.
func (a *app) planSyncTarget() syncPlan {
	qLen := len(a.playQueue)

	if qLen == 0 || a.curQueuePos < 0 || a.curQueuePos >= qLen {
		a.pendingNextPos = -1
		return syncPlan{doClear: true, nextPos: -1}
	}

	plan := syncPlan{
		doClear:    true,
		hasCurrent: true,
		curPos:     a.curQueuePos,
		curSongID:  a.playQueue[a.curQueuePos],
		nextPos:    -1,
	}

	a.pendingNextPos = a.nextQueuePos()
	if a.pendingNextPos >= 0 && a.pendingNextPos < qLen {
		plan.nextPos = a.pendingNextPos
		plan.nextSongID = a.playQueue[a.pendingNextPos]
	}

	return plan
}

// execSyncPlan executes the IPC calls described by the plan on every enabled output.
// Must NOT be called with playQueueMu held.
func (a *app) execSyncPlan(plan syncPlan) {
	for _, ti := range a.enabledTargetInfos() {
		a.execSyncPlanOn(ti, plan)
	}
}

// execSyncPlanOn executes the sync plan on a single output.
func (a *app) execSyncPlanOn(ti targetInfo, plan syncPlan) {
	// Agent targets use position-based commands
	if at, ok := ti.t.(*agentTarget); ok {
		if !plan.hasCurrent {
			_ = at.playlistClear()
			return
		}
		a.logger.Printf("syncTarget(agent %s): play pos %d next=%d paused=%v", ti.dev.ID, plan.curPos, plan.nextPos, plan.startPaused)
		if err := at.agentPlay(plan.curPos, plan.nextPos, plan.startPaused); err != nil {
			a.logger.Printf("syncTarget(agent %s): play failed: %v", ti.dev.ID, err)
		}
		return
	}

	if plan.doClear {
		_ = ti.t.playlistClear()
	}
	if !plan.hasCurrent {
		return
	}

	a.logger.Printf("syncTarget(%s): loading pos %d (songID=%s)", ti.dev.ID, plan.curPos, plan.curSongID)
	if err := ti.t.loadFile(a.streamURLFor(ti.dev, plan.curSongID), "replace", nil); err != nil {
		a.logger.Printf("syncTarget(%s): loadFile replace failed: %v", ti.dev.ID, err)
	}

	if plan.nextSongID != "" {
		a.logger.Printf("syncTarget(%s): preloading pos %d (songID=%s)", ti.dev.ID, plan.nextPos, plan.nextSongID)
		_ = ti.t.loadFile(a.streamURLFor(ti.dev, plan.nextSongID), "append", nil)
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
		plan.nextSongID = a.playQueue[a.pendingNextPos]
		plan.nextPos = a.pendingNextPos
	}

	return plan
}

// execNextTrackPlan executes the next-track preload IPC on every enabled output.
// Must NOT be called with playQueueMu held.
func (a *app) execNextTrackPlan(plan nextTrackPlan) {
	for _, ti := range a.enabledTargetInfos() {
		a.execNextTrackPlanOn(ti, plan)
	}
}

// execNextTrackPlanOn executes the next-track preload on a single output.
func (a *app) execNextTrackPlanOn(ti targetInfo, plan nextTrackPlan) {
	// An output with nothing loaded has no current track to preload after —
	// agent players no-op this themselves; skip uniformly so stopped outputs
	// stay untouched until an explicit play.
	if targetStopped(ti.t) {
		return
	}
	// Agent targets use position-based preload
	if at, ok := ti.t.(*agentTarget); ok {
		if err := at.agentPreload(plan.nextPos); err != nil {
			a.logger.Printf("execNextTrackPlan(agent %s): preload %d failed: %v", ti.dev.ID, plan.nextPos, err)
		}
		return
	}

	if plan.removeOld {
		_ = ti.t.playlistRemove(1)
	}
	if plan.nextSongID != "" {
		_ = ti.t.loadFile(a.streamURLFor(ti.dev, plan.nextSongID), "append", nil)
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
		if a.modeRepeat {
			plan := a.planSyncTarget()
			a.playQueueMu.Unlock()
			a.execSyncPlan(plan)
		} else {
			// MPD stops after the track in single mode: unload, keep the
			// queue pointer so play restarts the same track.
			a.pendingNextPos = -1
			a.playQueueMu.Unlock()
			_ = a.target().playlistClear()
		}
		a.mpdHub.notify(SubPlayer)
		return
	}

	// A nonzero priority on the finished track is spent: it is reset to zero
	// further below so the priority jump happens once while the row stays in
	// the queue (stock-MPD-style reset, no auto-consume).
	finishedPos := a.curQueuePos
	finishedHadPrio := finishedPos >= 0 && finishedPos < len(a.queuePriority) && a.queuePriority[finishedPos] > 0

	// Consume mode: remove the track that just finished
	if a.modeConsume && a.curQueuePos >= 0 && a.curQueuePos < qLen {
		// Adjust prioReturnPos for the removal
		if a.prioReturnPos > a.curQueuePos {
			a.prioReturnPos--
		}
		a.playQueue = append(a.playQueue[:a.curQueuePos], a.playQueue[a.curQueuePos+1:]...)
		a.queueIDs = append(a.queueIDs[:a.curQueuePos], a.queueIDs[a.curQueuePos+1:]...)
		if a.curQueuePos < len(a.queuePriority) {
			a.queuePriority = append(a.queuePriority[:a.curQueuePos], a.queuePriority[a.curQueuePos+1:]...)
		}
		a.bumpQueueVersionLocked()
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
		var nextSongID string
		if a.pendingNextPos >= 0 && a.pendingNextPos < qLen {
			nextSongID = a.playQueue[a.pendingNextPos]
		}
		a.playQueueMu.Unlock()

		// IPC calls outside lock
		a.advancePreload(nextPreloadPos, nextSongID)
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
		if finishedHadPrio {
			if finishedPos < len(a.queueIDs) {
				a.markPrioPlayed(a.queueIDs[finishedPos])
			}
			a.queuePriority[finishedPos] = 0
			a.bumpQueueVersionLocked()
			a.savePlayQueue()
		}
		a.playQueueMu.Unlock()
		_ = a.target().playlistClear()
		if finishedHadPrio {
			a.mpdHub.notify(SubPlaylist, SubPlayer)
		} else {
			a.mpdHub.notify(SubPlayer)
		}
		return
	}

	// Spend the finished track's priority before computing the next preload,
	// otherwise nextQueuePos would keep jumping back to it. The played mark
	// keeps it out of this cycle's sequential rotation.
	if finishedHadPrio {
		if finishedPos < len(a.queueIDs) {
			a.markPrioPlayed(a.queueIDs[finishedPos])
		}
		a.queuePriority[finishedPos] = 0
		a.bumpQueueVersionLocked()
		a.savePlayQueue()
	}

	// Compute next preload under lock
	a.pendingNextPos = a.nextQueuePos()
	nextPreloadPos := a.pendingNextPos
	var nextSongID string
	if a.pendingNextPos >= 0 && a.pendingNextPos < qLen {
		nextSongID = a.playQueue[a.pendingNextPos]
	}
	a.playQueueMu.Unlock()

	// IPC calls outside lock — clients can query status while these run
	a.advancePreload(nextPreloadPos, nextSongID)

	if finishedHadPrio {
		a.mpdHub.notify(SubPlaylist, SubPlayer)
	} else {
		a.mpdHub.notify(SubPlayer)
	}
}

// advancePreload updates every enabled output after a natural track advance:
// agents just learn the next preload position (their mpv already advanced
// gaplessly on its own); URL-based targets drop the finished slot 0 and
// append the new next track.
func (a *app) advancePreload(nextPreloadPos int, nextSongID string) {
	for _, ti := range a.enabledTargetInfos() {
		if at, ok := ti.t.(*agentTarget); ok {
			_ = at.agentPreload(nextPreloadPos)
			continue
		}
		_ = ti.t.playlistRemove(0)
		if nextSongID != "" {
			_ = ti.t.loadFile(a.streamURLFor(ti.dev, nextSongID), "append", nil)
		}
	}
}

// removeFromQueue removes a single track at pos from the server's queue.
// Does NOT touch the target playlist — caller must syncTarget if needed.
func (a *app) removeFromQueue(pos int) {
	// Caller must hold playQueueMu
	if pos < 0 || pos >= len(a.playQueue) {
		return
	}
	if pos < len(a.queueIDs) {
		delete(a.prioPlayedIDs, a.queueIDs[pos])
	}
	a.playQueue = append(a.playQueue[:pos], a.playQueue[pos+1:]...)
	a.queueIDs = append(a.queueIDs[:pos], a.queueIDs[pos+1:]...)
	if pos < len(a.queuePriority) {
		a.queuePriority = append(a.queuePriority[:pos], a.queuePriority[pos+1:]...)
	}
	a.bumpQueueVersionLocked()
	a.savePlayQueue()
}

func (a *app) ensureQueueVersionLocked() {
	now := int(time.Now().Unix())
	if a.queueVersion < now {
		a.queueVersion = now
	}
}

func (a *app) bumpQueueVersionLocked() {
	now := int(time.Now().Unix())
	if a.queueVersion < now {
		a.queueVersion = now
		return
	}
	a.queueVersion++
}

// ---------------------------------------------------------------------------
// Playback target / device helpers
// ---------------------------------------------------------------------------

// targetInfo pairs a device with its running playback target.
type targetInfo struct {
	dev *device
	t   playbackTarget
}

// targetForLocked returns the running agent/web target for devID, or nil.
// Caller must hold devicesMu.
func (a *app) targetForLocked(devID string) playbackTarget {
	if at, ok := a.agentTargets[devID]; ok && at.isRunning() {
		return at
	}
	if wt, ok := a.webTargets[devID]; ok && wt.isRunning() {
		return wt
	}
	return nil
}

// enabledTargetInfos returns running targets for all enabled devices,
// primary first, the rest in sortedDevices order.
func (a *app) enabledTargetInfos() []targetInfo {
	a.devicesMu.RLock()
	defer a.devicesMu.RUnlock()
	var infos []targetInfo
	if a.enabledOutputs[a.primaryOutput] {
		if t := a.targetForLocked(a.primaryOutput); t != nil {
			infos = append(infos, targetInfo{dev: a.devices[a.primaryOutput], t: t})
		}
	}
	for _, dev := range a.sortedDevices() {
		if dev.ID == a.primaryOutput || !a.enabledOutputs[dev.ID] {
			continue
		}
		if t := a.targetForLocked(dev.ID); t != nil {
			infos = append(infos, targetInfo{dev: dev, t: t})
		}
	}
	return infos
}

// target returns a playbackTarget that broadcasts writes to all enabled
// outputs and reads from the primary. With no enabled/running output it
// behaves like noopTarget.
func (a *app) target() playbackTarget {
	return &fanoutTarget{infos: a.enabledTargetInfos()}
}

// primaryTarget returns the primary device's running target, or noopTarget.
func (a *app) primaryTarget() playbackTarget {
	a.devicesMu.RLock()
	defer a.devicesMu.RUnlock()
	if t := a.targetForLocked(a.primaryOutput); t != nil {
		return t
	}
	return &noopTarget{}
}

// isPrimary reports whether devID is the current primary output.
func (a *app) isPrimary(devID string) bool {
	a.devicesMu.RLock()
	defer a.devicesMu.RUnlock()
	return devID != "" && a.primaryOutput == devID
}

// promotePrimaryLocked picks a new primary from the enabled set: first
// enabled and running device in sortedDevices order (local first), "" if none.
// Caller must hold devicesMu.
func (a *app) promotePrimaryLocked() {
	a.primaryOutput = ""
	for _, dev := range a.sortedDevices() {
		if a.enabledOutputs[dev.ID] && a.targetForLocked(dev.ID) != nil {
			a.primaryOutput = dev.ID
			return
		}
	}
}

// fanoutTarget broadcasts writes to all enabled outputs and reads from the
// primary (infos[0]). Write errors on one output don't stop the others; the
// first error is returned.
type fanoutTarget struct {
	infos []targetInfo // primary first; may be empty
}

func (f *fanoutTarget) each(fn func(playbackTarget) error) error {
	var firstErr error
	for _, ti := range f.infos {
		if err := fn(ti.t); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}

func (f *fanoutTarget) loadFile(url, mode string, meta map[string]any) error {
	return f.each(func(t playbackTarget) error { return t.loadFile(url, mode, meta) })
}

func (f *fanoutTarget) loadFileBatch(urls []string, mode string) error {
	return f.each(func(t playbackTarget) error { return t.loadFileBatch(urls, mode) })
}

func (f *fanoutTarget) playlistClear() error {
	return f.each(func(t playbackTarget) error { return t.playlistClear() })
}

func (f *fanoutTarget) playlistRemove(index int) error {
	return f.each(func(t playbackTarget) error { return t.playlistRemove(index) })
}

func (f *fanoutTarget) playlistMove(from, to int) error {
	return f.each(func(t playbackTarget) error { return t.playlistMove(from, to) })
}

func (f *fanoutTarget) setProperty(name string, value any) error {
	return f.each(func(t playbackTarget) error { return t.setProperty(name, value) })
}

func (f *fanoutTarget) getProperty(name string) (any, error) {
	if len(f.infos) == 0 {
		return nil, fmt.Errorf("no device")
	}
	return f.infos[0].t.getProperty(name)
}

func (f *fanoutTarget) isRunning() bool {
	return len(f.infos) > 0
}

// isStopped reports whether the primary output has nothing loaded.
func (f *fanoutTarget) isStopped() bool {
	if len(f.infos) == 0 {
		return true
	}
	return targetStopped(f.infos[0].t)
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
func (noopTarget) isStopped() bool                               { return true }

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
		a.bumpQueueVersionLocked()
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
		if len(a.playQueue) == 0 {
			// Inserting into an empty queue behaves like add: place the
			// tracks and point at the first one without touching playback.
			a.playQueue = append(a.playQueue, songIDs...)
			a.queuePriority = append(a.queuePriority, prios...)
			for range songIDs {
				a.queueIDCounter++
				a.queueIDs = append(a.queueIDs, a.queueIDCounter)
			}
			a.bumpQueueVersionLocked()
			a.curQueuePos = 0
			a.pendingNextPos = -1
			if a.modeRandom {
				a.generateShuffle()
			}
			a.savePlayQueue()
			a.playQueueMu.Unlock()
			return nil
		}
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
		a.bumpQueueVersionLocked()
		a.savePlayQueue()
		// Resync preloaded next track since insert may change it. Queue
		// mutation only — the transport state is left untouched.
		ntPlan := a.planNextTrack()
		a.playQueueMu.Unlock()
		a.execNextTrackPlan(ntPlan)
		return nil

	default: // "add"
		wasEmpty := len(a.playQueue) == 0
		oldNextPos := a.pendingNextPos
		a.playQueue = append(a.playQueue, songIDs...)
		a.queuePriority = append(a.queuePriority, prios...)
		for range songIDs {
			a.queueIDCounter++
			a.queueIDs = append(a.queueIDs, a.queueIDCounter)
		}
		a.bumpQueueVersionLocked()
		a.savePlayQueue()
		if wasEmpty {
			// Adding to an empty queue only mutates the queue — it must not
			// act like `play`. Point at the first track but leave every
			// output untouched (stopped stays stopped); an explicit play
			// command loads and starts it.
			a.curQueuePos = 0
			a.pendingNextPos = -1
			if a.modeRandom {
				a.generateShuffle()
			}
			a.playQueueMu.Unlock()
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
	Version    int      `json:"version,omitempty"`
}

// savePlayQueue persists the current play queue to disk (caller must hold playQueueMu or be safe).
func (a *app) savePlayQueue() {
	sq := savedQueue{Songs: a.playQueue, Priorities: a.queuePriority, Version: a.queueVersion}
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
		a.queueVersion = sq.Version
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
	plan.startPaused = true
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
	if maxBitrate < 0 {
		http.Error(w, "max_bitrate must be >= 0", http.StatusBadRequest)
		return
	}

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
	ext := transcodeFileExt(format)
	name := fmt.Sprintf("%s_%s_%d.%s", songID, ext, maxBitrate, ext)
	return filepath.Join(a.paths.TranscodeCacheDir, name)
}

func transcodeContentType(format string) string {
	spec, ok := transcodeSpecFor(format)
	if !ok {
		return "application/octet-stream"
	}
	return spec.contentType
}

type transcodeSpec struct {
	format      string
	fileExt     string
	contentType string
	args        []string
}

func normalizeTranscodeFormat(format string) string {
	format = strings.ToLower(strings.TrimSpace(format))
	if format == "" {
		return "mp3"
	}
	return format
}

func transcodeFileExt(format string) string {
	spec, ok := transcodeSpecFor(format)
	if !ok {
		return normalizeTranscodeFormat(format)
	}
	return spec.fileExt
}

func transcodeSpecFor(format string) (transcodeSpec, bool) {
	switch normalizeTranscodeFormat(format) {
	case "mp3":
		return transcodeSpec{
			format:      "mp3",
			fileExt:     "mp3",
			contentType: "audio/mpeg",
			args:        []string{"-f", "mp3", "-codec:a", "libmp3lame"},
		}, true
	case "opus":
		return transcodeSpec{
			format:      "opus",
			fileExt:     "opus",
			contentType: "audio/opus",
			args:        []string{"-f", "opus", "-codec:a", "libopus"},
		}, true
	case "ogg":
		return transcodeSpec{
			format:      "ogg",
			fileExt:     "ogg",
			contentType: "audio/ogg",
			args:        []string{"-f", "ogg", "-codec:a", "libvorbis"},
		}, true
	case "aac":
		return transcodeSpec{
			format:      "aac",
			fileExt:     "aac",
			contentType: "audio/aac",
			args:        []string{"-f", "adts", "-codec:a", "aac"},
		}, true
	case "flac":
		return transcodeSpec{
			format:      "flac",
			fileExt:     "flac",
			contentType: "audio/flac",
			args:        []string{"-f", "flac", "-codec:a", "flac"},
		}, true
	}
	return transcodeSpec{}, false
}

func (a *app) streamTranscoded(w http.ResponseWriter, r *http.Request, songID, path, format string, maxBitrate int, startTime float64) {
	spec, ok := transcodeSpecFor(format)
	if !ok {
		http.Error(w, fmt.Sprintf("unsupported transcode format: %s", format), http.StatusBadRequest)
		return
	}
	format = spec.format

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
	args = append(args, spec.args...)
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
			// A new file landed in the cache — enforce the size cap.
			go a.maybeEvictTranscodeCache()
		} else {
			os.Remove(tmpFile.Name())
			a.logger.Printf("transcode error for %s: %v — %s", songID, err, stderrBuf.String())
			// Abort the connection instead of letting the server write the
			// terminating chunk — a clean EOF would make download clients
			// treat the truncated stream as a complete file.
			panic(http.ErrAbortHandler)
		}
	} else {
		if tmpFile != nil {
			tmpFile.Close()
			os.Remove(tmpFile.Name())
		}
		io.Copy(w, stdout)
		if err := cmd.Wait(); err != nil {
			a.logger.Printf("transcode error for %s: %v — %s", songID, err, stderrBuf.String())
			panic(http.ErrAbortHandler)
		}
	}
}

// transcodeEvictMu ensures only one cache eviction sweep runs at a time.
var transcodeEvictMu sync.Mutex

// maybeEvictTranscodeCache enforces the configured cache size cap by deleting
// least-recently-used files (by access time) until the cache is back under 90%
// of the limit. A no-op when cache_max_mb is 0 (unlimited). Cheap to call after
// each new transcode; sweeps are coalesced so concurrent calls don't pile up.
func (a *app) maybeEvictTranscodeCache() {
	maxMB := a.cfg.Transcode.CacheMaxMB
	if maxMB <= 0 {
		return
	}
	if !transcodeEvictMu.TryLock() {
		return // a sweep is already in progress
	}
	defer transcodeEvictMu.Unlock()

	dir := a.paths.TranscodeCacheDir
	entries, err := os.ReadDir(dir)
	if err != nil {
		return
	}

	type cacheFile struct {
		path  string
		size  int64
		atime time.Time
	}
	var files []cacheFile
	var total int64
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		// Skip in-progress transcodes (written via CreateTemp as transcode_*.tmp).
		if strings.HasPrefix(e.Name(), "transcode_") && strings.HasSuffix(e.Name(), ".tmp") {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		files = append(files, cacheFile{
			path:  filepath.Join(dir, e.Name()),
			size:  info.Size(),
			atime: fileATime(info),
		})
		total += info.Size()
	}

	limit := int64(maxMB) * 1024 * 1024
	if total <= limit {
		return
	}

	// Evict least-recently-used first, down to a 90% low-water mark so we don't
	// re-run the sweep on every subsequent request.
	target := limit * 9 / 10
	sort.Slice(files, func(i, j int) bool { return files[i].atime.Before(files[j].atime) })
	var removed, freed int64
	for _, f := range files {
		if total <= target {
			break
		}
		if err := os.Remove(f.path); err == nil {
			total -= f.size
			freed += f.size
			removed++
		}
	}
	if removed > 0 {
		a.logger.Printf("transcode cache: evicted %d files (%d MB) — now %d/%d MB",
			removed, freed/(1024*1024), total/(1024*1024), maxMB)
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

// captureOutputState reads time-pos/pause/volume from a target. For agents it
// queries fresh values over IPC rather than using the cached heartbeat state.
// Returns volume=-1 if unknown.
func captureOutputState(t playbackTarget) (timePos float64, paused bool, volume float64) {
	volume = -1
	if t == nil {
		return
	}
	getProp := t.getProperty
	if at, ok := t.(*agentTarget); ok {
		getProp = at.getFreshProperty
	}
	if tpRaw, err := getProp("time-pos"); err == nil {
		if f, ok := tpRaw.(float64); ok {
			timePos = f
		}
	}
	if pauseRaw, err := getProp("pause"); err == nil {
		if p, ok := pauseRaw.(bool); ok {
			paused = p
		}
	}
	if volRaw, err := getProp("volume"); err == nil {
		if f, ok := volRaw.(float64); ok {
			volume = f
		}
	}
	return
}

// stopOutput pauses and clears a single target.
func stopOutput(t playbackTarget) {
	_ = t.setProperty("pause", true)
	_ = t.playlistClear()
}

// startOutputAt loads the current 2-track window into one output and seeks to
// timePos, transferring volume/replaygain first. Resumes unless paused is set.
func (a *app) startOutputAt(ti targetInfo, timePos float64, paused bool, volume float64) {
	if volume >= 0 {
		_ = ti.t.setProperty("volume", volume)
	}
	if a.cfg.Player.ReplayGain != "" {
		_ = ti.t.setProperty("replaygain", a.cfg.Player.ReplayGain)
	}

	a.playQueueMu.Lock()
	qLen := len(a.playQueue)
	plan := a.planSyncTarget()
	a.playQueueMu.Unlock()

	if qLen == 0 {
		return
	}

	// For agent targets, use agentPlayAt to seek atomically before audio starts
	if at, ok := ti.t.(*agentTarget); ok {
		at.ensureQueueSync()
		if err := at.agentPlayAt(plan.curPos, plan.nextPos, timePos, paused); err != nil {
			a.logger.Printf("startOutputAt(%s): agentPlayAt failed: %v", ti.dev.ID, err)
		}
		if paused {
			// Redundant on agents that honor paused=, re-pauses older ones.
			_ = ti.t.setProperty("pause", true)
		}
		return
	}

	a.execSyncPlanOn(ti, plan)
	// Wait for track to load before seeking
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if v, err := ti.t.getProperty("duration"); err == nil {
			if d, ok := v.(float64); ok && d > 0 {
				break
			}
		}
		time.Sleep(50 * time.Millisecond)
	}
	if timePos > 0 {
		_ = ti.t.setProperty("time-pos", timePos)
	}
	if !paused {
		_ = ti.t.setProperty("pause", false)
	}
}

// enableOutput adds a device to the enabled set without touching other
// outputs. The first enabled output becomes primary. If playback is under way
// the new output joins at the primary's position (best-effort sync),
// inheriting the primary's volume.
func (a *app) enableOutput(devID string) error {
	a.devicesMu.Lock()
	if _, exists := a.devices[devID]; !exists {
		a.devicesMu.Unlock()
		return fmt.Errorf("device not found: %s", devID)
	}
	if a.enabledOutputs[devID] {
		a.devicesMu.Unlock()
		return nil
	}
	primaryT := a.targetForLocked(a.primaryOutput)
	newT := a.targetForLocked(devID)
	a.enabledOutputs[devID] = true
	becamePrimary := false
	if a.primaryOutput == "" || primaryT == nil {
		a.primaryOutput = devID
		becamePrimary = true
	}
	dev := a.devices[devID]
	a.devicesMu.Unlock()
	a.persistOutputs()

	if newT != nil {
		if becamePrimary {
			// Nothing was playing — load the window paused at the queue position.
			a.startOutputAt(targetInfo{dev: dev, t: newT}, 0, true, -1)
		} else {
			timePos, paused, volume := captureOutputState(primaryT)
			a.startOutputAt(targetInfo{dev: dev, t: newT}, timePos, paused, volume)
		}
	}

	a.logger.Printf("output enabled: %s", devID)
	a.mpdHub.notify(SubOutput, SubPlayer, SubMixer)
	return nil
}

// disableOutput stops a device and removes it from the enabled set. Other
// outputs keep playing. If it was the primary, another enabled output is
// promoted; if it was the last one, playback is left without a clock and
// status reports "stop" until an output is enabled again.
func (a *app) disableOutput(devID string) error {
	a.devicesMu.Lock()
	if !a.enabledOutputs[devID] {
		a.devicesMu.Unlock()
		return nil
	}
	delete(a.enabledOutputs, devID)
	t := a.targetForLocked(devID)
	if t == nil {
		// Offline output the user no longer wants: nothing left to keep it
		// listed, so drop the record instead of leaving a ghost behind.
		delete(a.devices, devID)
		delete(a.agentResumes, devID)
	}
	if a.primaryOutput == devID {
		a.promotePrimaryLocked()
	}
	a.devicesMu.Unlock()
	a.persistOutputs()

	if t != nil {
		stopOutput(t)
	}

	a.logger.Printf("output disabled: %s", devID)
	a.mpdHub.notify(SubOutput, SubPlayer, SubMixer)
	return nil
}

// toggleOutput flips a device's enabled state.
func (a *app) toggleOutput(devID string) error {
	a.devicesMu.RLock()
	enabled := a.enabledOutputs[devID]
	a.devicesMu.RUnlock()
	if enabled {
		return a.disableOutput(devID)
	}
	return a.enableOutput(devID)
}

// reloadTransportDecision decides how a (re)connecting enabled agent joins
// playback: whether to load the queue window at all, and whether it starts
// paused. Another output actively playing means this one joins it (additive
// outputs). Otherwise the agent's own stashed state decides, then a paused
// sibling. A fresh registration with no evidence of active playback anywhere
// stays silent — Android restarting the app process in the background must
// never start audio on its own.
func reloadTransportDecision(othersPlaying, othersPaused bool,
	resume *agentResume) (load, startPaused bool) {
	switch {
	case othersPlaying:
		return true, false
	case resume != nil:
		return true, resume.state == "pause"
	case othersPaused:
		return true, true
	default:
		return false, false
	}
}

// reloadQueueIntoAgent loads the 2-track window into a reconnected agent.
// Called when an agent re-registers and was already the active device.
func (a *app) reloadQueueIntoAgent(at *agentTarget, dev *device, resume *agentResume,
	othersPlaying, othersPaused bool) {
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

	// If the replaced connection was mid-track on the same position, resume
	// there instead of restarting the track from the beginning.
	if resume != nil && resume.pos == plan.curPos && resume.elapsed > 0 && plan.hasCurrent {
		a.logger.Printf("agent reload: resuming %s at pos %d elapsed=%.1fs", dev.Name, plan.curPos, resume.elapsed)
		wasPaused := resume.state == "pause"
		if err := at.agentPlayAt(plan.curPos, plan.nextPos, resume.elapsed, wasPaused); err != nil {
			a.logger.Printf("agent reload: resume failed, falling back to full reload: %v", err)
		} else {
			if wasPaused {
				// Redundant on agents that honor paused=, re-pauses older ones.
				if _, err := at.sendCommand("pause"); err != nil {
					a.logger.Printf("agent reload: re-pause failed: %v", err)
				}
			}
			return
		}
	}

	load, startPaused := reloadTransportDecision(othersPlaying, othersPaused, resume)
	if !load {
		a.logger.Printf("agent reload: %s joined with no active playback anywhere — staying silent",
			dev.Name)
		return
	}
	plan.startPaused = startPaused
	a.execSyncPlanOn(targetInfo{dev: dev, t: at}, plan)
	a.logger.Printf("agent reload: loaded 2-track window into %s at pos %d (paused=%v)",
		dev.Name, a.curQueuePos, startPaused)
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

func floatFromAny(value any, fallback float64) float64 {
	return shared.FloatFromAny(value, fallback)
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
