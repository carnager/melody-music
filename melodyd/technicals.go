package main

import (
	"io"
	"os"
	"strconv"
	"strings"
)

// audioTechnicals are the stream properties the scanner stores per track
// (docs/protocol.md): codec name plus the decoded format. Zero values mean
// the property is unknown; lossy codecs have no stored bit depth.
type audioTechnicals struct {
	Codec         string
	SampleRate    int
	BitsPerSample int
	Channels      int
}

// technicalsFromFile reads stream properties from the same container
// headers the duration readers already parse. Unknown or unparsable files
// return zero values rather than errors; the scanner treats those as
// "probe knew nothing".
func technicalsFromFile(path string) audioTechnicals {
	f, err := os.Open(path)
	if err != nil {
		return audioTechnicals{}
	}
	defer f.Close()

	header := make([]byte, 8)
	if _, err := io.ReadFull(f, header); err != nil {
		return audioTechnicals{}
	}
	switch {
	case string(header[:4]) == "fLaC":
		f.Seek(4, io.SeekStart)
		return flacTechnicals(f)
	case string(header[:4]) == "OggS":
		f.Seek(0, io.SeekStart)
		return oggTechnicals(f)
	case header[0] == 0xFF && (header[1]&0xE0) == 0xE0:
		f.Seek(0, io.SeekStart)
		return mp3Technicals(f)
	case string(header[4:8]) == "ftyp":
		f.Seek(0, io.SeekStart)
		return mp4Technicals(f)
	}
	return audioTechnicals{}
}

// flacTechnicals reads the STREAMINFO block. File position must be right
// after the "fLaC" magic.
func flacTechnicals(f *os.File) audioTechnicals {
	blockHeader := make([]byte, 4)
	if _, err := io.ReadFull(f, blockHeader); err != nil {
		return audioTechnicals{}
	}
	if blockHeader[0]&0x7F != 0 { // must be STREAMINFO
		return audioTechnicals{}
	}
	si := make([]byte, 34)
	if _, err := io.ReadFull(f, si); err != nil {
		return audioTechnicals{}
	}
	sampleRate := int(si[10])<<12 | int(si[11])<<4 | int(si[12])>>4
	channels := int((si[12]>>1)&0x07) + 1
	bits := int(si[12]&0x01)<<4 | int(si[13])>>4
	return audioTechnicals{
		Codec:         "flac",
		SampleRate:    sampleRate,
		BitsPerSample: bits + 1,
		Channels:      channels,
	}
}

// oggTechnicals reads the identification packet on the first Ogg page.
func oggTechnicals(f *os.File) audioTechnicals {
	buf := make([]byte, 128)
	if _, err := io.ReadFull(f, buf); err != nil {
		return audioTechnicals{}
	}
	if string(buf[:4]) != "OggS" {
		return audioTechnicals{}
	}
	nSegments := int(buf[26])
	dataStart := 27 + nSegments
	if dataStart >= len(buf) {
		return audioTechnicals{}
	}
	// Vorbis identification header: channels u8 at +11, rate u32le at +12.
	if dataStart+16 < len(buf) && string(buf[dataStart+1:dataStart+7]) == "vorbis" {
		rate := int(buf[dataStart+12]) | int(buf[dataStart+13])<<8 |
			int(buf[dataStart+14])<<16 | int(buf[dataStart+15])<<24
		return audioTechnicals{
			Codec:      "vorbis",
			SampleRate: rate,
			Channels:   int(buf[dataStart+11]),
		}
	}
	// OpusHead: channel count u8 at +9; Opus always decodes at 48 kHz.
	if dataStart+10 < len(buf) && string(buf[dataStart:dataStart+8]) == "OpusHead" {
		return audioTechnicals{
			Codec:      "opus",
			SampleRate: 48000,
			Channels:   int(buf[dataStart+9]),
		}
	}
	return audioTechnicals{}
}

// mp3Technicals reads the first frame header, mirroring mp3Duration's sync
// search and tables.
func mp3Technicals(f *os.File) audioTechnicals {
	buf := make([]byte, 256)
	if _, err := io.ReadFull(f, buf); err != nil {
		return audioTechnicals{}
	}
	off := 0
	for off < len(buf)-4 {
		if buf[off] == 0xFF && (buf[off+1]&0xE0) == 0xE0 {
			break
		}
		off++
	}
	if off >= len(buf)-4 {
		return audioTechnicals{}
	}
	b1 := buf[off+1]
	b2 := buf[off+2]
	b3 := buf[off+3]
	version := (b1 >> 3) & 0x03
	sampleIdx := (b2 >> 2) & 0x03
	if version == 1 || sampleIdx == 3 {
		return audioTechnicals{}
	}
	sampleRateTable := [4]int{44100, 48000, 32000, 0}
	sampleRate := sampleRateTable[sampleIdx]
	if version != 3 {
		sampleRate /= 2
	}
	channels := 2
	if (b3>>6)&0x03 == 3 { // mono channel mode
		channels = 1
	}
	return audioTechnicals{Codec: "mp3", SampleRate: sampleRate, Channels: channels}
}

