package main

import (
	"bytes"
	"context"
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	upnpAVTransportService      = "urn:schemas-upnp-org:service:AVTransport:1"
	upnpRenderingControlService = "urn:schemas-upnp-org:service:RenderingControl:1"
	upnpMediaRendererST         = "urn:schemas-upnp-org:device:MediaRenderer:1"
	upnpRootDeviceST            = "upnp:rootdevice"
)

var errNotUPnPMediaRenderer = errors.New("not a UPnP MediaRenderer")

type upnpTarget struct {
	app       *app
	id        string
	name      string
	location  string
	baseURL   *url.URL
	avURL     string
	rcURL     string
	client    *http.Client
	mu        sync.Mutex
	current   string
	duration  float64
	volume    float64
	available bool
	playing   bool
	monitorID int
}

func (a *app) upnpDiscoveryLoop() {
	for {
		if err := a.discoverUPnPOnce(); err != nil {
			a.logger.Printf("upnp discovery: %v", err)
		}
		time.Sleep(60 * time.Second)
	}
}

func (a *app) discoverUPnPOnce() error {
	addr, err := net.ResolveUDPAddr("udp4", "239.255.255.250:1900")
	if err != nil {
		return err
	}
	conn, err := net.ListenPacket("udp4", "0.0.0.0:0")
	if err != nil {
		return err
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(4 * time.Second))

	targets := []string{
		upnpMediaRendererST,
		"urn:schemas-upnp-org:device:MediaRenderer:2",
		"urn:schemas-upnp-org:device:MediaRenderer:3",
		upnpRootDeviceST,
	}
	for _, st := range targets {
		msg := strings.Join([]string{
			"M-SEARCH * HTTP/1.1",
			"HOST: 239.255.255.250:1900",
			`MAN: "ssdp:discover"`,
			"MX: 2",
			"ST: " + st,
			"",
			"",
		}, "\r\n")
		if _, err := conn.WriteTo([]byte(msg), addr); err != nil {
			return err
		}
	}

	seen := map[string]struct{}{}
	buf := make([]byte, 64*1024)
	for {
		n, _, err := conn.ReadFrom(buf)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				return nil
			}
			return err
		}
		location := ssdpHeader(string(buf[:n]), "location")
		if location == "" {
			continue
		}
		if _, ok := seen[location]; ok {
			continue
		}
		seen[location] = struct{}{}
		if err := a.registerUPnPRenderer(location); err != nil {
			if errors.Is(err, errNotUPnPMediaRenderer) {
				continue
			}
			a.logger.Printf("upnp %s: %v", location, err)
		}
	}
}

func ssdpHeader(resp, name string) string {
	name = strings.ToLower(name)
	for _, line := range strings.Split(resp, "\n") {
		line = strings.TrimSpace(line)
		idx := strings.IndexByte(line, ':')
		if idx < 0 {
			continue
		}
		if strings.ToLower(strings.TrimSpace(line[:idx])) == name {
			return strings.TrimSpace(line[idx+1:])
		}
	}
	return ""
}

