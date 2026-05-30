package player

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

const (
	defaultMPVPath = "mpv"
	commandTimeout = 10 * time.Second
)

// TrackSpec describes a track to load into the backend.
// ReplayGain values are kept for API compatibility. mpv applies its own
// ReplayGain mode and does not use Melody's decoded gain values here.
type TrackSpec struct {
	Path       string
	FormatHint string
	RGTrack    float64
	RGAlbum    float64
}

type Player struct {
	mu sync.Mutex

	mpvPath    string
	socketPath string
	cmd        *exec.Cmd
	conn       net.Conn
	done       chan struct{}
	closed     bool
	startErr   error

	writeMu       sync.Mutex
	pendingMu     sync.Mutex
	nextRequestID int
	pending       map[int]chan commandResult
	eventCh       chan mpvEvent
	commandHook   func(args ...any) (mpvResponse, error)

	state      string
	volume     float64
	replayGain string

	currentLoaded  bool
	nextLoaded     bool
	currentPath    string
	nextPath       string
	currentEntryID int64
	nextEntryID    int64
	generation     uint64

	OnTrackEnd func()
}

type mpvRequest struct {
	Command   []any `json:"command"`
	RequestID int   `json:"request_id"`
}

type mpvResponse struct {
	Data      any    `json:"data,omitempty"`
	Error     string `json:"error,omitempty"`
	RequestID int    `json:"request_id,omitempty"`
}

type commandResult struct {
	resp mpvResponse
	err  error
}

type mpvEvent struct {
	Event           string `json:"event"`
	Reason          string `json:"reason,omitempty"`
	PlaylistEntryID int64  `json:"playlist_entry_id,omitempty"`
}

func New() *Player {
	p, err := NewWithConfig("", "")
	if err != nil {
		return &Player{
			mpvPath:       defaultMPVPath,
			socketPath:    defaultSocketPath(),
			state:         "stop",
			volume:        100,
			replayGain:    "no",
			pending:       make(map[int]chan commandResult),
			eventCh:       make(chan mpvEvent, 64),
			nextRequestID: 1,
			startErr:      err,
		}
	}
	return p
}

func NewWithConfig(mpvPath, socketPath string) (*Player, error) {
	if strings.TrimSpace(mpvPath) == "" {
		mpvPath = defaultMPVPath
	}
	if strings.TrimSpace(socketPath) == "" {
		socketPath = defaultSocketPath()
	}

	p := &Player{
		mpvPath:       mpvPath,
		socketPath:    socketPath,
		state:         "stop",
		volume:        100,
		replayGain:    "no",
		pending:       make(map[int]chan commandResult),
		eventCh:       make(chan mpvEvent, 64),
		nextRequestID: 1,
	}
	if err := p.startLocked(); err != nil {
		p.startErr = err
		return p, err
	}
	return p, nil
}

func defaultSocketPath() string {
	base := os.Getenv("XDG_RUNTIME_DIR")
	if base != "" {
		base = filepath.Join(base, "melody")
	} else {
		base = filepath.Join(os.TempDir(), fmt.Sprintf("melody-%d", os.Getuid()))
	}
	return filepath.Join(base, fmt.Sprintf("melody-%d.mpv.sock", os.Getpid()))
}

