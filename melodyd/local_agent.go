package main

import (
	"bufio"
	"bytes"
	"fmt"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/carnager/melody/internal/player"
)

// localAgent is an in-process audio player that connects to the server's MPD
// protocol via net.Pipe(). It replaces mpv for local playback.
type localAgent struct {
	app    *app
	player *player.Player

	// Queue state (synced from server)
	queueMu sync.Mutex
	queue   []queueItem
	curPos  int

	audioDevice string

	// Control connection to server (via net.Pipe)
	ctrlMu sync.Mutex
	ctrlW  *bufio.Writer
}

type queueItem struct {
	Position int
	File     string
	SongID   string
	Duration float64
	RGTrack  float64
	RGAlbum  float64
}

// startLocalAgent creates and runs an embedded agent.
// It connects to the MPD server via an in-memory pipe.
func (a *app) startLocalAgent() {
	p := player.New()

	la := &localAgent{
		app:    a,
		player: p,
		curPos: -1,
	}

	if a.cfg.Player.ReplayGain != "" {
		p.SetReplayGain(a.cfg.Player.ReplayGain)
	}

	p.OnTrackEnd = la.handleTrackEnd
	p.StartPositionReporter(2*time.Second, la.reportState)

	for {
		if err := la.runSession(); err != nil {
			a.logger.Printf("local-agent: session error: %v, restarting in 2s", err)
		}
		la.player.Stop()
		la.curPos = -1
		time.Sleep(2 * time.Second)
	}
}

func (la *localAgent) runSession() error {
	// Create in-memory pipe
	agentConn, serverConn := net.Pipe()
	defer agentConn.Close()

	// Hand server end to the MPD handler
	c := &mpdConn{
		conn:   serverConn,
		reader: bufio.NewReader(serverConn),
		writer: bufio.NewWriter(serverConn),
		app:    la.app,
		logger: la.app.logger,
	}
	go c.serve()

	reader := bufio.NewReader(agentConn)
	writer := bufio.NewWriter(agentConn)

	// Read MPD greeting
	greeting, err := reader.ReadString('\n')
	if err != nil {
		return fmt.Errorf("read greeting: %w", err)
	}
	if !strings.HasPrefix(greeting, "OK MPD") {
		return fmt.Errorf("unexpected greeting: %s", strings.TrimSpace(greeting))
	}

	// Register as local agent
	fmt.Fprintf(writer, "agent_register %s v2\n", localMPDQuote(la.app.cfg.Server.Name))
	if err := writer.Flush(); err != nil {
		return fmt.Errorf("write register: %w", err)
	}

	resp, err := reader.ReadString('\n')
	if err != nil {
		return fmt.Errorf("register response: %w", err)
	}
	if strings.TrimRight(resp, "\r\n") != "OK" {
		return fmt.Errorf("register failed: %s", strings.TrimSpace(resp))
	}
	la.app.logger.Printf("local-agent: registered as %q", la.app.cfg.Server.Name)

	la.ctrlMu.Lock()
	la.ctrlW = writer
	la.ctrlMu.Unlock()

	defer func() {
		la.ctrlMu.Lock()
		la.ctrlW = nil
		la.ctrlMu.Unlock()
	}()

	// Initial queue sync
	if err := la.syncQueue(); err != nil {
		la.app.logger.Printf("local-agent: initial queue sync failed: %v", err)
	}

	// Command loop
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
		la.handleCommand(respW, line)
		respW.Flush()

		la.ctrlMu.Lock()
		writer.Write(respBuf.Bytes())
		writer.Flush()
		la.ctrlMu.Unlock()
	}
}

// ---------------------------------------------------------------------------
// Command handling
// ---------------------------------------------------------------------------

