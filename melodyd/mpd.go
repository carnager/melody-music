package main

import (
	"bufio"
	"fmt"
	"log"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"
)

// MPD subsystem names for idle notifications.
const (
	SubPlayer         = "player"
	SubPlaylist       = "playlist"
	SubStoredPlaylist = "stored_playlist"
	SubDatabase       = "database"
	SubOutput         = "output"
	SubOptions        = "options"
	SubMixer          = "mixer"
	SubRating         = "rating"
)

// ---------------------------------------------------------------------------
// Notification hub
// ---------------------------------------------------------------------------

// notifyHub fans out subsystem change notifications to idle-waiting MPD connections.
type notifyHub struct {
	mu      sync.Mutex
	clients map[*mpdConn]struct{}
}

func newNotifyHub() *notifyHub {
	return &notifyHub{clients: make(map[*mpdConn]struct{})}
}

func (h *notifyHub) register(c *mpdConn) {
	h.mu.Lock()
	h.clients[c] = struct{}{}
	h.mu.Unlock()
}

func (h *notifyHub) unregister(c *mpdConn) {
	h.mu.Lock()
	delete(h.clients, c)
	h.mu.Unlock()
}

func (h *notifyHub) notify(subsystems ...string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for c := range h.clients {
		c.idleMu.Lock()
		if c.idling {
			// Check if this client is watching any of the changed subsystems
			var matched []string
			if len(c.idleSubs) == 0 {
				matched = subsystems // watching all
			} else {
				for _, s := range subsystems {
					if _, ok := c.idleSubs[s]; ok {
						matched = append(matched, s)
					}
				}
			}
			if len(matched) > 0 {
				select {
				case c.idleCh <- matched:
				default:
					// Channel full — merge new subsystems into pending so
					// the next idle picks them up immediately.
					if c.pendingSubs == nil {
						c.pendingSubs = make(map[string]struct{})
					}
					for _, s := range matched {
						c.pendingSubs[s] = struct{}{}
					}
				}
			}
		} else {
			// Not idle — buffer the events so the next idle returns immediately
			if c.pendingSubs == nil {
				c.pendingSubs = make(map[string]struct{})
			}
			for _, s := range subsystems {
				c.pendingSubs[s] = struct{}{}
			}
		}
		c.idleMu.Unlock()
	}
}

// ---------------------------------------------------------------------------
// MPD error type
// ---------------------------------------------------------------------------

type mpdError struct {
	code int
	pos  int
	cmd  string
	msg  string
}

func (e *mpdError) Error() string {
	return fmt.Sprintf("ACK [%d@%d] {%s} %s", e.code, e.pos, e.cmd, e.msg)
}

func mpdErr(code int, cmd, msg string) *mpdError {
	return &mpdError{code: code, cmd: cmd, msg: msg}
}

// MPD error codes
const (
	errNotList    = 1
	errArg        = 2
	errPassword   = 3
	errPermission = 4
	errUnknown    = 5
	errNoExist    = 50
	errPlaylist   = 55
	errSystem     = 56
)

// ---------------------------------------------------------------------------
// MPD connection
// ---------------------------------------------------------------------------

type mpdConn struct {
	conn   net.Conn
	reader *bufio.Reader
	writer *bufio.Writer
	app    *app
	logger *log.Logger

	idleMu      sync.Mutex
	idling      bool
	idleSubs    map[string]struct{}
	idleCh      chan []string
	pendingSubs map[string]struct{} // events that arrived while not idle

	// Window support for search/find commands
	windowStart int
	windowEnd   int // -1 = no window
	windowPos   int
}

func (c *mpdConn) writeLine(line string) {
	fmt.Fprintf(c.writer, "%s\n", line)
}

func (c *mpdConn) writef(format string, args ...any) {
	fmt.Fprintf(c.writer, format, args...)
}

func (c *mpdConn) writeKV(key string, value any) {
	fmt.Fprintf(c.writer, "%s: %s\n", key, fmt.Sprint(value))
}

func (c *mpdConn) flush() {
	c.writer.Flush()
}

func (c *mpdConn) writeACK(err *mpdError) {
	c.writeLine(err.Error())
}

