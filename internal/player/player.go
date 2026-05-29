package player

import (
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/gopxl/beep/v2"
	"github.com/gopxl/beep/v2/flac"
	"github.com/gopxl/beep/v2/mp3"
	"github.com/gopxl/beep/v2/speaker"
	"github.com/gopxl/beep/v2/vorbis"
	"github.com/gopxl/beep/v2/wav"
)

const (
	OutputSampleRate = beep.SampleRate(48000)
	bufferSize       = 4800 // 100ms at 48kHz
)

// Player handles audio decoding, gapless playback, ReplayGain, and output.
type Player struct {
	mu sync.Mutex

	// Current playback state
	current    beep.StreamSeekCloser
	currentFmt beep.Format
	currentSrc io.Closer // underlying file/http body

	// Preloaded next track for gapless
	next    beep.StreamSeekCloser
	nextFmt beep.Format
	nextSrc io.Closer

	// The mixer streamer registered with the speaker
	mixer *gaplessMixer

	// State
	paused   bool
	volume   float64 // 0-100
	rgMode   string  // "track", "album", "off"
	rgTrack  float64 // ReplayGain track gain in dB
	rgAlbum  float64 // ReplayGain album gain in dB
	nrgTrack float64 // next track RG
	nrgAlbum float64 // next track RG

	// Callbacks
	OnTrackEnd func() // called when current track ends naturally
}

// gaplessMixer is the streamer registered with the speaker. It wraps the
// current decoded stream and handles gapless transition to the next track.
type gaplessMixer struct {
	p *Player
}

func (g *gaplessMixer) Stream(samples [][2]float64) (int, bool) {
	g.p.mu.Lock()
	defer g.p.mu.Unlock()

	if g.p.current == nil {
		for i := range samples {
			samples[i] = [2]float64{}
		}
		return len(samples), true
	}

	if g.p.paused {
		for i := range samples {
			samples[i] = [2]float64{}
		}
		return len(samples), true
	}

	gain := g.p.currentGain()
	filled := 0

	for filled < len(samples) {
		n, ok := g.p.current.Stream(samples[filled:])
		for i := filled; i < filled+n; i++ {
			samples[i][0] *= gain
			samples[i][1] *= gain
		}
		filled += n

		if !ok {
			g.p.current.Close()
			if g.p.currentSrc != nil {
				g.p.currentSrc.Close()
				g.p.currentSrc = nil
			}

			if g.p.next != nil {
				// Gapless transition
				g.p.current = g.p.next
				g.p.currentFmt = g.p.nextFmt
				g.p.currentSrc = g.p.nextSrc
				g.p.rgTrack = g.p.nrgTrack
				g.p.rgAlbum = g.p.nrgAlbum
				g.p.next = nil
				g.p.nextSrc = nil
				gain = g.p.currentGain()

				if g.p.OnTrackEnd != nil {
					go g.p.OnTrackEnd()
				}
				continue
			}

			// No next track — playback stops
			g.p.current = nil
			if g.p.OnTrackEnd != nil {
				go g.p.OnTrackEnd()
			}

			for i := filled; i < len(samples); i++ {
				samples[i] = [2]float64{}
			}
			return len(samples), true
		}
	}
	return filled, true
}

func (*gaplessMixer) Err() error { return nil }

// currentGain returns the linear gain factor for the current track.
func (p *Player) currentGain() float64 {
	vol := p.volume / 100.0
	var rgDB float64
	switch p.rgMode {
	case "track":
		rgDB = p.rgTrack
	case "album":
		rgDB = p.rgAlbum
	default:
		rgDB = 0
	}
	rg := math.Pow(10, rgDB/20)
	return vol * rg
}

// New creates and initializes a new Player.
func New() *Player {
	p := &Player{
		volume: 100,
		rgMode: "off",
	}
	p.mixer = &gaplessMixer{p: p}

	speaker.Init(OutputSampleRate, bufferSize)
	speaker.Play(p.mixer)

	return p
}

