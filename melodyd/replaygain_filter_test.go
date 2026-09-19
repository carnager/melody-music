package main

import (
	"path/filepath"
	"strings"
	"testing"
)

// newReplayGainApp builds two tracks that carry ReplayGain values and one
// that does not — the shape that made "(replaygainalbumgain == ”)" match
// everything while the values sat in the tracks columns.
func newReplayGainApp(t *testing.T) *app {
	t.Helper()
	musicDir := t.TempDir()
	db, err := openMusicDB(filepath.Join(t.TempDir(), "music.db"))
	if err != nil {
		t.Fatalf("openMusicDB: %v", err)
	}
	t.Cleanup(func() { db.close() })

	add := func(title string, trackGain, albumGain, trackPeak, albumPeak float64) {
		artistID, err := db.upsertArtist("Gain Artist")
		if err != nil {
			t.Fatalf("upsertArtist: %v", err)
		}
		albumID, err := db.upsertAlbum(artistID, "Loud Album", "2001")
		if err != nil {
			t.Fatalf("upsertAlbum: %v", err)
		}
		if _, err := db.upsertTrack(&trackMeta{
			AlbumID:         albumID,
			Artist:          "Gain Artist",
			Title:           title,
			TrackNumber:     1,
			Duration:        120,
			Path:            filepath.Join(musicDir, title+".flac"),
			ReplayGainTrack: trackGain,
			ReplayGainAlbum: albumGain,
			PeakTrack:       trackPeak,
			PeakAlbum:       albumPeak,
			albumArtist:     "Gain Artist",
			album:           "Loud Album",
			date:            "2001",
		}); err != nil {
			t.Fatalf("upsertTrack: %v", err)
		}
	}
	add("Scanned", -7.5, -6.25, 0.98, 0.99)
	add("AlsoScanned", -3.25, -6.25, 0.91, 0.99)
	add("Unscanned", 0, 0, 0, 0)

	a := &app{mpdHub: newNotifyHub(), db: db}
	a.cfg.Library.MusicDir = musicDir
	return a
}

func TestReplayGainConditionsReadDedicatedColumns(t *testing.T) {
	a := newReplayGainApp(t)

	// The reported bug: the empty-value form matched every track because
	// ReplayGain never reaches track_tags.
	missing := fileOrder(dispatchCapture(t, a, `find "(replaygainalbumgain == \"\")"`))
	if len(missing) != 1 || !strings.HasSuffix(missing[0], "Unscanned") {
		t.Fatalf("album-gain MISSING = %v, want only Unscanned", missing)
	}
	present := fileOrder(dispatchCapture(t, a, `find "(replaygainalbumgain != \"\")"`))
	if len(present) != 2 {
		t.Fatalf("album-gain PRESENT = %v, want the two scanned tracks", present)
	}

	// The conventional underscore spelling addresses the same column.
	if got := fileOrder(dispatchCapture(t, a,
		`find "(replaygain_album_gain == \"\")"`)); len(got) != 1 {
		t.Fatalf("underscore spelling = %v, want only Unscanned", got)
	}

	// Decimal comparisons: gains are dB, peaks linear amplitudes.
	louder := fileOrder(dispatchCapture(t, a, `find "(replaygaintrackgain > -5)"`))
	if len(louder) != 1 || !strings.HasSuffix(louder[0], "AlsoScanned") {
		t.Fatalf("track gain > -5 = %v, want AlsoScanned", louder)
	}
	quieter := fileOrder(dispatchCapture(t, a, `find "(replaygaintrackgain < -5)"`))
	if len(quieter) != 1 || !strings.HasSuffix(quieter[0], "Scanned") {
		t.Fatalf("track gain < -5 = %v, want Scanned", quieter)
	}
	if got := fileOrder(dispatchCapture(t, a, `find "(replaygaintrackpeak >= 0.95)"`)); len(got) != 1 {
		t.Fatalf("track peak >= 0.95 = %v, want Scanned", got)
	}
	if got := fileOrder(dispatchCapture(t, a,
		`find "(replaygainalbumgain == -6.25)"`)); len(got) != 2 {
		t.Fatalf("album gain == -6.25 = %v, want both scanned tracks", got)
	}

	// Unscanned tracks never match a comparison, mirroring the technical
	// pseudo-fields.
	if got := fileOrder(dispatchCapture(t, a, `find "(replaygaintrackgain < 100)"`)); len(got) != 2 {
		t.Fatalf("comparison must skip unscanned tracks, got %v", got)
	}

	// Structured trees and searchalbums use the same resolution.
	combined := fileOrder(dispatchCapture(t, a,
		`find "((replaygainalbumgain != \"\") AND (replaygaintrackgain > -5))"`))
	if len(combined) != 1 || !strings.HasSuffix(combined[0], "AlsoScanned") {
		t.Fatalf("combined tree = %v, want AlsoScanned", combined)
	}
	albums := albumOrder(dispatchCapture(t, a, `searchalbums "(replaygainalbumgain != \"\")"`))
	if strings.Join(albums, ",") != "Loud Album" {
		t.Fatalf("searchalbums album-gain = %v, want Loud Album", albums)
	}
}
