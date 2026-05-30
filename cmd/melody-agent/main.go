package main

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/BurntSushi/toml"
	"github.com/carnager/melody/internal/player"
)

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

type agentConfig struct {
	Agent struct {
		Name       string `toml:"name"`
		Master     string `toml:"master"`    // host:port of melodyd MPD server
		MusicDir   string `toml:"music_dir"` // local path to music library (satellite mode)
		Format     string `toml:"format"`
		MaxBitRate int    `toml:"max_bitrate"`
	} `toml:"agent"`
	Player struct {
		ReplayGain string  `toml:"replaygain"` // "track", "album", "off"
		Volume     float64 `toml:"volume"`     // 0-100, default 100
		MPVPath    string  `toml:"mpv_path"`
		MPVSocket  string  `toml:"mpv_socket"`
	} `toml:"player"`
}

// ---------------------------------------------------------------------------
// Queue item — mirrors playlistinfo response
// ---------------------------------------------------------------------------

type queueItem struct {
	Position int
	File     string  // relative path (e.g., "Artist/Album/Track.flac")
	SongID   string  // X-SongId from melodyd DB
	Duration float64 // track duration in seconds
	RGTrack  float64 // X-ReplayGainTrack
	RGAlbum  float64 // X-ReplayGainAlbum
}

// ---------------------------------------------------------------------------
// Agent
// ---------------------------------------------------------------------------

type agent struct {
	cfg    agentConfig
	logger *log.Logger
	player *player.Player

	// Queue state (synced from server)
	queueMu sync.Mutex
	queue   []queueItem
	curPos  int // current queue position being played
	nextPos int // daemon-selected preloaded queue position, or -1

	// Control connection to server
	ctrlMu   sync.Mutex
	ctrlConn net.Conn
	ctrlW    *bufio.Writer
}

func main() {
	logger := log.New(os.Stdout, "melody-agent: ", log.LstdFlags)

	cfg, err := loadAgentConfig()
	if err != nil {
		logger.Fatalf("load config: %v", err)
	}

	p, err := player.NewWithConfig(cfg.Player.MPVPath, cfg.Player.MPVSocket)
	if err != nil {
		logger.Fatalf("start mpv player: %v", err)
	}
	defer p.Close()

	a := &agent{
		cfg:     cfg,
		logger:  logger,
		player:  p,
		curPos:  -1,
		nextPos: -1,
	}

	// Set initial player state from config
	if cfg.Player.ReplayGain != "" {
		p.SetReplayGain(cfg.Player.ReplayGain)
	}
	if cfg.Player.Volume > 0 {
		p.SetVolume(cfg.Player.Volume)
	}

	// Wire up track-end callback
	p.OnTrackEnd = a.handleTrackEnd

	// Start position reporter
	p.StartPositionReporter(2*time.Second, a.reportState)
	installShutdownHandler(logger, p)

	logger.Printf("agent %q connecting to master at %s", cfg.Agent.Name, cfg.Agent.Master)
	if cfg.Agent.MusicDir != "" {
		logger.Printf("music_dir: %s (direct file access)", cfg.Agent.MusicDir)
	} else {
		logger.Printf("no music_dir configured, will stream via HTTP")
	}

	a.connectLoop()
}

func installShutdownHandler(logger *log.Logger, p *player.Player) {
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		sig := <-sigCh
		logger.Printf("received %s, shutting down player", sig)
		p.Close()
		os.Exit(0)
	}()
}

func loadAgentConfig() (agentConfig, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return agentConfig{}, err
	}
	xdgConfig := getenvDefault("XDG_CONFIG_HOME", filepath.Join(home, ".config"))
	configPath := filepath.Join(xdgConfig, "melody", "melody-agent.toml")

	if err := os.MkdirAll(filepath.Dir(configPath), 0o755); err != nil {
		return agentConfig{}, err
	}

	if _, err := os.Stat(configPath); errors.Is(err, os.ErrNotExist) {
		if err := os.WriteFile(configPath, []byte(defaultAgentConfig()), 0o644); err != nil {
			return agentConfig{}, err
		}
	}

	var cfg agentConfig
	if _, err := toml.DecodeFile(configPath, &cfg); err != nil {
		return agentConfig{}, err
	}

	if cfg.Agent.Name == "" {
		hostname, _ := os.Hostname()
		cfg.Agent.Name = hostname
	}
	if cfg.Agent.Master == "" {
		cfg.Agent.Master = "localhost:6600"
	}
	if cfg.Player.Volume == 0 {
		cfg.Player.Volume = 100
	}
	if cfg.Player.MPVPath == "" {
		cfg.Player.MPVPath = "mpv"
	}

	return cfg, nil
}

