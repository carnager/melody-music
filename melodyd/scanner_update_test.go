package main

import (
	"log"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// TestScanPathTargetedSubtree verifies that scanPath ingests only the requested
// subtree and prunes tracks that disappeared from it, without disturbing the
// rest of the library. This is the path exercised by the MPD `update <uri>`
// command pushed from an external (NAS-side) file-watcher.
func TestScanPathTargetedSubtree(t *testing.T) {
	musicDir := t.TempDir()
	dbFile := filepath.Join(t.TempDir(), "music.db")

	db, err := openMusicDB(dbFile)
	if err != nil {
		t.Fatalf("openMusicDB: %v", err)
	}

	// Two albums on disk. (Empty .flac files fail tag parsing and fall back to
	// filename-based metadata, which is enough to exercise upsert + cleanup.)
	writeFile(t, filepath.Join(musicDir, "ArtistA", "Album1", "01.flac"))
	writeFile(t, filepath.Join(musicDir, "ArtistA", "Album1", "02.flac"))
	writeFile(t, filepath.Join(musicDir, "ArtistB", "Album2", "01.flac"))

	s := newScanner(musicDir, db, log.New(os.Stderr, "", 0), "")

	// Full scan picks up everything.
	if err := s.fullScan(); err != nil {
		t.Fatalf("fullScan: %v", err)
	}
	if n, _ := db.trackCount(); n != 3 {
		t.Fatalf("after full scan: got %d tracks, want 3", n)
	}

	// Add a track to Album1 and remove one; targeted update of only that subtree
	// must reflect both, and must NOT touch ArtistB/Album2.
	writeFile(t, filepath.Join(musicDir, "ArtistA", "Album1", "03.flac"))
	if err := os.Remove(filepath.Join(musicDir, "ArtistA", "Album1", "02.flac")); err != nil {
		t.Fatalf("remove: %v", err)
	}

	if err := s.scanPath("ArtistA/Album1"); err != nil {
		t.Fatalf("scanPath: %v", err)
	}

	// Album1 now has 01 + 03 (02 pruned); Album2 untouched → total 3.
	if n, _ := db.trackCount(); n != 3 {
		t.Fatalf("after targeted scan: got %d tracks, want 3", n)
	}
	if _, err := db.trackIDByPath(filepath.Join(musicDir, "ArtistA", "Album1", "03.flac")); err != nil {
		t.Fatalf("new track not ingested: %v", err)
	}
	if _, err := db.trackIDByPath(filepath.Join(musicDir, "ArtistA", "Album1", "02.flac")); err == nil {
		t.Fatalf("removed track still present in DB")
	}
	if _, err := db.trackIDByPath(filepath.Join(musicDir, "ArtistB", "Album2", "01.flac")); err != nil {
		t.Fatalf("untouched subtree was disturbed: %v", err)
	}

	// A deleted directory removes its subtree on targeted update.
	if err := os.RemoveAll(filepath.Join(musicDir, "ArtistB", "Album2")); err != nil {
		t.Fatalf("rmdir: %v", err)
	}
	if err := s.scanPath("ArtistB/Album2"); err != nil {
		t.Fatalf("scanPath deleted dir: %v", err)
	}
	if n, _ := db.trackCount(); n != 2 {
		t.Fatalf("after subtree delete: got %d tracks, want 2", n)
	}
}

// TestUpsertExistingArtistKeepsAlbumsJoinable is a regression test for a bug
// where adding a new album by an already-known artist through the incremental
// scan path produced an album row with a dangling artist_id (upsertArtist
// returned a phantom LastInsertId on conflict). The album then vanished from
// every artist-joined view even though its track existed.
func TestUpsertExistingArtistKeepsAlbumsJoinable(t *testing.T) {
	musicDir := t.TempDir()
	db, err := openMusicDB(filepath.Join(t.TempDir(), "music.db"))
	if err != nil {
		t.Fatalf("openMusicDB: %v", err)
	}
	s := newScanner(musicDir, db, log.New(os.Stderr, "", 0), "")

	// First album establishes the artist.
	writeFile(t, filepath.Join(musicDir, "Artist", "First", "01.flac"))
	if err := s.fullScan(); err != nil {
		t.Fatalf("fullScan: %v", err)
	}

	// A second album by the SAME artist, ingested incrementally.
	writeFile(t, filepath.Join(musicDir, "Artist", "Second", "01.flac"))
	if err := s.scanPath("Artist/Second"); err != nil {
		t.Fatalf("scanPath: %v", err)
	}

	albums, err := db.allAlbums(false)
	if err != nil {
		t.Fatalf("allAlbums: %v", err)
	}
	titles := map[string]bool{}
	for _, a := range albums {
		titles[stringify(a["album"])] = true
	}
	if !titles["First"] || !titles["Second"] {
		t.Fatalf("expected both albums joinable, got %v", titles)
	}

	// upsertArtist must be idempotent for an existing name.
	id1, err := db.upsertArtist("Artist")
	if err != nil {
		t.Fatalf("upsertArtist: %v", err)
	}
	id2, err := db.upsertArtist("Artist")
	if err != nil {
		t.Fatalf("upsertArtist: %v", err)
	}
	if id1 != id2 || id1 == 0 {
		t.Fatalf("upsertArtist not idempotent: %d vs %d", id1, id2)
	}
}

// TestRequestUpdateQueuesConcurrent verifies that updates arriving in a burst
// are all processed rather than dropped — the bug where a watcher's rapid
// create+rename events lost all but the first, leaving an album unscanned.
func TestRequestUpdateQueuesConcurrent(t *testing.T) {
	musicDir := t.TempDir()
	db, err := openMusicDB(filepath.Join(t.TempDir(), "music.db"))
	if err != nil {
		t.Fatalf("openMusicDB: %v", err)
	}
	writeFile(t, filepath.Join(musicDir, "AlbumA", "01.flac"))
	writeFile(t, filepath.Join(musicDir, "AlbumB", "01.flac"))
	writeFile(t, filepath.Join(musicDir, "AlbumC", "01.flac"))
	s := newScanner(musicDir, db, log.New(os.Stderr, "", 0), "")

	// Fire three updates back-to-back; some will land while another is scanning.
	s.requestUpdate("AlbumA")
	s.requestUpdate("AlbumB")
	s.requestUpdate("AlbumC")

	// Wait for the drain to finish.
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		s.scanMu.Lock()
		done := !s.draining && !s.scanning && len(s.pendingUpdates) == 0
		s.scanMu.Unlock()
		if done {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}

	if n, _ := db.trackCount(); n != 3 {
		t.Fatalf("queued updates were dropped: got %d tracks, want 3", n)
	}
}

// TestScanPathMissingParentDoesNotRemove guards against destructive removals
// when a targeted update points at a path whose parent doesn't exist — a
// malformed URI (e.g. a doubled "flac/flac/…" prefix) or an unreachable mount.
// Such an update must be a no-op, not wipe matching tracks.
func TestScanPathMissingParentDoesNotRemove(t *testing.T) {
	musicDir := t.TempDir()
	db, err := openMusicDB(filepath.Join(t.TempDir(), "music.db"))
	if err != nil {
		t.Fatalf("openMusicDB: %v", err)
	}
	writeFile(t, filepath.Join(musicDir, "The Shits", "01.flac"))
	s := newScanner(musicDir, db, log.New(os.Stderr, "", 0), "")
	if err := s.fullScan(); err != nil {
		t.Fatalf("fullScan: %v", err)
	}
	if n, _ := db.trackCount(); n != 1 {
		t.Fatalf("setup: got %d tracks, want 1", n)
	}

	// Bogus URI with a non-existent parent ("flac/" prefix duplication). The
	// real album lives at "The Shits", not "flac/The Shits".
	if err := s.scanPath("flac/The Shits"); err != nil {
		t.Fatalf("scanPath: %v", err)
	}
	if n, _ := db.trackCount(); n != 1 {
		t.Fatalf("bogus update removed tracks: got %d, want 1 (must be a no-op)", n)
	}

	// Sanity: a genuine deletion (parent still exists) still prunes.
	if err := os.RemoveAll(filepath.Join(musicDir, "The Shits")); err != nil {
		t.Fatalf("rm: %v", err)
	}
	if err := s.scanPath("The Shits"); err != nil {
		t.Fatalf("scanPath delete: %v", err)
	}
	if n, _ := db.trackCount(); n != 0 {
		t.Fatalf("genuine deletion not pruned: got %d, want 0", n)
	}
}

// TestScanPathContainsTraversal ensures a malicious URI cannot reach files
// outside the music root: ".." segments are clamped back inside the root.
func TestScanPathContainsTraversal(t *testing.T) {
	root := t.TempDir()
	musicDir := filepath.Join(root, "music")
	outside := filepath.Join(root, "secret")
	writeFile(t, filepath.Join(outside, "leak.flac"))

	db, err := openMusicDB(filepath.Join(t.TempDir(), "music.db"))
	if err != nil {
		t.Fatalf("openMusicDB: %v", err)
	}
	if err := os.MkdirAll(musicDir, 0o755); err != nil {
		t.Fatalf("mkdir music: %v", err)
	}
	s := newScanner(musicDir, db, log.New(os.Stderr, "", 0), "")

	// "../secret" must NOT escape musicDir and ingest the outside file.
	if err := s.scanPath("../secret"); err != nil {
		t.Fatalf("scanPath: %v", err)
	}
	if _, err := db.trackIDByPath(filepath.Join(outside, "leak.flac")); err == nil {
		t.Fatalf("traversal escaped the music root and ingested an outside file")
	}
	if n, _ := db.trackCount(); n != 0 {
		t.Fatalf("expected 0 tracks after clamped traversal, got %d", n)
	}
}

func writeFile(t *testing.T, path string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	if err := os.WriteFile(path, []byte("not really audio"), 0o644); err != nil {
		t.Fatalf("write %s: %v", path, err)
	}
}
