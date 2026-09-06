package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
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

	// Sort support for search/find commands ("sort [-]TAG"). Empty = no sort.
	sortTag  string
	sortDesc bool

	// added-since / modified-since filters (unix seconds, 0 = inactive)
	addedSince    int64
	modifiedSince int64

	// enqueueMode selects how matched tracks are queued by the enqueue command
	// ("add", "insert", or "replace"). Empty means the default "add" behaviour
	// used by findadd/searchadd.
	enqueueMode string
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

	// 0.24 signals support for the Added song attribute, "sort Added" on
	// find/search, and the added-since filter.
	c.writeLine("OK MPD 0.24.0")
	c.flush()

	for {
		line, err := c.reader.ReadString('\n')
		if err != nil {
			return
		}
		line = strings.TrimRight(line, "\r\n")
		if !c.handleLine(line) {
			return
		}
	}
}

func (c *mpdConn) handleLine(line string) bool {
	if line == "" {
		return true
	}

	if line == "command_list_begin" || line == "command_list_ok_begin" {
		c.handleCommandList(line == "command_list_ok_begin")
		return true
	}

	cmd, args := parseCommand(line)

	if cmd == "idle" {
		return c.handleIdle(args)
	}
	if cmd == "close" {
		return false
	}
	if cmd == "agent_register" {
		if len(args) < 1 {
			c.writeACK(mpdErr(errArg, "agent_register", "name required"))
			c.flush()
			return true
		}
		c.handleAgentRegister(args)
		return false // connection taken over by agentTarget
	}

	if err := c.dispatch(cmd, args); err != nil {
		c.writeACK(err)
	} else {
		c.writeLine("OK")
	}
	c.flush()
	return true
}

func (c *mpdConn) setIdle(active bool) {
	c.idleMu.Lock()
	c.idling = active
	c.idleMu.Unlock()
}

func (c *mpdConn) finishIdle() {
	c.setIdle(false)
}

func (c *mpdConn) endIdleWithOK() {
	c.finishIdle()
	c.writeLine("OK")
	c.flush()
}