func defaultAgentConfig() string {
	hostname, _ := os.Hostname()
	return `[agent]
name = "` + hostname + `"
master = "localhost:6600"
music_dir = ""
format = ""
max_bitrate = 0

[player]
replaygain = "track"
volume = 100
mpv_path = "mpv"
mpv_socket = ""
`
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

func (a *agent) connectLoop() {
	for {
		if err := a.runSession(); err != nil {
			a.logger.Printf("session error: %v, reconnecting in 5s", err)
		}
		a.player.Stop()
		a.curPos = -1
		a.nextPos = -1
		time.Sleep(5 * time.Second)
	}
}

func (a *agent) runSession() error {
	// Connect control connection
	conn, err := net.DialTimeout("tcp", a.cfg.Agent.Master, 5*time.Second)
	if err != nil {
		return fmt.Errorf("connect: %w", err)
	}
	defer conn.Close()

	reader := bufio.NewReader(conn)
	writer := bufio.NewWriter(conn)

	// Read MPD greeting
	greeting, err := reader.ReadString('\n')
	if err != nil {
		return fmt.Errorf("read greeting: %w", err)
	}
	if !strings.HasPrefix(greeting, "OK MPD") {
		return fmt.Errorf("unexpected greeting: %s", strings.TrimSpace(greeting))
	}
	a.logger.Printf("connected to master at %s", a.cfg.Agent.Master)

	// Send agent_register with v2 flag
	regCmd := fmt.Sprintf("agent_register %s v2", mpdQuote(a.cfg.Agent.Name))
	if a.cfg.Agent.Format != "" {
		regCmd += fmt.Sprintf(" format=%s", mpdQuote(a.cfg.Agent.Format))
	}
	if a.cfg.Agent.MaxBitRate > 0 {
		regCmd += fmt.Sprintf(" max_bitrate=%d", a.cfg.Agent.MaxBitRate)
	}
	fmt.Fprintf(writer, "%s\n", regCmd)
	if err := writer.Flush(); err != nil {
		return fmt.Errorf("write register: %w", err)
	}

	// Read response
	resp, err := reader.ReadString('\n')
	if err != nil {
		return fmt.Errorf("register response: %w", err)
	}
	resp = strings.TrimRight(resp, "\r\n")
	if resp != "OK" {
		return fmt.Errorf("register failed: %s", resp)
	}
	a.logger.Printf("registered as %q", a.cfg.Agent.Name)

	// Store control connection for state reporting
	a.ctrlMu.Lock()
	a.ctrlConn = conn
	a.ctrlW = writer
	a.ctrlMu.Unlock()

	defer func() {
		a.ctrlMu.Lock()
		a.ctrlConn = nil
		a.ctrlW = nil
		a.ctrlMu.Unlock()
	}()

	// Fetch initial queue
	if err := a.syncQueue(); err != nil {
		a.logger.Printf("initial queue sync failed: %v", err)
	}

	// Command loop — server sends commands, we execute.
	// Responses are written to a local buffer, then flushed to the
	// control connection under ctrlMu to prevent interleaving with
	// async messages (agent_state, agent_advance).
	var respBuf bytes.Buffer
	respW := bufio.NewWriter(&respBuf)
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			return fmt.Errorf("read command: %w", err)
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			continue
		}

		respBuf.Reset()
		respW.Reset(&respBuf)
		a.handleCommand(respW, line)
		respW.Flush()

		a.ctrlMu.Lock()
		writer.Write(respBuf.Bytes())
		writer.Flush()
		a.ctrlMu.Unlock()
	}
}

// ---------------------------------------------------------------------------
// Command handling (from server)
// ---------------------------------------------------------------------------

