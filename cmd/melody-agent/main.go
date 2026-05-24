package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/BurntSushi/toml"
)

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

type agentConfig struct {
	Agent struct {
		Name             string `toml:"name"`
		Master           string `toml:"master"` // host:port of master's MPD server
		Format           string `toml:"format"`
		MaxBitRate       int    `toml:"max_bitrate"`
		ResumeOnConnect  bool   `toml:"resume_on_connect"`
	} `toml:"agent"`
	MPV struct {
		Socket     string `toml:"socket"`
		Executable string `toml:"executable"`
		ReplayGain string `toml:"replaygain"`
	} `toml:"mpv"`
}

// ---------------------------------------------------------------------------
// mpv IPC
// ---------------------------------------------------------------------------

type mpvRequest struct {
	Command   []any `json:"command"`
	RequestID int   `json:"request_id"`
}

type mpvResponse struct {
	Data      any    `json:"data"`
	Error     string `json:"error"`
	RequestID int    `json:"request_id"`
}

type mpvClient struct {
	socketPath string
	executable string
	replaygain string
	mu         sync.Mutex
	process    *exec.Cmd
	reqID      int
}

func (m *mpvClient) isRunning() bool {
	conn, err := net.DialTimeout("unix", m.socketPath, 1*time.Second)
	if err != nil {
		return false
	}
	conn.Close()
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

	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(m.socketPath); err == nil {
			return nil
		}
		time.Sleep(100 * time.Millisecond)
	}
	return fmt.Errorf("mpv socket did not appear at %s", m.socketPath)
}