// serve handles one MPD client connection.
func (c *mpdConn) serve() {
	defer c.conn.Close()
	c.app.mpdHub.register(c)
	defer c.app.mpdHub.unregister(c)

	c.writeLine("OK MPD 0.23.5")
	c.flush()

	for {
		line, err := c.reader.ReadString('\n')
		if err != nil {
			return
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			continue
		}

		if line == "command_list_begin" || line == "command_list_ok_begin" {
			c.handleCommandList(line == "command_list_ok_begin")
			continue
		}

		cmd, args := parseCommand(line)

		if cmd == "idle" {
			c.handleIdle(args)
			continue
		}
		if cmd == "close" {
			return
		}
		if cmd == "agent_register" {
			if len(args) < 1 {
				c.writeACK(mpdErr(errArg, "agent_register", "name required"))
				c.flush()
				continue
			}
			c.handleAgentRegister(args)
			return // connection taken over by agentTarget
		}

		if err := c.dispatch(cmd, args); err != nil {
			c.writeACK(err)
		} else {
			c.writeLine("OK")
		}
		c.flush()
	}
}

func (c *mpdConn) dispatch(cmd string, args []string) *mpdError {
	handler, ok := commandTable[cmd]
	if !ok {
		return mpdErr(errUnknown, cmd, "unknown command")
	}
	return handler(c, args)
}

