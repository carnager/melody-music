package main

import (
	"log"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/dhowden/tag"
)

// fakeMetadata implements tag.Metadata with a fixed format and raw map.
type fakeMetadata struct {
	format tag.Format
	raw    map[string]interface{}
}

func (f *fakeMetadata) Format() tag.Format          { return f.format }
func (f *fakeMetadata) FileType() tag.FileType      { return tag.UnknownFileType }
func (f *fakeMetadata) Title() string               { return "" }
func (f *fakeMetadata) Album() string               { return "" }
func (f *fakeMetadata) Artist() string              { return "" }
func (f *fakeMetadata) AlbumArtist() string         { return "" }
func (f *fakeMetadata) Composer() string            { return "" }
func (f *fakeMetadata) Year() int                   { return 0 }
func (f *fakeMetadata) Genre() string               { return "" }
func (f *fakeMetadata) Track() (int, int)           { return 0, 0 }
func (f *fakeMetadata) Disc() (int, int)            { return 0, 0 }
func (f *fakeMetadata) Picture() *tag.Picture       { return nil }
func (f *fakeMetadata) Lyrics() string              { return "" }
func (f *fakeMetadata) Comment() string             { return "" }
func (f *fakeMetadata) Raw() map[string]interface{} { return f.raw }

func TestNormalizeRawTagsVorbis(t *testing.T) {
	m := &fakeMetadata{format: tag.VORBIS, raw: map[string]interface{}{
		"genre":                      "Shoegaze; Dream Pop",
		"composer":                   "Kevin Shields",
		"label":                      "Creation",
		"musicbrainz_trackid":        "aaaa-recording",
		"musicbrainz_releasetrackid": "bbbb-track",
		"musicbrainz_albumid":        "cccc-album",
		"musicbrainz_artistid":       "dddd-artist",
		"unknown_junk":               "ignored",
	}}
	got := normalizeRawTags(m)

	if want := []string{"Shoegaze", "Dream Pop"}; strings.Join(got["genre"], ",") != strings.Join(want, ",") {
		t.Errorf("genre = %v, want %v", got["genre"], want)
	}
	if v := got["composer"]; len(v) != 1 || v[0] != "Kevin Shields" {
		t.Errorf("composer = %v", v)
	}
	if v := got["label"]; len(v) != 1 || v[0] != "Creation" {
		t.Errorf("label = %v", v)
	}
	if v := got["musicbrainz_trackid"]; len(v) != 1 || v[0] != "aaaa-recording" {
		t.Errorf("musicbrainz_trackid = %v", v)
	}
	if v := got["musicbrainz_releasetrackid"]; len(v) != 1 || v[0] != "bbbb-track" {
		t.Errorf("musicbrainz_releasetrackid = %v", v)
	}
	if _, ok := got["unknown_junk"]; ok {
		t.Error("unknown raw keys must not be stored")
	}
}

func TestNormalizeRawTagsID3(t *testing.T) {
	m := &fakeMetadata{format: tag.ID3v2_4, raw: map[string]interface{}{
		"TCON":   "Post-Punk\x00Shoegaze",
		"TCOM":   "Composer A",
		"TPUB":   "4AD",
		"TXXX":   &tag.Comm{Description: "MusicBrainz Album Id", Text: "cccc-album"},
		"TXXX_0": &tag.Comm{Description: "MusicBrainz Artist Id", Text: "dddd-artist"},
		"TXXX_1": &tag.Comm{Description: "MusicBrainz Release Track Id", Text: "bbbb-track"},
		"UFID":   &tag.UFID{Provider: "http://musicbrainz.org", Identifier: []byte("aaaa-recording")},
	}}
	got := normalizeRawTags(m)

	if want := "Post-Punk,Shoegaze"; strings.Join(got["genre"], ",") != want {
		t.Errorf("genre = %v, want %v", got["genre"], want)
	}
	if v := got["label"]; len(v) != 1 || v[0] != "4AD" {
		t.Errorf("label = %v", v)
	}
	if v := got["musicbrainz_albumid"]; len(v) != 1 || v[0] != "cccc-album" {
		t.Errorf("musicbrainz_albumid = %v", v)
	}
	if v := got["musicbrainz_artistid"]; len(v) != 1 || v[0] != "dddd-artist" {
		t.Errorf("musicbrainz_artistid = %v", v)
	}
	if v := got["musicbrainz_releasetrackid"]; len(v) != 1 || v[0] != "bbbb-track" {
		t.Errorf("musicbrainz_releasetrackid = %v", v)
	}
	if v := got["musicbrainz_trackid"]; len(v) != 1 || v[0] != "aaaa-recording" {
		t.Errorf("musicbrainz_trackid = %v", v)
	}
}

