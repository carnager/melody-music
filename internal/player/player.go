package player

/*
#cgo pkg-config: mpv
#include <stdlib.h>
#include <mpv/client.h>

static int mpv_cmd1(mpv_handle *ctx, const char *a0) {
	const char *args[] = {a0, NULL};
	return mpv_command(ctx, args);
}

static int mpv_cmd3(mpv_handle *ctx, const char *a0, const char *a1, const char *a2) {
	const char *args[] = {a0, a1, a2, NULL};
	return mpv_command(ctx, args);
}

static int mpv_cmd4(mpv_handle *ctx, const char *a0, const char *a1, const char *a2, const char *a3) {
	const char *args[] = {a0, a1, a2, a3, NULL};
	return mpv_command(ctx, args);
}

static int mpv_set_flag(mpv_handle *ctx, const char *name, int value) {
	return mpv_set_property(ctx, name, MPV_FORMAT_FLAG, &value);
}

static int mpv_get_flag(mpv_handle *ctx, const char *name, int *value) {
	return mpv_get_property(ctx, name, MPV_FORMAT_FLAG, value);
}

static int mpv_set_double(mpv_handle *ctx, const char *name, double value) {
	return mpv_set_property(ctx, name, MPV_FORMAT_DOUBLE, &value);
}

static int mpv_get_double(mpv_handle *ctx, const char *name, double *value) {
	return mpv_get_property(ctx, name, MPV_FORMAT_DOUBLE, value);
}

static mpv_event_end_file *mpv_event_end_file_data(mpv_event *event) {
	return (mpv_event_end_file *)event->data;
}
*/
import "C"

import (
	"fmt"
	"math"
	"sync"
	"time"
	"unsafe"
)

const OutputSampleRate = 48000

// Player wraps libmpv behind the playback API used by Melody agents.
type Player struct {
	mu sync.Mutex

	ctx  *C.mpv_handle
	done chan struct{}

	volume   float64
	rgMode   string
	rgTrack  float64
	rgAlbum  float64
	nrgTrack float64
	nrgAlbum float64

	OnTrackEnd func()
}

// New creates and initializes a new Player.
func New() *Player {
	ctx := C.mpv_create()
	if ctx == nil {
		panic("mpv_create failed")
	}

	p := &Player{
		ctx:    ctx,
		done:   make(chan struct{}),
		volume: 100,
		rgMode: "off",
	}

	mustSetOption(ctx, "terminal", "no")
	mustSetOption(ctx, "msg-level", "all=no")
	mustSetOption(ctx, "vid", "no")
	mustSetOption(ctx, "audio-display", "no")
	mustSetOption(ctx, "idle", "yes")
	mustSetOption(ctx, "keep-open", "no")
	mustSetOption(ctx, "gapless-audio", "yes")
	mustSetOption(ctx, "replaygain", "no")

	if rc := C.mpv_initialize(ctx); rc < 0 {
		C.mpv_terminate_destroy(ctx)
		panic(fmt.Sprintf("mpv_initialize failed: %s", mpvError(rc)))
	}

	go p.eventLoop(ctx)
	return p
}

func mustSetOption(ctx *C.mpv_handle, name, value string) {
	cName := C.CString(name)
	cValue := C.CString(value)
	defer C.free(unsafe.Pointer(cName))
	defer C.free(unsafe.Pointer(cValue))
	if rc := C.mpv_set_option_string(ctx, cName, cValue); rc < 0 {
		C.mpv_terminate_destroy(ctx)
		panic(fmt.Sprintf("mpv option %s=%s failed: %s", name, value, mpvError(rc)))
	}
}

// Play loads and starts playing a track. Stops any current playback.
func (p *Player) Play(path, _ string, rgTrack, rgAlbum float64) error {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.ctx == nil {
		return fmt.Errorf("player closed")
	}

	p.rgTrack = rgTrack
	p.rgAlbum = rgAlbum
	p.nrgTrack = 0
	p.nrgAlbum = 0

	if err := p.command3Locked("loadfile", path, "replace"); err != nil {
		return err
	}
	if err := p.setPauseLocked(false); err != nil {
		return err
	}
	return p.applyVolumeLocked()
}

// Preload prepares the next track for mpv playlist auto-advance.
func (p *Player) Preload(path, _ string, rgTrack, rgAlbum float64) error {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.ctx == nil {
		return fmt.Errorf("player closed")
	}

	p.nrgTrack = rgTrack
	p.nrgAlbum = rgAlbum

	// Keep only the current entry and the requested next entry.
	_ = p.command1Locked("playlist-clear")
	return p.command3Locked("loadfile", path, "append")
}

// Pause pauses playback.
func (p *Player) Pause() {
	p.mu.Lock()
	defer p.mu.Unlock()
	_ = p.setPauseLocked(true)
}

// Resume resumes playback.
func (p *Player) Resume() {
	p.mu.Lock()
	defer p.mu.Unlock()
	_ = p.setPauseLocked(false)
}

// Stop stops playback and clears all playlist entries.
func (p *Player) Stop() {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.ctx == nil {
		return
	}
	_ = p.command1Locked("stop")
	_ = p.command1Locked("playlist-clear")
}

// Seek seeks to the given position in seconds.
func (p *Player) Seek(seconds float64) error {
	if seconds < 0 {
		seconds = 0
	}

	p.mu.Lock()
	defer p.mu.Unlock()
	if p.ctx == nil {
		return fmt.Errorf("player closed")
	}
	return p.command4Locked("seek", fmt.Sprintf("%.3f", seconds), "absolute", "exact")
}

// SetVolume sets the volume (0-100).
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
	_ = p.applyVolumeLocked()
}

