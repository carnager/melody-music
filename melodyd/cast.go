package main

import (
	"context"
	"fmt"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/grandcat/zeroconf"
	"github.com/vishen/go-chromecast/application"
	"github.com/vishen/go-chromecast/cast"
)

const castServiceName = "_googlecast._tcp"

type castTarget struct {
	app       *app
	id        string
	name      string
	model     string
	uuid      string
	addr      string
	port      int
	mu        sync.Mutex
	device    *application.Application
	available bool
	current   string
	duration  float64
	timePos   float64
	paused    bool
	volume    float64
	monitorID int
}

type castDiscoveryStats struct {
	entries int
	devices int
}

func (a *app) castDiscoveryLoop() {
	for {
		stats, err := a.discoverCastOnce()
		if err != nil {
			a.logger.Printf("cast discovery: %v", err)
		} else {
			a.logger.Printf("cast discovery: entries=%d devices=%d", stats.entries, stats.devices)
		}
		time.Sleep(60 * time.Second)
	}
}

func (a *app) discoverCastOnce() (castDiscoveryStats, error) {
	var stats castDiscoveryStats
	resolver, err := zeroconf.NewResolver(zeroconf.SelectIPTraffic(zeroconf.IPv4))
	if err != nil {
		return stats, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 4*time.Second)
	defer cancel()

	entries := make(chan *zeroconf.ServiceEntry, 32)
	errCh := make(chan error, 1)
	go func() {
		errCh <- resolver.Browse(ctx, castServiceName, "local.", entries)
	}()

	seen := map[string]struct{}{}
	for {
		select {
		case err := <-errCh:
			if err != nil {
				return stats, err
			}
			errCh = nil
		case <-ctx.Done():
			return stats, nil
		case entry := <-entries:
			if entry == nil {
				continue
			}
			stats.entries++
			addr := castEntryAddr(entry)
			if addr == "" || entry.Port == 0 {
				continue
			}
			info := castInfoFields(entry.Text)
			uuid := firstNonEmpty(info["id"], entry.HostName, addr)
			key := uuid + "@" + addr
			if _, ok := seen[key]; ok {
				continue
			}
			seen[key] = struct{}{}
			a.registerCastDevice(entry, info, addr)
			stats.devices++
		}
	}
}

func castEntryAddr(entry *zeroconf.ServiceEntry) string {
	if len(entry.AddrIPv4) > 0 {
		return entry.AddrIPv4[0].String()
	}
	if len(entry.AddrIPv6) > 0 {
		return entry.AddrIPv6[0].String()
	}
	return ""
}

func castInfoFields(fields []string) map[string]string {
	info := make(map[string]string, len(fields))
	for _, field := range fields {
		k, v, ok := strings.Cut(field, "=")
		if ok {
			info[k] = castDecodeTXT(v)
		}
	}
	return info
}

func castDecodeTXT(value string) string {
	if !strings.Contains(value, "\\") {
		return value
	}
	var out strings.Builder
	for i := 0; i < len(value); {
		if value[i] == '\\' && i+3 < len(value) {
			if n, err := strconv.Atoi(value[i+1 : i+4]); err == nil {
				out.WriteByte(byte(n))
				i += 4
				continue
			}
		}
		out.WriteByte(value[i])
		i++
	}
	return out.String()
}

func (a *app) registerCastDevice(entry *zeroconf.ServiceEntry, info map[string]string, addr string) {
	uuid := firstNonEmpty(info["id"], entry.HostName, addr)
	name := firstNonEmpty(info["fn"], entry.Instance, addr)
	model := info["md"]
	id := "cast-" + sanitizeDeviceID(uuid)

	a.devicesMu.Lock()
	target, existed := a.castTargets[id]
	changed := false
	if existed {
		target.mu.Lock()
		changed = target.addr != addr || target.port != entry.Port || target.name != name
		target.name = name
		target.model = model
		target.uuid = uuid
		target.addr = addr
		target.port = entry.Port
		target.available = true
		target.mu.Unlock()
	} else {
		target = &castTarget{
			app:       a,
			id:        id,
			name:      name,
			model:     model,
			uuid:      uuid,
			addr:      addr,
			port:      entry.Port,
			available: true,
			paused:    true,
			volume:    100,
		}
		a.castTargets[id] = target
	}
	a.devices[id] = &device{
		ID:       id,
		Name:     name,
		Address:  net.JoinHostPort(addr, strconv.Itoa(entry.Port)),
		IsLocal:  false,
		Type:     "cast",
		Format:   a.cfg.Chromecast.Format,
		LastSeen: time.Now(),
	}
	a.devicesMu.Unlock()

	if !existed {
		a.logger.Printf("cast device discovered: %s id=%s addr=%s model=%s", name, id, net.JoinHostPort(addr, strconv.Itoa(entry.Port)), model)
		a.mpdHub.notify(SubOutput)
	} else if changed {
		a.logger.Printf("cast device updated: %s id=%s addr=%s model=%s", name, id, net.JoinHostPort(addr, strconv.Itoa(entry.Port)), model)
	}
}