// newTagsTestApp builds an app with two albums; the first carries genre and
// MusicBrainz tags on its track.
func newTagsTestApp(t *testing.T) *app {
	t.Helper()
	musicDir := t.TempDir()
	db, err := openMusicDB(filepath.Join(t.TempDir(), "music.db"))
	if err != nil {
		t.Fatalf("openMusicDB: %v", err)
	}
	t.Cleanup(func() { db.close() })

	add := func(artist, album, title string, tags map[string][]string) {
		artistID, err := db.upsertArtist(artist)
		if err != nil {
			t.Fatalf("upsertArtist: %v", err)
		}
		albumID, err := db.upsertAlbum(artistID, album, "1991")
		if err != nil {
			t.Fatalf("upsertAlbum: %v", err)
		}
		if _, err := db.upsertTrack(&trackMeta{
			AlbumID:      albumID,
			Artist:       artist,
			Title:        title,
			TrackNumber:  1,
			Duration:     100,
			Path:         filepath.Join(musicDir, title+".flac"),
			FileModified: 1000,
			albumArtist:  artist,
			album:        album,
			date:         "1991",
			tags:         tags,
		}); err != nil {
			t.Fatalf("upsertTrack: %v", err)
		}
	}

	add("My Bloody Valentine", "Loveless", "Only Shallow", map[string][]string{
		"genre":               {"Shoegaze", "Dream Pop"},
		"musicbrainz_trackid": {"aaaa-recording"},
		"musicbrainz_albumid": {"cccc-album"},
	})
	add("Slowdive", "Souvlaki", "Alison", map[string][]string{
		"genre": {"Shoegaze"},
	})
	add("Autechre", "Incunabula", "Bike", map[string][]string{
		"genre": {"IDM"},
	})

	a := &app{mpdHub: newNotifyHub(), db: db}
	a.cfg.Library.MusicDir = musicDir
	return a
}

func TestTagTypesIncludesGenericTags(t *testing.T) {
	a := newTagsTestApp(t)
	out := dispatchCapture(t, a, "tagtypes")
	for _, want := range []string{"tagtype: Genre", "tagtype: MUSICBRAINZ_TRACKID", "tagtype: MUSICBRAINZ_ALBUMID"} {
		if !strings.Contains(out, want) {
			t.Errorf("tagtypes missing %q:\n%s", want, out)
		}
	}
}

func TestTrackResponseCarriesGenericTags(t *testing.T) {
	a := newTagsTestApp(t)
	out := dispatchCapture(t, a, `find "(Album == 'Loveless')"`)
	for _, want := range []string{
		"Genre: Shoegaze",
		"Genre: Dream Pop",
		"MUSICBRAINZ_TRACKID: aaaa-recording",
		"MUSICBRAINZ_ALBUMID: cccc-album",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("find response missing %q:\n%s", want, out)
		}
	}
}

func TestListGenre(t *testing.T) {
	a := newTagsTestApp(t)
	out := dispatchCapture(t, a, "list Genre")
	for _, want := range []string{"Genre: Shoegaze", "Genre: Dream Pop", "Genre: IDM"} {
		if !strings.Contains(out, want) {
			t.Errorf("list Genre missing %q:\n%s", want, out)
		}
	}

	// Filtered by albumartist
	out = dispatchCapture(t, a, `list Genre AlbumArtist "Autechre"`)
	if !strings.Contains(out, "Genre: IDM") || strings.Contains(out, "Shoegaze") {
		t.Errorf("list Genre AlbumArtist Autechre = \n%s", out)
	}
}

func TestListAlbumByGenre(t *testing.T) {
	a := newTagsTestApp(t)
	out := dispatchCapture(t, a, `list Album Genre "Shoegaze"`)
	if !strings.Contains(out, "Album: Loveless") || !strings.Contains(out, "Album: Souvlaki") {
		t.Errorf("list Album Genre Shoegaze missing albums:\n%s", out)
	}
	if strings.Contains(out, "Incunabula") {
		t.Errorf("list Album Genre Shoegaze must not include Incunabula:\n%s", out)
	}
}

