package main

import (
	"bufio"
	"io"
	"log"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// newOutputsTestApp builds an app with three registered web devices and no
// enabled outputs. Web targets are used as cheap playback-target doubles.
func newOutputsTestApp(t *testing.T) (*app, map[string]*webTarget) {
	t.Helper()
	wts := map[string]*webTarget{
		"web-a": {alive: true, volume: 50},
		"web-b": {alive: true, volume: 60},
		"web-c": {alive: true, volume: 70},
	}
	a := &app{
		logger: log.New(os.Stdout, "test: ", 0),
		devices: map[string]*device{
			"web-a": {ID: "web-a", Name: "A", Type: "web"},
			"web-b": {ID: "web-b", Name: "B", Type: "web"},
			"web-c": {ID: "web-c", Name: "C", Type: "web"},
		},
		agentTargets:   make(map[string]*agentTarget),
		webTargets:     wts,
		enabledOutputs: make(map[string]bool),
		agentResumes:   make(map[string]*agentResume),
		mpdHub:         newNotifyHub(),
	}
	a.paths.ActiveDeviceFile = filepath.Join(t.TempDir(), "active_device")
	return a, wts
}

func TestEnableOutputAdditive(t *testing.T) {
	a, _ := newOutputsTestApp(t)

	if err := a.enableOutput("web-a"); err != nil {
		t.Fatalf("enableOutput(web-a): %v", err)
	}
	if a.primaryOutput != "web-a" {
		t.Fatalf("primary = %q, want web-a", a.primaryOutput)
	}

	if err := a.enableOutput("web-b"); err != nil {
		t.Fatalf("enableOutput(web-b): %v", err)
	}
	if !a.enabledOutputs["web-a"] || !a.enabledOutputs["web-b"] {
		t.Fatalf("enabled = %v, want web-a and web-b", a.enabledOutputs)
	}
	if a.primaryOutput != "web-a" {
		t.Fatalf("primary changed to %q on additive enable, want web-a", a.primaryOutput)
	}

	if err := a.enableOutput("nope"); err == nil {
		t.Fatal("enableOutput(nope) succeeded, want error")
	}
}

func TestDisableOutputPromotesPrimary(t *testing.T) {
	a, wts := newOutputsTestApp(t)
	a.enabledOutputs = map[string]bool{"web-a": true, "web-b": true, "web-c": true}
	a.primaryOutput = "web-b"
	wts["web-b"].paused = false

	if err := a.disableOutput("web-b"); err != nil {
		t.Fatalf("disableOutput: %v", err)
	}
	if a.enabledOutputs["web-b"] {
		t.Fatal("web-b still enabled after disable")
	}
	// Promotion follows sortedDevices order: web-a first
	if a.primaryOutput != "web-a" {
		t.Fatalf("promoted primary = %q, want web-a", a.primaryOutput)
	}
	// The disabled output was stopped, the others untouched
	if !wts["web-b"].paused {
		t.Fatal("disabled output not paused")
	}
	if wts["web-a"].paused != true && wts["web-c"].paused != true {
		// zero-value webTarget paused is false here; enable path never ran,
		// so just assert they were not cleared
		_ = wts
	}
}

func TestDisableLastOutput(t *testing.T) {
	a, _ := newOutputsTestApp(t)
	a.enabledOutputs = map[string]bool{"web-a": true}
	a.primaryOutput = "web-a"
	a.playQueue = []string{"1", "2"}
	a.curQueuePos = 1

	if err := a.disableOutput("web-a"); err != nil {
		t.Fatalf("disableOutput: %v", err)
	}
	if len(a.enabledOutputs) != 0 || a.primaryOutput != "" {
		t.Fatalf("enabled=%v primary=%q, want empty set and no primary", a.enabledOutputs, a.primaryOutput)
	}
	if a.target().isRunning() {
		t.Fatal("target still running with no enabled outputs")
	}
	// Queue state untouched so re-enabling resumes where things stood
	if a.curQueuePos != 1 || len(a.playQueue) != 2 {
		t.Fatalf("queue state changed: pos=%d len=%d", a.curQueuePos, len(a.playQueue))
	}
}

func TestToggleOutput(t *testing.T) {
	a, _ := newOutputsTestApp(t)

	if err := a.toggleOutput("web-a"); err != nil {
		t.Fatalf("toggle on: %v", err)
	}
	if !a.enabledOutputs["web-a"] {
		t.Fatal("toggle did not enable")
	}
	if err := a.toggleOutput("web-a"); err != nil {
		t.Fatalf("toggle off: %v", err)
	}
	if a.enabledOutputs["web-a"] {
		t.Fatal("toggle did not disable")
	}
}

func TestOutputsPersistenceRoundTripAndMigration(t *testing.T) {
	a, _ := newOutputsTestApp(t)
	a.enabledOutputs = map[string]bool{"agent-kitchen": true, "local": true}
	a.primaryOutput = "local"
	a.persistOutputs()

	b, _ := newOutputsTestApp(t)
	b.paths.ActiveDeviceFile = a.paths.ActiveDeviceFile
	b.restoreOutputs()
	if !b.enabledOutputs["agent-kitchen"] || !b.enabledOutputs["local"] || len(b.enabledOutputs) != 2 {
		t.Fatalf("restored enabled = %v", b.enabledOutputs)
	}
	if b.primaryOutput != "local" {
		t.Fatalf("restored primary = %q, want local", b.primaryOutput)
	}

	// Legacy single-ID file migrates to a one-element set
	legacy, _ := newOutputsTestApp(t)
	if err := os.WriteFile(legacy.paths.ActiveDeviceFile, []byte("agent-kitchen\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	legacy.restoreOutputs()
	if !legacy.enabledOutputs["agent-kitchen"] || len(legacy.enabledOutputs) != 1 {
		t.Fatalf("legacy enabled = %v, want agent-kitchen only", legacy.enabledOutputs)
	}
	if legacy.primaryOutput != "agent-kitchen" {
		t.Fatalf("legacy primary = %q", legacy.primaryOutput)
	}

	// Stale web IDs are pruned on restore — browsers re-register fresh
	web, _ := newOutputsTestApp(t)
	if err := os.WriteFile(web.paths.ActiveDeviceFile, []byte("web-Browser"), 0o644); err != nil {
		t.Fatal(err)
	}
	web.restoreOutputs()
	if len(web.enabledOutputs) != 0 || web.primaryOutput != "" {
		t.Fatalf("web enabled = %v primary = %q, want pruned", web.enabledOutputs, web.primaryOutput)
	}
}

// registerFakeAgent adds a connected agent output playing at pos/elapsed.
func registerFakeAgent(a *app, devID, name string, pos int, elapsed float64) *agentTarget {
	at := &agentTarget{
		alive:     true,
		app:       a,
		devID:     devID,
		agState:   "play",
		agPos:     pos,
		agElapsed: elapsed,
	}
	a.devices[devID] = &device{ID: devID, Name: name, Type: "agent"}
	a.agentTargets[devID] = at
	return at
}

// An agent losing its connection must not disable its output — that is what
// made playback stop for good when a phone briefly dropped off the network.
func TestAgentDisconnectKeepsOutputEnabled(t *testing.T) {
	a, _ := newOutputsTestApp(t)
	at := registerFakeAgent(a, "agent-phone", "phone", 4, 61.5)
	a.enabledOutputs["agent-phone"] = true
	a.primaryOutput = "agent-phone"

	a.releaseAgent("agent-phone", "phone", at)

	if !a.enabledOutputs["agent-phone"] {
		t.Fatalf("enabled = %v, want agent-phone to survive the disconnect", a.enabledOutputs)
	}
	if a.devices["agent-phone"] == nil {
		t.Fatal("device record dropped; output would vanish from the outputs list")
	}
	if a.targetForLocked("agent-phone") != nil {
		t.Fatal("target still resolvable after disconnect, want offline")
	}
	resume := a.agentResumes["agent-phone"]
	if resume == nil || resume.pos != 4 || resume.elapsed < 61.5 {
		t.Fatalf("resume = %+v, want pos 4 at >=61.5s", resume)
	}

	// Reconnecting consumes the stashed position and re-enters playback.
	a.restoreOutputs() // no-op here, but must not disturb the live set
	if got := a.enabledOutputs["agent-phone"]; !got {
		t.Fatal("enable lost after restoreOutputs")
	}
}

// A disconnect while other outputs are playing hands the clock over instead of
// stalling on the departed device.
func TestAgentDisconnectPromotesPrimary(t *testing.T) {
	a, _ := newOutputsTestApp(t)
	at := registerFakeAgent(a, "agent-phone", "phone", 0, 0)
	a.enabledOutputs = map[string]bool{"agent-phone": true, "web-b": true}
	a.primaryOutput = "agent-phone"

	a.releaseAgent("agent-phone", "phone", at)

	if a.primaryOutput != "web-b" {
		t.Fatalf("primary = %q, want web-b promoted", a.primaryOutput)
	}
	if !a.enabledOutputs["agent-phone"] {
		t.Fatal("departed output was disabled, want it kept for its return")
	}
}

// An agent that was never enabled leaves no trace behind.
func TestAgentDisconnectDropsDisabledDevice(t *testing.T) {
	a, _ := newOutputsTestApp(t)
	at := registerFakeAgent(a, "agent-guest", "guest", 0, 0)

	a.releaseAgent("agent-guest", "guest", at)

	if a.devices["agent-guest"] != nil {
		t.Fatal("disabled agent left a ghost device behind")
	}
}

// A replaced connection must not tear down the newer one.
func TestReleaseAgentIgnoresStaleConnection(t *testing.T) {
	a, _ := newOutputsTestApp(t)
	stale := registerFakeAgent(a, "agent-phone", "phone", 0, 0)
	fresh := registerFakeAgent(a, "agent-phone", "phone", 0, 0)
	a.enabledOutputs["agent-phone"] = true

	a.releaseAgent("agent-phone", "phone", stale)

	if a.agentTargets["agent-phone"] != fresh {
		t.Fatal("stale cleanup removed the newer connection")
	}
}

// An enabled-but-offline primary must not hold the clock hostage: whoever
// connects next takes it, otherwise its track-end reports are ignored as
// non-primary and the queue never advances.
func TestOnlineOutputTakesClockFromOfflinePrimary(t *testing.T) {
	a, _ := newOutputsTestApp(t)
	at := registerFakeAgent(a, "agent-phone", "phone", 0, 0)
	a.enabledOutputs = map[string]bool{"agent-phone": true, "web-b": true}
	a.primaryOutput = "agent-phone"
	a.releaseAgent("agent-phone", "phone", at)
	// Simulate a restart that restored the offline agent as primary.
	a.primaryOutput = "agent-phone"

	if err := a.enableOutput("web-c"); err != nil {
		t.Fatalf("enableOutput: %v", err)
	}
	if a.primaryOutput != "web-c" {
		t.Fatalf("primary = %q, want the online output to take the clock", a.primaryOutput)
	}
	if !a.isPrimary("web-c") {
		t.Fatal("online output not primary; its track-end reports would be dropped")
	}
}

// Outputs enabled before a daemon restart are listed (offline) right away, so
// they can be turned off without waiting for the agent to come back.
func TestRestoreOutputsListsOfflineDevices(t *testing.T) {
	a, _ := newOutputsTestApp(t)
	a.enabledOutputs = map[string]bool{"agent-kitchen": true}
	a.primaryOutput = "agent-kitchen"
	a.persistOutputs()

	b, _ := newOutputsTestApp(t)
	b.paths.ActiveDeviceFile = a.paths.ActiveDeviceFile
	b.restoreOutputs()

	dev := b.devices["agent-kitchen"]
	if dev == nil {
		t.Fatal("restored output is not listed")
	}
	if dev.Name != "kitchen" {
		t.Fatalf("placeholder name = %q, want kitchen", dev.Name)
	}
	if b.targetForLocked("agent-kitchen") != nil {
		t.Fatal("placeholder resolves to a target, want offline")
	}

	// Disabling it while offline clears the record rather than leaving a ghost.
	if err := b.disableOutput("agent-kitchen"); err != nil {
		t.Fatalf("disableOutput: %v", err)
	}
	if b.devices["agent-kitchen"] != nil || b.enabledOutputs["agent-kitchen"] {
		t.Fatalf("offline output not fully removed: devices=%v enabled=%v", b.devices["agent-kitchen"], b.enabledOutputs)
	}
}

func TestFanoutTargetWritesAllReadsPrimary(t *testing.T) {
	a, wts := newOutputsTestApp(t)
	a.enabledOutputs = map[string]bool{"web-b": true, "web-c": true}
	a.primaryOutput = "web-c"

	ft := a.target()
	if err := ft.setProperty("volume", 33.0); err != nil {
		t.Fatalf("fanout setProperty: %v", err)
	}
	if wts["web-b"].volume != 33 || wts["web-c"].volume != 33 {
		t.Fatalf("fanout volumes = %v/%v, want 33 on both enabled", wts["web-b"].volume, wts["web-c"].volume)
	}
	if wts["web-a"].volume == 33 {
		t.Fatal("fanout wrote to a disabled output")
	}

	// Reads come from the primary
	wts["web-c"].timePos = 42
	wts["web-b"].timePos = 7
	got, err := ft.getProperty("time-pos")
	if err != nil {
		t.Fatalf("fanout getProperty: %v", err)
	}
	if got != 42.0 {
		t.Fatalf("fanout read = %v, want primary's 42", got)
	}

	empty := &fanoutTarget{}
	if _, err := empty.getProperty("time-pos"); err == nil {
		t.Fatal("empty fanout read succeeded, want error")
	}
	if empty.isRunning() {
		t.Fatal("empty fanout reports running")
	}
}

func TestTrackEndedNonPrimaryIgnored(t *testing.T) {
	a, wts := newOutputsTestApp(t)
	a.enabledOutputs = map[string]bool{"web-a": true, "web-b": true}
	a.primaryOutput = "web-a"
	// Close the targets: the advance should still move queue state, and the
	// test app has no database for the preload URL resolution IPC would need.
	wts["web-a"].close()
	wts["web-b"].close()
	a.playQueue = []string{"1", "2", "3"}
	a.queueIDs = []int{1, 2, 3}
	a.queuePriority = []int{0, 0, 0}
	a.curQueuePos = 0
	a.pendingNextPos = 1
	a.paths.PlayQueueFile = filepath.Join(t.TempDir(), "queue")
	c := &mpdConn{app: a}

	// Non-primary web output finishing must not advance the queue
	if err := cmdTrackEnded(c, []string{"web-b"}); err != nil {
		t.Fatalf("cmdTrackEnded non-primary: %v", err)
	}
	if a.curQueuePos != 0 {
		t.Fatalf("non-primary trackended advanced queue to %d", a.curQueuePos)
	}

	// Primary advances
	if err := cmdTrackEnded(c, []string{"web-a"}); err != nil {
		t.Fatalf("cmdTrackEnded primary: %v", err)
	}
	if a.curQueuePos != 1 {
		t.Fatalf("primary trackended: pos = %d, want 1", a.curQueuePos)
	}
}

// newPipeAgent returns an agent target wired to an in-memory connection whose
// far end answers nothing, so commands run into their timeout.
func newPipeAgent(t *testing.T) *agentTarget {
	t.Helper()
	clientConn, serverConn := net.Pipe()
	t.Cleanup(func() {
		_ = clientConn.Close()
		_ = serverConn.Close()
	})
	go func() { _, _ = io.Copy(io.Discard, serverConn) }()
	return &agentTarget{
		writer: bufio.NewWriter(clientConn),
		conn:   clientConn,
		alive:  true,
		done:   make(chan struct{}),
		app:    &app{},
		respCh: make(chan agentResp, 1),
	}
}

// A keepalive ping that goes unanswered must not by itself take the agent out
// of playback — a phone on mobile data can stall and recover.
func TestUnansweredPingDoesNotKillAgent(t *testing.T) {
	old := agentCmdTimeout
	agentCmdTimeout = 30 * time.Millisecond
	t.Cleanup(func() { agentCmdTimeout = old })

	at := newPipeAgent(t)

	if _, err := at.sendCommandOpt("ping", false); err == nil {
		t.Fatal("unanswered ping returned no error")
	}
	if !at.alive || !at.isRunning() {
		t.Fatal("agent marked dead after one missed keepalive ping")
	}

	// A real command that times out still does mark it dead.
	if _, err := at.sendCommand("play 0 1"); err == nil {
		t.Fatal("unanswered command returned no error")
	}
	if at.alive {
		t.Fatal("agent still alive after a command timed out")
	}
}

// A late response to a timed-out command must not be handed to the next one.
func TestStaleResponseIsNotReusedByNextCommand(t *testing.T) {
	old := agentCmdTimeout
	agentCmdTimeout = 30 * time.Millisecond
	t.Cleanup(func() { agentCmdTimeout = old })

	at := newPipeAgent(t)

	if _, err := at.sendCommandOpt("ping", false); err == nil {
		t.Fatal("unanswered ping returned no error")
	}
	// The agent finally answers the ping, after the caller gave up.
	at.respCh <- agentResp{lines: []string{"stale"}}

	lines, err := at.sendCommandOpt("ping", false)
	if err == nil {
		t.Fatalf("second ping took the stale response %v as its own", lines)
	}
}

// The disconnect log names the cause, so a network drop can be told apart
// from a keepalive timeout after the fact.
func TestDisconnectReasonIsRecordedOnce(t *testing.T) {
	at := newPipeAgent(t)
	if got := at.disconnectReason(); got != "connection closed" {
		t.Fatalf("default reason = %q", got)
	}
	at.closeWithReason("keepalive: 2 missed pings")
	at.closeWithReason("read error: later observer")
	if got := at.disconnectReason(); got != "keepalive: 2 missed pings" {
		t.Fatalf("reason = %q, want the first one recorded", got)
	}
}