func (t *castTarget) loadFile(url, mode string, meta map[string]any) error {
	return t.loadFileAt(url, mode, 0)
}

func (t *castTarget) loadFileAt(url, mode string, startTime float64) error {
	if mode == "append" {
		return nil
	}
	t.app.logger.Printf("cast %s: load mode=%s start=%.3f url=%s", t.name, mode, startTime, url)
	if err := t.ensureConnected(); err != nil {
		return err
	}
	if mode == "replace" {
		_ = t.stop()
	}
	t.mu.Lock()
	t.current = url
	t.timePos = startTime
	t.duration = 0
	t.paused = false
	t.monitorID++
	t.mu.Unlock()

	if err := t.loadURL(url, t.contentType(), startTime); err != nil {
		t.app.logger.Printf("cast %s: load failed: %v", t.name, err)
		return err
	}

	t.mu.Lock()
	t.monitorID++
	monitorID := t.monitorID
	t.mu.Unlock()
	go t.monitorPlayback(monitorID)
	return nil
}

func (t *castTarget) loadFileBatch(urls []string, mode string) error {
	if len(urls) == 0 {
		return nil
	}
	return t.loadFile(urls[0], mode, nil)
}

func (t *castTarget) playlistClear() error {
	t.app.logger.Printf("cast %s: stop", t.name)
	t.mu.Lock()
	t.monitorID++
	t.current = ""
	t.timePos = 0
	t.duration = 0
	t.paused = true
	t.mu.Unlock()
	return t.stop()
}

func (t *castTarget) playlistRemove(index int) error  { return nil }
func (t *castTarget) playlistMove(from, to int) error { return nil }

func (t *castTarget) getProperty(name string) (any, error) {
	switch name {
	case "pause":
		if err := t.refreshStatus(); err != nil {
			return nil, err
		}
		t.mu.Lock()
		defer t.mu.Unlock()
		return t.paused, nil
	case "time-pos":
		if err := t.refreshStatus(); err != nil {
			return nil, err
		}
		t.mu.Lock()
		defer t.mu.Unlock()
		return t.timePos, nil
	case "duration":
		if err := t.refreshStatus(); err != nil {
			return nil, err
		}
		t.mu.Lock()
		defer t.mu.Unlock()
		return t.duration, nil
	case "volume":
		if err := t.refreshStatus(); err != nil {
			return nil, err
		}
		t.mu.Lock()
		defer t.mu.Unlock()
		return t.volume, nil
	default:
		return nil, fmt.Errorf("unknown Cast property: %s", name)
	}
}

func (t *castTarget) setProperty(name string, value any) error {
	if err := t.ensureConnected(); err != nil {
		return err
	}
	switch name {
	case "pause":
		paused, _ := value.(bool)
		var err error
		if paused {
			err = t.device.Pause()
		} else {
			err = t.device.Unpause()
		}
		if isCastNoMediaError(err) {
			err = nil
		}
		if err == nil {
			t.mu.Lock()
			t.paused = paused
			t.mu.Unlock()
		}
		return err
	case "time-pos":
		seconds, ok := value.(float64)
		if !ok {
			return fmt.Errorf("invalid seek value")
		}
		return t.device.SeekToTime(float32(seconds))
	case "volume":
		vol, ok := value.(float64)
		if !ok {
			return fmt.Errorf("invalid volume value")
		}
		if vol < 0 {
			vol = 0
		}
		if vol > 100 {
			vol = 100
		}
		err := t.device.SetVolume(float32(vol / 100))
		if err == nil {
			t.mu.Lock()
			t.volume = vol
			t.mu.Unlock()
		}
		return err
	case "replaygain":
		return nil
	default:
		return fmt.Errorf("unknown Cast property: %s", name)
	}
}