// Play loads and starts playing a track. Stops any current playback.
// formatHint is an optional filename used for format detection when path has no extension (e.g. HTTP URLs).
func (p *Player) Play(path, formatHint string, rgTrack, rgAlbum float64) error {
	stream, format, src, err := openAudio(path, formatHint)
	if err != nil {
		return fmt.Errorf("open %s: %w", path, err)
	}

	resampled := resampleStream(stream, format)

	speaker.Lock()
	p.mu.Lock()
	if p.current != nil {
		p.current.Close()
	}
	if p.currentSrc != nil {
		p.currentSrc.Close()
	}
	if p.next != nil {
		p.next.Close()
	}
	if p.nextSrc != nil {
		p.nextSrc.Close()
	}
	p.current = resampled
	p.currentFmt = format
	p.currentSrc = src
	p.rgTrack = rgTrack
	p.rgAlbum = rgAlbum
	p.next = nil
	p.nextSrc = nil
	p.paused = false
	p.mu.Unlock()
	speaker.Unlock()

	return nil
}

// Preload prepares the next track for gapless playback.
// formatHint is an optional filename used for format detection when path has no extension.
func (p *Player) Preload(path, formatHint string, rgTrack, rgAlbum float64) error {
	stream, format, src, err := openAudio(path, formatHint)
	if err != nil {
		return fmt.Errorf("preload %s: %w", path, err)
	}

	resampled := resampleStream(stream, format)

	speaker.Lock()
	p.mu.Lock()
	if p.next != nil {
		p.next.Close()
	}
	if p.nextSrc != nil {
		p.nextSrc.Close()
	}
	p.next = resampled
	p.nextFmt = format
	p.nextSrc = src
	p.nrgTrack = rgTrack
	p.nrgAlbum = rgAlbum
	p.mu.Unlock()
	speaker.Unlock()

	return nil
}

// Pause pauses playback.
func (p *Player) Pause() {
	speaker.Lock()
	p.mu.Lock()
	p.paused = true
	p.mu.Unlock()
	speaker.Unlock()
}

// Resume resumes playback.
func (p *Player) Resume() {
	speaker.Lock()
	p.mu.Lock()
	p.paused = false
	p.mu.Unlock()
	speaker.Unlock()
}

// Stop stops playback and clears all streams.
func (p *Player) Stop() {
	speaker.Lock()
	p.mu.Lock()
	if p.current != nil {
		p.current.Close()
		p.current = nil
	}
	if p.currentSrc != nil {
		p.currentSrc.Close()
		p.currentSrc = nil
	}
	if p.next != nil {
		p.next.Close()
		p.next = nil
	}
	if p.nextSrc != nil {
		p.nextSrc.Close()
		p.nextSrc = nil
	}
	p.paused = false
	p.mu.Unlock()
	speaker.Unlock()
}

// Seek seeks to the given position in seconds.
func (p *Player) Seek(seconds float64) error {
	speaker.Lock()
	p.mu.Lock()
	defer func() {
		p.mu.Unlock()
		speaker.Unlock()
	}()

	if p.current == nil {
		return nil
	}

	samplePos := int(seconds * float64(OutputSampleRate))
	total := p.current.Len()
	if samplePos < 0 {
		samplePos = 0
	}
	if samplePos > total {
		samplePos = total
	}
	return p.current.Seek(samplePos)
}

// SetVolume sets the volume (0-100).
func (p *Player) SetVolume(level float64) {
	if level < 0 {
		level = 0
	}
	if level > 100 {
		level = 100
	}
	speaker.Lock()
	p.mu.Lock()
	p.volume = level
	p.mu.Unlock()
	speaker.Unlock()
}

// SetReplayGain sets the ReplayGain mode ("track", "album", "off").
func (p *Player) SetReplayGain(mode string) {
	speaker.Lock()
	p.mu.Lock()
	p.rgMode = mode
	p.mu.Unlock()
	speaker.Unlock()
}

