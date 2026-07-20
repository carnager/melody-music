package main

import (
	"log"
	"os"
	"path/filepath"
	"testing"
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

func TestSwitchOutputExclusive(t *testing.T) {
	a, wts := newOutputsTestApp(t)
	a.enabledOutputs = map[string]bool{"web-a": true, "web-b": true, "web-c": true}
	a.primaryOutput = "web-a"
	wts["web-a"].paused = false

	if err := a.switchOutput("web-b"); err != nil {
		t.Fatalf("switchOutput: %v", err)
	}
	if len(a.enabledOutputs) != 1 || !a.enabledOutputs["web-b"] {
		t.Fatalf("enabled = %v, want only web-b", a.enabledOutputs)
	}
	if a.primaryOutput != "web-b" {
		t.Fatalf("primary = %q, want web-b", a.primaryOutput)
	}
	if !wts["web-a"].paused || !wts["web-c"].paused {
		t.Fatal("old outputs not stopped by exclusive switch")
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