func TestFindByGenreAndMBID(t *testing.T) {
	a := newTagsTestApp(t)

	out := dispatchCapture(t, a, `find "(Genre == 'Shoegaze')"`)
	if got := fileOrder(out); len(got) != 2 {
		t.Fatalf("find Genre Shoegaze matched %v, want 2 tracks\n%s", got, out)
	}

	// search is case-insensitive
	out = dispatchCapture(t, a, `search "(Genre == 'shoegaze')"`)
	if got := fileOrder(out); len(got) != 2 {
		t.Fatalf("search genre shoegaze matched %v, want 2 tracks\n%s", got, out)
	}

	// find is case-sensitive
	out = dispatchCapture(t, a, `find "(Genre == 'shoegaze')"`)
	if got := fileOrder(out); len(got) != 0 {
		t.Fatalf("find genre shoegaze (wrong case) matched %v, want 0\n%s", got, out)
	}

	// combined with albumartist
	out = dispatchCapture(t, a, `find "((Genre == 'Shoegaze') AND (AlbumArtist == 'Slowdive'))"`)
	if got := fileOrder(out); len(got) != 1 || got[0] != "Alison" {
		t.Fatalf("find Genre+AlbumArtist = %v, want [Alison]\n%s", got, out)
	}

	// lookup by MusicBrainz recording id
	out = dispatchCapture(t, a, `find "(MUSICBRAINZ_TRACKID == 'aaaa-recording')"`)
	if got := fileOrder(out); len(got) != 1 || got[0] != "Only Shallow" {
		t.Fatalf("find MUSICBRAINZ_TRACKID = %v, want [Only Shallow]\n%s", got, out)
	}
}

func TestTrackTagsRemovedWithTrack(t *testing.T) {
	a := newTagsTestApp(t)
	if err := a.db.deleteTrackByPath(filepath.Join(a.cfg.Library.MusicDir, "Only Shallow.flac")); err != nil {
		t.Fatalf("deleteTrackByPath: %v", err)
	}
	var n int
	if err := a.db.db.QueryRow(`SELECT COUNT(*) FROM track_tags tt
		WHERE tt.track_id NOT IN (SELECT id FROM tracks)`).Scan(&n); err != nil {
		t.Fatalf("count orphans: %v", err)
	}
	if n != 0 {
		t.Fatalf("orphaned track_tags rows = %d, want 0", n)
	}
	out := dispatchCapture(t, a, "list Genre")
	if strings.Contains(out, "Dream Pop") {
		t.Errorf("deleted track's genre still listed:\n%s", out)
	}
}

func TestListGenericTagGrouped(t *testing.T) {
	a := newTagsTestApp(t)
	if _, err := a.db.db.Exec(`INSERT INTO track_tags(track_id, tag, value)
		SELECT id, 'musicbrainz_releasegroupid', 'rg-' || id FROM tracks`); err != nil {
		t.Fatalf("seed release group ids: %v", err)
	}

	out := dispatchCapture(t, a, "list musicbrainz_releasegroupid group album group albumartist")
	lines := strings.Split(strings.TrimSpace(out), "\n")
	// Expect one AlbumArtist/Album/MBID triplet per album, then OK
	var triplets [][3]string
	for i := 0; i+2 < len(lines); i += 3 {
		if !strings.HasPrefix(lines[i], "AlbumArtist: ") ||
			!strings.HasPrefix(lines[i+1], "Album: ") ||
			!strings.HasPrefix(lines[i+2], "MUSICBRAINZ_RELEASEGROUPID: ") {
			t.Fatalf("row %d not an AlbumArtist/Album/MBID triplet:\n%s", i/3, out)
		}
		triplets = append(triplets, [3]string{lines[i], lines[i+1], lines[i+2]})
	}
	if len(triplets) != 3 {
		t.Fatalf("got %d triplets, want 3 (one per album):\n%s", len(triplets), out)
	}
	if triplets[0][0] != "AlbumArtist: Autechre" || triplets[0][1] != "Album: Incunabula" {
		t.Errorf("triplets not sorted by albumartist:\n%s", out)
	}

	// Unsupported group tag must error, not silently return flat values
	c := &mpdConn{app: a, logger: log.New(os.Stderr, "", 0)}
	if err := cmdList(c, []string{"genre", "group", "composer"}); err == nil {
		t.Error("list Genre group Composer should return an error, not flat values")
	}
}