func (a *app) registerUPnPRenderer(location string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, location, nil)
	if err != nil {
		return err
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fmt.Errorf("device description returned %s", resp.Status)
	}
	data, err := io.ReadAll(io.LimitReader(resp.Body, 2*1024*1024))
	if err != nil {
		return err
	}

	var desc upnpDeviceDescription
	if err := xml.Unmarshal(data, &desc); err != nil {
		return err
	}
	dev := findUPnPDevice(desc.Device, "urn:schemas-upnp-org:device:MediaRenderer:1")
	if dev == nil {
		dev = findUPnPDevice(desc.Device, "urn:schemas-upnp-org:device:MediaRenderer:2")
	}
	if dev == nil {
		dev = findUPnPDevice(desc.Device, "urn:schemas-upnp-org:device:MediaRenderer:3")
	}
	if dev == nil {
		return errNotUPnPMediaRenderer
	}
	av := findUPnPService(*dev, upnpAVTransportService)
	rc := findUPnPService(*dev, upnpRenderingControlService)
	if av == nil {
		return fmt.Errorf("missing AVTransport service")
	}

	locURL, err := url.Parse(location)
	if err != nil {
		return err
	}
	base := locURL
	if desc.URLBase != "" {
		if u, err := url.Parse(strings.TrimSpace(desc.URLBase)); err == nil {
			base = u
		}
	}

	name := strings.TrimSpace(dev.FriendlyName)
	if name == "" {
		name = locURL.Host
	}
	id := "upnp-" + sanitizeDeviceID(firstNonEmpty(dev.UDN, location))
	avURL := resolveUPnPURL(base, av.ControlURL)
	rcURL := ""
	if rc != nil {
		rcURL = resolveUPnPURL(base, rc.ControlURL)
	}

	a.devicesMu.Lock()
	target, existed := a.upnpTargets[id]
	changed := false
	if existed {
		target.mu.Lock()
		changed = target.location != location || target.avURL != avURL || target.rcURL != rcURL
		target.name = name
		target.location = location
		target.baseURL = base
		target.avURL = avURL
		target.rcURL = rcURL
		target.available = true
		target.mu.Unlock()
	} else {
		target = &upnpTarget{
			app:       a,
			id:        id,
			name:      name,
			location:  location,
			baseURL:   base,
			avURL:     avURL,
			rcURL:     rcURL,
			client:    &http.Client{Timeout: 8 * time.Second},
			volume:    100,
			available: true,
		}
		a.upnpTargets[id] = target
	}
	a.devices[id] = &device{
		ID:       id,
		Name:     name,
		Address:  locURL.Host,
		IsLocal:  false,
		Type:     "upnp",
		Format:   "mp3",
		LastSeen: time.Now(),
	}
	a.devicesMu.Unlock()

	if !existed {
		a.logger.Printf("upnp renderer discovered: %s id=%s addr=%s av=%s rc=%t", name, id, locURL.Host, avURL, rcURL != "")
		a.mpdHub.notify(SubOutput)
	} else if changed {
		a.logger.Printf("upnp renderer updated: %s id=%s addr=%s av=%s rc=%t", name, id, locURL.Host, avURL, rcURL != "")
	}
	return nil
}

type upnpDeviceDescription struct {
	URLBase string     `xml:"URLBase"`
	Device  upnpDevice `xml:"device"`
}

type upnpDevice struct {
	DeviceType   string        `xml:"deviceType"`
	FriendlyName string        `xml:"friendlyName"`
	UDN          string        `xml:"UDN"`
	Services     []upnpService `xml:"serviceList>service"`
	Devices      []upnpDevice  `xml:"deviceList>device"`
}

type upnpService struct {
	ServiceType string `xml:"serviceType"`
	ControlURL  string `xml:"controlURL"`
}

func findUPnPDevice(dev upnpDevice, deviceType string) *upnpDevice {
	if dev.DeviceType == deviceType {
		return &dev
	}
	for _, child := range dev.Devices {
		if found := findUPnPDevice(child, deviceType); found != nil {
			return found
		}
	}
	return nil
}

func findUPnPService(dev upnpDevice, serviceType string) *upnpService {
	for _, svc := range dev.Services {
		if svc.ServiceType == serviceType {
			return &svc
		}
	}
	return nil
}

func resolveUPnPURL(base *url.URL, raw string) string {
	u, err := url.Parse(strings.TrimSpace(raw))
	if err != nil {
		return raw
	}
	return base.ResolveReference(u).String()
}

func sanitizeDeviceID(s string) string {
	s = strings.TrimPrefix(s, "uuid:")
	var b strings.Builder
	for _, r := range strings.ToLower(s) {
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '-' || r == '_' {
			b.WriteRune(r)
		}
	}
	if b.Len() == 0 {
		return "renderer"
	}
	return b.String()
}