func (c *mpdConn) handleCommandList(withOK bool) {
	var commands []struct {
		cmd  string
		args []string
	}
	for {
		line, err := c.reader.ReadString('\n')
		if err != nil {
			return
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "command_list_end" {
			break
		}
		cmd, args := parseCommand(line)
		commands = append(commands, struct {
			cmd  string
			args []string
		}{cmd, args})
	}
	for i, entry := range commands {
		if err := c.dispatch(entry.cmd, entry.args); err != nil {
			err.pos = i
			c.writeACK(err)
			c.flush()
			return
		}
		if withOK {
			c.writeLine("list_OK")
		}
	}
	c.writeLine("OK")
	c.flush()
}

func (c *mpdConn) handleIdle(subs []string) {
	// Loop instead of recursing to prevent unbounded stack growth when
	// clients rapidly re-enter idle (e.g. on rapid track changes).
	for {
		c.idleMu.Lock()

		// Check for pending events that arrived while we were not idle.
		var pendingMatch []string
		if len(c.pendingSubs) > 0 {
			watchAll := len(subs) == 0
			for s := range c.pendingSubs {
				if watchAll {
					pendingMatch = append(pendingMatch, s)
				} else {
					for _, sub := range subs {
						if s == sub {
							pendingMatch = append(pendingMatch, s)
							break
						}
					}
				}
			}
			if len(pendingMatch) > 0 {
				c.pendingSubs = nil
			}
		}

		c.idling = true
		c.idleSubs = make(map[string]struct{})
		for _, s := range subs {
			c.idleSubs[s] = struct{}{}
		}
		c.idleCh = make(chan []string, 1)
		c.idleMu.Unlock()

		// Deliver pending events through the normal channel so the select
		// below handles them like any other notification.
		if len(pendingMatch) > 0 {
			c.idleCh <- pendingMatch
		}

		// Set a generous read deadline so that silently-dropped clients
		// don't block this goroutine forever.
		c.conn.SetReadDeadline(time.Now().Add(5 * time.Minute))

		// Read next line in a goroutine so we can select between notification and noidle
		lineCh := make(chan string, 1)
		go func() {
			line, err := c.reader.ReadString('\n')
			if err != nil {
				lineCh <- ""
				return
			}
			lineCh <- strings.TrimRight(line, "\r\n")
		}()

		continueIdle := false

		select {
		case changed := <-c.idleCh:
			// Immediately mark as non-idle so any notifications that arrive
			// while we wait for the reader goroutine go to pendingSubs instead
			// of being sent to idleCh (which nobody reads from here).
			c.idleMu.Lock()
			c.idling = false
			c.idleMu.Unlock()

			// Deduplicate subsystems
			seen := map[string]bool{}
			for _, s := range changed {
				if !seen[s] {
					seen[s] = true
					c.writef("changed: %s\n", s)
				}
			}
			c.writeLine("OK")
			c.flush()
			// Wait for the reader goroutine to finish — it may have already
			// consumed the next line from the connection.
			nextLine := <-lineCh
			if nextLine != "" && nextLine != "noidle" {
				cmd, args := parseCommand(nextLine)
				if cmd == "idle" {
					// Instead of recursing, loop with new subs
					subs = args
					continueIdle = true
				} else if err := c.dispatch(cmd, args); err != nil {
					c.writeACK(err)
					c.flush()
				} else {
					c.writeLine("OK")
					c.flush()
				}
			}

		case line := <-lineCh:
			c.idleMu.Lock()
			c.idling = false
			c.idleMu.Unlock()

			if line == "" {
				c.conn.SetReadDeadline(time.Time{})
				return // connection closed
			}
			if line == "noidle" {
				c.writeLine("OK")
				c.flush()
				c.conn.SetReadDeadline(time.Time{})
				return
			}
			// Client sent a real command instead of noidle — handle it
			c.writeLine("OK") // end idle with no changes
			cmd, args := parseCommand(line)
			if err := c.dispatch(cmd, args); err != nil {
				c.writeACK(err)
			} else {
				c.writeLine("OK")
			}
			c.flush()
		}

		c.conn.SetReadDeadline(time.Time{})
		if !continueIdle {
			return
		}
	}
}

// ---------------------------------------------------------------------------
// Command parsing
// ---------------------------------------------------------------------------

// parseCommand splits an MPD command line into command name and arguments.
// Handles quoted arguments: add "Artist/Album/Track.flac"
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
// MPD TCP server
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Agent registration
// ---------------------------------------------------------------------------

func (c *mpdConn) handleAgentRegister(args []string) {
	name := args[0]

	// Parse optional key=value args
	format := ""
	maxBitRate := 0
	for _, arg := range args[1:] {
		if strings.HasPrefix(arg, "format=") {
			format = strings.TrimPrefix(arg, "format=")
		} else if strings.HasPrefix(arg, "max_bitrate=") {
			n, _ := strconv.Atoi(strings.TrimPrefix(arg, "max_bitrate="))
			maxBitRate = n
		}
	}

	// If this agent has the same name as the server, it's the embedded local agent.
	// Use "local" as its device ID so it replaces the local mpv target.
	isLocal := name == c.app.cfg.Server.Name
	devID := "agent-" + name
	if isLocal {
		devID = "local"
	}

	at := &agentTarget{
		writer:  c.writer,
		conn:    c.conn,
		alive:   true,
		done:    make(chan struct{}),
		app:     c.app,
		devID:   devID,
		respCh:  make(chan agentResp, 1),
		agState: "stop",
	}

	dev := &device{
		ID:         devID,
		Name:       name,
		Address:    safeRemoteAddr(c.conn),
		IsLocal:    isLocal,
		Type:       "agent",
		Format:     format,
		MaxBitRate: maxBitRate,
		LastSeen:   time.Now(),
	}

	c.app.devicesMu.Lock()
	// Close old agent with same name if it exists
	wasActive := c.app.activeDevice == devID || (isLocal && strings.HasPrefix(c.app.activeDevice, coreAudioDevicePrefix))
	if oldAt, ok := c.app.agentTargets[devID]; ok {
		oldAt.close()
		c.app.logger.Printf("agent replaced: %s (old connection closed)", name)
	}
	c.app.devices[devID] = dev
	c.app.agentTargets[devID] = at
	// Auto-activate the local embedded agent if no device is active
	if isLocal && c.app.activeDevice == "" {
		c.app.activeDevice = devID
		wasActive = true
	}
	c.app.devicesMu.Unlock()

	c.app.logger.Printf("agent registered: %s (id=%s, addr=%s)", name, devID, dev.Address)
	c.writeLine("OK")
	c.flush()

	// Start reader goroutine — processes all incoming messages from agent
	go at.readLoop(c.reader)

	// If this agent was the active device before reconnecting, reload the
	// play queue into it so playback continues seamlessly.
	if wasActive {
		c.app.reloadQueueIntoAgent(at, dev)
	}

	c.app.mpdHub.notify(SubOutput)

	// Keepalive: ping agent periodically to detect disconnection
	go func() {
		for {
			time.Sleep(15 * time.Second)
			if _, err := at.sendCommand("ping"); err != nil {
				at.close()
				return
			}
		}
	}()

	// Block until agent disconnects
	<-at.done

	// Clean up — only remove if we're still the registered agent (a newer
	// connection may have already replaced us)
	c.app.devicesMu.Lock()
	if c.app.agentTargets[devID] == at {
		delete(c.app.devices, devID)
		delete(c.app.agentTargets, devID)
		if c.app.activeDevice == devID {
			// Fall back to another connected device, preferring "local"
			c.app.activeDevice = ""
			if _, ok := c.app.agentTargets["local"]; ok {
				c.app.activeDevice = "local"
			} else {
				for id, a := range c.app.agentTargets {
					if a.isRunning() {
						c.app.activeDevice = id
						break
					}
				}
			}
		}
		c.app.logger.Printf("agent disconnected: %s", name)
	} else {
		c.app.logger.Printf("agent replaced (stale cleanup skipped): %s", name)
	}
	c.app.devicesMu.Unlock()
	c.app.mpdHub.notify(SubOutput, SubPlayer)
}

// ---------------------------------------------------------------------------
// agentTarget — controls a remote autonomous agent over a persistent connection.
//
// The agent handles its own audio playback (decoding, gapless, ReplayGain).
// Communication is bidirectional on a single TCP connection:
//   Server → Agent: play, preload, pause, resume, stop, seek, volume, replaygain, queue_changed, ping
//   Agent → Server: agent_state (periodic), agent_advance (track end)
//
// A reader goroutine processes all incoming messages. Async messages
// (agent_state, agent_advance) are handled inline or in goroutines.
// Command responses (OK/ACK) are routed to a channel for sendCommand.
// ---------------------------------------------------------------------------

type agentTarget struct {
	cmdMu     sync.Mutex // serializes sendCommand (write + wait for response)
	writer    *bufio.Writer
	conn      net.Conn
	alive     bool
	done      chan struct{}
	closeOnce sync.Once
	app       *app
	devID     string

	// Response channel — reader goroutine sends command responses here
	respCh chan agentResp

	// Cached state from periodic agent_state messages
	stateMu     sync.RWMutex
	agState     string // "play", "pause", "stop"
	agPos       int
	agElapsed   float64
	agDuration  float64
	agVolume    float64
	agStateTime time.Time // when state was last received (for interpolation)

	// Queue sync tracking — avoids redundant syncs
	lastSyncVersion int
}

type agentResp struct {
	lines []string
	err   error
}

func (at *agentTarget) close() {
	at.closeOnce.Do(func() {
		at.alive = false
		at.conn.Close()
		close(at.done)
	})
}

// readLoop processes all incoming messages from the agent.
// Must be run as a goroutine. Calls close() on error.
func (at *agentTarget) readLoop(reader *bufio.Reader) {
	defer at.close()
	var pendingLines []string
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			return
		}
		line = strings.TrimRight(line, "\r\n")

		// Handle async agent messages
		if strings.HasPrefix(line, "agent_state ") {
			at.handleAgentState(line)
			continue
		}
		if strings.HasPrefix(line, "agent_advance ") {
			go at.handleAgentAdvance(line)
			continue
		}

		// Command response
		if line == "OK" {
			select {
			case at.respCh <- agentResp{lines: pendingLines}:
			default:
				// No pending command — discard stale response
			}
			pendingLines = nil
			continue
		}
		if strings.HasPrefix(line, "ACK") {
			select {
			case at.respCh <- agentResp{err: fmt.Errorf("%s", line)}:
			default:
			}
			pendingLines = nil
			continue
		}
		pendingLines = append(pendingLines, line)
	}
}