func (a *agent) handleCommand(w *bufio.Writer, line string) {
	cmd, args := parseCommand(line)
	switch cmd {
	case "ping":
		fmt.Fprintln(w, "OK")

	case "play":
		a.handlePlay(w, args)

	case "preload":
		a.handlePreload(w, args)

	case "pause":
		a.player.Pause()
		fmt.Fprintln(w, "OK")

	case "resume":
		a.player.Resume()
		fmt.Fprintln(w, "OK")

	case "stop":
		a.player.Stop()
		a.curPos = -1
		a.nextPos = -1
		fmt.Fprintln(w, "OK")

	case "seek":
		if len(args) < 1 {
			fmt.Fprintln(w, "ACK [2@0] {seek} missing seconds")
			return
		}
		secs, _ := strconv.ParseFloat(args[0], 64)
		if err := a.player.Seek(secs); err != nil {
			fmt.Fprintf(w, "ACK [56@0] {seek} %s\n", err)
			return
		}
		fmt.Fprintln(w, "OK")

	case "volume":
		if len(args) < 1 {
			fmt.Fprintln(w, "ACK [2@0] {volume} missing level")
			return
		}
		level, _ := strconv.ParseFloat(args[0], 64)
		a.player.SetVolume(level)
		fmt.Fprintln(w, "OK")

	case "replaygain":
		if len(args) < 1 {
			fmt.Fprintln(w, "ACK [2@0] {replaygain} missing mode")
			return
		}
		a.player.SetReplayGain(args[0])
		fmt.Fprintln(w, "OK")

	case "queue_changed":
		if err := a.syncQueue(); err != nil {
			a.logger.Printf("queue sync failed: %v", err)
			fmt.Fprintf(w, "ACK [56@0] {queue_changed} %s\n", err)
			return
		}
		fmt.Fprintln(w, "OK")

	case "get_property":
		// Compatibility: return cached state for status queries
		if len(args) < 1 {
			fmt.Fprintln(w, "ACK [2@0] {get_property} missing name")
			return
		}
		a.handleGetProperty(w, args[0])

	case "set_property":
		// Compatibility: handle property sets
		if len(args) < 2 {
			fmt.Fprintln(w, "ACK [2@0] {set_property} missing arguments")
			return
		}
		a.handleSetProperty(w, args[0], args[1])

	default:
		fmt.Fprintf(w, "ACK [5@0] {%s} unknown command\n", cmd)
	}
}

func (a *agent) handlePlay(w *bufio.Writer, args []string) {
	if len(args) < 1 {
		fmt.Fprintln(w, "ACK [2@0] {play} missing queue position")
		return
	}

	pos, err := strconv.Atoi(args[0])
	if err != nil {
		fmt.Fprintln(w, "ACK [2@0] {play} invalid position")
		return
	}

	// Parse optional next=N and seek=<seconds>
	nextPos := -1
	var seekPos float64 = -1
	for _, arg := range args[1:] {
		if strings.HasPrefix(arg, "next=") {
			nextPos, _ = strconv.Atoi(strings.TrimPrefix(arg, "next="))
		} else if strings.HasPrefix(arg, "seek=") {
			seekPos, _ = strconv.ParseFloat(strings.TrimPrefix(arg, "seek="), 64)
		}
	}

	a.queueMu.Lock()
	if pos < 0 || pos >= len(a.queue) {
		a.queueMu.Unlock()
		fmt.Fprintf(w, "ACK [50@0] {play} position %d out of range\n", pos)
		return
	}
	item := a.queue[pos]
	var nextItem *queueItem
	if nextPos >= 0 && nextPos < len(a.queue) {
		ni := a.queue[nextPos]
		nextItem = &ni
	}
	a.queueMu.Unlock()

	path := a.resolveTrackPath(item)
	if path == "" {
		fmt.Fprintf(w, "ACK [50@0] {play} cannot resolve path for position %d\n", pos)
		return
	}

	currentSpec := player.TrackSpec{
		Path:       path,
		FormatHint: item.File,
		RGTrack:    item.RGTrack,
		RGAlbum:    item.RGAlbum,
	}
	var nextSpec *player.TrackSpec
	if nextItem != nil {
		nextPath := a.resolveTrackPath(*nextItem)
		if nextPath != "" {
			nextSpec = &player.TrackSpec{
				Path:       nextPath,
				FormatHint: nextItem.File,
				RGTrack:    nextItem.RGTrack,
				RGAlbum:    nextItem.RGAlbum,
			}
		}
	}

	if err := a.player.PlayPair(currentSpec, nextSpec, seekPos); err != nil {
		fmt.Fprintf(w, "ACK [56@0] {play} %s\n", err)
		return
	}
	a.curPos = pos
	if nextSpec != nil {
		a.nextPos = nextPos
	} else {
		a.nextPos = -1
	}

	fmt.Fprintln(w, "OK")
}

