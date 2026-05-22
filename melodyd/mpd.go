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
			matched := false
			if len(c.idleSubs) == 0 {
				matched = true // watching all
			} else {
				for _, s := range subsystems {
					if _, ok := c.idleSubs[s]; ok {
						matched = true
						break
					}
				}
			}
			if matched {
				select {
				case c.idleCh <- subsystems:
				default:
					// Channel full, merge — client will get the notification
				}
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

	idleMu   sync.Mutex
	idling   bool
	idleSubs map[string]struct{}
	idleCh   chan []string
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
	c.idleMu.Lock()
	c.idling = true
	c.idleSubs = make(map[string]struct{})
	for _, s := range subs {
		c.idleSubs[s] = struct{}{}
	}
	c.idleCh = make(chan []string, 1)
	c.idleMu.Unlock()

	c.app.mpdHub.register(c)
	defer func() {
		c.app.mpdHub.unregister(c)
		c.idleMu.Lock()
		c.idling = false
		c.idleMu.Unlock()
	}()

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

	select {
	case changed := <-c.idleCh:
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
		// Drain the reader goroutine — it will get the next command
		// which we need to handle after idle returns
		select {
		case nextLine := <-lineCh:
			if nextLine != "" && nextLine != "noidle" {
				cmd, args := parseCommand(nextLine)
				if err := c.dispatch(cmd, args); err != nil {
					c.writeACK(err)
				} else {
					c.writeLine("OK")
				}
				c.flush()
			}
		default:
			// Reader hasn't produced a line yet, that's fine
		}

	case line := <-lineCh:
		if line == "" {
			return // connection closed
		}
		if line == "noidle" {
			c.writeLine("OK")
			c.flush()
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

	devID := "agent-" + name
	at := &agentTarget{
		reader: c.reader,
		writer: c.writer,
		conn:   c.conn,
		alive:  true,
		done:   make(chan struct{}),
	}

	dev := &device{
		ID:         devID,
		Name:       name,
		Address:    safeRemoteAddr(c.conn),
		IsLocal:    false,
		Type:       "agent",
		Format:     format,
		MaxBitRate: maxBitRate,
		LastSeen:   time.Now(),
	}

	c.app.devicesMu.Lock()
	// Close old agent with same name if it exists
	if oldAt, ok := c.app.agentTargets[devID]; ok {
		oldAt.close()
		c.app.logger.Printf("agent replaced: %s (old connection closed)", name)
	}
	c.app.devices[devID] = dev
	c.app.agentTargets[devID] = at
	c.app.devicesMu.Unlock()

	c.app.logger.Printf("agent registered: %s (id=%s, addr=%s)", name, devID, dev.Address)
	c.writeLine("OK")
	c.flush()
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
			c.app.activeDevice = "local"
		}
		c.app.logger.Printf("agent disconnected: %s", name)
	} else {
		c.app.logger.Printf("agent replaced (stale cleanup skipped): %s", name)
	}
	c.app.devicesMu.Unlock()
	c.app.mpdHub.notify(SubOutput)
}

// ---------------------------------------------------------------------------
// agentTarget — controls a remote agent over a persistent MPD-style connection
// ---------------------------------------------------------------------------

type agentTarget struct {
	mu        sync.Mutex
	reader    *bufio.Reader
	writer    *bufio.Writer
	conn      net.Conn
	alive     bool
	done      chan struct{}
	closeOnce sync.Once
}

func (at *agentTarget) close() {
	at.closeOnce.Do(func() {
		at.mu.Lock()
		at.alive = false
		at.mu.Unlock()
		at.conn.Close()
		close(at.done)
	})
}

func (at *agentTarget) sendCommand(cmdLine string) ([]string, error) {
	at.mu.Lock()
	defer at.mu.Unlock()
	if !at.alive {
		return nil, fmt.Errorf("agent disconnected")
	}
	at.conn.SetDeadline(time.Now().Add(10 * time.Second))
	defer at.conn.SetDeadline(time.Time{})

	fmt.Fprintf(at.writer, "%s\n", cmdLine)
	if err := at.writer.Flush(); err != nil {
		at.alive = false
		return nil, err
	}
	var lines []string
	for {
		line, err := at.reader.ReadString('\n')
		if err != nil {
			at.alive = false
			return nil, err
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "OK" {
			return lines, nil
		}
		if strings.HasPrefix(line, "ACK") {
			return nil, fmt.Errorf("%s", line)
		}
		lines = append(lines, line)
	}
}

func (at *agentTarget) loadFile(url, mode string, meta map[string]any) error {
	_, err := at.sendCommand(fmt.Sprintf("loadfile %s %s", mpdQuoteArg(url), mpdQuoteArg(mode)))
	return err
}

func (at *agentTarget) playlistClear() error {
	_, err := at.sendCommand("playlist_clear")
	return err
}

func (at *agentTarget) playlistRemove(index int) error {
	_, err := at.sendCommand(fmt.Sprintf("playlist_remove %d", index))
	return err
}

func (at *agentTarget) playlistMove(from, to int) error {
	_, err := at.sendCommand(fmt.Sprintf("playlist_move %d %d", from, to))
	return err
}

func (at *agentTarget) getProperty(name string) (any, error) {
	lines, err := at.sendCommand(fmt.Sprintf("get_property %s", mpdQuoteArg(name)))
	if err != nil {
		return nil, err
	}
	for _, l := range lines {
		if strings.HasPrefix(l, "value: ") {
			return parseAgentValue(strings.TrimPrefix(l, "value: ")), nil
		}
	}
	return nil, nil
}

func (at *agentTarget) setProperty(name string, value any) error {
	_, err := at.sendCommand(fmt.Sprintf("set_property %s %s", mpdQuoteArg(name), mpdQuoteArg(fmt.Sprint(value))))
	return err
}

func (at *agentTarget) command(args ...any) (*mpvResponse, error) {
	if len(args) == 0 {
		return nil, fmt.Errorf("empty command")
	}
	cmd := fmt.Sprintf("%v", args[0])
	switch cmd {
	case "playlist-next":
		_, err := at.sendCommand("mpv_command playlist-next")
		return &mpvResponse{}, err
	case "playlist-prev":
		_, err := at.sendCommand("mpv_command playlist-prev")
		return &mpvResponse{}, err
	case "playlist-clear":
		return &mpvResponse{}, at.playlistClear()
	case "playlist-move":
		if len(args) >= 3 {
			return &mpvResponse{}, at.playlistMove(intFromAny(args[1], 0), intFromAny(args[2], 0))
		}
		return &mpvResponse{}, nil
	default:
		return nil, fmt.Errorf("unsupported remote command: %s", cmd)
	}
}

func (at *agentTarget) handoff(playlistPos int, timePos float64, paused bool) error {
	_, err := at.sendCommand(fmt.Sprintf("handoff %d %f %t", playlistPos, timePos, paused))
	return err
}

func (at *agentTarget) isRunning() bool {
	at.mu.Lock()
	alive := at.alive
	at.mu.Unlock()
	return alive
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
