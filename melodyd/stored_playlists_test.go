package main

import (
	"fmt"
	"strings"
	"testing"
)

// playlistFiles extracts the bare filenames of "file:" lines, keeping order.
func playlistFiles(out string) []string {
	return fileOrder(out)
}

func newPlaylistApp(t *testing.T) *app {
	t.Helper()
	a, _ := newSearchAlbumsApp(t)
	// Seed a playlist from the fixture library: Opening, Only, Closing.
	for _, title := range []string{"Opening", "Only", "Closing"} {
		uri := map[string]string{
			"Opening": "Alpha Artist/First Album/Opening.flac",
			"Only":    "Beta Artist/Second Album/Only.flac",
			"Closing": "Alpha Artist/First Album/Closing.flac",
		}[title]
		dispatchCapture(t, a, fmt.Sprintf("playlistadd %q %q", "Mix", uri))
	}
	return a
}

func TestListPlaylistReturnsOrderedURIs(t *testing.T) {
	a := newPlaylistApp(t)
	out := dispatchCapture(t, a, `listplaylist "Mix"`)
	got := playlistFiles(out)
	want := "Alpha Artist/First Album/Opening,Beta Artist/Second Album/Only," +
		"Alpha Artist/First Album/Closing"
	if strings.Join(got, ",") != want {
		t.Fatalf("listplaylist order = %v\n%s", got, out)
	}
	if strings.Contains(out, "Title:") {
		t.Fatalf("listplaylist must be URI-only:\n%s", out)
	}
	if err := dispatchError(t, a, `listplaylist "Missing"`); err == nil {
		t.Fatalf("missing playlist must ACK")
	}
}

func TestPlaylistAddInsertsAtPosition(t *testing.T) {
	a := newPlaylistApp(t)
	dispatchCapture(t, a, `playlistadd "Mix" "Gamma Artist/Undated Album/Mystery.flac" 1`)
	got := playlistFiles(dispatchCapture(t, a, `listplaylist "Mix"`))
	if len(got) != 4 || !strings.HasSuffix(got[1], "Mystery") {
		t.Fatalf("insert at 1 = %v", got)
	}
}

func TestPlaylistDeleteCompactsPositions(t *testing.T) {
	a := newPlaylistApp(t)
	dispatchCapture(t, a, `playlistdelete "Mix" 1`)
	got := playlistFiles(dispatchCapture(t, a, `listplaylist "Mix"`))
	if len(got) != 2 || !strings.HasSuffix(got[0], "Opening") ||
		!strings.HasSuffix(got[1], "Closing") {
		t.Fatalf("after delete = %v", got)
	}
	// Positions stayed addressable after compaction: delete the new tail.
	dispatchCapture(t, a, `playlistdelete "Mix" 1`)
	got = playlistFiles(dispatchCapture(t, a, `listplaylist "Mix"`))
	if len(got) != 1 || !strings.HasSuffix(got[0], "Opening") {
		t.Fatalf("after second delete = %v", got)
	}
	if err := dispatchError(t, a, `playlistdelete "Mix" 5`); err == nil {
		t.Fatalf("out-of-range delete must ACK")
	}
	if err := dispatchError(t, a, `playlistdelete "Missing" 0`); err == nil {
		t.Fatalf("missing playlist must ACK")
	}
}

func TestPlaylistMoveBothDirections(t *testing.T) {
	a := newPlaylistApp(t)
	dispatchCapture(t, a, `playlistmove "Mix" 0 2`)
	got := playlistFiles(dispatchCapture(t, a, `listplaylist "Mix"`))
	if !strings.HasSuffix(got[2], "Opening") || !strings.HasSuffix(got[0], "Only") {
		t.Fatalf("move 0->2 = %v", got)
	}
	dispatchCapture(t, a, `playlistmove "Mix" 2 0`)
	got = playlistFiles(dispatchCapture(t, a, `listplaylist "Mix"`))
	if !strings.HasSuffix(got[0], "Opening") {
		t.Fatalf("move 2->0 = %v", got)
	}
	if err := dispatchError(t, a, `playlistmove "Mix" 0 9`); err == nil {
		t.Fatalf("out-of-range move must ACK")
	}
}

func TestRenamePlaylist(t *testing.T) {
	a := newPlaylistApp(t)
	dispatchCapture(t, a, `rename "Mix" "Roadtrip"`)
	if err := dispatchError(t, a, `listplaylist "Mix"`); err == nil {
		t.Fatalf("old name must be gone")
	}
	if got := playlistFiles(dispatchCapture(t, a, `listplaylist "Roadtrip"`)); len(got) != 3 {
		t.Fatalf("renamed playlist content = %v", got)
	}
	dispatchCapture(t, a, `playlistadd "Second" "Beta Artist/Second Album/Only.flac"`)
	if err := dispatchError(t, a, `rename "Roadtrip" "Second"`); err == nil {
		t.Fatalf("rename onto an existing name must ACK")
	}
	if err := dispatchError(t, a, `rename "Missing" "X"`); err == nil {
		t.Fatalf("missing playlist must ACK")
	}
}

func TestPlaylistClearKeepsListing(t *testing.T) {
	a := newPlaylistApp(t)
	dispatchCapture(t, a, `playlistclear "Mix"`)
	if got := playlistFiles(dispatchCapture(t, a, `listplaylist "Mix"`)); len(got) != 0 {
		t.Fatalf("cleared playlist still has %v", got)
	}
	if !strings.Contains(dispatchCapture(t, a, `listplaylists`), "playlist: Mix") {
		t.Fatalf("cleared playlist must stay listed")
	}
	if err := dispatchError(t, a, `playlistclear "Missing"`); err == nil {
		t.Fatalf("missing playlist must ACK")
	}
}

func TestSaveReplacesExistingPlaylist(t *testing.T) {
	a := newPlaylistApp(t)
	// An empty queue saved over "Mix" must replace its content, not add a
	// duplicate playlist row.
	dispatchCapture(t, a, `save "Mix"`)
	if got := playlistFiles(dispatchCapture(t, a, `listplaylist "Mix"`)); len(got) != 0 {
		t.Fatalf("save must replace content, got %v", got)
	}
	if got := strings.Count(dispatchCapture(t, a, `listplaylists`), "playlist: Mix"); got != 1 {
		t.Fatalf("save created %d playlist rows named Mix, want 1", got)
	}
}