// SetReplayGain sets the ReplayGain mode ("track", "album", "off").
func (p *Player) SetReplayGain(mode string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.rgMode = mode
	_ = p.applyVolumeLocked()
}

// State returns the current playback state.
func (p *Player) State() (state string, elapsed, duration float64, vol float64) {
	p.mu.Lock()
	defer p.mu.Unlock()

	vol = p.volume
	if p.ctx == nil {
		return "stop", 0, 0, vol
	}

	if p.getFlagLocked("idle-active") {
		return "stop", 0, 0, vol
	}

	if p.getFlagLocked("pause") {
		state = "pause"
	} else {
		state = "play"
	}
	elapsed = p.getDoubleLocked("time-pos")
	duration = p.getDoubleLocked("duration")
	return state, elapsed, duration, vol
}

// Close shuts down the player.
func (p *Player) Close() {
	p.mu.Lock()
	ctx := p.ctx
	if ctx == nil {
		p.mu.Unlock()
		return
	}
	p.ctx = nil
	p.mu.Unlock()

	command := C.CString("quit")
	C.mpv_command_string(ctx, command)
	C.free(unsafe.Pointer(command))
	C.mpv_wakeup(ctx)
	<-p.done
}

// StartPositionReporter calls fn every interval with the current state.
func (p *Player) StartPositionReporter(interval time.Duration, fn func(state string, elapsed, duration, vol float64)) {
	go func() {
		for {
			time.Sleep(interval)
			state, elapsed, dur, vol := p.State()
			fn(state, elapsed, dur, vol)
		}
	}()
}

func (p *Player) eventLoop(ctx *C.mpv_handle) {
	defer func() {
		C.mpv_destroy(ctx)
		close(p.done)
	}()

	for {
		event := C.mpv_wait_event(ctx, -1)
		if event == nil {
			return
		}
		switch event.event_id {
		case C.MPV_EVENT_SHUTDOWN:
			return
		case C.MPV_EVENT_END_FILE:
			data := C.mpv_event_end_file_data(event)
			if data == nil || data.reason != C.MPV_END_FILE_REASON_EOF {
				continue
			}
			p.promoteNextReplayGain()
			if p.OnTrackEnd != nil {
				go p.OnTrackEnd()
			}
		}
	}
}

func (p *Player) promoteNextReplayGain() {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.rgTrack = p.nrgTrack
	p.rgAlbum = p.nrgAlbum
	p.nrgTrack = 0
	p.nrgAlbum = 0
	_ = p.applyVolumeLocked()
}

func (p *Player) setPauseLocked(paused bool) error {
	value := C.int(0)
	if paused {
		value = 1
	}
	name := C.CString("pause")
	defer C.free(unsafe.Pointer(name))
	if rc := C.mpv_set_flag(p.ctx, name, value); rc < 0 {
		return fmt.Errorf("mpv set pause: %s", mpvError(rc))
	}
	return nil
}

func (p *Player) applyVolumeLocked() error {
	if p.ctx == nil {
		return nil
	}

	gainDB := 0.0
	switch p.rgMode {
	case "track":
		gainDB = p.rgTrack
	case "album":
		gainDB = p.rgAlbum
	}
	level := p.volume * math.Pow(10, gainDB/20)
	if level < 0 {
		level = 0
	}
	if level > 100 {
		level = 100
	}

	name := C.CString("volume")
	defer C.free(unsafe.Pointer(name))
	if rc := C.mpv_set_double(p.ctx, name, C.double(level)); rc < 0 {
		return fmt.Errorf("mpv set volume: %s", mpvError(rc))
	}
	return nil
}

func (p *Player) command1Locked(a0 string) error {
	c0 := C.CString(a0)
	defer C.free(unsafe.Pointer(c0))
	if rc := C.mpv_cmd1(p.ctx, c0); rc < 0 {
		return fmt.Errorf("mpv %s: %s", a0, mpvError(rc))
	}
	return nil
}

func (p *Player) command3Locked(a0, a1, a2 string) error {
	c0 := C.CString(a0)
	c1 := C.CString(a1)
	c2 := C.CString(a2)
	defer C.free(unsafe.Pointer(c0))
	defer C.free(unsafe.Pointer(c1))
	defer C.free(unsafe.Pointer(c2))
	if rc := C.mpv_cmd3(p.ctx, c0, c1, c2); rc < 0 {
		return fmt.Errorf("mpv %s: %s", a0, mpvError(rc))
	}
	return nil
}

func (p *Player) command4Locked(a0, a1, a2, a3 string) error {
	c0 := C.CString(a0)
	c1 := C.CString(a1)
	c2 := C.CString(a2)
	c3 := C.CString(a3)
	defer C.free(unsafe.Pointer(c0))
	defer C.free(unsafe.Pointer(c1))
	defer C.free(unsafe.Pointer(c2))
	defer C.free(unsafe.Pointer(c3))
	if rc := C.mpv_cmd4(p.ctx, c0, c1, c2, c3); rc < 0 {
		return fmt.Errorf("mpv %s: %s", a0, mpvError(rc))
	}
	return nil
}

func (p *Player) getFlagLocked(prop string) bool {
	name := C.CString(prop)
	defer C.free(unsafe.Pointer(name))
	var value C.int
	if rc := C.mpv_get_flag(p.ctx, name, &value); rc < 0 {
		return false
	}
	return value != 0
}

func (p *Player) getDoubleLocked(prop string) float64 {
	name := C.CString(prop)
	defer C.free(unsafe.Pointer(name))
	var value C.double
	if rc := C.mpv_get_double(p.ctx, name, &value); rc < 0 {
		return 0
	}
	return float64(value)
}

func mpvError(rc C.int) string {
	return C.GoString(C.mpv_error_string(rc))
}