// handleAgentState parses and caches the agent's periodic state report.
// Format: agent_state <state> <pos> <elapsed> <duration> <volume>
func (at *agentTarget) handleAgentState(line string) {
	parts := strings.Fields(line)
	if len(parts) < 6 {
		return
	}
	newState := parts[1]
	newPos, _ := strconv.Atoi(parts[2])
	newElapsed, _ := strconv.ParseFloat(parts[3], 64)
	newDuration, _ := strconv.ParseFloat(parts[4], 64)
	newVolume, _ := strconv.ParseFloat(parts[5], 64)

	at.stateMu.Lock()
	// Only notify idle clients when something meaningful changed —
	// not on every 2-second heartbeat which just updates elapsed time.
	changed := at.agState != newState || at.agPos != newPos ||
		at.agDuration != newDuration || at.agVolume != newVolume
	at.agState = newState
	at.agPos = newPos
	at.agElapsed = newElapsed
	at.agDuration = newDuration
	at.agVolume = newVolume
	at.agStateTime = time.Now()
	at.stateMu.Unlock()

	if changed {
		at.app.mpdHub.notify(SubPlayer)
	}
}

// handleAgentAdvance is called when the agent reports a natural track end.
// It triggers the server's track advance logic (queue state, consume, preload next).
func (at *agentTarget) handleAgentAdvance(line string) {
	parts := strings.Fields(line)
	if len(parts) < 2 {
		return
	}
	oldPos, _ := strconv.Atoi(parts[1])
	at.app.logger.Printf("agent advance: track ended at pos %d", oldPos)
	at.app.advanceTrack()
}