func (m *mpvClient) command(args ...any) (*mpvResponse, error) {
	m.mu.Lock()
	m.reqID++
	reqID := m.reqID
	m.mu.Unlock()

	conn, err := net.DialTimeout("unix", m.socketPath, 2*time.Second)
	if err != nil {
		return nil, fmt.Errorf("mpv connect: %w", err)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(5 * time.Second))

	req := mpvRequest{Command: args, RequestID: reqID}
	data, err := json.Marshal(req)
	if err != nil {
		return nil, err
	}
	data = append(data, '\n')
	if _, err := conn.Write(data); err != nil {
		return nil, fmt.Errorf("mpv write: %w", err)
	}

	scanner := bufio.NewScanner(conn)
	for scanner.Scan() {
		var resp mpvResponse
		if err := json.Unmarshal(scanner.Bytes(), &resp); err != nil {
			continue
		}
		if resp.RequestID == reqID {
			if resp.Error != "" && resp.Error != "success" {
				return nil, fmt.Errorf("mpv: %s", resp.Error)
			}
			return &resp, nil
		}
	}
	return nil, fmt.Errorf("mpv: no response")
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

func (m *mpvClient) loadFile(url string, mode string) error {
	_, err := m.command("loadfile", url, mode)
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

// ---------------------------------------------------------------------------
// Agent
// ---------------------------------------------------------------------------

type agent struct {
	cfg       agentConfig
	logger    *log.Logger
	mpv       *mpvClient
	trackEnd  chan struct{} // signals natural track end (mpv end-file eof)
}

func main() {
	logger := log.New(os.Stdout, "melody-agent: ", log.LstdFlags)

	cfg, err := loadAgentConfig()
	if err != nil {
		logger.Fatalf("load config: %v", err)
	}

	a := &agent{
		cfg:    cfg,
		logger: logger,
		mpv: &mpvClient{
			socketPath: cfg.MPV.Socket,
			executable: cfg.MPV.Executable,
			replaygain: cfg.MPV.ReplayGain,
		},
		trackEnd: make(chan struct{}, 1),
	}

	go a.ensureMPV()
	go a.watchMPVEvents()

	logger.Printf("agent %q connecting to master at %s", cfg.Agent.Name, cfg.Agent.Master)
	a.connectLoop()
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
	if cfg.MPV.Socket == "" {
		runtimeDir := getenvDefault("XDG_RUNTIME_DIR", filepath.Join(os.TempDir(), fmt.Sprintf("melody-%d", os.Getuid())))
		cfg.MPV.Socket = filepath.Join(runtimeDir, "melody", "agent-mpv.sock")
	}
	if cfg.MPV.Executable == "" {
		cfg.MPV.Executable = "mpv"
	}

	return cfg, nil
}

func defaultAgentConfig() string {
	hostname, _ := os.Hostname()
	return `[agent]
name = "` + hostname + `"
master = "localhost:6600"
format = ""
max_bitrate = 0
resume_on_connect = false

[mpv]
socket = ""
executable = "mpv"
replaygain = ""
`
}

func (a *agent) ensureMPV() {
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

// watchMPVEvents listens for mpv end-file events on a dedicated IPC connection.
// When a track ends naturally (reason=eof), it signals the agent to send
// trackended to the server.
func (a *agent) watchMPVEvents() {
	for {
		for !a.mpv.isRunning() {
			time.Sleep(1 * time.Second)
		}

		conn, err := net.DialTimeout("unix", a.mpv.socketPath, 2*time.Second)
		if err != nil {
			time.Sleep(2 * time.Second)
			continue
		}

		a.logger.Printf("mpv events: listening")
		scanner := bufio.NewScanner(conn)
		for scanner.Scan() {
			var evt struct {
				Event  string `json:"event"`
				Reason string `json:"reason"`
			}
			if err := json.Unmarshal(scanner.Bytes(), &evt); err != nil {
				continue
			}
			if evt.Event == "end-file" && evt.Reason == "eof" {
				a.logger.Printf("mpv events: track ended naturally")
				select {
				case a.trackEnd <- struct{}{}:
				default:
				}
			}
		}
		conn.Close()
		time.Sleep(1 * time.Second)
	}
}

// ---------------------------------------------------------------------------
// MPD protocol connection to master
// ---------------------------------------------------------------------------

func (a *agent) connectLoop() {
	for {
		if err := a.runSession(); err != nil {
			a.logger.Printf("session error: %v, reconnecting in 5s", err)
		}
		// Server disconnected — stop mpv playback immediately.
		// The agent must not play anything without a live server connection.
		_ = a.mpv.playlistClear()
		_ = a.mpv.setProperty("pause", true)
		time.Sleep(5 * time.Second)
	}
}

func (a *agent) runSession() error {
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

	// Send agent_register
	regCmd := fmt.Sprintf("agent_register %s", mpdQuote(a.cfg.Agent.Name))
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

	// Resume playback if configured
	if a.cfg.Agent.ResumeOnConnect {
		go a.sendResume()
	}

	// Watch for natural track ends and send trackended to server via separate connection
	done := make(chan struct{})
	defer close(done)
	go func() {
		for {
			select {
			case <-done:
				return
			case <-a.trackEnd:
				a.sendTrackEnded()
			}
		}
	}()

	// Command loop — master sends commands, we execute and respond
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			return fmt.Errorf("read command: %w", err)
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			continue
		}

		a.handleCommand(writer, line)
		if err := writer.Flush(); err != nil {
			return fmt.Errorf("write response: %w", err)
		}
	}
}

func (a *agent) sendTrackEnded() {
	conn, err := net.DialTimeout("tcp", a.cfg.Agent.Master, 5*time.Second)
	if err != nil {
		a.logger.Printf("trackended: dial failed: %v", err)
		return
	}
	defer conn.Close()

	r := bufio.NewReader(conn)
	// Read greeting
	if _, err := r.ReadString('\n'); err != nil {
		return
	}
	fmt.Fprintf(conn, "trackended\n")
	if _, err := r.ReadString('\n'); err != nil {
		return
	}
	fmt.Fprintf(conn, "close\n")
	a.logger.Printf("trackended: sent to server")
}

func (a *agent) sendResume() {
	conn, err := net.DialTimeout("tcp", a.cfg.Agent.Master, 5*time.Second)
	if err != nil {
		a.logger.Printf("resume-on-connect: dial failed: %v", err)
		return
	}
	defer conn.Close()

	r := bufio.NewReader(conn)
	// Read greeting
	if _, err := r.ReadString('\n'); err != nil {
		return
	}
	fmt.Fprintf(conn, "pause 0\n")
	if _, err := r.ReadString('\n'); err != nil {
		return
	}
	fmt.Fprintf(conn, "close\n")
	a.logger.Printf("resume-on-connect: sent unpause")
}

func (a *agent) handleCommand(w *bufio.Writer, line string) {
	cmd, args := parseCommand(line)

	switch cmd {
	case "ping":
		fmt.Fprintln(w, "OK")

	case "loadfile":
		if len(args) < 2 {
			fmt.Fprintln(w, "ACK [2@0] {loadfile} missing arguments")
			return
		}
		if err := a.mpv.loadFile(args[0], args[1]); err != nil {
			fmt.Fprintf(w, "ACK [56@0] {loadfile} %s\n", err)
			return
		}
		fmt.Fprintln(w, "OK")

	case "playlist_clear":
		if err := a.mpv.playlistClear(); err != nil {
			fmt.Fprintf(w, "ACK [56@0] {playlist_clear} %s\n", err)
			return
		}
		fmt.Fprintln(w, "OK")

	case "playlist_remove":
		if len(args) < 1 {
			fmt.Fprintln(w, "ACK [2@0] {playlist_remove} missing index")
			return
		}
		idx, _ := strconv.Atoi(args[0])
		if err := a.mpv.playlistRemove(idx); err != nil {
			fmt.Fprintf(w, "ACK [56@0] {playlist_remove} %s\n", err)
			return
		}
		fmt.Fprintln(w, "OK")

	case "playlist_move":
		if len(args) < 2 {
			fmt.Fprintln(w, "ACK [2@0] {playlist_move} missing arguments")
			return
		}
		from, _ := strconv.Atoi(args[0])
		to, _ := strconv.Atoi(args[1])
		if _, err := a.mpv.command("playlist-move", from, to); err != nil {
			fmt.Fprintf(w, "ACK [56@0] {playlist_move} %s\n", err)
			return
		}
		fmt.Fprintln(w, "OK")

	case "get_property":
		if len(args) < 1 {
			fmt.Fprintln(w, "ACK [2@0] {get_property} missing name")
			return
		}
		val, err := a.mpv.getProperty(args[0])
		if err != nil {
			fmt.Fprintf(w, "ACK [56@0] {get_property} %s\n", err)
			return
		}
		fmt.Fprintf(w, "value: %v\n", val)
		fmt.Fprintln(w, "OK")

	case "set_property":
		if len(args) < 2 {
			fmt.Fprintln(w, "ACK [2@0] {set_property} missing arguments")
			return
		}
		name := args[0]
		value := parsePropertyValue(args[1])
		if err := a.mpv.setProperty(name, value); err != nil {
			fmt.Fprintf(w, "ACK [56@0] {set_property} %s\n", err)
			return
		}
		fmt.Fprintln(w, "OK")

	case "mpv_command":
		if len(args) < 1 {
			fmt.Fprintln(w, "ACK [2@0] {mpv_command} missing command")
			return
		}
		mpvArgs := make([]any, len(args))
		for i, a := range args {
			mpvArgs[i] = a
		}
		if _, err := a.mpv.command(mpvArgs...); err != nil {
			fmt.Fprintf(w, "ACK [56@0] {mpv_command} %s\n", err)
			return
		}
		fmt.Fprintln(w, "OK")

	case "handoff":
		if len(args) < 3 {
			fmt.Fprintln(w, "ACK [2@0] {handoff} missing arguments")
			return
		}
		playlistPos, _ := strconv.Atoi(args[0])
		timePos, _ := strconv.ParseFloat(args[1], 64)
		paused := args[2] == "true" || args[2] == "1"
		a.doHandoff(playlistPos, timePos, paused)
		fmt.Fprintln(w, "OK")

	default:
		fmt.Fprintf(w, "ACK [5@0] {%s} unknown command\n", cmd)
	}
}

func (a *agent) doHandoff(playlistPos int, timePos float64, paused bool) {
	if err := a.mpv.setProperty("playlist-pos", playlistPos); err != nil {
		a.logger.Printf("handoff: set playlist-pos failed: %v", err)
	}

	// Wait for mpv to load the file
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		time.Sleep(50 * time.Millisecond)
		if v, err := a.mpv.getProperty("duration"); err == nil {
			if d, ok := v.(float64); ok && d > 0 {
				break
			}
		}
	}

	if timePos > 0 {
		if err := a.mpv.setProperty("time-pos", timePos); err != nil {
			a.logger.Printf("handoff: seek failed: %v", err)
		}
	}

	if err := a.mpv.setProperty("pause", paused); err != nil {
		a.logger.Printf("handoff: set pause failed: %v", err)
	}
}

// ---------------------------------------------------------------------------
// Command parsing (same as MPD server)
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

func parsePropertyValue(s string) any {
	if s == "true" {
		return true
	}
	if s == "false" {
		return false
	}
	if i, err := strconv.Atoi(s); err == nil {
		return i
	}
	if f, err := strconv.ParseFloat(s, 64); err == nil {
		return f
	}
	return s
}

func getenvDefault(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