func (p *Player) startLocked() error {
	if p.closed {
		return errors.New("player closed")
	}
	if p.conn != nil && p.cmd != nil && p.cmd.Process != nil {
		return nil
	}

	if err := os.MkdirAll(filepath.Dir(p.socketPath), 0o700); err != nil {
		return fmt.Errorf("create mpv socket dir: %w", err)
	}
	_ = os.Remove(p.socketPath)

	cmd := exec.Command(p.mpvPath,
		"--idle=yes",
		"--no-video",
		"--no-terminal",
		"--audio-display=no",
		"--force-window=no",
		"--gapless-audio=yes",
		"--input-ipc-server="+p.socketPath,
	)
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("start mpv %q: %w", p.mpvPath, err)
	}

	deadline := time.Now().Add(5 * time.Second)
	var conn net.Conn
	var err error
	for time.Now().Before(deadline) {
		conn, err = net.DialTimeout("unix", p.socketPath, 200*time.Millisecond)
		if err == nil {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if conn == nil {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		return fmt.Errorf("connect mpv IPC %s: %w", p.socketPath, err)
	}

	p.cmd = cmd
	p.conn = conn
	p.done = make(chan struct{})
	p.pending = make(map[int]chan commandResult)
	p.nextRequestID = 1
	p.startErr = nil

	go p.readLoop(conn, p.done)
	go p.eventLoop(p.done)
	go p.waitLoop(cmd, p.done)
	return nil
}

func (p *Player) ensureStartedLocked() error {
	if p.commandHook != nil {
		return nil
	}
	if p.conn != nil && p.cmd != nil && p.cmd.Process != nil {
		return nil
	}
	if p.startErr != nil {
		p.startErr = nil
	}
	return p.startLocked()
}

func (p *Player) waitLoop(cmd *exec.Cmd, done chan struct{}) {
	_ = cmd.Wait()
	p.mu.Lock()
	if p.cmd == cmd {
		p.conn = nil
		p.cmd = nil
		p.currentLoaded = false
		p.nextLoaded = false
		p.currentPath = ""
		p.nextPath = ""
		p.currentEntryID = 0
		p.nextEntryID = 0
		p.state = "stop"
		p.generation++
	}
	p.mu.Unlock()
	p.failPending(errors.New("mpv exited"))
	close(done)
}

func (p *Player) readLoop(conn net.Conn, done chan struct{}) {
	scanner := bufio.NewScanner(conn)
	buf := make([]byte, 0, 64*1024)
	scanner.Buffer(buf, 1024*1024)
	for scanner.Scan() {
		line := scanner.Bytes()
		var raw struct {
			RequestID       int    `json:"request_id,omitempty"`
			Event           string `json:"event,omitempty"`
			Reason          string `json:"reason,omitempty"`
			PlaylistEntryID int64  `json:"playlist_entry_id,omitempty"`
			Error           string `json:"error,omitempty"`
			Data            any    `json:"data,omitempty"`
		}
		if err := json.Unmarshal(line, &raw); err != nil {
			continue
		}
		if raw.RequestID != 0 {
			p.pendingMu.Lock()
			ch := p.pending[raw.RequestID]
			delete(p.pending, raw.RequestID)
			p.pendingMu.Unlock()
			if ch != nil {
				ch <- commandResult{resp: mpvResponse{
					Data:      raw.Data,
					Error:     raw.Error,
					RequestID: raw.RequestID,
				}}
			}
			continue
		}
		if raw.Event != "" {
			ev := mpvEvent{
				Event:           raw.Event,
				Reason:          raw.Reason,
				PlaylistEntryID: raw.PlaylistEntryID,
			}
			select {
			case p.eventCh <- ev:
			case <-done:
				return
			}
		}
	}
	p.failPending(errors.New("mpv IPC closed"))
}

func (p *Player) eventLoop(done chan struct{}) {
	for {
		select {
		case ev := <-p.eventCh:
			p.handleEvent(ev)
		case <-done:
			return
		}
	}
}

func (p *Player) failPending(err error) {
	p.pendingMu.Lock()
	for id, ch := range p.pending {
		delete(p.pending, id)
		ch <- commandResult{err: err}
	}
	p.pendingMu.Unlock()
}

func (p *Player) handleEvent(ev mpvEvent) {
	if ev.Event != "end-file" || ev.Reason != "eof" {
		return
	}

	var cb func()
	p.mu.Lock()
	if !p.currentLoaded || ev.PlaylistEntryID != p.currentEntryID {
		p.mu.Unlock()
		return
	}

	if p.nextLoaded {
		_, _ = p.commandLocked("playlist-remove", 0)
		p.currentLoaded = true
		p.currentPath = p.nextPath
		p.currentEntryID = p.nextEntryID
		p.nextLoaded = false
		p.nextPath = ""
		p.nextEntryID = 0
		p.state = "play"
	} else {
		p.currentLoaded = false
		p.currentPath = ""
		p.currentEntryID = 0
		p.state = "stop"
	}
	p.generation++
	cb = p.OnTrackEnd
	p.mu.Unlock()

	if cb != nil {
		go cb()
	}
}

func (p *Player) commandLocked(args ...any) (mpvResponse, error) {
	if p.commandHook != nil {
		return p.commandHook(args...)
	}
	if p.conn == nil {
		return mpvResponse{}, errors.New("mpv is not running")
	}

	p.pendingMu.Lock()
	id := p.nextRequestID
	p.nextRequestID++
	ch := make(chan commandResult, 1)
	p.pending[id] = ch
	p.pendingMu.Unlock()

	req := mpvRequest{Command: args, RequestID: id}
	data, err := json.Marshal(req)
	if err != nil {
		p.removePending(id)
		return mpvResponse{}, err
	}
	data = append(data, '\n')

	p.writeMu.Lock()
	_, err = p.conn.Write(data)
	p.writeMu.Unlock()
	if err != nil {
		p.removePending(id)
		return mpvResponse{}, err
	}

	select {
	case res := <-ch:
		if res.err != nil {
			return mpvResponse{}, res.err
		}
		if res.resp.Error != "" && res.resp.Error != "success" {
			return res.resp, errors.New(res.resp.Error)
		}
		return res.resp, nil
	case <-time.After(commandTimeout):
		p.removePending(id)
		return mpvResponse{}, fmt.Errorf("mpv command %v timed out", args)
	}
}

func (p *Player) removePending(id int) {
	p.pendingMu.Lock()
	delete(p.pending, id)
	p.pendingMu.Unlock()
}

func (p *Player) Play(path, formatHint string, rgTrack, rgAlbum float64) error {
	return p.PlayPair(TrackSpec{
		Path:       path,
		FormatHint: formatHint,
		RGTrack:    rgTrack,
		RGAlbum:    rgAlbum,
	}, nil, -1)
}

func (p *Player) PlayPair(current TrackSpec, next *TrackSpec, seek float64) error {
	p.mu.Lock()
	defer p.mu.Unlock()

	if current.Path == "" {
		return errors.New("empty path")
	}
	if err := p.ensureStartedLocked(); err != nil {
		p.startErr = err
		return err
	}

	p.generation++
	if err := p.clearPlaylistLocked(); err != nil {
		return err
	}
	if _, err := p.loadCurrentLocked(current.Path, seek); err != nil {
		p.currentLoaded = false
		p.nextLoaded = false
		p.state = "stop"
		return err
	}
	curID, err := p.playlistEntryIDLocked(0)
	if err != nil {
		return err
	}

	nextLoaded := false
	var nextPath string
	var nextID int64
	if next != nil && next.Path != "" {
		if _, err := p.commandLocked("loadfile", next.Path, "append"); err != nil {
			p.nextLoaded = false
			return err
		}
		nextID, err = p.playlistEntryIDLocked(1)
		if err != nil {
			return err
		}
		nextLoaded = true
		nextPath = next.Path
	}

	_ = p.setReplayGainLocked(p.replayGain)
	_, _ = p.commandLocked("set_property", "volume", p.volume)
	if _, err := p.commandLocked("set_property", "pause", false); err != nil {
		return err
	}

	p.currentLoaded = true
	p.currentPath = current.Path
	p.currentEntryID = curID
	p.nextLoaded = nextLoaded
	p.nextPath = nextPath
	p.nextEntryID = nextID
	p.state = "play"
	return nil
}

func (p *Player) loadCurrentLocked(path string, seek float64) (mpvResponse, error) {
	if seek > 0 {
		return p.commandLocked("loadfile", path, "replace", -1, map[string]string{
			"start": fmt.Sprintf("%.3f", seek),
		})
	}
	return p.commandLocked("loadfile", path, "replace")
}

func (p *Player) Preload(path, formatHint string, rgTrack, rgAlbum float64) error {
	p.mu.Lock()
	defer p.mu.Unlock()

	if path == "" {
		return errors.New("empty path")
	}
	if !p.currentLoaded {
		p.nextLoaded = false
		p.nextPath = ""
		p.nextEntryID = 0
		return nil
	}
	if err := p.ensureStartedLocked(); err != nil {
		p.startErr = err
		return err
	}
	if err := p.clearPreloadLocked(); err != nil {
		return err
	}
	if _, err := p.commandLocked("loadfile", path, "append"); err != nil {
		p.nextLoaded = false
		p.nextPath = ""
		p.nextEntryID = 0
		return err
	}
	id, err := p.playlistEntryIDLocked(1)
	if err != nil {
		p.nextLoaded = false
		p.nextPath = ""
		p.nextEntryID = 0
		return err
	}
	p.nextLoaded = true
	p.nextPath = path
	p.nextEntryID = id
	return nil
}

func (p *Player) ClearPreload() error {
	p.mu.Lock()
	defer p.mu.Unlock()

	if !p.currentLoaded {
		p.nextLoaded = false
		p.nextPath = ""
		p.nextEntryID = 0
		return nil
	}
	if err := p.ensureStartedLocked(); err != nil {
		p.startErr = err
		return err
	}
	return p.clearPreloadLocked()
}

func (p *Player) Pause() {
	p.mu.Lock()
	defer p.mu.Unlock()
	if err := p.ensureStartedLocked(); err != nil {
		p.startErr = err
		return
	}
	_, _ = p.commandLocked("set_property", "pause", true)
	if p.currentLoaded {
		p.state = "pause"
	}
}

func (p *Player) Resume() {
	p.mu.Lock()
	defer p.mu.Unlock()
	if err := p.ensureStartedLocked(); err != nil {
		p.startErr = err
		return
	}
	_, _ = p.commandLocked("set_property", "pause", false)
	if p.currentLoaded {
		p.state = "play"
	}
}

func (p *Player) Stop() {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.generation++
	if p.conn != nil {
		_ = p.clearPlaylistLocked()
	}
	p.currentLoaded = false
	p.nextLoaded = false
	p.currentPath = ""
	p.nextPath = ""
	p.currentEntryID = 0
	p.nextEntryID = 0
	p.state = "stop"
}

func (p *Player) Seek(seconds float64) error {
	p.mu.Lock()
	defer p.mu.Unlock()
	if err := p.ensureStartedLocked(); err != nil {
		p.startErr = err
		return err
	}
	_, err := p.commandLocked("seek", seconds, "absolute", "exact")
	return err
}

func (p *Player) SetVolume(level float64) {
	if level < 0 {
		level = 0
	}
	if level > 100 {
		level = 100
	}

	p.mu.Lock()
	defer p.mu.Unlock()
	p.volume = level
	if err := p.ensureStartedLocked(); err != nil {
		p.startErr = err
		return
	}
	_, _ = p.commandLocked("set_property", "volume", level)
}

func (p *Player) SetReplayGain(mode string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.replayGain = normalizeReplayGain(mode)
	if err := p.ensureStartedLocked(); err != nil {
		p.startErr = err
		return
	}
	_ = p.setReplayGainLocked(p.replayGain)
}

func (p *Player) State() (state string, elapsed, duration float64, vol float64) {
	p.mu.Lock()
	defer p.mu.Unlock()

	state = p.state
	vol = p.volume
	if p.conn == nil || !p.currentLoaded {
		return state, 0, 0, vol
	}

	if paused, err := p.getBoolPropertyLocked("pause"); err == nil {
		if paused {
			state = "pause"
		} else {
			state = "play"
		}
		p.state = state
	}
	if v, err := p.getFloatPropertyLocked("time-pos"); err == nil {
		elapsed = v
	}
	if v, err := p.getFloatPropertyLocked("duration"); err == nil {
		duration = v
	}
	if v, err := p.getFloatPropertyLocked("volume"); err == nil {
		vol = v
		p.volume = v
	}
	return state, elapsed, duration, vol
}

func (p *Player) StartPositionReporter(interval time.Duration, fn func(state string, elapsed, duration, vol float64)) {
	if interval <= 0 {
		interval = 2 * time.Second
	}
	go func() {
		t := time.NewTicker(interval)
		defer t.Stop()
		for range t.C {
			p.mu.Lock()
			closed := p.closed
			p.mu.Unlock()
			if closed {
				return
			}
			fn(p.State())
		}
	}()
}

func (p *Player) Close() {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.closed = true
	p.generation++
	if p.conn != nil {
		_ = p.commandNoWaitLocked("quit")
		_ = p.conn.Close()
		p.conn = nil
	}
	if p.cmd != nil && p.cmd.Process != nil {
		_ = p.cmd.Process.Kill()
	}
	p.currentLoaded = false
	p.nextLoaded = false
	p.state = "stop"
}

func (p *Player) clearPlaylistLocked() error {
	if p.conn == nil && p.commandHook == nil {
		return nil
	}
	_, err := p.commandLocked("playlist-clear")
	p.currentLoaded = false
	p.nextLoaded = false
	p.currentPath = ""
	p.nextPath = ""
	p.currentEntryID = 0
	p.nextEntryID = 0
	return err
}

func (p *Player) clearPreloadLocked() error {
	if p.conn == nil && p.commandHook == nil {
		p.nextLoaded = false
		p.nextPath = ""
		p.nextEntryID = 0
		return nil
	}
	for {
		count, err := p.playlistCountLocked()
		if err != nil {
			return err
		}
		if count <= 1 {
			break
		}
		if _, err := p.commandLocked("playlist-remove", 1); err != nil {
			return err
		}
	}
	p.nextLoaded = false
	p.nextPath = ""
	p.nextEntryID = 0
	return nil
}

func (p *Player) playlistCountLocked() (int, error) {
	resp, err := p.commandLocked("get_property", "playlist-count")
	if err != nil {
		return 0, err
	}
	return intFromAny(resp.Data)
}

func (p *Player) playlistEntryIDLocked(index int) (int64, error) {
	resp, err := p.commandLocked("get_property", fmt.Sprintf("playlist/%d/id", index))
	if err != nil {
		return 0, err
	}
	return int64FromAny(resp.Data)
}

func (p *Player) getFloatPropertyLocked(name string) (float64, error) {
	resp, err := p.commandLocked("get_property", name)
	if err != nil {
		return 0, err
	}
	return floatFromAny(resp.Data)
}

func (p *Player) getBoolPropertyLocked(name string) (bool, error) {
	resp, err := p.commandLocked("get_property", name)
	if err != nil {
		return false, err
	}
	v, ok := resp.Data.(bool)
	if !ok {
		return false, fmt.Errorf("property %s is not bool", name)
	}
	return v, nil
}

func (p *Player) setReplayGainLocked(mode string) error {
	_, err := p.commandLocked("set_property", "replaygain", normalizeReplayGain(mode))
	return err
}

func (p *Player) commandNoWaitLocked(args ...any) error {
	if p.conn == nil {
		return nil
	}
	req := struct {
		Command []any `json:"command"`
	}{Command: args}
	data, err := json.Marshal(req)
	if err != nil {
		return err
	}
	data = append(data, '\n')
	p.writeMu.Lock()
	defer p.writeMu.Unlock()
	_, err = p.conn.Write(data)
	return err
}

func normalizeReplayGain(mode string) string {
	switch strings.ToLower(strings.TrimSpace(mode)) {
	case "track":
		return "track"
	case "album":
		return "album"
	default:
		return "no"
	}
}

func intFromAny(v any) (int, error) {
	switch n := v.(type) {
	case float64:
		return int(n), nil
	case int:
		return n, nil
	default:
		return 0, fmt.Errorf("not an int: %T", v)
	}
}

func int64FromAny(v any) (int64, error) {
	switch n := v.(type) {
	case float64:
		return int64(n), nil
	case int:
		return int64(n), nil
	case int64:
		return n, nil
	default:
		return 0, fmt.Errorf("not an int64: %T", v)
	}
}

func floatFromAny(v any) (float64, error) {
	switch n := v.(type) {
	case float64:
		return n, nil
	case int:
		return float64(n), nil
	case nil:
		return 0, nil
	default:
		return 0, fmt.Errorf("not a float: %T", v)
	}
}
