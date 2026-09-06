package main

import (
	"log"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

// newAddedTestApp builds an app whose database holds three tracks with distinct
// Added timestamps (oldest.flac < middle.flac < newest.flac).
func newAddedTestApp(t *testing.T) *app {
	t.Helper()
	musicDir := t.TempDir()
	db, err := openMusicDB(filepath.Join(t.TempDir(), "music.db"))
	if err != nil {
		t.Fatalf("openMusicDB: %v", err)
	}
	t.Cleanup(func() { db.close() })

	base := time.Date(2024, 3, 1, 12, 0, 0, 0, time.UTC).Unix()
	for i, name := range []string{"oldest", "middle", "newest"} {
		artistID, err := db.upsertArtist("Artist " + name)
		if err != nil {
			t.Fatalf("upsertArtist: %v", err)
		}
		albumID, err := db.upsertAlbum(artistID, "Album "+name, "2020")
		if err != nil {
			t.Fatalf("upsertAlbum: %v", err)
		}
		path := filepath.Join(musicDir, name+".flac")
		if _, err := db.upsertTrack(&trackMeta{
			AlbumID:     albumID,
			Title:       "Song " + name,
			TrackNumber: 1,
			Duration:    100,
			Path:        path,
			// file_modified is stored in unix milliseconds, matching the scanner
			FileModified: (base + int64(i)*86400) * 1000,
			albumArtist:  "Artist " + name,
			album:        "Album " + name,
		}); err != nil {
			t.Fatalf("upsertTrack: %v", err)
		}
		if _, err := db.db.Exec(`UPDATE tracks SET added = ? WHERE path = ?`,
			base+int64(i)*86400, path); err != nil {
			t.Fatalf("set added: %v", err)
		}
	}

	a := &app{mpdHub: newNotifyHub(), db: db}
	a.cfg.Library.MusicDir = musicDir
	return a
}

// fileOrder extracts the bare filenames from "file:" lines of an MPD response.
func fileOrder(out string) []string {
	var files []string
	for _, line := range strings.Split(out, "\n") {
		if strings.HasPrefix(line, "file: ") {
			files = append(files, strings.TrimSuffix(strings.TrimPrefix(line, "file: "), ".flac"))
		}
	}
	return files
}

func TestFindSortAdded(t *testing.T) {
	a := newAddedTestApp(t)

	out := dispatchCapture(t, a, `find "(base '')" sort -Added`)
	if got := fileOrder(out); strings.Join(got, ",") != "newest,middle,oldest" {
		t.Fatalf("sort -Added order = %v, want newest,middle,oldest\n%s", got, out)
	}
	if !strings.Contains(out, "Added: 2024-03-03T12:00:00Z") {
		t.Fatalf("response missing Added timestamp:\n%s", out)
	}
	if !strings.Contains(out, "Last-Modified: 2024-03-03T12:00:00Z") {
		t.Fatalf("response missing Last-Modified (ms->s conversion?):\n%s", out)
	}

	out = dispatchCapture(t, a, `find "(base '')" sort Added`)
	if got := fileOrder(out); strings.Join(got, ",") != "oldest,middle,newest" {
		t.Fatalf("sort Added order = %v, want oldest,middle,newest\n%s", got, out)
	}

	// window applies after sorting
	out = dispatchCapture(t, a, `find "(base '')" sort -Added window 0:1`)
	if got := fileOrder(out); strings.Join(got, ",") != "newest" {
		t.Fatalf("sort -Added window 0:1 = %v, want newest\n%s", got, out)
	}
}

func TestFindSortUnsupportedTag(t *testing.T) {
	a := newAddedTestApp(t)
	c := &mpdConn{app: a, logger: log.New(os.Stderr, "", 0)}
	if err := cmdFind(c, []string{"(base '')", "sort", "-Nonsense"}); err == nil {
		t.Fatal("find sort -Nonsense should return an error")
	}
}

func TestFindAddedSince(t *testing.T) {
	a := newAddedTestApp(t)

	// Cutoff between "middle" (2024-03-02) and "newest" (2024-03-03)
	out := dispatchCapture(t, a, `find "(added-since '2024-03-02T18:00:00Z')"`)
	if got := fileOrder(out); strings.Join(got, ",") != "newest" {
		t.Fatalf("added-since = %v, want newest\n%s", got, out)
	}

	// Date-only and unix-seconds forms parse too
	out = dispatchCapture(t, a, `find "(added-since '2024-01-01')"`)
	if got := fileOrder(out); len(got) != 3 {
		t.Fatalf("added-since 2024-01-01 matched %v, want all 3", got)
	}
	cutoff := time.Date(2024, 3, 2, 18, 0, 0, 0, time.UTC).Unix()
	out = dispatchCapture(t, a, `find "(added-since '`+strconv.FormatInt(cutoff, 10)+`')"`)
	if got := fileOrder(out); strings.Join(got, ",") != "newest" {
		t.Fatalf("added-since unix = %v, want newest\n%s", got, out)
	}

	// The since filter must not leak into subsequent commands on the same conn.
	out = dispatchCapture(t, a, `find "(base '')"`)
	if got := fileOrder(out); len(got) != 3 {
		t.Fatalf("plain find after added-since matched %v, want all 3", got)
	}
}

func TestFindModifiedSince(t *testing.T) {
	a := newAddedTestApp(t)
	// Cutoff (seconds) between "middle" and "newest"; file_modified is in ms.
	out := dispatchCapture(t, a, `find "(modified-since '2024-03-02T18:00:00Z')"`)
	if got := fileOrder(out); strings.Join(got, ",") != "newest" {
		t.Fatalf("modified-since = %v, want newest\n%s", got, out)
	}
}

// TestAddedSurvivesRescan verifies the insertion timestamp is kept when an
// existing path is upserted again (the scanner's rescan path).
func TestAddedSurvivesRescan(t *testing.T) {
	a := newAddedTestApp(t)
	path := filepath.Join(a.cfg.Library.MusicDir, "oldest.flac")

	var before int64
	if err := a.db.db.QueryRow(`SELECT added FROM tracks WHERE path = ?`, path).Scan(&before); err != nil {
		t.Fatalf("read added: %v", err)
	}
	track, err := a.db.trackByPath(path)
	if err != nil {
		t.Fatalf("trackByPath: %v", err)
	}
	albumID := int64(intFromAny(track["album_id"], 0))
	if _, err := a.db.upsertTrack(&trackMeta{
		AlbumID:      albumID,
		Title:        "Song oldest",
		TrackNumber:  1,
		Duration:     100,
		Path:         path,
		FileModified: 12345,
		albumArtist:  "Artist oldest",
		album:        "Album oldest",
	}); err != nil {
		t.Fatalf("re-upsert: %v", err)
	}
	var after int64
	if err := a.db.db.QueryRow(`SELECT added FROM tracks WHERE path = ?`, path).Scan(&after); err != nil {
		t.Fatalf("read added after rescan: %v", err)
	}
	if before != after {
		t.Fatalf("added changed across rescan: %d -> %d", before, after)
	}
}

// TestAddedMigrationBackfill verifies rows predating the added column inherit
// their created_at scan-in time when migrate() runs.
func TestAddedMigrationBackfill(t *testing.T) {
	a := newAddedTestApp(t)
	if _, err := a.db.db.Exec(`UPDATE tracks SET added = 0, created_at = '2023-06-15 08:30:00'`); err != nil {
		t.Fatalf("reset added: %v", err)
	}
	if err := a.db.migrate(); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	want := time.Date(2023, 6, 15, 8, 30, 0, 0, time.UTC).Unix()
	var got int64
	if err := a.db.db.QueryRow(`SELECT added FROM tracks LIMIT 1`).Scan(&got); err != nil {
		t.Fatalf("read added: %v", err)
	}
	if got != want {
		t.Fatalf("backfilled added = %d, want %d (2023-06-15T08:30:00Z)", got, want)
	}
}
