package main

import (
	"bufio"
	"fmt"
	"net"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestPlChangesNeedsFull(t *testing.T) {
	tests := []struct {
		name       string
		clientVer  int
		currentVer int
		want       bool
	}{
		{name: "zero requests full playlist", clientVer: 0, currentVer: 7, want: true},
		{name: "older client version requests full playlist", clientVer: 6, currentVer: 7, want: true},
		{name: "same version has no changes", clientVer: 7, currentVer: 7, want: false},
		{name: "newer client version requests full playlist", clientVer: 8, currentVer: 7, want: true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := plChangesNeedsFull(tt.clientVer, tt.currentVer); got != tt.want {
				t.Fatalf("plChangesNeedsFull(%d, %d) = %v, want %v", tt.clientVer, tt.currentVer, got, tt.want)
			}
		})
	}
}

func TestBumpQueueVersionLocked(t *testing.T) {
	a := &app{}
	before := int(time.Now().Unix())
	a.bumpQueueVersionLocked()
	after := int(time.Now().Unix())

	if a.queueVersion < before || a.queueVersion > after {
		t.Fatalf("queueVersion = %d, want between %d and %d", a.queueVersion, before, after)
	}

	a.queueVersion = after + 100
	a.bumpQueueVersionLocked()
	if a.queueVersion != after+101 {
		t.Fatalf("future queueVersion = %d, want %d", a.queueVersion, after+101)
	}
}

func TestVolumeCommandsUpdateTarget(t *testing.T) {
	wt := &webTarget{alive: true, volume: 40}
	a := &app{
		webTargets:   map[string]*webTarget{"web": wt},
		activeDevice: "web",
		mpdHub:       newNotifyHub(),
	}
	c := &mpdConn{app: a}

	if err := cmdSetVol(c, []string{"70"}); err != nil {
		t.Fatalf("cmdSetVol: %v", err)
	}
	if wt.volume != 70 {
		t.Fatalf("setvol target volume = %v, want 70", wt.volume)
	}

	if err := cmdVolume(c, []string{"-50"}); err != nil {
		t.Fatalf("cmdVolume: %v", err)
	}
	if wt.volume != 20 {
		t.Fatalf("volume -50 target volume = %v, want 20", wt.volume)
	}

	if err := cmdVolume(c, []string{"-50"}); err != nil {
		t.Fatalf("cmdVolume clamp low: %v", err)
	}
	if wt.volume != 0 {
		t.Fatalf("volume clamp low = %v, want 0", wt.volume)
	}

	if err := cmdVolume(c, []string{"+150"}); err != nil {
		t.Fatalf("cmdVolume clamp high: %v", err)
	}
	if wt.volume != 100 {
		t.Fatalf("volume clamp high = %v, want 100", wt.volume)
	}
}

func TestAgentPreloadSendsClearCommand(t *testing.T) {
	got := captureAgentCommand(t, func(at *agentTarget) error {
		return at.agentPreload(-1)
	})
	if got != "preload -1" {
		t.Fatalf("agentPreload(-1) sent %q, want preload -1", got)
	}
}

func TestAgentPlaySendsNegativeNext(t *testing.T) {
	got := captureAgentCommand(t, func(at *agentTarget) error {
		return at.agentPlayAt(3, -1, -1)
	})
	if got != "play 3 next=-1" {
		t.Fatalf("agentPlayAt sent %q, want play 3 next=-1", got)
	}
}

func TestAgentFreshPropertyQueriesAgent(t *testing.T) {
	clientConn, serverConn := net.Pipe()
	t.Cleanup(func() {
		_ = clientConn.Close()
		_ = serverConn.Close()
	})

	at := &agentTarget{
		writer: bufio.NewWriter(clientConn),
		conn:   clientConn,
		alive:  true,
		done:   make(chan struct{}),
		app:    &app{},
		respCh: make(chan agentResp, 1),
	}
	go at.readLoop(bufio.NewReader(clientConn))

	cmdCh := make(chan string, 1)
	errCh := make(chan error, 1)
	go func() {
		r := bufio.NewReader(serverConn)
		line, err := r.ReadString('\n')
		if err != nil {
			errCh <- err
			return
		}
		cmdCh <- strings.TrimRight(line, "\r\n")
		if _, err := fmt.Fprintln(serverConn, "value: 12.500000"); err != nil {
			errCh <- err
			return
		}
		_, err = fmt.Fprintln(serverConn, "OK")
		errCh <- err
	}()

	got, err := at.getFreshProperty("time-pos")
	if err != nil {
		t.Fatalf("getFreshProperty: %v", err)
	}
	if got != 12.5 {
		t.Fatalf("fresh time-pos = %v, want 12.5", got)
	}
	if at.agElapsed != 12.5 {
		t.Fatalf("cached elapsed = %v, want 12.5", at.agElapsed)
	}
	at.close()

	if err := <-errCh; err != nil {
		t.Fatalf("server side property response: %v", err)
	}
	if cmd := <-cmdCh; cmd != "get_property time-pos" {
		t.Fatalf("command = %q, want get_property time-pos", cmd)
	}
}