func (t *upnpTarget) loadFile(url, mode string, meta map[string]any) error {
	if mode == "append" {
		return nil
	}
	t.app.logger.Printf("upnp %s: load mode=%s url=%s", t.name, mode, url)
	t.mu.Lock()
	t.current = url
	t.playing = false
	t.monitorID++
	t.mu.Unlock()

	if mode == "replace" {
		_ = t.stop()
	}
	if err := t.soap(t.avURL, upnpAVTransportService, "SetAVTransportURI", map[string]string{
		"InstanceID":         "0",
		"CurrentURI":         url,
		"CurrentURIMetaData": "",
	}, nil); err != nil {
		t.app.logger.Printf("upnp %s: SetAVTransportURI failed: %v", t.name, err)
		return err
	}
	if err := t.setProperty("pause", false); err != nil {
		t.app.logger.Printf("upnp %s: Play failed: %v", t.name, err)
		return err
	}
	t.mu.Lock()
	t.monitorID++
	monitorID := t.monitorID
	t.mu.Unlock()
	go t.monitorPlayback(monitorID)
	return nil
}

func (t *upnpTarget) loadFileBatch(urls []string, mode string) error {
	if len(urls) == 0 {
		return nil
	}
	return t.loadFile(urls[0], mode, nil)
}

func (t *upnpTarget) playlistClear() error {
	t.app.logger.Printf("upnp %s: stop", t.name)
	t.mu.Lock()
	t.monitorID++
	t.current = ""
	t.playing = false
	t.mu.Unlock()
	return t.stop()
}

func (t *upnpTarget) stop() error {
	return t.soap(t.avURL, upnpAVTransportService, "Stop", map[string]string{"InstanceID": "0"}, nil)
}

func (t *upnpTarget) playlistRemove(index int) error  { return nil }
func (t *upnpTarget) playlistMove(from, to int) error { return nil }

func (t *upnpTarget) getProperty(name string) (any, error) {
	switch name {
	case "pause":
		state, err := t.transportState()
		if err != nil {
			return nil, err
		}
		return state != "PLAYING" && state != "TRANSITIONING", nil
	case "time-pos":
		pos, _, err := t.positionInfo()
		return pos, err
	case "duration":
		_, dur, err := t.positionInfo()
		if dur > 0 {
			t.mu.Lock()
			t.duration = dur
			t.mu.Unlock()
		}
		return dur, err
	case "volume":
		if t.rcURL == "" {
			t.mu.Lock()
			v := t.volume
			t.mu.Unlock()
			return v, nil
		}
		var out map[string]string
		err := t.soap(t.rcURL, upnpRenderingControlService, "GetVolume", map[string]string{
			"InstanceID": "0",
			"Channel":    "Master",
		}, &out)
		if err != nil {
			return nil, err
		}
		v, _ := strconv.ParseFloat(out["CurrentVolume"], 64)
		t.mu.Lock()
		t.volume = v
		t.mu.Unlock()
		return v, nil
	default:
		return nil, fmt.Errorf("unknown UPnP property: %s", name)
	}
}

func (t *upnpTarget) setProperty(name string, value any) error {
	switch name {
	case "pause":
		paused, _ := value.(bool)
		action := "Play"
		args := map[string]string{"InstanceID": "0", "Speed": "1"}
		if paused {
			action = "Pause"
			args = map[string]string{"InstanceID": "0"}
		}
		err := t.soap(t.avURL, upnpAVTransportService, action, args, nil)
		if err == nil {
			t.mu.Lock()
			t.playing = !paused
			t.mu.Unlock()
		}
		return err
	case "time-pos":
		seconds, ok := value.(float64)
		if !ok {
			return fmt.Errorf("invalid seek value")
		}
		return t.soap(t.avURL, upnpAVTransportService, "Seek", map[string]string{
			"InstanceID": "0",
			"Unit":       "REL_TIME",
			"Target":     secondsToUPnPTime(seconds),
		}, nil)
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
		t.mu.Lock()
		t.volume = vol
		t.mu.Unlock()
		if t.rcURL == "" {
			return nil
		}
		return t.soap(t.rcURL, upnpRenderingControlService, "SetVolume", map[string]string{
			"InstanceID":    "0",
			"Channel":       "Master",
			"DesiredVolume": strconv.Itoa(int(vol + 0.5)),
		}, nil)
	case "replaygain":
		return nil
	default:
		return fmt.Errorf("unknown UPnP property: %s", name)
	}
}