func (a *agent) handlePreload(w *bufio.Writer, args []string) {
	if len(args) < 1 {
		fmt.Fprintln(w, "ACK [2@0] {preload} missing queue position")
		return
	}

	pos, err := strconv.Atoi(args[0])
	if err != nil {
		fmt.Fprintln(w, "ACK [2@0] {preload} invalid position")
		return
	}
	if pos < 0 {
		if err := a.player.ClearPreload(); err != nil {
			fmt.Fprintf(w, "ACK [56@0] {preload} %s\n", err)
			return
		}
		a.nextPos = -1
		fmt.Fprintln(w, "OK")
		return
	}

	a.queueMu.Lock()
	if pos >= len(a.queue) {
		a.queueMu.Unlock()
		fmt.Fprintf(w, "ACK [50@0] {preload} position %d out of range\n", pos)
		return
	}
	item := a.queue[pos]
	a.queueMu.Unlock()

	path := a.resolveTrackPath(item)
	if path == "" {
		fmt.Fprintf(w, "ACK [50@0] {preload} cannot resolve path for position %d\n", pos)
		return
	}

	if err := a.player.Preload(path, item.File, item.RGTrack, item.RGAlbum); err != nil {
		fmt.Fprintf(w, "ACK [56@0] {preload} %s\n", err)
		return
	}
	a.nextPos = pos
	fmt.Fprintln(w, "OK")
}

// handleGetProperty returns cached player state (compatibility with v1 protocol)
func (a *agent) handleGetProperty(w *bufio.Writer, name string) {
	state, elapsed, duration, vol := a.player.State()
	switch name {
	case "pause":
		fmt.Fprintf(w, "value: %v\n", state == "pause" || state == "stop")
	case "time-pos":
		fmt.Fprintf(w, "value: %f\n", elapsed)
	case "duration":
		fmt.Fprintf(w, "value: %f\n", duration)
	case "volume":
		fmt.Fprintf(w, "value: %f\n", vol)
	default:
		fmt.Fprintf(w, "ACK [56@0] {get_property} unknown property: %s\n", name)
		return
	}
	fmt.Fprintln(w, "OK")
}

// handleSetProperty handles property changes (compatibility with v1 protocol)
func (a *agent) handleSetProperty(w *bufio.Writer, name, rawValue string) {
	switch name {
	case "pause":
		if rawValue == "true" || rawValue == "1" || rawValue == "yes" {
			a.player.Pause()
		} else {
			a.player.Resume()
		}
	case "time-pos":
		secs, _ := strconv.ParseFloat(rawValue, 64)
		a.player.Seek(secs)
	case "volume":
		vol, _ := strconv.ParseFloat(rawValue, 64)
		a.player.SetVolume(vol)
	case "replaygain":
		a.player.SetReplayGain(rawValue)
	default:
		fmt.Fprintf(w, "ACK [56@0] {set_property} unknown property: %s\n", name)
		return
	}
	fmt.Fprintln(w, "OK")
}

// ---------------------------------------------------------------------------
// Track end handling
// ---------------------------------------------------------------------------

func (a *agent) handleTrackEnd() {
	a.queueMu.Lock()
	qLen := len(a.queue)
	a.queueMu.Unlock()

	if qLen == 0 {
		return
	}

	oldPos := a.curPos
	if a.nextPos >= 0 {
		a.curPos = a.nextPos
		a.nextPos = -1
	} else {
		a.curPos = -1
	}
	a.sendToServer(fmt.Sprintf("agent_advance %d", oldPos))
}

// ---------------------------------------------------------------------------
// State reporting
// ---------------------------------------------------------------------------

func (a *agent) reportState(state string, elapsed, duration, vol float64) {
	if a.ctrlConn == nil {
		return
	}
	a.sendToServer(fmt.Sprintf("agent_state %s %d %.3f %.3f %.0f",
		state, a.curPos, elapsed, duration, vol))
}

func (a *agent) sendToServer(msg string) {
	a.ctrlMu.Lock()
	defer a.ctrlMu.Unlock()

	if a.ctrlW == nil {
		return
	}
	fmt.Fprintf(a.ctrlW, "%s\n", msg)
	a.ctrlW.Flush()
}

