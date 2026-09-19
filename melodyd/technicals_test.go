package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writeTempFile(t *testing.T, name string, data []byte) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatalf("write %s: %v", name, err)
	}
	return path
}

func TestTechnicalsFromFileParsesContainers(t *testing.T) {
	// FLAC: fLaC magic + STREAMINFO with 96000 Hz, 2 channels, 24 bits.
	streaminfo := make([]byte, 34)
	const rate = 96000
	streaminfo[10] = byte(rate >> 12)
	streaminfo[11] = byte((rate >> 4) & 0xFF)
	streaminfo[12] = byte(rate&0x0F)<<4 | byte(2-1)<<1 | byte((24-1)>>4)
	streaminfo[13] = byte((24-1)&0x0F) << 4
	flac := append([]byte("fLaC"), 0x00, 0x00, 0x00, 34)
	flac = append(flac, streaminfo...)
	got := technicalsFromFile(writeTempFile(t, "tone.flac", flac))
	want := audioTechnicals{Codec: "flac", SampleRate: 96000, BitsPerSample: 24, Channels: 2}
	if got != want {
		t.Fatalf("flac technicals = %+v, want %+v", got, want)
	}

	// Ogg Opus: one page whose packet is an OpusHead with 2 channels.
	opus := make([]byte, 128)
	copy(opus, "OggS")
	opus[26] = 1  // one segment
	opus[27] = 19 // segment length
	copy(opus[28:], "OpusHead")
	opus[28+9] = 2
	got = technicalsFromFile(writeTempFile(t, "tone.opus", opus))
	want = audioTechnicals{Codec: "opus", SampleRate: 48000, Channels: 2}
	if got != want {
		t.Fatalf("opus technicals = %+v, want %+v", got, want)
	}

	// Ogg Vorbis: identification header with 44100 Hz stereo.
	vorbis := make([]byte, 128)
	copy(vorbis, "OggS")
	vorbis[26] = 1
	vorbis[27] = 30
	vorbis[28] = 0x01
	copy(vorbis[29:], "vorbis")
	vorbis[28+11] = 2
	binary.LittleEndian.PutUint32(vorbis[28+12:], 44100)
	got = technicalsFromFile(writeTempFile(t, "tone.ogg", vorbis))
	want = audioTechnicals{Codec: "vorbis", SampleRate: 44100, Channels: 2}
	if got != want {
		t.Fatalf("vorbis technicals = %+v, want %+v", got, want)
	}

	// MP3: MPEG1 Layer III header, 44100 Hz, joint stereo.
	mp3 := make([]byte, 256)
	mp3[0] = 0xFF
	mp3[1] = 0xFB // MPEG1, Layer III
	mp3[2] = 0x90 // bitrate 128, 44100 Hz
	mp3[3] = 0x40 // joint stereo
	got = technicalsFromFile(writeTempFile(t, "tone.mp3", mp3))
	want = audioTechnicals{Codec: "mp3", SampleRate: 44100, Channels: 2}
	if got != want {
		t.Fatalf("mp3 technicals = %+v, want %+v", got, want)
	}

	// M4A: ftyp box followed by an mp4a sample entry (44100 Hz stereo).
	m4a := make([]byte, 512)
	binary.BigEndian.PutUint32(m4a[0:], 16)
	copy(m4a[4:], "ftypM4A ")
	entry := 64
	copy(m4a[entry:], "mp4a")
	binary.BigEndian.PutUint16(m4a[entry+4+16:], 2)     // channels
	binary.BigEndian.PutUint16(m4a[entry+4+18:], 16)    // sample size
	binary.BigEndian.PutUint16(m4a[entry+4+24:], 44100) // rate hi16
	got = technicalsFromFile(writeTempFile(t, "tone.m4a", m4a))
	want = audioTechnicals{Codec: "aac", SampleRate: 44100, Channels: 2}
	if got != want {
		t.Fatalf("m4a technicals = %+v, want %+v", got, want)
	}
}

func TestTechnicalFiltersAndListingLines(t *testing.T) {
	a, trackIDs := newSearchAlbumsApp(t)
	// The scanner stores technicals per track; inject them directly.
	set := func(title, codec string, rate, bits, channels int) {
		if _, err := a.db.db.Exec(
			`UPDATE tracks SET codec=?, sample_rate=?, bits_per_sample=?, channels=? WHERE id=?`,
			codec, rate, bits, channels, trackIDs[title]); err != nil {
			t.Fatalf("set technicals: %v", err)
		}
	}
	set("Opening", "flac", 96000, 24, 2)
	set("Closing", "flac", 44100, 16, 2)
	set("Only", "opus", 48000, 0, 2)

	out := dispatchCapture(t, a, `find "(samplerate >= 96000)"`)
	if got := fileOrder(out); strings.Join(got, ",") != "Alpha Artist/First Album/Opening" {
		t.Fatalf("samplerate filter = %v\n%s", got, out)
	}
	if !strings.Contains(out, "Format: 96000:24:2") {
		t.Fatalf("expected Format line:\n%s", out)
	}
	if !strings.Contains(out, "X-Codec: flac") {
		t.Fatalf("expected X-Codec line:\n%s", out)
	}

	// Lossy tracks report float bits in Format and never match a bit-depth
	// comparison; unprobed tracks never match anything technical.
	out = dispatchCapture(t, a, `find "(codec == \"opus\")"`)
	if !strings.Contains(out, "Format: 48000:f:2") {
		t.Fatalf("expected float Format for opus:\n%s", out)
	}
	out = dispatchCapture(t, a, `find "(bitspersample >= 24)"`)
	if got := fileOrder(out); strings.Join(got, ",") != "Alpha Artist/First Album/Opening" {
		t.Fatalf("bitspersample filter = %v\n%s", got, out)
	}
	out = dispatchCapture(t, a, `find "(length >= 100)"`)
	if len(fileOrder(out)) != 4 {
		t.Fatalf("length filter should match all fixture tracks:\n%s", out)
	}

	// Technical terms combine with tag terms and reach searchalbums.
	out = dispatchCapture(t, a, `find "(Genre == \"Jazz\")" "(samplerate >= 96000)"`)
	if got := fileOrder(out); strings.Join(got, ",") != "Alpha Artist/First Album/Opening" {
		t.Fatalf("combined genre+samplerate = %v\n%s", got, out)
	}
	out = dispatchCapture(t, a, `searchalbums "(samplerate >= 96000)"`)
	if got := albumOrder(out); strings.Join(got, ",") != "First Album" {
		t.Fatalf("searchalbums samplerate = %v, want First Album\n%s", got, out)
	}
	out = dispatchCapture(t, a, fmt.Sprintf(`searchalbums "(codec == \"opus\")"`))
	if got := albumOrder(out); strings.Join(got, ",") != "Second Album" {
		t.Fatalf("searchalbums codec = %v, want Second Album\n%s", got, out)
	}
}