func (t *castTarget) isRunning() bool {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.available
}

func (t *castTarget) ensureConnected() error {
	t.mu.Lock()
	if t.device != nil {
		t.mu.Unlock()
		return nil
	}
	addr := t.addr
	port := t.port
	t.mu.Unlock()

	ip := net.ParseIP(addr)
	if ip == nil {
		return fmt.Errorf("invalid Cast address: %s", addr)
	}
	dev := application.NewApplication(
		application.WithCacheDisabled(true),
		application.WithConnectionRetries(1),
		application.WithDeviceNameOverride(t.name),
	)
	if err := dev.Start(ip.String(), port); err != nil {
		return err
	}
	t.mu.Lock()
	if t.device == nil {
		t.device = dev
	} else {
		_ = dev.Close(false)
	}
	t.mu.Unlock()
	return nil
}

func (t *castTarget) loadURL(url, mimeType string, startTime float64) error {
	if startTime < 0 {
		startTime = 0
	}
	return t.device.Load(url, int(startTime+0.5), mimeType, false, true, true)
}

func (t *castTarget) contentType() string {
	switch t.app.cfg.Chromecast.Format {
	case "flac":
		return "audio/flac"
	default:
		return "audio/mpeg"
	}
}

func (t *castTarget) stop() error {
	if err := t.ensureConnected(); err != nil {
		return err
	}
	err := t.device.StopMedia()
	if isCastNoMediaError(err) {
		return nil
	}
	return err
}

func isCastNoMediaError(err error) bool {
	return err != nil && strings.Contains(err.Error(), "media not yet initialised")
}

func (t *castTarget) refreshStatus() error {
	if err := t.ensureConnected(); err != nil {
		return err
	}
	if err := t.device.Update(); err != nil {
		return err
	}
	_, media, volume := t.device.Status()
	t.applyMediaStatus(media)
	if volume != nil {
		t.mu.Lock()
		t.volume = float64(volume.Level) * 100
		t.mu.Unlock()
	}
	return nil
}

func (t *castTarget) applyMediaStatus(st *cast.Media) (state, idle string) {
	if st == nil {
		return "", ""
	}
	t.mu.Lock()
	t.timePos = float64(st.CurrentTime)
	t.duration = float64(st.Media.Duration)
	t.paused = st.PlayerState != "PLAYING" && st.PlayerState != "BUFFERING"
	if st.Volume.Level > 0 {
		t.volume = float64(st.Volume.Level) * 100
	}
	t.mu.Unlock()
	return st.PlayerState, st.IdleReason
}

func (t *castTarget) monitorPlayback(id int) {
	ticker := time.NewTicker(2 * time.Second)
	defer ticker.Stop()
	wasPlaying := false
	lastState := ""
	for range ticker.C {
		t.mu.Lock()
		active := id == t.monitorID
		t.mu.Unlock()
		if !active {
			return
		}
		if err := t.ensureConnected(); err != nil {
			t.app.logger.Printf("cast %s: connect failed: %v", t.name, err)
			continue
		}
		if err := t.device.Update(); err != nil {
			t.app.logger.Printf("cast %s: status poll failed: %v", t.name, err)
			continue
		}
		_, media, _ := t.device.Status()
		state, idle := t.applyMediaStatus(media)
		stateLog := state
		if idle != "" {
			stateLog += "/" + idle
		}
		if stateLog != lastState {
			t.app.logger.Printf("cast %s: media state=%s", t.name, stateLog)
			lastState = stateLog
		}
		if state == "PLAYING" || state == "BUFFERING" {
			wasPlaying = true
			continue
		}
		if wasPlaying && state == "IDLE" && idle == "FINISHED" {
			t.app.logger.Printf("cast %s: track ended, advancing queue", t.name)
			t.app.advanceTrack()
			return
		}
	}
}