// sendCommand sends a command to the agent and waits for the response.
// Serialized by cmdMu so only one command is in flight at a time.
func (at *agentTarget) sendCommand(cmdLine string) ([]string, error) {
	at.cmdMu.Lock()
	defer at.cmdMu.Unlock()
	if !at.alive {
		return nil, fmt.Errorf("agent disconnected")
	}
	fmt.Fprintf(at.writer, "%s\n", cmdLine)
	if err := at.writer.Flush(); err != nil {
		at.alive = false
		return nil, err
	}
	select {
	case resp := <-at.respCh:
		return resp.lines, resp.err
	case <-at.done:
		return nil, fmt.Errorf("agent disconnected")
	case <-time.After(10 * time.Second):
		at.alive = false
		return nil, fmt.Errorf("agent response timeout")
	}
}

// ensureQueueSync sends queue_changed to the agent if the server's queue
// version has changed since the last sync. Retries if the version changed
// during the sync to prevent TOCTOU races.
func (at *agentTarget) ensureQueueSync() {
	for {
		at.app.playQueueMu.Lock()
		ver := at.app.queueVersion
		at.app.playQueueMu.Unlock()

		if ver == at.lastSyncVersion {
			return
		}

		if _, err := at.sendCommand("queue_changed"); err != nil {
			at.app.logger.Printf("agent queue sync failed: %v", err)
			return
		}

		// Check if version changed during sync — if so, sync again
		at.app.playQueueMu.Lock()
		currentVer := at.app.queueVersion
		at.app.playQueueMu.Unlock()

		at.lastSyncVersion = currentVer
		if currentVer == ver {
			return // no changes during sync, we're good
		}
		// Version changed during sync — loop to re-sync
	}
}

// agentPlay tells the agent to play a queue position with optional next track preload.
func (at *agentTarget) agentPlay(curPos, nextPos int) error {
	return at.agentPlayAt(curPos, nextPos, -1)
}

// agentPlayAt tells the agent to play a queue position, optionally seeking to
// a position before audio output starts.
func (at *agentTarget) agentPlayAt(curPos, nextPos int, seekPos float64) error {
	at.ensureQueueSync()
	cmd := fmt.Sprintf("play %d", curPos)
	if nextPos >= 0 {
		cmd += fmt.Sprintf(" next=%d", nextPos)
	}
	if seekPos > 0 {
		cmd += fmt.Sprintf(" seek=%.3f", seekPos)
	}
	_, err := at.sendCommand(cmd)
	return err
}

// agentPreload tells the agent to preload a queue position for gapless playback.
func (at *agentTarget) agentPreload(nextPos int) error {
	at.ensureQueueSync()
	_, err := at.sendCommand(fmt.Sprintf("preload %d", nextPos))
	return err
}

// ---------------------------------------------------------------------------
// playbackTarget interface implementation
// ---------------------------------------------------------------------------

