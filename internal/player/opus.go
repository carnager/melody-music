package player

import (
	"fmt"
	"io"

	"github.com/gopxl/beep/v2"
	"github.com/pion/opus"
	"github.com/pion/opus/pkg/oggreader"
)

// maxOpusFrameSamples is the maximum number of samples per channel in a single
// Opus frame (120ms at 48kHz).
const maxOpusFrameSamples = 5760

// opusStream wraps a pion/opus decoder behind a beep.StreamSeekCloser.
type opusStream struct {
	src     io.ReadSeekCloser
	decoder opus.Decoder
	header  *oggreader.OggHeader
	ogg     *oggreader.OggReader

	channels int
	preSkip  int
	totalLen int // total playable samples (granule - preskip)

	// Decoded sample buffer (stereo [2]float64 regardless of source channels).
	buf    [][2]float64
	bufPos int

	// Global sample position (in output frames).
	position int

	// Reusable decode buffer.
	decodeBuf []float32
}

func decodeOpus(src io.ReadSeekCloser) (beep.StreamSeekCloser, beep.Format, error) {
	ogg, header, err := oggreader.NewWith(src)
	if err != nil {
		return nil, beep.Format{}, fmt.Errorf("ogg: %w", err)
	}

	channels := int(header.Channels)
	if channels < 1 || channels > 2 {
		return nil, beep.Format{}, fmt.Errorf("unsupported opus channel count: %d", channels)
	}

	// Scan to find total length from last page granule position.
	var totalGranule uint64
	for {
		_, pageHeader, pErr := ogg.ParseNextPacket()
		if pErr != nil {
			break
		}
		if pageHeader.GranulePosition > 0 {
			totalGranule = pageHeader.GranulePosition
		}
	}

	// Seek back to start and re-init.
	if _, err = src.Seek(0, io.SeekStart); err != nil {
		return nil, beep.Format{}, err
	}
	ogg, header, err = oggreader.NewWith(src)
	if err != nil {
		return nil, beep.Format{}, fmt.Errorf("ogg re-init: %w", err)
	}

	dec, err := opus.NewDecoderWithOutput(48000, channels)
	if err != nil {
		return nil, beep.Format{}, fmt.Errorf("opus decoder: %w", err)
	}

	preSkip := int(header.PreSkip)
	totalLen := int(totalGranule) - preSkip
	if totalLen < 0 {
		totalLen = 0
	}

	s := &opusStream{
		src:       src,
		decoder:   dec,
		header:    header,
		ogg:       ogg,
		channels:  channels,
		preSkip:   preSkip,
		totalLen:  totalLen,
		decodeBuf: make([]float32, maxOpusFrameSamples*channels),
	}

	// Skip PreSkip samples.
	s.skipSamples(preSkip)

	format := beep.Format{
		SampleRate:  beep.SampleRate(48000),
		NumChannels: 2,
		Precision:   4,
	}

	return s, format, nil
}

// decodeNextPacket reads and decodes one Opus packet, appending to s.buf.
func (s *opusStream) decodeNextPacket() error {
	packet, _, err := s.ogg.ParseNextPacket()
	if err != nil {
		return err
	}

	n, err := s.decoder.DecodeToFloat32(packet, s.decodeBuf)
	if err != nil {
		return fmt.Errorf("opus decode: %w", err)
	}

	for i := 0; i < n; i++ {
		var sample [2]float64
		if s.channels == 1 {
			v := float64(s.decodeBuf[i])
			sample = [2]float64{v, v}
		} else {
			sample = [2]float64{
				float64(s.decodeBuf[i*2]),
				float64(s.decodeBuf[i*2+1]),
			}
		}
		s.buf = append(s.buf, sample)
	}
	return nil
}

// skipSamples discards n samples from the decode buffer, decoding more if needed.
func (s *opusStream) skipSamples(n int) {
	for n > 0 {
		avail := len(s.buf) - s.bufPos
		if avail <= 0 {
			if err := s.decodeNextPacket(); err != nil {
				return
			}
			continue
		}
		skip := n
		if skip > avail {
			skip = avail
		}
		s.bufPos += skip
		n -= skip
	}
	// Compact buffer.
	s.buf = s.buf[s.bufPos:]
	s.bufPos = 0
}

func (s *opusStream) Stream(samples [][2]float64) (int, bool) {
	filled := 0
	for filled < len(samples) {
		avail := len(s.buf) - s.bufPos
		if avail <= 0 {
			if err := s.decodeNextPacket(); err != nil {
				// EOF or error — fill rest with silence.
				for i := filled; i < len(samples); i++ {
					samples[i] = [2]float64{}
				}
				s.position += filled
				return filled, false
			}
			continue
		}
		n := len(samples) - filled
		if n > avail {
			n = avail
		}
		copy(samples[filled:], s.buf[s.bufPos:s.bufPos+n])
		s.bufPos += n
		filled += n
	}
	s.position += filled

	// Compact periodically.
	if s.bufPos > 4096 {
		s.buf = append([][2]float64(nil), s.buf[s.bufPos:]...)
		s.bufPos = 0
	}

	return filled, true
}

func (s *opusStream) Err() error { return nil }

func (s *opusStream) Len() int {
	return s.totalLen
}

func (s *opusStream) Position() int {
	return s.position
}

func (s *opusStream) Seek(p int) error {
	if p < 0 {
		p = 0
	}
	if p > s.totalLen {
		p = s.totalLen
	}

	// Reset to beginning.
	if _, err := s.src.Seek(0, io.SeekStart); err != nil {
		return err
	}
	ogg, _, err := oggreader.NewWith(s.src)
	if err != nil {
		return err
	}
	dec, err := opus.NewDecoderWithOutput(48000, s.channels)
	if err != nil {
		return err
	}

	s.ogg = ogg
	s.decoder = dec
	s.buf = s.buf[:0]
	s.bufPos = 0
	s.position = 0

	// Skip PreSkip + target position.
	s.skipSamples(s.preSkip + p)
	s.position = p

	return nil
}

func (s *opusStream) Close() error {
	return s.src.Close()
}
