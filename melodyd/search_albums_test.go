package main

import (
	"bufio"
	"bytes"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// newSearchAlbumsApp builds three albums:
//   - "Alpha Artist — First Album (1990)": two tracks, genre Jazz
//   - "Beta Artist — Second Album (2001)": one track
//   - "Gamma Artist — Undated Album (0000)": one track
func newSearchAlbumsApp(t *testing.T) (*app, map[string]int64) {
	t.Helper()
	musicDir := t.TempDir()
	db, err := openMusicDB(filepath.Join(t.TempDir(), "music.db"))
	if err != nil {
		t.Fatalf("openMusicDB: %v", err)
	}
	t.Cleanup(func() { db.close() })

	trackIDs := map[string]int64{}
	add := func(artist, album, date, title string, number int, genre string) {
		artistID, err := db.upsertArtist(artist)
		if err != nil {
			t.Fatalf("upsertArtist: %v", err)
		}
		albumID, err := db.upsertAlbum(artistID, album, date)
		if err != nil {
			t.Fatalf("upsertAlbum: %v", err)
		}
		meta := &trackMeta{
			AlbumID:     albumID,
			Artist:      artist,
			Title:       title,
			TrackNumber: number,
			Duration:    120,
			Path:        filepath.Join(musicDir, artist, album, title+".flac"),
			albumArtist: artist,
			album:       album,
			date:        date,
		}
		if genre != "" {
			meta.tags = map[string][]string{"genre": {genre}}
		}
		id, err := db.upsertTrack(meta)
		if err != nil {
			t.Fatalf("upsertTrack: %v", err)
		}
		trackIDs[title] = id
	}
	add("Alpha Artist", "First Album", "1990", "Opening", 1, "Jazz")
	add("Alpha Artist", "First Album", "1990", "Closing", 2, "Jazz")
	add("Beta Artist", "Second Album", "2001", "Only", 1, "")
	add("Gamma Artist", "Undated Album", "0000", "Mystery", 1, "")

	a := &app{mpdHub: newNotifyHub(), db: db}
	a.cfg.Library.MusicDir = musicDir
	return a, trackIDs
}

// albumOrder extracts the Album lines of a searchalbums response in order.
func albumOrder(out string) []string {
	var albums []string
	for _, line := range strings.Split(out, "\n") {
		if strings.HasPrefix(line, "Album: ") {
			albums = append(albums, strings.TrimPrefix(line, "Album: "))
		}
	}
	return albums
}

func TestSearchAlbumsFiltersSortsAndWindows(t *testing.T) {
	a, trackIDs := newSearchAlbumsApp(t)

	dispatchCapture(t, a, `albumrate "Alpha Artist" "First Album" "1990" 8`)
	dispatchCapture(t, a, fmt.Sprintf(`rate %d 9`, trackIDs["Only"]))

	// Album-level rating filter returns the album record with its data lines.
	out := dispatchCapture(t, a, `searchalbums "(albumrating >= 8)"`)
	if got := albumOrder(out); strings.Join(got, ",") != "First Album" {
		t.Fatalf("albumrating filter = %v, want First Album\n%s", got, out)
	}
	for _, want := range []string{
		"AlbumArtist: Alpha Artist",
		"Date: 1990",
		"X-TrackCount: 2",
		"X-Duration: 240",
		"X-Rating: 8",
		"X-ArtworkUri: Alpha Artist/First Album/Opening.flac",
	} {
		if !strings.Contains(out, want) {
			t.Fatalf("albumrating response missing %q:\n%s", want, out)
		}
	}
	if strings.Contains(out, "X-ComputedRating") {
		t.Fatalf("computed rating must need the 70%% track threshold:\n%s", out)
	}

	// Track-level terms match albums containing a matching track; the fully
	// rated single-track album reports its computed mean.
	out = dispatchCapture(t, a, `searchalbums "(rating >= 9)"`)
	if got := albumOrder(out); strings.Join(got, ",") != "Second Album" {
		t.Fatalf("track rating filter = %v, want Second Album\n%s", got, out)
	}
	if !strings.Contains(out, "X-ComputedRating: 9.0") {
		t.Fatalf("expected computed rating for fully rated album:\n%s", out)
	}
	out = dispatchCapture(t, a, `searchalbums "(Genre == \"Jazz\")"`)
	if got := albumOrder(out); strings.Join(got, ",") != "First Album" {
		t.Fatalf("genre filter = %v, want First Album\n%s", got, out)
	}

	// Album-level text terms use search semantics (case-insensitive).
	out = dispatchCapture(t, a, `searchalbums "(albumartist contains \"beta\")"`)
	if got := albumOrder(out); strings.Join(got, ",") != "Second Album" {
		t.Fatalf("albumartist contains = %v, want Second Album\n%s", got, out)
	}

	// Undated albums report their stored 0000 identity verbatim.
	out = dispatchCapture(t, a, `searchalbums "(album == \"Undated Album\")"`)
	if !strings.Contains(out, "Date: 0000") {
		t.Fatalf("undated album must report Date: 0000:\n%s", out)
	}

	// Sort by rating descending, then window the top result.
	out = dispatchCapture(t, a, `searchalbums "(albumartist contains \"\")" sort -rating`)
	if got := albumOrder(out); got[0] != "First Album" {
		t.Fatalf("sort -rating first = %v, want First Album\n%s", got, out)
	}
	out = dispatchCapture(t, a, `searchalbums "(albumartist contains \"\")" sort -rating window 0:1`)
	if got := albumOrder(out); strings.Join(got, ",") != "First Album" {
		t.Fatalf("window 0:1 = %v, want First Album\n%s", got, out)
	}

	// Unsupported terms and sort tags fail closed.
	if err := dispatchError(t, a, `searchalbums "(base \"x\")"`); err == nil {
		t.Fatalf("unsupported filter tag must ACK")
	}
	if err := dispatchError(t, a, `searchalbums "(album == \"x\")" sort title`); err == nil {
		t.Fatalf("unsupported sort tag must ACK")
	}
}

func TestAlbumRateNormalizesEmptyDate(t *testing.T) {
	a, _ := newSearchAlbumsApp(t)

	// Standard listings omit "Date: 0000", so clients rate undated albums
	// with an empty date; both spellings address one identity.
	dispatchCapture(t, a, `albumrate "Gamma Artist" "Undated Album" "" 6`)
	out := dispatchCapture(t, a, `getalbumrating "Gamma Artist" "Undated Album" "0000"`)
	if !strings.Contains(out, "rating: 6") {
		t.Fatalf("empty-date albumrate must resolve to the 0000 identity:\n%s", out)
	}
	out = dispatchCapture(t, a, `searchalbums "(albumrating == 6)"`)
	if got := albumOrder(out); strings.Join(got, ",") != "Undated Album" {
		t.Fatalf("albumrating after empty-date rate = %v, want Undated Album\n%s", got, out)
	}
}

// dispatchError runs one command and returns its protocol error, if any.
func dispatchError(t *testing.T, a *app, line string) *mpdError {
	t.Helper()
	c := &mpdConn{
		writer: bufio.NewWriter(&bytes.Buffer{}),
		app:    a,
		logger: log.New(os.Stderr, "", 0),
	}
	cmd, args := parseCommand(line)
	return c.dispatch(cmd, args)
}