func TestIdleNotificationPreservesFollowingCommandList(t *testing.T) {
	c, rw, closeFn := newTestMPDConn(t)
	defer closeFn()

	if got := readLine(t, rw); !strings.HasPrefix(got, "OK MPD") {
		t.Fatalf("greeting = %q", got)
	}

	writeLine(t, rw, "idle playlist")
	waitForIdle(t, c)
	c.app.mpdHub.notify(SubPlaylist)

	if got := readLine(t, rw); got != "changed: playlist" {
		t.Fatalf("idle changed line = %q", got)
	}
	if got := readLine(t, rw); got != "OK" {
		t.Fatalf("idle OK = %q", got)
	}

	writeLine(t, rw, "command_list_begin")
	writeLine(t, rw, "ping")
	writeLine(t, rw, "command_list_end")
	if got := readLine(t, rw); got != "OK" {
		t.Fatalf("command list response = %q", got)
	}
}

func captureAgentCommand(t *testing.T, call func(*agentTarget) error) string {
	t.Helper()

	clientConn, serverConn := net.Pipe()
	t.Cleanup(func() {
		_ = clientConn.Close()
		_ = serverConn.Close()
	})

	at := &agentTarget{
		writer:    bufio.NewWriter(clientConn),
		conn:      clientConn,
		alive:     true,
		done:      make(chan struct{}),
		app:       &app{},
		respCh:    make(chan agentResp, 1),
		closeOnce: sync.Once{},
	}
	go at.readLoop(bufio.NewReader(clientConn))

	cmdCh := make(chan string, 1)
	errCh := make(chan error, 1)
	go func() {
		r := bufio.NewReader(serverConn)
		line, err := r.ReadString('\n')
		if err != nil {
			errCh <- err
			return
		}
		cmdCh <- strings.TrimRight(line, "\r\n")
		_, err = fmt.Fprintln(serverConn, "OK")
		errCh <- err
	}()

	if err := call(at); err != nil {
		t.Fatalf("agent call: %v", err)
	}
	at.close()

	select {
	case err := <-errCh:
		if err != nil {
			t.Fatalf("server side command capture: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("timed out waiting for server side command capture")
	}

	select {
	case got := <-cmdCh:
		return got
	default:
		t.Fatal("no command captured")
	}
	return ""
}

func TestIdleAllowsCommandInsteadOfNoidle(t *testing.T) {
	_, rw, closeFn := newTestMPDConn(t)
	defer closeFn()

	if got := readLine(t, rw); !strings.HasPrefix(got, "OK MPD") {
		t.Fatalf("greeting = %q", got)
	}

	writeLine(t, rw, "idle playlist")
	writeLine(t, rw, "ping")

	if got := readLine(t, rw); got != "OK" {
		t.Fatalf("idle end response = %q", got)
	}
	if got := readLine(t, rw); got != "OK" {
		t.Fatalf("ping response = %q", got)
	}
}

func TestIdleDeliversPendingNotificationBeforeReadingNextCommand(t *testing.T) {
	c, rw, closeFn := newTestMPDConn(t)
	defer closeFn()

	if got := readLine(t, rw); !strings.HasPrefix(got, "OK MPD") {
		t.Fatalf("greeting = %q", got)
	}

	c.app.mpdHub.notify(SubPlaylist)
	writeLine(t, rw, "idle playlist")
	if got := readLine(t, rw); got != "changed: playlist" {
		t.Fatalf("pending idle changed line = %q", got)
	}
	if got := readLine(t, rw); got != "OK" {
		t.Fatalf("pending idle OK = %q", got)
	}

	writeLine(t, rw, "ping")
	if got := readLine(t, rw); got != "OK" {
		t.Fatalf("post-pending ping response = %q", got)
	}
}

func newTestMPDConn(t *testing.T) (*mpdConn, *bufio.ReadWriter, func()) {
	t.Helper()

	server, client := net.Pipe()
	a := &app{mpdHub: newNotifyHub()}
	c := &mpdConn{
		conn:   server,
		reader: bufio.NewReader(server),
		writer: bufio.NewWriter(server),
		app:    a,
	}
	go c.serve()

	rw := bufio.NewReadWriter(bufio.NewReader(client), bufio.NewWriter(client))
	return c, rw, func() {
		_ = client.Close()
		_ = server.Close()
	}
}

func waitForIdle(t *testing.T, c *mpdConn) {
	t.Helper()

	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		c.idleMu.Lock()
		idling := c.idling
		c.idleMu.Unlock()
		if idling {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("MPD connection did not enter idle")
}

func writeLine(t *testing.T, rw *bufio.ReadWriter, line string) {
	t.Helper()
	if _, err := rw.WriteString(line + "\n"); err != nil {
		t.Fatalf("write %q: %v", line, err)
	}
	if err := rw.Flush(); err != nil {
		t.Fatalf("flush %q: %v", line, err)
	}
}

func readLine(t *testing.T, rw *bufio.ReadWriter) string {
	t.Helper()
	line, err := rw.ReadString('\n')
	if err != nil {
		t.Fatalf("read line: %v", err)
	}
	return strings.TrimRight(line, "\r\n")
}