// State returns the current playback state.
func (p *Player) State() (state string, elapsed, duration float64, vol float64) {
	p.mu.Lock()
	defer p.mu.Unlock()

	vol = p.volume

	if p.current == nil {
		return "stop", 0, 0, vol
	}
	if p.paused {
		state = "pause"
	} else {
		state = "play"
	}

	elapsed = float64(p.current.Position()) / float64(OutputSampleRate)
	duration = float64(p.current.Len()) / float64(OutputSampleRate)

	return state, elapsed, duration, vol
}

// Close shuts down the player.
func (p *Player) Close() {
	p.Stop()
	speaker.Close()
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

// ---------------------------------------------------------------------------
// Audio file opening and format detection
// ---------------------------------------------------------------------------

func openAudio(path, formatHint string) (beep.StreamSeekCloser, beep.Format, io.Closer, error) {
	var src io.ReadSeekCloser
	var closer io.Closer

	if strings.HasPrefix(path, "http://") || strings.HasPrefix(path, "https://") {
		resp, err := http.Get(path)
		if err != nil {
			return nil, beep.Format{}, nil, err
		}
		tmp, err := os.CreateTemp("", "melody-stream-*"+filepath.Ext(path))
		if err != nil {
			resp.Body.Close()
			return nil, beep.Format{}, nil, err
		}
		if _, err := io.Copy(tmp, resp.Body); err != nil {
			resp.Body.Close()
			tmp.Close()
			os.Remove(tmp.Name())
			return nil, beep.Format{}, nil, err
		}
		resp.Body.Close()
		if _, err := tmp.Seek(0, io.SeekStart); err != nil {
			tmp.Close()
			os.Remove(tmp.Name())
			return nil, beep.Format{}, nil, err
		}
		src = tmp
		closer = &tempFileCloser{f: tmp}
	} else {
		f, err := os.Open(path)
		if err != nil {
			return nil, beep.Format{}, nil, err
		}
		src = f
		closer = f
	}

	fmtPath := path
	if formatHint != "" {
		fmtPath = formatHint
	}
	stream, format, err := decodeAudio(src, fmtPath)
	if err != nil {
		closer.Close()
		return nil, beep.Format{}, nil, err
	}

	return stream, format, closer, nil
}

type tempFileCloser struct {
	f *os.File
}

func (t *tempFileCloser) Close() error {
	name := t.f.Name()
	t.f.Close()
	return os.Remove(name)
}

func decodeAudio(src io.ReadSeekCloser, path string) (beep.StreamSeekCloser, beep.Format, error) {
	ext := strings.ToLower(filepath.Ext(path))
	switch ext {
	case ".flac":
		return flac.Decode(src)
	case ".mp3":
		return mp3.Decode(src)
	case ".ogg", ".oga":
		return vorbis.Decode(src)
	case ".wav":
		return wav.Decode(src)
	case ".opus":
		return decodeOpus(src)
	default:
		return nil, beep.Format{}, fmt.Errorf("unsupported format: %s", ext)
	}
}

func resampleStream(s beep.StreamSeekCloser, format beep.Format) beep.StreamSeekCloser {
	if format.SampleRate == OutputSampleRate {
		return s
	}
	return &resampledStream{
		resampled: beep.Resample(4, format.SampleRate, OutputSampleRate, s),
		inner:     s,
		ratio:     float64(OutputSampleRate) / float64(format.SampleRate),
	}
}

type resampledStream struct {
	resampled beep.Streamer
	inner     beep.StreamSeekCloser
	ratio     float64
}

func (r *resampledStream) Stream(samples [][2]float64) (int, bool) {
	return r.resampled.Stream(samples)
}

func (r *resampledStream) Err() error {
	return r.inner.Err()
}

func (r *resampledStream) Len() int {
	return int(float64(r.inner.Len()) * r.ratio)
}

func (r *resampledStream) Position() int {
	return int(float64(r.inner.Position()) * r.ratio)
}

func (r *resampledStream) Seek(p int) error {
	innerPos := int(float64(p) / r.ratio)
	return r.inner.Seek(innerPos)
}

func (r *resampledStream) Close() error {
	return r.inner.Close()
}