// mp4Technicals scans the leading boxes for an mp4a or alac audio sample
// entry and reads its channel count, sample size, and sample rate.
func mp4Technicals(f *os.File) audioTechnicals {
	const scanLimit = 256 * 1024
	buf := make([]byte, scanLimit)
	read, err := io.ReadFull(f, buf)
	if err != nil && read == 0 {
		return audioTechnicals{}
	}
	buf = buf[:read]
	for off := 0; off+28 <= len(buf); off++ {
		fourcc := string(buf[off : off+4])
		if fourcc != "mp4a" && fourcc != "alac" {
			continue
		}
		// AudioSampleEntry after the fourcc: 6 reserved + 2 data-reference
		// index + 8 reserved, then channelcount u16, samplesize u16,
		// 4 predefined/reserved, samplerate as 16.16 fixed point.
		entry := buf[off+4:]
		if len(entry) < 26 {
			continue
		}
		channels := int(entry[16])<<8 | int(entry[17])
		sampleSize := int(entry[18])<<8 | int(entry[19])
		sampleRate := int(entry[24])<<8 | int(entry[25])
		if channels == 0 || channels > 8 || sampleRate < 8000 || sampleRate > 384000 {
			continue
		}
		technicals := audioTechnicals{
			Codec:      "aac",
			SampleRate: sampleRate,
			Channels:   channels,
		}
		if fourcc == "alac" {
			technicals.Codec = "alac"
			technicals.BitsPerSample = sampleSize
		}
		return technicals
	}
	return audioTechnicals{}
}

// isTechnicalConditionTag reports whether a filter condition addresses the
// stored stream technicals or ReplayGain values rather than a generic tag.
// Both live in dedicated `tracks` columns, never in track_tags, so they must
// bypass the tag-table lookup — otherwise every track looks like it lacks
// them.
func isTechnicalConditionTag(tag string) bool {
	switch tag {
	case "codec", "samplerate", "bitspersample", "channels", "length":
		return true
	}
	return replayGainConditionField(tag) != ""
}

// replayGainConditionField maps a filter tag to its replay_gain map key,
// accepting both the canonical separator-free spelling clients send
// (replaygainalbumgain) and the conventional tag spelling
// (replaygain_album_gain). Returns "" when the tag is not a ReplayGain
// condition.
func replayGainConditionField(tag string) string {
	switch strings.ReplaceAll(strings.ToLower(tag), "_", "") {
	case "replaygaintrackgain":
		return "track_gain"
	case "replaygainalbumgain":
		return "album_gain"
	case "replaygaintrackpeak":
		return "track_peak"
	case "replaygainalbumpeak":
		return "album_peak"
	}
	return ""
}

// replayGainValue reports the stored value and whether the track carries it.
// buildTrackMap omits the replay_gain map entirely when a file has no gain
// data, and stores 0 for individual values the scanner did not find.
func replayGainValue(track map[string]any, field string) (float64, bool) {
	values, ok := track["replay_gain"].(map[string]any)
	if !ok {
		return 0, false
	}
	value, present := values[field]
	if !present {
		return 0, false
	}
	number := floatFromAny(value, 0)
	return number, number != 0
}

// matchReplayGainCondition evaluates one ReplayGain condition. The
// empty-value forms carry MPD's present/absent semantics; comparisons are
// decimal (gains are dB, peaks are linear amplitudes), so they compare as
// floats rather than through the integer rating comparator.
func matchReplayGainCondition(track map[string]any, cond filterCondition, field string) bool {
	value, present := replayGainValue(track, field)
	if cond.value == "" {
		switch cond.op {
		case "==":
			return !present
		case "!=":
			return present
		}
		return false
	}
	if !present {
		return false
	}
	operand, err := strconv.ParseFloat(strings.TrimSuffix(strings.TrimSpace(cond.value), "dB"), 64)
	if err != nil {
		return false
	}
	switch cond.op {
	case ">":
		return value > operand
	case ">=":
		return value >= operand
	case "<":
		return value < operand
	case "<=":
		return value <= operand
	case "!=":
		return value != operand
	default:
		return value == operand
	}
}

// matchTechnicalCondition evaluates one technical condition against a track
// map. Unknown values (0 or empty: the scanner has not probed the file, or
// the codec has no stored bit depth) never match, mirroring tkq's
// pseudo-field semantics.
func matchTechnicalCondition(track map[string]any, cond filterCondition) bool {
	if field := replayGainConditionField(cond.tag); field != "" {
		return matchReplayGainCondition(track, cond, field)
	}
	if cond.tag == "codec" {
		codec := stringify(track["codec"])
		if codec == "" {
			return false
		}
		if cond.op == "contains" {
			return strings.Contains(strings.ToLower(codec), strings.ToLower(cond.value))
		}
		return strings.EqualFold(codec, cond.value)
	}
	var value int
	switch cond.tag {
	case "samplerate":
		value = intFromAny(track["samplerate"], 0)
	case "bitspersample":
		value = intFromAny(track["bitspersample"], 0)
	case "channels":
		value = intFromAny(track["channels"], 0)
	case "length":
		value = int(floatFromAny(track["duration"], 0))
	}
	if value <= 0 {
		return false
	}
	operand, err := strconv.Atoi(cond.value)
	if err != nil {
		return false
	}
	op := cond.op
	if op == "" {
		op = "=="
	}
	return compareRating(value, op, operand)
}

// filterTracksByTechnicals keeps the tracks satisfying every condition.
func filterTracksByTechnicals(tracks []map[string]any, conds []filterCondition) []map[string]any {
	if len(conds) == 0 {
		return tracks
	}
	var kept []map[string]any
	for _, track := range tracks {
		matches := true
		for _, cond := range conds {
			if !matchTechnicalCondition(track, cond) {
				matches = false
				break
			}
		}
		if matches {
			kept = append(kept, track)
		}
	}
	return kept
}