func (at *agentTarget) loadFile(url, mode string, meta map[string]any) error {
	// Not used for autonomous agents — use agentPlay/agentPreload instead.
	return nil
}

func (at *agentTarget) loadFileBatch(urls []string, mode string) error {
	return nil
}

func (at *agentTarget) playlistClear() error {
	_, err := at.sendCommand("stop")
	return err
}

func (at *agentTarget) playlistRemove(index int) error {
	// Agent manages its own playlist — no-op.
	return nil
}

func (at *agentTarget) playlistMove(from, to int) error {
	return nil
}

func (at *agentTarget) getProperty(name string) (any, error) {
	at.stateMu.RLock()
	defer at.stateMu.RUnlock()
	switch name {
	case "pause":
		return at.agState != "play", nil
	case "time-pos":
		elapsed := at.agElapsed
		// Interpolate position if playing
		if at.agState == "play" && !at.agStateTime.IsZero() {
			elapsed += time.Since(at.agStateTime).Seconds()
			if at.agDuration > 0 && elapsed > at.agDuration {
				elapsed = at.agDuration
			}
		}
		return elapsed, nil
	case "duration":
		return at.agDuration, nil
	case "volume":
		return at.agVolume, nil
	default:
		return nil, nil
	}
}

func (at *agentTarget) setProperty(name string, value any) error {
	switch name {
	case "pause":
		if b, ok := value.(bool); ok {
			if b {
				_, err := at.sendCommand("pause")
				return err
			}
			_, err := at.sendCommand("resume")
			return err
		}
	case "time-pos":
		if f, ok := value.(float64); ok {
			_, err := at.sendCommand(fmt.Sprintf("seek %f", f))
			if err == nil {
				// Update cached state immediately so interpolation
				// starts from the new position without waiting for
				// the next periodic agent_state report.
				at.stateMu.Lock()
				at.agElapsed = f
				at.agStateTime = time.Now()
				at.stateMu.Unlock()
			}
			return err
		}
	case "volume":
		if f, ok := value.(float64); ok {
			_, err := at.sendCommand(fmt.Sprintf("volume %f", f))
			return err
		}
	case "replaygain":
		_, err := at.sendCommand(fmt.Sprintf("replaygain %s", fmt.Sprint(value)))
		return err
	}
	return nil
}

func (at *agentTarget) isRunning() bool {
	return at.alive
}

// mpdQuoteArg quotes a string for the agent protocol if it contains special characters.
func safeRemoteAddr(conn net.Conn) string {
	defer func() { recover() }()
	if addr := conn.RemoteAddr(); addr != nil {
		return addr.String()
	}
	return "unknown"
}

func mpdQuoteArg(s string) string {
	if strings.ContainsAny(s, " \t\"\\") {
		escaped := strings.ReplaceAll(s, `\`, `\\`)
		escaped = strings.ReplaceAll(escaped, `"`, `\"`)
		return `"` + escaped + `"`
	}
	return s
}

// parseAgentValue converts a string response value back to a Go type.
func parseAgentValue(s string) any {
	if s == "true" {
		return true
	}
	if s == "false" {
		return false
	}
	if s == "<nil>" || s == "" {
		return nil
	}
	if f, err := strconv.ParseFloat(s, 64); err == nil {
		return f
	}
	return s
}

func (a *app) serveMPD() error {
	addr := fmt.Sprintf("0.0.0.0:%d", a.cfg.MPD.Port)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("mpd listen: %w", err)
	}
	a.logger.Printf("mpd: listening on %s", addr)

	for {
		conn, err := ln.Accept()
		if err != nil {
			a.logger.Printf("mpd: accept error: %v", err)
			continue
		}
		// Enable TCP keep-alive so the OS detects silently-dropped clients
		// (e.g. mobile network switches) instead of leaking goroutines.
		if tc, ok := conn.(*net.TCPConn); ok {
			tc.SetKeepAlive(true)
			tc.SetKeepAlivePeriod(30 * time.Second)
		}
		c := &mpdConn{
			conn:   conn,
			reader: bufio.NewReader(conn),
			writer: bufio.NewWriter(conn),
			app:    a,
			logger: a.logger,
		}
		go c.serve()
	}
}
