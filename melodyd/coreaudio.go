package main

import (
	"fmt"
	"net"
	"strconv"
	"strings"
	"time"
)

const coreAudioDevicePrefix = "coreaudio-"

type coreAudioOutputDevice struct {
	ID        string
	Name      string
	MPVDevice string
}

type coreAudioTarget struct {
	app       *app
	id        string
	name      string
	mpvDevice string
}

func (a *app) coreAudioOutputLoop() {
	for {
		if err := a.refreshCoreAudioOutputs(); err != nil {
			a.logger.Printf("coreaudio discovery: %v", err)
		}
		time.Sleep(60 * time.Second)
	}
}

func (a *app) refreshCoreAudioOutputs() error {
	outputs, err := listCoreAudioOutputDevices()
	if err != nil {
		return err
	}
	a.logger.Printf("coreaudio discovery: outputs=%d %s", len(outputs), describeCoreAudioOutputs(outputs))
	if len(outputs) == 0 {
		return nil
	}

	seen := make(map[string]struct{}, len(outputs))
	changed := false

	a.devicesMu.Lock()
	for _, out := range outputs {
		if out.ID == "" || out.Name == "" || out.MPVDevice == "" {
			continue
		}
		seen[out.ID] = struct{}{}

		target, existed := a.coreTargets[out.ID]
		if existed {
			if target.name != out.Name || target.mpvDevice != out.MPVDevice {
				target.name = out.Name
				target.mpvDevice = out.MPVDevice
				changed = true
			}
		} else {
			target = &coreAudioTarget{
				app:       a,
				id:        out.ID,
				name:      out.Name,
				mpvDevice: out.MPVDevice,
			}
			a.coreTargets[out.ID] = target
			a.logger.Printf("coreaudio output discovered: %s id=%s device=%s", out.Name, out.ID, out.MPVDevice)
			changed = true
		}

		addr := net.JoinHostPort("localhost", strconv.Itoa(a.cfg.MPD.Port))
		a.devices[out.ID] = &device{
			ID:       out.ID,
			Name:     out.Name,
			Address:  addr,
			IsLocal:  true,
			Type:     "coreaudio",
			LastSeen: time.Now(),
		}
	}

	for id := range a.coreTargets {
		if _, ok := seen[id]; ok {
			continue
		}
		delete(a.coreTargets, id)
		delete(a.devices, id)
		if a.activeDevice == id {
			a.activeDevice = "local"
		}
		changed = true
	}
	a.devicesMu.Unlock()

	if changed {
		a.mpdHub.notify(SubOutput)
	}
	return nil
}

func (t *coreAudioTarget) agent() (*agentTarget, error) {
	t.app.devicesMu.RLock()
	at := t.app.agentTargets["local"]
	t.app.devicesMu.RUnlock()
	if at == nil || !at.isRunning() {
		return nil, fmt.Errorf("local macOS audio agent is not connected")
	}
	return at, nil
}

func (t *coreAudioTarget) selectDevice() (*agentTarget, error) {
	at, err := t.agent()
	if err != nil {
		return nil, err
	}
	t.app.logger.Printf("coreaudio %s: selecting mpv audio device %s", t.name, t.mpvDevice)
	if _, err := at.sendCommand("audio_device " + mpdQuoteArg(t.mpvDevice)); err != nil {
		return nil, err
	}
	return at, nil
}

func (t *coreAudioTarget) ensureQueueSync() {
	at, err := t.selectDevice()
	if err != nil {
		t.app.logger.Printf("coreaudio %s: select failed: %v", t.name, err)
		return
	}
	at.ensureQueueSync()
}

func (t *coreAudioTarget) agentPlay(curPos, nextPos int) error {
	return t.agentPlayAt(curPos, nextPos, -1)
}

func (t *coreAudioTarget) agentPlayAt(curPos, nextPos int, seekPos float64) error {
	at, err := t.selectDevice()
	if err != nil {
		return err
	}
	return at.agentPlayAt(curPos, nextPos, seekPos)
}

func (t *coreAudioTarget) agentPreload(nextPos int) error {
	at, err := t.selectDevice()
	if err != nil {
		return err
	}
	return at.agentPreload(nextPos)
}

func (t *coreAudioTarget) loadFile(string, string, map[string]any) error { return nil }
func (t *coreAudioTarget) loadFileBatch([]string, string) error          { return nil }

func (t *coreAudioTarget) playlistClear() error {
	at, err := t.agent()
	if err != nil {
		return err
	}
	return at.playlistClear()
}

func (t *coreAudioTarget) playlistRemove(index int) error {
	at, err := t.agent()
	if err != nil {
		return err
	}
	return at.playlistRemove(index)
}

func (t *coreAudioTarget) playlistMove(from, to int) error {
	at, err := t.agent()
	if err != nil {
		return err
	}
	return at.playlistMove(from, to)
}

func (t *coreAudioTarget) getProperty(name string) (any, error) {
	at, err := t.agent()
	if err != nil {
		return nil, err
	}
	return at.getProperty(name)
}

func (t *coreAudioTarget) setProperty(name string, value any) error {
	at, err := t.agent()
	if err != nil {
		return err
	}
	return at.setProperty(name, value)
}

func (t *coreAudioTarget) isRunning() bool {
	_, err := t.agent()
	return err == nil
}

func coreAudioDeviceID(uid, name string) string {
	key := strings.TrimSpace(uid)
	if key == "" {
		key = name
	}
	return coreAudioDevicePrefix + sanitizeDeviceID(key)
}

func describeCoreAudioOutputs(outputs []coreAudioOutputDevice) string {
	if len(outputs) == 0 {
		return "[]"
	}
	parts := make([]string, 0, len(outputs))
	for _, out := range outputs {
		parts = append(parts, fmt.Sprintf("%q=%s", out.Name, out.MPVDevice))
	}
	return "[" + strings.Join(parts, ", ") + "]"
}