// ---------------------------------------------------------------------------
// Queue sync — fetch queue from server via separate MPD connection
// ---------------------------------------------------------------------------

func (a *agent) syncQueue() error {
	conn, err := net.DialTimeout("tcp", a.cfg.Agent.Master, 5*time.Second)
	if err != nil {
		return fmt.Errorf("connect: %w", err)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(10 * time.Second))

	r := bufio.NewReader(conn)

	// Read greeting
	if _, err := r.ReadString('\n'); err != nil {
		return fmt.Errorf("greeting: %w", err)
	}

	// Send playlistinfo
	fmt.Fprintf(conn, "playlistinfo\n")

	var items []queueItem
	current := make(map[string]string)

	for {
		line, err := r.ReadString('\n')
		if err != nil {
			return fmt.Errorf("read playlistinfo: %w", err)
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "OK" {
			// Flush last item
			if _, ok := current["file"]; ok {
				items = append(items, parseQueueItem(current))
			}
			break
		}
		if strings.HasPrefix(line, "ACK") {
			return fmt.Errorf("playlistinfo: %s", line)
		}

		k, v, ok := strings.Cut(line, ": ")
		if !ok {
			continue
		}

		// New track group starts with "file:"
		if k == "file" && len(current) > 0 {
			items = append(items, parseQueueItem(current))
			current = make(map[string]string)
		}
		current[k] = v
	}

	a.queueMu.Lock()
	a.queue = items
	a.queueMu.Unlock()

	fmt.Fprintf(conn, "close\n")
	return nil
}

func parseQueueItem(kv map[string]string) queueItem {
	pos, _ := strconv.Atoi(kv["Pos"])
	dur, _ := strconv.ParseFloat(kv["duration"], 64)
	if dur == 0 {
		dur, _ = strconv.ParseFloat(kv["Time"], 64)
	}
	rgt, _ := strconv.ParseFloat(kv["X-ReplayGainTrack"], 64)
	rga, _ := strconv.ParseFloat(kv["X-ReplayGainAlbum"], 64)
	return queueItem{
		Position: pos,
		File:     kv["file"],
		SongID:   kv["X-SongId"],
		Duration: dur,
		RGTrack:  rgt,
		RGAlbum:  rga,
	}
}

// ---------------------------------------------------------------------------
// Track path resolution
// ---------------------------------------------------------------------------

func (a *agent) resolveTrackPath(item queueItem) string {
	// If music_dir is configured, use direct file access (satellite mode)
	if a.cfg.Agent.MusicDir != "" {
		return filepath.Join(a.cfg.Agent.MusicDir, item.File)
	}

	// Otherwise, stream via HTTP from server
	if item.SongID == "" {
		return ""
	}

	// Derive from master address — server HTTP API runs on port 6701 on the same host
	h, _, err := net.SplitHostPort(a.cfg.Agent.Master)
	if err != nil {
		h = a.cfg.Agent.Master
	}
	return fmt.Sprintf("http://%s:6701/api/v1/stream/%s", h, item.SongID)
}

// ---------------------------------------------------------------------------
// Command parsing
// ---------------------------------------------------------------------------

func parseCommand(line string) (string, []string) {
	var cmd string
	var args []string
	var current strings.Builder
	inQuote := false
	escaped := false
	first := true

	for _, r := range line {
		if escaped {
			current.WriteRune(r)
			escaped = false
			continue
		}
		if r == '\\' && inQuote {
			escaped = true
			continue
		}
		if r == '"' {
			inQuote = !inQuote
			continue
		}
		if r == ' ' && !inQuote {
			if current.Len() > 0 {
				if first {
					cmd = current.String()
					first = false
				} else {
					args = append(args, current.String())
				}
				current.Reset()
			}
			continue
		}
		current.WriteRune(r)
	}
	if current.Len() > 0 {
		if first {
			cmd = current.String()
		} else {
			args = append(args, current.String())
		}
	}
	return cmd, args
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

func mpdQuote(s string) string {
	escaped := strings.ReplaceAll(s, `\`, `\\`)
	escaped = strings.ReplaceAll(escaped, `"`, `\"`)
	return `"` + escaped + `"`
}

func getenvDefault(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