func (c *mpdConn) writeIdleChanges(changed []string) {
	c.finishIdle()
	seen := map[string]bool{}
	for _, s := range changed {
		if !seen[s] {
			seen[s] = true
			c.writef("changed: %s\n", s)
		}
	}
	c.writeLine("OK")
	c.flush()
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

func (c *mpdConn) handleIdle(subs []string) bool {
	// Loop instead of recursing to prevent unbounded stack growth when
	// clients rapidly re-enter idle after every notification.
	for {
		pendingMatch := c.beginIdle(subs)
		if len(pendingMatch) > 0 {
			c.writeIdleChanges(pendingMatch)
			return true
		}

		lineCh := make(chan string, 1)
		go func() {
			line, err := c.reader.ReadString('\n')
			if err != nil {
				lineCh <- ""
				return
			}
			lineCh <- strings.TrimRight(line, "\r\n")
		}()

		select {
		case changed := <-c.idleCh:
			c.writeIdleChanges(changed)

			// The reader goroutine owns the socket read while idle is active.
			// If it consumed the client's next command, route that line through
			// the same dispatcher as the main serve loop so command lists,
			// close, and agent registration keep their normal semantics.
			nextLine := <-lineCh
			if nextLine == "" || nextLine == "noidle" {
				return nextLine != ""
			}
			cmd, args := parseCommand(nextLine)
			if cmd == "idle" {
				subs = args
				continue
			}
			return c.handleLine(nextLine)

		case line := <-lineCh:
			c.endIdleWithOK()
			if line == "" {
				return false
			}
			if line == "noidle" {
				return true
			}
			return c.handleLine(line)
		}
	}
}

func (c *mpdConn) beginIdle(subs []string) []string {
	c.idleMu.Lock()
	defer c.idleMu.Unlock()

	var pendingMatch []string
	if len(c.pendingSubs) > 0 {
		watchAll := len(subs) == 0
		for s := range c.pendingSubs {
			if watchAll {
				pendingMatch = append(pendingMatch, s)
				continue
			}
			for _, sub := range subs {
				if s == sub {
					pendingMatch = append(pendingMatch, s)
					break
				}
			}
		}
		if len(pendingMatch) > 0 {
			c.pendingSubs = nil
		}
	}

	c.idling = true
	c.idleSubs = make(map[string]struct{}, len(subs))
	for _, s := range subs {
		c.idleSubs[s] = struct{}{}
	}
	c.idleCh = make(chan []string, 1)
	return pendingMatch
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
	// quoted tracks whether the current token contained an explicit quote, so an
	// empty quoted argument (e.g. `find file ""`) is preserved as a real empty
	// argument instead of being dropped as if it were absent.
	quoted := false

	emit := func() {
		if current.Len() == 0 && !quoted {
			return
		}
		if first {
			cmd = current.String()
			first = false
		} else {
			args = append(args, current.String())
		}
		current.Reset()
		quoted = false
	}

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
			quoted = true
			continue
		}
		if r == ' ' && !inQuote {
			emit()
			continue
		}
		current.WriteRune(r)
	}
	emit()
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
	instanceID := ""
	for _, arg := range args[1:] {
		if strings.HasPrefix(arg, "format=") {
			format = strings.TrimPrefix(arg, "format=")
		} else if strings.HasPrefix(arg, "max_bitrate=") {
			n, _ := strconv.Atoi(strings.TrimPrefix(arg, "max_bitrate="))
			maxBitRate = n
		} else if strings.HasPrefix(arg, "instance=") {
			instanceID = strings.TrimPrefix(arg, "instance=")
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
		writer:     c.writer,
		conn:       c.conn,
		alive:      true,
		done:       make(chan struct{}),
		app:        c.app,
		devID:      devID,
		instanceID: instanceID,
		respCh:     make(chan agentResp, 1),
		agState:    "stop",
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
	var resume *agentResume
	if oldAt, ok := c.app.agentTargets[devID]; ok {
		resume = oldAt.playbackSnapshot()
		if oldAt.instanceID != "" && instanceID != "" && oldAt.instanceID != instanceID {
			c.app.logger.Printf("WARNING: agent %s replaced by a DIFFERENT process (old instance=%s, new instance=%s) — two agents may be running with the same name, fighting over the registration", name, oldAt.instanceID, instanceID)
		}
		oldAt.closeWithReason("replaced by a new connection from the same agent")
		c.app.logger.Printf("agent replaced: %s (old connection closed)", name)
	}
	// No live predecessor, but the agent may have dropped off earlier while
	// enabled — pick up where that connection left off.
	if resume == nil {
		resume = c.app.agentResumes[devID]
	}
	delete(c.app.agentResumes, devID)
	// Cached transport states of the other enabled outputs decide how this
	// agent joins playback; computed before inserting it so it cannot see
	// itself, and from caches so no IPC runs under the lock.
	othersPlaying := false
	othersPaused := false
	for id, other := range c.app.agentTargets {
		if id == devID || !c.app.enabledOutputs[id] {
			continue
		}
		switch other.cachedState() {
		case "play":
			othersPlaying = true
		case "pause":
			othersPaused = true
		}
	}
	c.app.devices[devID] = dev
	c.app.agentTargets[devID] = at
	// Auto-enable the local embedded agent if nothing is enabled yet
	if isLocal && len(c.app.enabledOutputs) == 0 {
		c.app.enabledOutputs[devID] = true
	}
	isEnabled := c.app.enabledOutputs[devID]
	// Take the clock if nothing holds it, or if the holder is itself offline —
	// a primary that never came back would otherwise ignore this agent's
	// track-end reports and stall the queue.
	if isEnabled && (c.app.primaryOutput == "" || c.app.targetForLocked(c.app.primaryOutput) == nil) {
		c.app.primaryOutput = devID
	}
	c.app.devicesMu.Unlock()
	if isEnabled {
		c.app.persistOutputs()
	}

	if isEnabled {
		c.app.logger.Printf("agent registered: %s (id=%s, addr=%s, output enabled — rejoining playback)", name, devID, dev.Address)
	} else {
		c.app.logger.Printf("agent registered: %s (id=%s, addr=%s)", name, devID, dev.Address)
	}
	c.writeLine("OK")
	c.flush()

	// Start reader goroutine — processes all incoming messages from agent
	go at.readLoop(c.reader)

	// If this agent is in the enabled set (it reconnected, or its enable
	// survived a daemon restart), reload the play queue into it so playback
	// continues seamlessly.
	if isEnabled {
		c.app.reloadQueueIntoAgent(at, dev, resume, othersPlaying, othersPaused)
	}

	c.app.mpdHub.notify(SubOutput)

	// Keepalive: ping the agent periodically to detect a dead connection.
	// A single missed ping is not proof of death — a phone on mobile data can
	// stall past the response timeout and recover — so only give up after two
	// in a row. A ping that times out is not treated as fatal to the
	// connection either, which is why it uses the non-fatal send.
	go func() {
		misses := 0
		for {
			time.Sleep(agentPingInterval)
			if _, err := at.sendCommandOpt("ping", false); err != nil {
				misses++
				if misses < agentPingMisses {
					c.app.logger.Printf("agent %s: keepalive miss %d/%d (%v)", name, misses, agentPingMisses, err)
					continue
				}
				at.closeWithReason(fmt.Sprintf("keepalive: %d missed pings (%v)", misses, err))
				return
			}
			misses = 0
		}
	}()

	// Block until agent disconnects
	<-at.done

	c.app.releaseAgent(devID, name, at)
}

// releaseAgent cleans up after an agent connection ends. It only acts if the
// connection is still the registered one — a newer connection may have already
// replaced it.
//
// A dropped connection is not the user turning the output off, so the device
// keeps its place in enabledOutputs (MPD never resets outputs) and stays listed
// as an offline output, which targetForLocked reports by returning nil. The
// agent's last playback position is stashed so its reconnect resumes mid-track
// rather than restarting it.
func (a *app) releaseAgent(devID, name string, at *agentTarget) {
	a.devicesMu.Lock()
	if a.agentTargets[devID] == at {
		delete(a.agentTargets, devID)
		if a.enabledOutputs[devID] {
			if snap := at.playbackSnapshot(); snap != nil {
				a.agentResumes[devID] = snap
			}
			if d, ok := a.devices[devID]; ok {
				d.LastSeen = time.Now()
			}
			a.logger.Printf("agent disconnected: %s — %s (output stays enabled, offline until it returns)", name, at.disconnectReason())
		} else {
			delete(a.devices, devID)
			a.logger.Printf("agent disconnected: %s — %s", name, at.disconnectReason())
		}
		if a.primaryOutput == devID {
			// Other enabled outputs keep playing; hand the clock to one of them.
			a.promotePrimaryLocked()
		}
	} else {
		a.logger.Printf("agent replaced (stale cleanup skipped): %s", name)
	}
	a.devicesMu.Unlock()
	a.mpdHub.notify(SubOutput, SubPlayer)
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

// Keepalive and command timing. An agent is only declared dead after
// agentPingMisses consecutive unanswered pings, so a transient stall on a
// mobile connection does not cost it its place in playback.
// Variables rather than constants so tests can shorten them.
var (
	agentCmdTimeout   = 10 * time.Second
	agentPingInterval = 15 * time.Second
)

const agentPingMisses = 2

type agentTarget struct {
	cmdMu     sync.Mutex // serializes sendCommand (write + wait for response)
	writer    *bufio.Writer
	conn      net.Conn
	alive     bool
	done      chan struct{}
	closeOnce sync.Once
	app       *app
	devID     string

	// Why the connection ended, for the disconnect log. First reason wins:
	// whatever noticed first is the cause, later observers see the aftermath.
	reasonMu    sync.Mutex
	closeReason string

	// Random per-process ID from agent_register (empty for older agents).
	// Used to detect two distinct processes registering under the same name.
	instanceID string

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

// agentResume captures where a replaced agent connection left off, so a
// reconnecting agent can continue playback instead of restarting the track.
type agentResume struct {
	state   string // "play" or "pause"
	pos     int
	elapsed float64
}

// isStopped reports whether the agent's last known state is "stop" (nothing loaded).
func (at *agentTarget) isStopped() bool {
	at.stateMu.RLock()
	defer at.stateMu.RUnlock()
	return at.agState == "stop"
}

// cachedState returns the agent's last reported transport state ("play",
// "pause", "stop", or "" before the first report), without any IPC.
func (at *agentTarget) cachedState() string {
	at.stateMu.RLock()
	defer at.stateMu.RUnlock()
	return at.agState
}

// playbackSnapshot returns the agent's last reported playback position,
// extrapolated to now for playing tracks. Returns nil if the agent was
// stopped or if the track would have ended by now.
func (at *agentTarget) playbackSnapshot() *agentResume {
	at.stateMu.RLock()
	defer at.stateMu.RUnlock()
	if at.agState != "play" && at.agState != "pause" {
		return nil
	}
	elapsed := at.agElapsed
	if at.agState == "play" && !at.agStateTime.IsZero() {
		elapsed += time.Since(at.agStateTime).Seconds()
	}
	if at.agDuration > 0 && elapsed >= at.agDuration {
		return nil
	}
	return &agentResume{state: at.agState, pos: at.agPos, elapsed: elapsed}
}

// closeWithReason records why the connection is ending, then closes it. The
// reason is reported by the disconnect log so a drop can be told apart from a
// keepalive timeout or a deliberate replacement after the fact.
func (at *agentTarget) closeWithReason(reason string) {
	at.reasonMu.Lock()
	if at.closeReason == "" {
		at.closeReason = reason
	}
	at.reasonMu.Unlock()
	at.close()
}

// disconnectReason returns the recorded cause, or a generic one if the
// connection ended without any observer naming it.
func (at *agentTarget) disconnectReason() string {
	at.reasonMu.Lock()
	defer at.reasonMu.Unlock()
	if at.closeReason == "" {
		return "connection closed"
	}
	return at.closeReason
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
	var pendingLines []string
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			if errors.Is(err, io.EOF) {
				at.closeWithReason("agent closed the connection")
			} else {
				at.closeWithReason(fmt.Sprintf("read error: %v", err))
			}
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

	// Only the primary output drives player notifications — non-primary
	// heartbeats would wake idle clients constantly for state that status
	// doesn't report.
	if changed && at.app.isPrimary(at.devID) {
		at.app.mpdHub.notify(SubPlayer)
	}
}

// handleAgentAdvance is called when the agent reports a natural track end.
// It triggers the server's track advance logic (queue state, consume, preload next).
// Only the primary output advances the queue — with several outputs enabled,
// each mpv reaches the track end and reports independently, and honoring all
// of them would advance the queue multiple times per track.
func (at *agentTarget) handleAgentAdvance(line string) {
	parts := strings.Fields(line)
	if len(parts) < 2 {
		return
	}
	oldPos, _ := strconv.Atoi(parts[1])
	if !at.app.isPrimary(at.devID) {
		at.app.logger.Printf("agent advance (ignored, non-primary %s): track ended at pos %d", at.devID, oldPos)
		return
	}
	at.app.logger.Printf("agent advance: track ended at pos %d", oldPos)
	at.app.advanceTrack()
}

// sendCommand sends a command to the agent and waits for the response.
// Serialized by cmdMu so only one command is in flight at a time. A timeout
// marks the agent dead — callers of real commands cannot do anything useful
// with an agent that stopped answering.
func (at *agentTarget) sendCommand(cmdLine string) ([]string, error) {
	return at.sendCommandOpt(cmdLine, true)
}

// sendCommandOpt is sendCommand with control over whether a response timeout
// marks the connection dead. The keepalive passes false so that one slow
// response — routine on a phone whose radio just stalled — does not by itself
// take the agent out of playback.
func (at *agentTarget) sendCommandOpt(cmdLine string, fatalTimeout bool) ([]string, error) {
	at.cmdMu.Lock()
	defer at.cmdMu.Unlock()
	if !at.alive {
		return nil, fmt.Errorf("agent disconnected")
	}
	// Discard any response left over from a command that timed out earlier —
	// it belongs to that command, not this one.
	select {
	case <-at.respCh:
	default:
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
	case <-time.After(agentCmdTimeout):
		if fatalTimeout {
			at.alive = false
		}
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

// agentPlay tells the agent to play a queue position with optional next track
// preload. With paused set, the agent loads the track without starting audio
// (agents predating the flag ignore it and start playing).
func (at *agentTarget) agentPlay(curPos, nextPos int, paused bool) error {
	return at.agentPlayAt(curPos, nextPos, -1, paused)
}

// agentPlayAt tells the agent to play a queue position, optionally seeking to
// a position before audio output starts.
func (at *agentTarget) agentPlayAt(curPos, nextPos int, seekPos float64, paused bool) error {
	at.ensureQueueSync()
	cmd := fmt.Sprintf("play %d next=%d", curPos, nextPos)
	if seekPos > 0 {
		cmd += fmt.Sprintf(" seek=%.3f", seekPos)
	}
	if paused {
		cmd += " paused=1"
	}
	_, err := at.sendCommand(cmd)
	if err == nil {
		// Update the cached state immediately — the next periodic
		// agent_state report is up to 2s away, and stopped/paused decisions
		// (cmdPlay resync, transportState) must not act on the old state.
		at.stateMu.Lock()
		if paused {
			at.agState = "pause"
		} else {
			at.agState = "play"
		}
		if seekPos > 0 {
			at.agElapsed = seekPos
		} else {
			at.agElapsed = 0
		}
		at.agStateTime = time.Now()
		at.stateMu.Unlock()
	}
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
	if err == nil {
		// Mark stopped immediately so a play issued right after (e.g. the
		// clear/add/play replace sequence) sees the output as stopped and
		// does a full resync instead of resuming a stale track.
		at.stateMu.Lock()
		at.agState = "stop"
		at.agElapsed = 0
		at.agStateTime = time.Now()
		at.stateMu.Unlock()
	}
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

func (at *agentTarget) getFreshProperty(name string) (any, error) {
	lines, err := at.sendCommand(fmt.Sprintf("get_property %s", name))
	if err != nil {
		return nil, err
	}
	for _, line := range lines {
		key, raw, ok := strings.Cut(line, ": ")
		if !ok || key != "value" {
			continue
		}
		switch name {
		case "pause":
			v := raw == "true" || raw == "1" || raw == "yes"
			at.stateMu.Lock()
			if v {
				at.agState = "pause"
			} else if at.agState == "pause" {
				at.agState = "play"
			}
			at.agStateTime = time.Now()
			at.stateMu.Unlock()
			return v, nil
		case "time-pos", "duration", "volume":
			v, err := strconv.ParseFloat(raw, 64)
			if err != nil {
				return nil, err
			}
			at.stateMu.Lock()
			switch name {
			case "time-pos":
				at.agElapsed = v
				at.agStateTime = time.Now()
			case "duration":
				at.agDuration = v
			case "volume":
				at.agVolume = v
			}
			at.stateMu.Unlock()
			return v, nil
		default:
			return raw, nil
		}
	}
	return nil, fmt.Errorf("agent property %s missing value", name)
}

func (at *agentTarget) setProperty(name string, value any) error {
	switch name {
	case "pause":
		if b, ok := value.(bool); ok {
			cmd := "resume"
			if b {
				cmd = "pause"
			}
			_, err := at.sendCommand(cmd)
			if err == nil {
				// Keep the cached state in sync ahead of the next periodic
				// report. A stopped agent stays stopped: pause/resume don't
				// load anything.
				at.stateMu.Lock()
				if b && at.agState == "play" {
					// Freeze interpolated progress at the pause point.
					if !at.agStateTime.IsZero() {
						at.agElapsed += time.Since(at.agStateTime).Seconds()
						if at.agDuration > 0 && at.agElapsed > at.agDuration {
							at.agElapsed = at.agDuration
						}
					}
					at.agState = "pause"
				} else if !b && at.agState == "pause" {
					at.agState = "play"
				}
				at.agStateTime = time.Now()
				at.stateMu.Unlock()
			}
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
		if f, ok := numericFloat(value); ok {
			_, err := at.sendCommand(fmt.Sprintf("volume %f", f))
			if err == nil {
				at.stateMu.Lock()
				at.agVolume = f
				at.stateMu.Unlock()
			}
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

func numericFloat(value any) (float64, bool) {
	switch v := value.(type) {
	case float64:
		return v, true
	case int:
		return float64(v), true
	default:
		return 0, false
	}
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