func (t *upnpTarget) isRunning() bool {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.available
}

func (t *upnpTarget) monitorPlayback(id int) {
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
		state, err := t.transportState()
		if err != nil {
			t.app.logger.Printf("upnp %s: transport poll failed: %v", t.name, err)
			continue
		}
		if state != lastState {
			t.app.logger.Printf("upnp %s: transport state=%s", t.name, state)
			lastState = state
		}
		if state == "PLAYING" || state == "TRANSITIONING" {
			wasPlaying = true
			continue
		}
		if wasPlaying && (state == "STOPPED" || state == "NO_MEDIA_PRESENT") {
			t.app.logger.Printf("upnp %s: track ended, advancing queue", t.name)
			t.app.advanceTrack()
			return
		}
	}
}

func (t *upnpTarget) transportState() (string, error) {
	var out map[string]string
	if err := t.soap(t.avURL, upnpAVTransportService, "GetTransportInfo", map[string]string{
		"InstanceID": "0",
	}, &out); err != nil {
		return "", err
	}
	return out["CurrentTransportState"], nil
}

func (t *upnpTarget) positionInfo() (float64, float64, error) {
	var out map[string]string
	err := t.soap(t.avURL, upnpAVTransportService, "GetPositionInfo", map[string]string{
		"InstanceID": "0",
	}, &out)
	if err != nil {
		return 0, 0, err
	}
	return parseUPnPTime(out["RelTime"]), parseUPnPTime(out["TrackDuration"]), nil
}

func (t *upnpTarget) soap(endpoint, service, action string, args map[string]string, out *map[string]string) error {
	if endpoint == "" {
		return fmt.Errorf("missing UPnP endpoint")
	}
	body := buildSOAPBody(service, action, args)
	req, err := http.NewRequest(http.MethodPost, endpoint, strings.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", `text/xml; charset="utf-8"`)
	req.Header.Set("SOAPACTION", `"`+service+"#"+action+`"`)
	resp, err := t.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(io.LimitReader(resp.Body, 2*1024*1024))
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fmt.Errorf("%s returned %s: %s", action, resp.Status, strings.TrimSpace(string(data)))
	}
	if out != nil {
		*out = parseSOAPValues(data)
	}
	return nil
}

func buildSOAPBody(service, action string, args map[string]string) string {
	var b strings.Builder
	b.WriteString(`<?xml version="1.0"?>`)
	b.WriteString(`<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>`)
	b.WriteString(`<u:` + action + ` xmlns:u="` + service + `">`)
	for k, v := range args {
		b.WriteString("<" + k + ">")
		xml.EscapeText(&b, []byte(v))
		b.WriteString("</" + k + ">")
	}
	b.WriteString(`</u:` + action + `></s:Body></s:Envelope>`)
	return b.String()
}

func parseSOAPValues(data []byte) map[string]string {
	dec := xml.NewDecoder(bytes.NewReader(data))
	values := map[string]string{}
	var key string
	for {
		tok, err := dec.Token()
		if err != nil {
			return values
		}
		switch t := tok.(type) {
		case xml.StartElement:
			key = t.Name.Local
		case xml.CharData:
			if key != "" {
				v := strings.TrimSpace(string(t))
				if v != "" {
					values[key] = v
				}
			}
		case xml.EndElement:
			if key == t.Name.Local {
				key = ""
			}
		}
	}
}

func parseUPnPTime(s string) float64 {
	parts := strings.Split(strings.TrimSpace(s), ":")
	if len(parts) != 3 {
		return 0
	}
	h, _ := strconv.ParseFloat(parts[0], 64)
	m, _ := strconv.ParseFloat(parts[1], 64)
	sec, _ := strconv.ParseFloat(parts[2], 64)
	return h*3600 + m*60 + sec
}

func secondsToUPnPTime(seconds float64) string {
	if seconds < 0 {
		seconds = 0
	}
	total := int(seconds + 0.5)
	h := total / 3600
	m := (total % 3600) / 60
	s := total % 60
	return fmt.Sprintf("%02d:%02d:%02d", h, m, s)
}