func (la *localAgent) handleCommand(w *bufio.Writer, line string) {
	cmd, args := parseCommand(line)

	switch cmd {
	case "ping":
		fmt.Fprintln(w, "OK")

	case "play":
		la.handlePlay(w, args)

	case "preload":
		la.handlePreload(w, args)

	case "pause":
		la.player.Pause()
		fmt.Fprintln(w, "OK")

	case "resume":
		la.player.Resume()
		fmt.Fprintln(w, "OK")

	case "stop":
		la.player.Stop()
		la.curPos = -1
		fmt.Fprintln(w, "OK")

	case "seek":
		if len(args) < 1 {
			fmt.Fprintln(w, "ACK [2@0] {seek} missing seconds")
			return
		}
		secs, _ := strconv.ParseFloat(args[0], 64)
		if err := la.player.Seek(secs); err != nil {
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
		la.player.SetVolume(level)
		fmt.Fprintln(w, "OK")

	case "replaygain":
		if len(args) < 1 {
			fmt.Fprintln(w, "ACK [2@0] {replaygain} missing mode")
			return
		}
		la.player.SetReplayGain(args[0])
		fmt.Fprintln(w, "OK")

	case "audio_device":
		if len(args) < 1 {
			fmt.Fprintln(w, "ACK [2@0] {audio_device} missing device")
			return
		}
		la.app.logger.Printf("local-agent: selecting audio output %s", args[0])
		if err := la.player.SetAudioDevice(args[0]); err != nil {
			la.app.logger.Printf("local-agent: select audio output %s failed: %v", args[0], err)
			fmt.Fprintf(w, "ACK [56@0] {audio_device} %s\n", err)
			return
		}
		actual, err := la.player.AudioDevice()
		if err != nil {
			la.app.logger.Printf("local-agent: selected audio output %s; readback failed: %v", args[0], err)
		} else {
			la.app.logger.Printf("local-agent: selected audio output requested=%s active=%s", args[0], actual)
		}
		if la.audioDevice != args[0] {
			la.audioDevice = args[0]
		}
		fmt.Fprintln(w, "OK")

	case "queue_changed":
		if err := la.syncQueue(); err != nil {
			fmt.Fprintf(w, "ACK [56@0] {queue_changed} %s\n", err)
			return
		}
		fmt.Fprintln(w, "OK")

	case "get_property":
		if len(args) < 1 {
			fmt.Fprintln(w, "ACK [2@0] {get_property} missing name")
			return
		}
		la.handleGetProperty(w, args[0])

	case "set_property":
		if len(args) < 2 {
			fmt.Fprintln(w, "ACK [2@0] {set_property} missing arguments")
			return
		}
		la.handleSetProperty(w, args[0], args[1])

	default:
		fmt.Fprintf(w, "ACK [5@0] {%s} unknown command\n", cmd)
	}
}

func (la *localAgent) handlePlay(w *bufio.Writer, args []string) {
	if len(args) < 1 {
		fmt.Fprintln(w, "ACK [2@0] {play} missing queue position")
		return
	}

	pos, err := strconv.Atoi(args[0])
	if err != nil {
		fmt.Fprintln(w, "ACK [2@0] {play} invalid position")
		return
	}

	nextPos := -1
	var seekPos float64 = -1
	for _, arg := range args[1:] {
		if strings.HasPrefix(arg, "next=") {
			nextPos, _ = strconv.Atoi(strings.TrimPrefix(arg, "next="))
		} else if strings.HasPrefix(arg, "seek=") {
			seekPos, _ = strconv.ParseFloat(strings.TrimPrefix(arg, "seek="), 64)
		}
	}

	la.queueMu.Lock()
	if pos < 0 || pos >= len(la.queue) {
		la.queueMu.Unlock()
		fmt.Fprintf(w, "ACK [50@0] {play} position %d out of range\n", pos)
		return
	}
	item := la.queue[pos]
	var nextItem *queueItem
	if nextPos >= 0 && nextPos < len(la.queue) {
		ni := la.queue[nextPos]
		nextItem = &ni
	}
	la.queueMu.Unlock()

	path := la.resolveTrackPath(item)
	if path == "" {
		fmt.Fprintf(w, "ACK [50@0] {play} cannot resolve path for position %d\n", pos)
		return
	}

	if err := la.player.Play(path, item.File, item.RGTrack, item.RGAlbum); err != nil {
		fmt.Fprintf(w, "ACK [56@0] {play} %s\n", err)
		return
	}
	la.curPos = pos

	// Seek before any audio reaches the speaker (within the speaker buffer window)
	if seekPos > 0 {
		la.player.Seek(seekPos)
	}

	if nextItem != nil {
		nextPath := la.resolveTrackPath(*nextItem)
		if nextPath != "" {
			if err := la.player.Preload(nextPath, nextItem.File, nextItem.RGTrack, nextItem.RGAlbum); err != nil {
				la.app.logger.Printf("local-agent: preload pos %d failed: %v", nextPos, err)
			}
		}
	}

	fmt.Fprintln(w, "OK")
}

func (la *localAgent) handlePreload(w *bufio.Writer, args []string) {
	if len(args) < 1 {
		fmt.Fprintln(w, "ACK [2@0] {preload} missing queue position")
		return
	}

	pos, err := strconv.Atoi(args[0])
	if err != nil || pos < 0 {
		fmt.Fprintln(w, "ACK [2@0] {preload} invalid position")
		return
	}

	la.queueMu.Lock()
	if pos >= len(la.queue) {
		la.queueMu.Unlock()
		fmt.Fprintf(w, "ACK [50@0] {preload} position %d out of range\n", pos)
		return
	}
	item := la.queue[pos]
	la.queueMu.Unlock()

	path := la.resolveTrackPath(item)
	if path == "" {
		fmt.Fprintf(w, "ACK [50@0] {preload} cannot resolve path for position %d\n", pos)
		return
	}

	if err := la.player.Preload(path, item.File, item.RGTrack, item.RGAlbum); err != nil {
		fmt.Fprintf(w, "ACK [56@0] {preload} %s\n", err)
		return
	}
	fmt.Fprintln(w, "OK")
}

func (la *localAgent) handleGetProperty(w *bufio.Writer, name string) {
	state, elapsed, duration, vol := la.player.State()
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

func (la *localAgent) handleSetProperty(w *bufio.Writer, name, rawValue string) {
	switch name {
	case "pause":
		if rawValue == "true" || rawValue == "1" || rawValue == "yes" {
			la.player.Pause()
		} else {
			la.player.Resume()
		}
	case "time-pos":
		secs, _ := strconv.ParseFloat(rawValue, 64)
		la.player.Seek(secs)
	case "volume":
		vol, _ := strconv.ParseFloat(rawValue, 64)
		la.player.SetVolume(vol)
	case "replaygain":
		la.player.SetReplayGain(rawValue)
	default:
		fmt.Fprintf(w, "ACK [56@0] {set_property} unknown property: %s\n", name)
		return
	}
	fmt.Fprintln(w, "OK")
}

// ---------------------------------------------------------------------------
// Track end handling
// ---------------------------------------------------------------------------

func (la *localAgent) handleTrackEnd() {
	la.queueMu.Lock()
	qLen := len(la.queue)
	la.queueMu.Unlock()

	if qLen == 0 {
		return
	}

	oldPos := la.curPos
	la.sendToServer(fmt.Sprintf("agent_advance %d", oldPos))
}

// ---------------------------------------------------------------------------
// State reporting
// ---------------------------------------------------------------------------

func (la *localAgent) reportState(state string, elapsed, duration, vol float64) {
	la.sendToServer(fmt.Sprintf("agent_state %s %d %.3f %.3f %.0f",
		state, la.curPos, elapsed, duration, vol))
}

func (la *localAgent) sendToServer(msg string) {
	la.ctrlMu.Lock()
	defer la.ctrlMu.Unlock()

	if la.ctrlW == nil {
		return
	}
	fmt.Fprintf(la.ctrlW, "%s\n", msg)
	la.ctrlW.Flush()
}

// ---------------------------------------------------------------------------
// Queue sync — reads directly from server state (in-process)
// ---------------------------------------------------------------------------

func (la *localAgent) syncQueue() error {
	la.app.playQueueMu.Lock()
	songIDs := make([]string, len(la.app.playQueue))
	copy(songIDs, la.app.playQueue)
	la.app.playQueueMu.Unlock()

	// Batch fetch all track info in a single query
	ids := make([]int64, 0, len(songIDs))
	for _, sid := range songIDs {
		id, err := strconv.ParseInt(sid, 10, 64)
		if err != nil {
			continue
		}
		ids = append(ids, id)
	}
	infoMap, err := la.app.db.trackPlayInfoByIDs(ids)
	if err != nil {
		return err
	}

	items := make([]queueItem, 0, len(songIDs))
	for i, sid := range songIDs {
		id, _ := strconv.ParseInt(sid, 10, 64)
		info, ok := infoMap[id]
		if !ok {
			continue
		}
		items = append(items, queueItem{
			Position: i,
			File:     info.Path,
			SongID:   sid,
			Duration: info.Duration,
			RGTrack:  info.RGTrack,
			RGAlbum:  info.RGAlbum,
		})
	}

	la.queueMu.Lock()
	la.queue = items
	la.queueMu.Unlock()

	return nil
}

// ---------------------------------------------------------------------------
// Track path resolution — local agent always uses direct file access
// ---------------------------------------------------------------------------

func (la *localAgent) resolveTrackPath(item queueItem) string {
	return item.File
}

func localMPDQuote(s string) string {
	escaped := strings.ReplaceAll(s, `\`, `\\`)
	escaped = strings.ReplaceAll(escaped, `"`, `\"`)
	return `"` + escaped + `"`
}
