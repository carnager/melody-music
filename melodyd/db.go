package main

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"fmt"
	"strconv"
	"strings"
	"sync"

	"errors"
	_ "modernc.org/sqlite"
)

type musicDB struct {
	db *sql.DB

	// Cached results for expensive queries, invalidated on scan.
	cacheMu                     sync.Mutex
	cachedAlbumsLatest          []map[string]any
	cachedAlbumsLatestFormatted string // pre-formatted MPD response lines
	// Pre-built, ready-to-send lists for the rofi/launcher client.
	cachedRofiAlbums       string
	cachedRofiAlbumsLatest string
	cachedRofiTracks       string
}

func openMusicDB(path string) (*musicDB, error) {
	// foreign_keys must be on for the ON DELETE CASCADE the schema declares;
	// without it a deleted playlist leaves its tracks behind, and the next
	// playlist to reuse that row id inherits them.
	db, err := sql.Open("sqlite",
		path+"?_journal_mode=WAL&_busy_timeout=5000&_pragma=foreign_keys(1)")
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	m := &musicDB{db: db}
	if err := m.migrate(); err != nil {
		db.Close()
		return nil, err
	}
	return m, nil
}

func (m *musicDB) close() error {
	return m.db.Close()
}

func (m *musicDB) migrateTrackTagsCoveringIndex() error {
	tx, err := m.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`CREATE INDEX IF NOT EXISTS idx_track_tags_tag_value_track
		ON track_tags(tag, value, track_id)`); err != nil {
		return err
	}
	if _, err := tx.Exec(`DROP INDEX IF EXISTS idx_track_tags_tag_value`); err != nil {
		return err
	}
	return tx.Commit()
}

func (m *musicDB) migrate() error {
	_, err := m.db.Exec(`
		CREATE TABLE IF NOT EXISTS library_mounts (
			music_dir TEXT NOT NULL,
			mount_path TEXT NOT NULL,
			PRIMARY KEY (music_dir, mount_path)
		);
		CREATE TABLE IF NOT EXISTS artists (
			id INTEGER PRIMARY KEY,
			name TEXT NOT NULL UNIQUE
		);
		CREATE TABLE IF NOT EXISTS albums (
			id INTEGER PRIMARY KEY,
			artist_id INTEGER REFERENCES artists(id),
			title TEXT NOT NULL,
			date TEXT NOT NULL DEFAULT '0000',
			created_at TEXT NOT NULL DEFAULT (datetime('now'))
		);
		CREATE UNIQUE INDEX IF NOT EXISTS idx_albums_artist_title_date
			ON albums(artist_id, title, date);

		CREATE TABLE IF NOT EXISTS tracks (
			id INTEGER PRIMARY KEY,
			album_id INTEGER REFERENCES albums(id),
			artist TEXT NOT NULL DEFAULT '',
			title TEXT NOT NULL,
			track_number INTEGER DEFAULT 0,
			disc_number INTEGER DEFAULT 1,
			duration REAL DEFAULT 0,
			path TEXT NOT NULL UNIQUE,
			file_modified INTEGER NOT NULL DEFAULT 0,
			replay_gain_track REAL DEFAULT 0,
			replay_gain_album REAL DEFAULT 0,
			peak_track REAL DEFAULT 0,
			peak_album REAL DEFAULT 0,
			rating TEXT NOT NULL DEFAULT '',
			rating_hash TEXT NOT NULL DEFAULT '',
			created_at TEXT NOT NULL DEFAULT (datetime('now')),
			added INTEGER NOT NULL DEFAULT 0,
			codec TEXT NOT NULL DEFAULT '',
			sample_rate INTEGER NOT NULL DEFAULT 0,
			bits_per_sample INTEGER NOT NULL DEFAULT 0,
			channels INTEGER NOT NULL DEFAULT 0
		);
		CREATE TABLE IF NOT EXISTS playlists (
			id INTEGER PRIMARY KEY,
			name TEXT NOT NULL,
			created_at TEXT NOT NULL DEFAULT (datetime('now'))
		);
		CREATE TABLE IF NOT EXISTS playlist_tracks (
			id INTEGER PRIMARY KEY,
			playlist_id INTEGER REFERENCES playlists(id) ON DELETE CASCADE,
			track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE,
			position INTEGER NOT NULL DEFAULT 0
		);
		CREATE INDEX IF NOT EXISTS idx_playlist_tracks_playlist
			ON playlist_tracks(playlist_id, position);

		CREATE TABLE IF NOT EXISTS ratings (
			hash TEXT PRIMARY KEY,
			type TEXT NOT NULL,
			rating INTEGER NOT NULL DEFAULT 0,
			updated_at TEXT NOT NULL DEFAULT (datetime('now'))
		);
	`)
	if err != nil {
		return err
	}

	// Generic per-track tags (genre, composer, MusicBrainz IDs, ...).
	// Detect first-time creation so existing libraries get a forced metadata
	// re-read: the scanner skips files whose mtime is unchanged, which would
	// otherwise leave track_tags empty forever.
	var hadTrackTags int
	m.db.QueryRow(`SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='track_tags'`).Scan(&hadTrackTags)
	if _, err := m.db.Exec(`CREATE TABLE IF NOT EXISTS track_tags (
			track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
			tag TEXT NOT NULL,
			value TEXT NOT NULL
		)`); err != nil {
		return err
	}
	m.db.Exec(`CREATE INDEX IF NOT EXISTS idx_track_tags_track ON track_tags(track_id)`)
	// Upgrade the old two-column index transactionally: genre and other
	// tag lookups can return track IDs directly from the covering index.
	if err := m.migrateTrackTagsCoveringIndex(); err != nil {
		return err
	}
	if hadTrackTags == 0 {
		m.db.Exec(`UPDATE tracks SET file_modified = 0`)
	}
	// Performance indexes
	m.db.Exec(`CREATE INDEX IF NOT EXISTS idx_tracks_album_id ON tracks(album_id)`)
	m.db.Exec(`CREATE INDEX IF NOT EXISTS idx_albums_created_at ON albums(created_at)`)

	// FTS5 for fast search
	m.db.Exec(`CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5(
		artist, albumartist, title, album
	)`)

	// Migration: add stream technicals (docs/protocol.md). Zero/empty means
	// the scanner has not probed the file yet; startup scans backfill lazily.
	m.db.Exec(`ALTER TABLE tracks ADD COLUMN codec TEXT NOT NULL DEFAULT ''`)
	m.db.Exec(`ALTER TABLE tracks ADD COLUMN sample_rate INTEGER NOT NULL DEFAULT 0`)
	m.db.Exec(`ALTER TABLE tracks ADD COLUMN bits_per_sample INTEGER NOT NULL DEFAULT 0`)
	m.db.Exec(`ALTER TABLE tracks ADD COLUMN channels INTEGER NOT NULL DEFAULT 0`)

	// Migration: drop playlist entries whose playlist is gone. Cascades did
	// not fire while foreign keys were off, so these orphans surface as a
	// brand-new playlist arriving pre-filled with someone else's tracks.
	m.db.Exec(`DELETE FROM playlist_tracks WHERE playlist_id NOT IN (SELECT id FROM playlists)`)

	// Migration: a playlist can be a client's scratch list — a working tab
	// rather than a curated playlist. The list itself is an ordinary stored
	// playlist; the flag only says whether clients should present it as one.
	m.db.Exec(`ALTER TABLE playlists ADD COLUMN scratch INTEGER NOT NULL DEFAULT 0`)

	// Migration: add rating_hash column if missing
	m.db.Exec(`ALTER TABLE tracks ADD COLUMN rating_hash TEXT NOT NULL DEFAULT ''`)
	m.db.Exec(`CREATE INDEX IF NOT EXISTS idx_tracks_rating_hash ON tracks(rating_hash)`)

	// Migration: add MPD 0.24 "Added" (database insertion time, unix seconds).
	// Existing rows inherit their created_at, which is the original scan-in time.
	m.db.Exec(`ALTER TABLE tracks ADD COLUMN added INTEGER NOT NULL DEFAULT 0`)
	m.db.Exec(`UPDATE tracks SET added = COALESCE(CAST(strftime('%s', created_at) AS INTEGER), CAST(strftime('%s','now') AS INTEGER)) WHERE added = 0`)
	m.db.Exec(`CREATE INDEX IF NOT EXISTS idx_tracks_added ON tracks(added)`)
	// Backfill empty rating_hash for existing tracks
	var count int
	m.db.QueryRow(`SELECT COUNT(*) FROM tracks WHERE rating_hash = ''`).Scan(&count)
	if count > 0 {
		m.backfillRatingHashes()
	}
	return nil
}

func (m *musicDB) backfillRatingHashes() {
	rows, err := m.db.Query(`SELECT t.id, t.title, t.track_number, a.name, al.title
		FROM tracks t
		INNER JOIN albums al ON al.id = t.album_id
		INNER JOIN artists a ON a.id = al.artist_id
		WHERE t.rating_hash = ''`)
	if err != nil {
		return
	}
	defer rows.Close()
	type entry struct {
		id   int64
		hash string
	}
	var entries []entry
	for rows.Next() {
		var id int64
		var title, albumArtist, album string
		var trackNum int
		if err := rows.Scan(&id, &title, &trackNum, &albumArtist, &album); err != nil {
			return
		}
		entries = append(entries, entry{id, trackRatingHash(albumArtist, album, title, trackNum)})
	}
	tx, err := m.db.Begin()
	if err != nil {
		return
	}
	stmt, err := tx.Prepare(`UPDATE tracks SET rating_hash = ? WHERE id = ?`)
	if err != nil {
		tx.Rollback()
		return
	}
	for _, e := range entries {
		stmt.Exec(e.hash, e.id)
	}
	stmt.Close()
	tx.Commit()
}

// ---------------------------------------------------------------------------
// Artist queries
// ---------------------------------------------------------------------------

func (m *musicDB) upsertArtist(name string) (int64, error) {
	// Use ON CONFLICT DO UPDATE ... RETURNING id so the existing row id is
	// returned on conflict. ON CONFLICT DO NOTHING cannot be relied upon here:
	// after a no-op conflict LastInsertId() returns a stale/phantom rowid rather
	// than 0, which previously produced albums with a dangling artist_id.
	var id int64
	err := m.db.QueryRow(`INSERT INTO artists(name) VALUES(?)
		ON CONFLICT(name) DO UPDATE SET name = excluded.name
		RETURNING id`, name).Scan(&id)
	return id, err
}

func (m *musicDB) allArtists() ([]string, error) {
	rows, err := m.db.Query(`SELECT DISTINCT a.name FROM artists a
		INNER JOIN albums al ON al.artist_id = a.id
		ORDER BY a.name COLLATE NOCASE`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var names []string
	for rows.Next() {
		var name string
		if err := rows.Scan(&name); err != nil {
			return nil, err
		}
		names = append(names, name)
	}
	return names, rows.Err()
}

// ---------------------------------------------------------------------------
// Album queries
// ---------------------------------------------------------------------------

func (m *musicDB) upsertAlbum(artistID int64, title, date string) (int64, error) {
	// RETURNING id reliably yields the existing row id on conflict; see the note
	// in upsertArtist about why LastInsertId() can't be trusted here.
	var id int64
	err := m.db.QueryRow(`INSERT INTO albums(artist_id, title, date)
		VALUES(?, ?, ?)
		ON CONFLICT(artist_id, title, date) DO UPDATE SET title = excluded.title
		RETURNING id`, artistID, title, date).Scan(&id)
	return id, err
}

func (m *musicDB) allAlbums(sortLatest bool) ([]map[string]any, error) {
	if sortLatest {
		m.cacheMu.Lock()
		cached := m.cachedAlbumsLatest
		m.cacheMu.Unlock()
		if cached != nil {
			return cached, nil
		}
	}

	order := "a.name COLLATE NOCASE, al.date, al.title COLLATE NOCASE"
	if sortLatest {
		// Latest = database insertion time (MPD 0.24 "Added"). File mtime breaks
		// ties so a full rebuild (all rows share one Added) keeps a useful order.
		order = "max_added DESC, max_mtime DESC"
	}
	query := fmt.Sprintf(`SELECT al.id, a.name, al.title, al.date,
			COALESCE(MAX(t.added), 0) AS max_added, COALESCE(MAX(t.file_modified), 0) AS max_mtime
		FROM albums al
		INNER JOIN artists a ON a.id = al.artist_id
		LEFT JOIN tracks t ON t.album_id = al.id
		GROUP BY al.id
		ORDER BY %s`, order)
	rows, err := m.db.Query(query)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var albums []map[string]any
	for rows.Next() {
		var id, maxAdded, maxMtime int64
		var artist, title, date string
		if err := rows.Scan(&id, &artist, &title, &date, &maxAdded, &maxMtime); err != nil {
			return nil, err
		}
		albums = append(albums, map[string]any{
			"id":          strconv.FormatInt(id, 10),
			"albumartist": artist,
			"album":       title,
			"date":        date,
			"album_id":    strconv.FormatInt(id, 10),
			"added":       maxAdded,
		})
	}
	if albums == nil {
		albums = []map[string]any{}
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}

	if sortLatest {
		// Pre-format the MPD response string
		var sb strings.Builder
		for _, album := range albums {
			fmt.Fprintf(&sb, "AlbumArtist: %s\n", album["albumartist"])
			fmt.Fprintf(&sb, "Date: %s\n", album["date"])
			fmt.Fprintf(&sb, "Album: %s\n", album["album"])
			if v, _ := album["album_id"].(string); v != "" {
				fmt.Fprintf(&sb, "X-AlbumId: %s\n", v)
			}
		}

		m.cacheMu.Lock()
		m.cachedAlbumsLatest = albums
		m.cachedAlbumsLatestFormatted = sb.String()
		m.cacheMu.Unlock()
	}
	return albums, nil
}

func (m *musicDB) invalidateCache() {
	m.cacheMu.Lock()
	m.cachedAlbumsLatest = nil
	m.cachedAlbumsLatestFormatted = ""
	m.cachedRofiAlbums = ""
	m.cachedRofiAlbumsLatest = ""
	m.cachedRofiTracks = ""
	m.cacheMu.Unlock()
}

func (m *musicDB) warmCache() {
	m.allAlbums(true)
	// Pre-build the launcher lists so the first rofi invocation is instant.
	// Tracks can be large, so build it lazily on first request instead.
	m.rofiAlbumsResponse(false)
	m.rofiAlbumsResponse(true)
}

// rofiAlbumsResponse returns a pre-built, cached list of albums for the rofi
// client. Each line is "X-Album: <albumid>\t<display>" so the client can show
// <display> and enqueue by id. latest sorts most-recently-added first.
func (m *musicDB) rofiAlbumsResponse(latest bool) (string, error) {
	m.cacheMu.Lock()
	cached := m.cachedRofiAlbums
	if latest {
		cached = m.cachedRofiAlbumsLatest
	}
	m.cacheMu.Unlock()
	if cached != "" {
		return cached, nil
	}

	albums, err := m.allAlbums(latest)
	if err != nil {
		return "", err
	}
	var sb strings.Builder
	for _, al := range albums {
		id, _ := al["album_id"].(string)
		artist, _ := al["albumartist"].(string)
		title, _ := al["album"].(string)
		date, _ := al["date"].(string)
		// Tab-separated raw fields: id, artist, album, date. The client aligns
		// these into columns; sending fields (not a rendered string) keeps the
		// server cheap and lets the client own presentation.
		fmt.Fprintf(&sb, "X-Album: %s\t%s\t%s\t%s\n",
			id, sanitizeField(artist), sanitizeField(title), sanitizeField(date))
	}
	s := sb.String()

	m.cacheMu.Lock()
	if latest {
		m.cachedRofiAlbumsLatest = s
	} else {
		m.cachedRofiAlbums = s
	}
	m.cacheMu.Unlock()
	return s, nil
}

// rofiTracksResponse returns a pre-built, cached list of every track for the
// rofi client. Each line is "X-Track: <trackid>\t<display>".
func (m *musicDB) rofiTracksResponse() (string, error) {
	m.cacheMu.Lock()
	cached := m.cachedRofiTracks
	m.cacheMu.Unlock()
	if cached != "" {
		return cached, nil
	}

	tracks, err := m.allTracks()
	if err != nil {
		return "", err
	}
	var sb strings.Builder
	for _, t := range tracks {
		id, _ := t["song_id"].(string)
		artist, _ := t["artist"].(string)
		if artist == "" {
			artist, _ = t["albumartist"].(string)
		}
		title, _ := t["title"].(string)
		album, _ := t["album"].(string)
		// Tab-separated raw fields: id, artist, title, album (client aligns).
		fmt.Fprintf(&sb, "X-Track: %s\t%s\t%s\t%s\n",
			id, sanitizeField(artist), sanitizeField(title), sanitizeField(album))
	}
	s := sb.String()

	m.cacheMu.Lock()
	m.cachedRofiTracks = s
	m.cacheMu.Unlock()
	return s, nil
}

// sanitizeField strips tabs/newlines so a field stays on one tab-delimited line.
func sanitizeField(s string) string {
	return strings.NewReplacer("\t", " ", "\n", " ", "\r", " ").Replace(s)
}

// cachedAlbumsLatestResponse returns the pre-formatted MPD response for
// "list Album ... sort latest" if available. Returns "" on cache miss.
func (m *musicDB) cachedAlbumsLatestResponse() string {
	m.cacheMu.Lock()
	defer m.cacheMu.Unlock()
	return m.cachedAlbumsLatestFormatted
}

func (m *musicDB) albumsByArtist(artist string) ([]map[string]any, error) {
	rows, err := m.db.Query(`SELECT al.id, a.name, al.title, al.date
		FROM albums al
		INNER JOIN artists a ON a.id = al.artist_id
		WHERE a.name = ?
		ORDER BY al.date, al.title COLLATE NOCASE`, artist)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var albums []map[string]any
	for rows.Next() {
		var id int64
		var artistName, title, date string
		if err := rows.Scan(&id, &artistName, &title, &date); err != nil {
			return nil, err
		}
		albums = append(albums, map[string]any{
			"id":          strconv.FormatInt(id, 10),
			"albumartist": artistName,
			"album":       title,
			"date":        date,
			"album_id":    strconv.FormatInt(id, 10),
		})
	}
	if albums == nil {
		albums = []map[string]any{}
	}
	return albums, rows.Err()
}

// albumIDsByRating returns album IDs whose album rating hash matches the given rating.
// It loads all albums, computes their hashes, and batch-checks against the ratings table.
func (m *musicDB) albumIDsByRatingOp(op string, value int) ([]int64, error) {
	sqlOp := op
	if sqlOp == "==" {
		sqlOp = "="
	}
	rows, err := m.db.Query(`SELECT DISTINCT al.id FROM albums al
		INNER JOIN ratings r ON r.type = 'album' AND r.rating `+sqlOp+` ?
		WHERE r.hash IN (
			SELECT r2.hash FROM ratings r2 WHERE r2.type = 'album' AND r2.rating `+sqlOp+` ?
		)`, value, value)
	if err != nil {
		// Fallback: compute hashes and match in memory
		return m.albumIDsByRatingFallback(op, value)
	}
	defer rows.Close()
	// The JOIN above doesn't work because we can't join hash to album directly.
	// Use fallback approach with allRatings.
	rows.Close()
	return m.albumIDsByRatingFallback(op, value)
}

func (m *musicDB) albumIDsByRatingFallback(op string, value int) ([]int64, error) {
	albums, err := m.allAlbums(false)
	if err != nil {
		return nil, err
	}
	if len(albums) == 0 {
		return nil, nil
	}
	hashes := make([]string, len(albums))
	for i, a := range albums {
		hashes[i] = albumRatingHash(stringify(a["albumartist"]), stringify(a["album"]), stringify(a["date"]))
	}
	ratings, err := m.getRatingsBatch(hashes)
	if err != nil {
		return nil, err
	}
	var ids []int64
	for i, h := range hashes {
		if r, ok := ratings[h]; ok && compareRating(r, op, value) {
			id, _ := strconv.ParseInt(stringify(albums[i]["id"]), 10, 64)
			ids = append(ids, id)
		}
	}
	return ids, nil
}

func (m *musicDB) albumByID(id int64) (map[string]any, error) {
	var artist, title, date string
	err := m.db.QueryRow(`SELECT a.name, al.title, al.date
		FROM albums al INNER JOIN artists a ON a.id = al.artist_id
		WHERE al.id = ?`, id).Scan(&artist, &title, &date)
	if err != nil {
		return nil, err
	}
	return map[string]any{
		"id":          strconv.FormatInt(id, 10),
		"albumartist": artist,
		"album":       title,
		"date":        date,
		"album_id":    strconv.FormatInt(id, 10),
	}, nil
}

// ---------------------------------------------------------------------------
// Track queries
// ---------------------------------------------------------------------------

type trackMeta struct {
	AlbumID         int64
	Artist          string
	Title           string
	TrackNumber     int
	DiscNumber      int
	Duration        float64
	Path            string
	FileModified    int64
	ReplayGainTrack float64
	ReplayGainAlbum float64
	PeakTrack       float64
	PeakAlbum       float64
	Codec           string
	SampleRate      int
	BitsPerSample   int
	Channels        int

	// Resolved during scan, used by batch writer to look up artist/album IDs
	albumArtist string
	album       string
	date        string

	// Generic tags (genre, composer, MusicBrainz IDs, ...) keyed by
	// canonical name; written to the track_tags table.
	tags map[string][]string
}

func (m *musicDB) upsertTrack(t *trackMeta) (int64, error) {
	rHash := trackRatingHash(t.albumArtist, t.album, t.Title, t.TrackNumber)
	// RETURNING id yields the row id whether the track was inserted or updated,
	// so the FTS index below always targets the correct rowid (LastInsertId is
	// unreliable on the ON CONFLICT DO UPDATE path).
	var id int64
	// "added" is set on first insert only; the conflict branch leaves it alone so
	// the MPD 0.24 Added timestamp survives rescans of an existing file.
	err := m.db.QueryRow(`INSERT INTO tracks(album_id, artist, title, track_number, disc_number,
			duration, path, file_modified, replay_gain_track, replay_gain_album, peak_track, peak_album, rating_hash,
			codec, sample_rate, bits_per_sample, channels, added)
		VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CAST(strftime('%s','now') AS INTEGER))
		ON CONFLICT(path) DO UPDATE SET
			album_id = excluded.album_id,
			artist = excluded.artist,
			title = excluded.title,
			track_number = excluded.track_number,
			disc_number = excluded.disc_number,
			duration = excluded.duration,
			file_modified = excluded.file_modified,
			replay_gain_track = excluded.replay_gain_track,
			replay_gain_album = excluded.replay_gain_album,
			peak_track = excluded.peak_track,
			peak_album = excluded.peak_album,
			rating_hash = excluded.rating_hash,
			codec = excluded.codec,
			sample_rate = excluded.sample_rate,
			bits_per_sample = excluded.bits_per_sample,
			channels = excluded.channels
		RETURNING id`,
		t.AlbumID, t.Artist, t.Title, t.TrackNumber, t.DiscNumber,
		t.Duration, t.Path, t.FileModified, t.ReplayGainTrack, t.ReplayGainAlbum, t.PeakTrack, t.PeakAlbum, rHash,
		t.Codec, t.SampleRate, t.BitsPerSample, t.Channels).Scan(&id)
	if err != nil {
		return 0, err
	}
	// Update FTS index for this track
	m.db.Exec(`DELETE FROM tracks_fts WHERE rowid = ?`, id)
	m.db.Exec(`INSERT INTO tracks_fts(rowid, artist, albumartist, title, album) VALUES(?, ?, ?, ?, ?)`,
		id, t.Artist, t.albumArtist, t.Title, t.album)
	m.replaceTrackTags(id, t.tags)
	return id, nil
}

// replaceTrackTags rewrites the generic tag rows for a track.
func (m *musicDB) replaceTrackTags(trackID int64, tags map[string][]string) {
	m.db.Exec(`DELETE FROM track_tags WHERE track_id = ?`, trackID)
	for name, vals := range tags {
		for _, v := range vals {
			m.db.Exec(`INSERT INTO track_tags(track_id, tag, value) VALUES(?, ?, ?)`, trackID, name, v)
		}
	}
}

// tagsForTracks batch-fetches generic tags for a set of track IDs. For large
// sets a full table read avoids oversized IN clauses (SQLite variable limit).
func (m *musicDB) tagsForTracks(ids []int64) (map[int64]map[string][]string, error) {
	if len(ids) == 0 {
		return map[int64]map[string][]string{}, nil
	}
	var rows *sql.Rows
	var err error
	if len(ids) > 400 {
		rows, err = m.db.Query(`SELECT track_id, tag, value FROM track_tags`)
	} else {
		placeholders := make([]string, len(ids))
		args := make([]any, len(ids))
		for i, id := range ids {
			placeholders[i] = "?"
			args[i] = id
		}
		rows, err = m.db.Query(`SELECT track_id, tag, value FROM track_tags
			WHERE track_id IN (`+strings.Join(placeholders, ",")+`)`, args...)
	}
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	idSet := make(map[int64]struct{}, len(ids))
	for _, id := range ids {
		idSet[id] = struct{}{}
	}
	result := map[int64]map[string][]string{}
	for rows.Next() {
		var id int64
		var name, value string
		if err := rows.Scan(&id, &name, &value); err != nil {
			return nil, err
		}
		if _, ok := idSet[id]; !ok {
			continue
		}
		t := result[id]
		if t == nil {
			t = map[string][]string{}
			result[id] = t
		}
		t[name] = append(t[name], value)
	}
	return result, rows.Err()
}

// enrichWithTags attaches generic tags to track maps under the "tags" key.
func (m *musicDB) enrichWithTags(tracks []map[string]any) {
	if len(tracks) == 0 {
		return
	}
	ids := make([]int64, 0, len(tracks))
	for _, t := range tracks {
		if id, err := strconv.ParseInt(stringify(t["id"]), 10, 64); err == nil {
			ids = append(ids, id)
		}
	}
	tags, err := m.tagsForTracks(ids)
	if err != nil {
		return
	}
	for _, t := range tracks {
		id, err := strconv.ParseInt(stringify(t["id"]), 10, 64)
		if err != nil {
			continue
		}
		if tt, ok := tags[id]; ok {
			t["tags"] = tt
		}
	}
}

// listTagGroupOrder is the emission and sort order for structural group
// columns on generic tag listings, matching the existing "list Album group
// AlbumArtist group Date" output shape (AlbumArtist, Date, Album, main tag).
var listTagGroupOrder = []string{"albumartist", "artist", "date", "album"}

var listTagGroupCols = map[string]string{
	"albumartist": "a.name",
	"artist":      "t.artist",
	"date":        "al.date",
	"album":       "al.title",
}

// listTagValues returns rows for a generic tag listing. Each row holds the
// tag value under "value" plus one entry per requested group column
// (albumartist, artist, album, date), deduplicated per group combination —
// e.g. list musicbrainz_releasegroupid group album group albumartist yields
// one row per album. An albumartist/artist/album or generic-tag filter can
// restrict the listing.
func (m *musicDB) listTagValues(tagName, filterTag, filterVal string, groupTags []string) ([]map[string]string, error) {
	sel := []string{"tt.value"}
	var selNames, orderCols []string
	for _, g := range listTagGroupOrder {
		for _, want := range groupTags {
			if want == g {
				sel = append(sel, listTagGroupCols[g])
				selNames = append(selNames, g)
				orderCols = append(orderCols, listTagGroupCols[g]+" COLLATE NOCASE")
				break
			}
		}
	}
	query := `SELECT DISTINCT ` + strings.Join(sel, ", ") + ` FROM track_tags tt
		JOIN tracks t ON t.id = tt.track_id
		JOIN albums al ON al.id = t.album_id
		JOIN artists a ON a.id = al.artist_id
		WHERE tt.tag = ?`
	args := []any{tagName}
	switch {
	case filterTag == "artist":
		query += ` AND (a.name = ? OR t.artist = ?)`
		args = append(args, filterVal, filterVal)
	case filterTag == "albumartist":
		query += ` AND a.name = ?`
		args = append(args, filterVal)
	case filterTag == "album":
		query += ` AND al.title = ?`
		args = append(args, filterVal)
	case filterTag != "":
		query += ` AND EXISTS (SELECT 1 FROM track_tags f
			WHERE f.track_id = tt.track_id AND f.tag = ? AND f.value = ?)`
		args = append(args, filterTag, filterVal)
	}
	query += ` ORDER BY ` + strings.Join(append(orderCols, "tt.value COLLATE NOCASE"), ", ")
	rows, err := m.db.Query(query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []map[string]string
	for rows.Next() {
		dest := make([]any, len(sel))
		fields := make([]string, len(sel))
		for i := range dest {
			dest[i] = &fields[i]
		}
		if err := rows.Scan(dest...); err != nil {
			return nil, err
		}
		row := map[string]string{"value": fields[0]}
		for i, name := range selNames {
			row[name] = fields[i+1]
		}
		result = append(result, row)
	}
	return result, rows.Err()
}

// albumsByTag returns albums that contain at least one track carrying the
// given generic tag value (e.g. list album genre "Rock").
func (m *musicDB) albumsByTag(tagName, value string) ([]map[string]any, error) {
	rows, err := m.db.Query(`SELECT DISTINCT al.id, al.title, al.date, a.name
		FROM albums al
		INNER JOIN artists a ON a.id = al.artist_id
		INNER JOIN tracks t ON t.album_id = al.id
		INNER JOIN track_tags tt ON tt.track_id = t.id
		WHERE tt.tag = ? AND tt.value = ? COLLATE NOCASE
		ORDER BY a.name COLLATE NOCASE, al.date, al.title COLLATE NOCASE`, tagName, value)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var albums []map[string]any
	for rows.Next() {
		var id int64
		var title, date, artist string
		if err := rows.Scan(&id, &title, &date, &artist); err != nil {
			return nil, err
		}
		albums = append(albums, map[string]any{
			"id":          strconv.FormatInt(id, 10),
			"album_id":    strconv.FormatInt(id, 10),
			"album":       title,
			"date":        date,
			"albumartist": artist,
		})
	}
	return albums, rows.Err()
}

// tracksByConditions resolves generic tag conditions (plus optional
// artist/albumartist/album/date equality filters) in a single query.
func (m *musicDB) tracksByConditions(generic []filterCondition, artist, albumArtist, album, date string, caseInsensitive bool) ([]map[string]any, error) {
	query := `SELECT t.id, t.album_id, t.artist, t.title,
		t.track_number, t.disc_number, t.duration, t.path,
		t.replay_gain_track, t.replay_gain_album, t.peak_track, t.peak_album,
		t.rating, t.rating_hash, t.added, t.file_modified, t.codec, t.sample_rate,
		t.bits_per_sample, t.channels, a.name, al.title, al.date
		FROM tracks t
		INNER JOIN albums al ON al.id = t.album_id
		INNER JOIN artists a ON a.id = al.artist_id
		WHERE 1=1`
	var args []any
	eq := " = ?"
	if caseInsensitive {
		eq = " = ? COLLATE NOCASE"
	}
	for _, cond := range generic {
		if cond.op == "contains" {
			query += ` AND EXISTS (SELECT 1 FROM track_tags tt
				WHERE tt.track_id = t.id AND tt.tag = ? AND tt.value LIKE '%' || ? || '%')`
		} else {
			query += ` AND EXISTS (SELECT 1 FROM track_tags tt
				WHERE tt.track_id = t.id AND tt.tag = ? AND tt.value` + eq + `)`
		}
		args = append(args, cond.tag, cond.value)
	}
	if artist != "" {
		query += ` AND (a.name` + eq + ` OR t.artist` + eq + `)`
		args = append(args, artist, artist)
	}
	if albumArtist != "" {
		query += ` AND a.name` + eq
		args = append(args, albumArtist)
	}
	if album != "" {
		query += ` AND al.title` + eq
		args = append(args, album)
	}
	if date != "" {
		query += ` AND al.date = ?`
		args = append(args, date)
	}
	query += ` ORDER BY a.name COLLATE NOCASE, al.date, al.title COLLATE NOCASE, t.disc_number, t.track_number`
	rows, err := m.db.Query(query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return m.scanTrackRows(rows)
}

func (m *musicDB) trackByID(id int64) (map[string]any, error) {
	return m.scanTrackRow(m.db.QueryRow(`SELECT t.id, t.album_id, t.artist, t.title,
		t.track_number, t.disc_number, t.duration, t.path,
		t.replay_gain_track, t.replay_gain_album, t.peak_track, t.peak_album,
		t.rating, t.rating_hash, t.added, t.file_modified, t.codec, t.sample_rate,
		t.bits_per_sample, t.channels, a.name, al.title, al.date
		FROM tracks t
		INNER JOIN albums al ON al.id = t.album_id
		INNER JOIN artists a ON a.id = al.artist_id
		WHERE t.id = ?`, id))
}

func (m *musicDB) trackBySongID(songID string) (map[string]any, error) {
	id, err := strconv.ParseInt(songID, 10, 64)
	if err != nil {
		return nil, fmt.Errorf("invalid song_id: %s", songID)
	}
	return m.trackByID(id)
}

func (m *musicDB) trackPathByID(id int64) (string, error) {
	var path string
	err := m.db.QueryRow(`SELECT path FROM tracks WHERE id = ?`, id).Scan(&path)
	return path, err
}

// trackPlayInfoByIDs fetches path, duration, and replay gain for multiple
// track IDs in a single query. Returns a map keyed by track ID.
type trackPlayInfo struct {
	Path     string
	Duration float64
	RGTrack  float64
	RGAlbum  float64
}

func (m *musicDB) trackPlayInfoByIDs(ids []int64) (map[int64]trackPlayInfo, error) {
	if len(ids) == 0 {
		return nil, nil
	}
	placeholders := make([]string, len(ids))
	args := make([]any, len(ids))
	for i, id := range ids {
		placeholders[i] = "?"
		args[i] = id
	}
	query := `SELECT id, path, duration, replay_gain_track, replay_gain_album
		FROM tracks WHERE id IN (` + strings.Join(placeholders, ",") + `)`
	rows, err := m.db.Query(query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	result := make(map[int64]trackPlayInfo, len(ids))
	for rows.Next() {
		var id int64
		var info trackPlayInfo
		if err := rows.Scan(&id, &info.Path, &info.Duration, &info.RGTrack, &info.RGAlbum); err != nil {
			continue
		}
		result[id] = info
	}
	return result, nil
}

func (m *musicDB) trackByPath(path string) (map[string]any, error) {
	return m.scanTrackRow(m.db.QueryRow(`SELECT t.id, t.album_id, t.artist, t.title,
		t.track_number, t.disc_number, t.duration, t.path,
		t.replay_gain_track, t.replay_gain_album, t.peak_track, t.peak_album,
		t.rating, t.rating_hash, t.added, t.file_modified, t.codec, t.sample_rate,
		t.bits_per_sample, t.channels, a.name, al.title, al.date
		FROM tracks t
		INNER JOIN albums al ON al.id = t.album_id
		INNER JOIN artists a ON a.id = al.artist_id
		WHERE t.path = ?`, path))
}

func (m *musicDB) tracksByPathPrefix(prefix string) ([]map[string]any, error) {
	rows, err := m.db.Query(`SELECT t.id, t.album_id, t.artist, t.title,
		t.track_number, t.disc_number, t.duration, t.path,
		t.replay_gain_track, t.replay_gain_album, t.peak_track, t.peak_album,
		t.rating, t.rating_hash, t.added, t.file_modified, t.codec, t.sample_rate,
		t.bits_per_sample, t.channels, a.name, al.title, al.date
		FROM tracks t
		INNER JOIN albums al ON al.id = t.album_id
		INNER JOIN artists a ON a.id = al.artist_id
		WHERE t.path LIKE ? || '%'
		ORDER BY t.path`, prefix)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return m.scanTrackRows(rows)
}

func (m *musicDB) tracksByAlbum(albumID int64) ([]map[string]any, error) {
	rows, err := m.db.Query(`SELECT t.id, t.album_id, t.artist, t.title,
		t.track_number, t.disc_number, t.duration, t.path,
		t.replay_gain_track, t.replay_gain_album, t.peak_track, t.peak_album,
		t.rating, t.rating_hash, t.added, t.file_modified, t.codec, t.sample_rate,
		t.bits_per_sample, t.channels, a.name, al.title, al.date
		FROM tracks t
		INNER JOIN albums al ON al.id = t.album_id
		INNER JOIN artists a ON a.id = al.artist_id
		WHERE t.album_id = ?
		ORDER BY t.disc_number, t.track_number, t.title COLLATE NOCASE`, albumID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return m.scanTrackRows(rows)
}

func (m *musicDB) trackSongIDsByAlbum(albumID int64) ([]string, error) {
	rows, err := m.db.Query(`SELECT id FROM tracks WHERE album_id = ?
		ORDER BY disc_number, track_number, title COLLATE NOCASE`, albumID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var ids []string
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, strconv.FormatInt(id, 10))
	}
	return ids, rows.Err()
}

func (m *musicDB) allTracks() ([]map[string]any, error) {
	rows, err := m.db.Query(`SELECT t.id, t.album_id, t.artist, t.title,
		t.track_number, t.disc_number, t.duration, t.path,
		t.replay_gain_track, t.replay_gain_album, t.peak_track, t.peak_album,
		t.rating, t.rating_hash, t.added, t.file_modified, t.codec, t.sample_rate,
		t.bits_per_sample, t.channels, a.name, al.title, al.date
		FROM tracks t
		INNER JOIN albums al ON al.id = t.album_id
		INNER JOIN artists a ON a.id = al.artist_id
		ORDER BY a.name COLLATE NOCASE, al.date, al.title COLLATE NOCASE, t.disc_number, t.track_number`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return m.scanTrackRows(rows)
}

// tracksMatching narrows by one indexed condition instead of scanning the
// library: a date or title lives in a column, any other tag in track_tags.
// The caller still evaluates the full condition set, so a near-miss here
// only costs a few extra comparisons.
func (m *musicDB) tracksMatching(tag, value string, caseInsensitive bool) ([]map[string]any,
	bool, error) {
	const columns = `SELECT t.id, t.album_id, t.artist, t.title,
		t.track_number, t.disc_number, t.duration, t.path,
		t.replay_gain_track, t.replay_gain_album, t.peak_track, t.peak_album,
		t.rating, t.rating_hash, t.added, t.file_modified, t.codec, t.sample_rate,
		t.bits_per_sample, t.channels, a.name, al.title, al.date
		FROM tracks t
		INNER JOIN albums al ON al.id = t.album_id
		INNER JOIN artists a ON a.id = al.artist_id `
	const ordering = ` ORDER BY a.name COLLATE NOCASE, al.date, al.title COLLATE NOCASE,
		t.disc_number, t.track_number`
	// COLLATE NOCASE keeps the index usable for the case-insensitive form;
	// exact matching only, since a substring search cannot use one anyway.
	comparison := " = ?"
	if caseInsensitive {
		comparison = " = ? COLLATE NOCASE"
	}
	var query string
	switch tag {
	case "date":
		query = columns + "WHERE al.date" + comparison + ordering
	case "title":
		query = columns + "WHERE t.title" + comparison + ordering
	case "album":
		query = columns + "WHERE al.title" + comparison + ordering
	case "":
		return nil, false, nil
	default:
		query = columns + `WHERE t.id IN (SELECT track_id FROM track_tags
			WHERE tag = ? AND value` + comparison + ")" + ordering
	}
	var rows *sql.Rows
	var err error
	if strings.Contains(query, "track_tags") {
		rows, err = m.db.Query(query, tag, value)
	} else {
		rows, err = m.db.Query(query, value)
	}
	if err != nil {
		return nil, false, err
	}
	defer rows.Close()
	tracks, err := m.scanTrackRows(rows)
	return tracks, true, err
}

func (m *musicDB) randomAlbumID() (int64, error) {
	var id int64
	err := m.db.QueryRow(`SELECT id FROM albums ORDER BY RANDOM() LIMIT 1`).Scan(&id)
	return id, err
}

func (m *musicDB) randomTrackIDs(count int) ([]string, error) {
	rows, err := m.db.Query(`SELECT id FROM tracks ORDER BY RANDOM() LIMIT ?`, count)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var ids []string
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, strconv.FormatInt(id, 10))
	}
	return ids, rows.Err()
}

func (m *musicDB) trackCount() (int, error) {
	var count int
	err := m.db.QueryRow(`SELECT COUNT(*) FROM tracks`).Scan(&count)
	return count, err
}

func (m *musicDB) albumCount() (int, error) {
	var count int
	err := m.db.QueryRow(`SELECT COUNT(*) FROM albums`).Scan(&count)
	return count, err
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

func (m *musicDB) rebuildFTS() error {
	tx, err := m.db.Begin()
	if err != nil {
		return err
	}
	tx.Exec(`DELETE FROM tracks_fts`)
	_, err = tx.Exec(`INSERT INTO tracks_fts(rowid, artist, albumartist, title, album)
		SELECT t.id, t.artist, a.name, t.title, al.title
		FROM tracks t
		INNER JOIN albums al ON al.id = t.album_id
		INNER JOIN artists a ON a.id = al.artist_id`)
	if err != nil {
		tx.Rollback()
		return err
	}
	return tx.Commit()
}

func (m *musicDB) search(query string, maxResults int) (albums []map[string]any, tracks []map[string]any, err error) {
	terms := strings.Fields(query)
	if len(terms) == 0 {
		return []map[string]any{}, []map[string]any{}, nil
	}

	// Build FTS5 match expression: each term gets prefix matching
	var ftsTerms []string
	for _, t := range terms {
		// Escape double quotes for FTS5
		escaped := strings.ReplaceAll(t, `"`, `""`)
		ftsTerms = append(ftsTerms, `"`+escaped+`"*`)
	}
	matchExpr := strings.Join(ftsTerms, " ")

	// Search albums via FTS5 (distinct album_id, not limited by track count)
	albumRows, err := m.db.Query(`SELECT DISTINCT al.id, a.name, al.title, al.date
		FROM tracks_fts fts
		INNER JOIN tracks t ON t.id = fts.rowid
		INNER JOIN albums al ON al.id = t.album_id
		INNER JOIN artists a ON a.id = al.artist_id
		WHERE tracks_fts MATCH ?
		ORDER BY a.name COLLATE NOCASE, al.date, al.title COLLATE NOCASE
		LIMIT ?`, matchExpr, maxResults)
	if err == nil {
		defer albumRows.Close()
		for albumRows.Next() {
			var id int64
			var artist, title, date string
			if err := albumRows.Scan(&id, &artist, &title, &date); err != nil {
				break
			}
			idStr := strconv.FormatInt(id, 10)
			albums = append(albums, map[string]any{
				"id":          idStr,
				"albumartist": artist,
				"album":       title,
				"date":        date,
				"album_id":    idStr,
			})
		}
	}

	// Search tracks via FTS5
	rows, err := m.db.Query(`SELECT t.id, t.album_id, t.artist, t.title,
		t.track_number, t.disc_number, t.duration, t.path,
		t.replay_gain_track, t.replay_gain_album, t.peak_track, t.peak_album,
		t.rating, t.rating_hash, t.added, t.file_modified, t.codec, t.sample_rate,
		t.bits_per_sample, t.channels, a.name, al.title, al.date
		FROM tracks_fts fts
		INNER JOIN tracks t ON t.id = fts.rowid
		INNER JOIN albums al ON al.id = t.album_id
		INNER JOIN artists a ON a.id = al.artist_id
		WHERE tracks_fts MATCH ?
		LIMIT ?`, matchExpr, maxResults)
	if err != nil {
		// Fallback to in-memory search if FTS fails
		return m.searchFallback(terms, maxResults)
	}
	defer rows.Close()
	tracks, err = m.scanTrackRows(rows)
	if err != nil {
		return nil, nil, err
	}

	if albums == nil {
		albums = []map[string]any{}
	}
	if tracks == nil {
		tracks = []map[string]any{}
	}
	return albums, tracks, nil
}

func (m *musicDB) searchFallback(terms []string, maxResults int) ([]map[string]any, []map[string]any, error) {
	lowerTerms := make([]string, len(terms))
	for i, t := range terms {
		lowerTerms[i] = strings.ToLower(t)
	}

	allAlbums, err := m.allAlbums(false)
	if err != nil {
		return nil, nil, err
	}
	var albums []map[string]any
	for _, album := range allAlbums {
		text := strings.ToLower(stringify(album["albumartist"]) + " " + stringify(album["album"]) + " " + stringify(album["date"]))
		if matchesAll(text, lowerTerms) {
			albums = append(albums, album)
			if len(albums) >= maxResults {
				break
			}
		}
	}

	allTracks, err := m.allTracks()
	if err != nil {
		return nil, nil, err
	}
	var tracks []map[string]any
	for _, track := range allTracks {
		text := strings.ToLower(stringify(track["title"]) + " " + stringify(track["artist"]) + " " + stringify(track["album"]) + " " + stringify(track["albumartist"]))
		if matchesAll(text, lowerTerms) {
			tracks = append(tracks, track)
			if len(tracks) >= maxResults {
				break
			}
		}
	}

	if albums == nil {
		albums = []map[string]any{}
	}
	if tracks == nil {
		tracks = []map[string]any{}
	}
	return albums, tracks, nil
}

// ---------------------------------------------------------------------------
// Ratings (content-hash based, survives path changes and DB rebuilds)
// ---------------------------------------------------------------------------

func compareRating(r int, op string, value int) bool {
	switch op {
	case "==":
		return r == value
	case ">":
		return r > value
	case ">=":
		return r >= value
	case "<":
		return r < value
	case "<=":
		return r <= value
	}
	return false
}

func trackRatingHash(albumArtist, album, title string, trackNum int) string {
	h := sha256.New()
	h.Write([]byte(albumArtist))
	h.Write([]byte{0})
	h.Write([]byte(album))
	h.Write([]byte{0})
	h.Write([]byte(title))
	h.Write([]byte{0})
	h.Write([]byte(strconv.Itoa(trackNum)))
	return hex.EncodeToString(h.Sum(nil))
}

// normalizeAlbumDate maps the two client spellings of "no date" onto the
// scanner's stored placeholder so both address the same album identity.
func normalizeAlbumDate(date string) string {
	if date == "" {
		return "0000"
	}
	return date
}

func albumRatingHash(albumArtist, album, date string) string {
	h := sha256.New()
	h.Write([]byte(albumArtist))
	h.Write([]byte{0})
	h.Write([]byte(album))
	h.Write([]byte{0})
	h.Write([]byte(date))
	return hex.EncodeToString(h.Sum(nil))
}

func (m *musicDB) setRating(hash, ratingType string, rating int) error {
	if rating == 0 {
		_, err := m.db.Exec(`DELETE FROM ratings WHERE hash = ?`, hash)
		return err
	}
	_, err := m.db.Exec(`INSERT INTO ratings(hash, type, rating, updated_at)
		VALUES(?, ?, ?, datetime('now'))
		ON CONFLICT(hash) DO UPDATE SET rating = excluded.rating, updated_at = excluded.updated_at`,
		hash, ratingType, rating)
	return err
}

func (m *musicDB) getRating(hash string) (int, error) {
	var rating int
	err := m.db.QueryRow(`SELECT rating FROM ratings WHERE hash = ?`, hash).Scan(&rating)
	if err == sql.ErrNoRows {
		return 0, nil
	}
	return rating, err
}

func (m *musicDB) getRatingsBatch(hashes []string) (map[string]int, error) {
	if len(hashes) == 0 {
		return map[string]int{}, nil
	}
	// Fetch all ratings (typically very few) and match against the provided hashes.
	allRatings, err := m.allRatings()
	if err != nil {
		return nil, err
	}
	hashSet := make(map[string]struct{}, len(hashes))
	for _, h := range hashes {
		hashSet[h] = struct{}{}
	}
	result := make(map[string]int)
	for h, r := range allRatings {
		if _, ok := hashSet[h]; ok {
			result[h] = r
		}
	}
	return result, nil
}

func (m *musicDB) allRatings() (map[string]int, error) {
	rows, err := m.db.Query(`SELECT hash, rating FROM ratings`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	result := make(map[string]int)
	for rows.Next() {
		var h string
		var r int
		if err := rows.Scan(&h, &r); err != nil {
			return nil, err
		}
		result[h] = r
	}
	return result, rows.Err()
}

// tracksByRating returns tracks with a specific rating by joining tracks and ratings tables.
func (m *musicDB) tracksByRatingOp(op string, value int) ([]map[string]any, error) {
	sqlOp := op
	if sqlOp == "==" {
		sqlOp = "="
	}
	rows, err := m.db.Query(`SELECT t.id, t.album_id, t.artist, t.title,
		t.track_number, t.disc_number, t.duration, t.path,
		t.replay_gain_track, t.replay_gain_album, t.peak_track, t.peak_album,
		t.rating, t.rating_hash, t.added, t.file_modified, t.codec, t.sample_rate,
		t.bits_per_sample, t.channels, a.name, al.title, al.date
		FROM tracks t
		INNER JOIN ratings r ON r.hash = t.rating_hash AND r.type = 'track' AND r.rating `+sqlOp+` ?
		INNER JOIN albums al ON al.id = t.album_id
		INNER JOIN artists a ON a.id = al.artist_id
		ORDER BY a.name COLLATE NOCASE, al.date, al.title COLLATE NOCASE, t.disc_number, t.track_number`, value)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return m.scanTrackRows(rows)
}

// enrichWithRatings batch-fetches ratings for a slice of track maps and sets
// the "rating" field on each. Call after scanTrackRows for bulk operations.
func (m *musicDB) enrichWithRatings(tracks []map[string]any) {
	if len(tracks) == 0 {
		return
	}
	hashes := make([]string, len(tracks))
	for i, t := range tracks {
		hashes[i] = stringify(t["rating_hash"])
	}
	ratings, err := m.getRatingsBatch(hashes)
	if err != nil {
		return
	}
	for i, h := range hashes {
		if r, ok := ratings[h]; ok && r > 0 {
			tracks[i]["rating"] = r
		}
	}
}

// ---------------------------------------------------------------------------
// Playlists
// ---------------------------------------------------------------------------

func (m *musicDB) allPlaylists() ([]map[string]any, error) {
	rows, err := m.db.Query(`SELECT p.id, p.name, p.created_at,
		COUNT(pt.id) as song_count,
		COALESCE(SUM(t.duration), 0) as total_duration
		FROM playlists p
		LEFT JOIN playlist_tracks pt ON pt.playlist_id = p.id
		LEFT JOIN tracks t ON t.id = pt.track_id
		GROUP BY p.id
		ORDER BY p.name COLLATE NOCASE`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var playlists []map[string]any
	for rows.Next() {
		var id int64
		var name, createdAt string
		var songCount int
		var duration float64
		if err := rows.Scan(&id, &name, &createdAt, &songCount, &duration); err != nil {
			return nil, err
		}
		playlists = append(playlists, map[string]any{
			"id":         strconv.FormatInt(id, 10),
			"name":       name,
			"song_count": songCount,
			"duration":   int(duration),
		})
	}
	if playlists == nil {
		playlists = []map[string]any{}
	}
	return playlists, rows.Err()
}

func (m *musicDB) playlistTracks(playlistID int64) ([]map[string]any, error) {
	rows, err := m.db.Query(`SELECT t.id, t.album_id, t.artist, t.title,
		t.track_number, t.disc_number, t.duration, t.path,
		t.replay_gain_track, t.replay_gain_album, t.peak_track, t.peak_album,
		t.rating, t.rating_hash, t.added, t.file_modified, t.codec, t.sample_rate,
		t.bits_per_sample, t.channels, a.name, al.title, al.date
		FROM playlist_tracks pt
		INNER JOIN tracks t ON t.id = pt.track_id
		INNER JOIN albums al ON al.id = t.album_id
		INNER JOIN artists a ON a.id = al.artist_id
		WHERE pt.playlist_id = ?
		ORDER BY pt.position`, playlistID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return m.scanTrackRows(rows)
}

func (m *musicDB) playlistTrackSongIDs(playlistID int64) ([]string, error) {
	rows, err := m.db.Query(`SELECT t.id FROM playlist_tracks pt
		INNER JOIN tracks t ON t.id = pt.track_id
		WHERE pt.playlist_id = ?
		ORDER BY pt.position`, playlistID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var ids []string
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, strconv.FormatInt(id, 10))
	}
	return ids, rows.Err()
}

// setPlaylistScratch marks a playlist as a client's working list, or clears
// the mark to promote it to an ordinary playlist.
func (m *musicDB) setPlaylistScratch(id int64, scratch bool) error {
	value := 0
	if scratch {
		value = 1
	}
	_, err := m.db.Exec(`UPDATE playlists SET scratch = ? WHERE id = ?`, value, id)
	return err
}

func (m *musicDB) scratchPlaylistNames() ([]string, error) {
	rows, err := m.db.Query(`SELECT name FROM playlists WHERE scratch != 0
		ORDER BY name COLLATE NOCASE`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var names []string
	for rows.Next() {
		var name string
		if err := rows.Scan(&name); err != nil {
			return nil, err
		}
		names = append(names, name)
	}
	return names, rows.Err()
}

func (m *musicDB) createPlaylist(name string) (int64, error) {
	res, err := m.db.Exec(`INSERT INTO playlists(name) VALUES(?)`, name)
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

func (m *musicDB) findOrCreatePlaylist(name string) (int64, error) {
	var id int64
	err := m.db.QueryRow(`SELECT id FROM playlists WHERE name = ?`, name).Scan(&id)
	if err == nil {
		return id, nil
	}
	return m.createPlaylist(name)
}

func (m *musicDB) deletePlaylist(id int64) error {
	// Explicit rather than relying on the cascade alone: databases written
	// before foreign keys were enforced hold orphans this would leave.
	if _, err := m.db.Exec(`DELETE FROM playlist_tracks WHERE playlist_id = ?`, id); err != nil {
		return err
	}
	_, err := m.db.Exec(`DELETE FROM playlists WHERE id = ?`, id)
	return err
}

func (m *musicDB) addTrackToPlaylist(playlistID, trackID int64) error {
	_, err := m.db.Exec(`INSERT INTO playlist_tracks(playlist_id, track_id, position)
		VALUES(?, ?, (SELECT COALESCE(MAX(position), 0) + 1 FROM playlist_tracks WHERE playlist_id = ?))`,
		playlistID, trackID, playlistID)
	return err
}

// addTracksToPlaylist appends many tracks in one transaction. One statement
// per track costs a commit each, which is what makes adding a search result
// track-by-track slow; here the whole batch is one commit.
func (m *musicDB) addTracksToPlaylist(playlistID int64, trackIDs []int64) error {
	if len(trackIDs) == 0 {
		return nil
	}
	tx, err := m.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var position int
	if err := tx.QueryRow(`SELECT COALESCE(MAX(position), 0) FROM playlist_tracks
		WHERE playlist_id = ?`, playlistID).Scan(&position); err != nil {
		return err
	}
	stmt, err := tx.Prepare(`INSERT INTO playlist_tracks(playlist_id, track_id, position)
		VALUES(?, ?, ?)`)
	if err != nil {
		return err
	}
	defer stmt.Close()
	for _, trackID := range trackIDs {
		position++
		if _, err := stmt.Exec(playlistID, trackID, position); err != nil {
			return err
		}
	}
	return tx.Commit()
}

func (m *musicDB) playlistIDByName(name string) (int64, error) {
	var id int64
	err := m.db.QueryRow(`SELECT id FROM playlists WHERE name = ?`, name).Scan(&id)
	return id, err
}

func (m *musicDB) renamePlaylist(id int64, newName string) error {
	_, err := m.db.Exec(`UPDATE playlists SET name = ? WHERE id = ?`, newName, id)
	return err
}

func (m *musicDB) clearPlaylist(id int64) error {
	_, err := m.db.Exec(`DELETE FROM playlist_tracks WHERE playlist_id = ?`, id)
	return err
}

// playlistEntryIDs returns the playlist_tracks row ids in position order —
// the addressing unit for positional edits, because the same track may
// appear at several positions.
func (m *musicDB) playlistEntryIDs(playlistID int64) ([]int64, error) {
	rows, err := m.db.Query(`SELECT id FROM playlist_tracks WHERE playlist_id = ?
		ORDER BY position`, playlistID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var ids []int64
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}

var errPlaylistPosition = errors.New("playlist position out of range")

// deletePlaylistTrackAt removes the entry at the 0-based position and
// renumbers the remainder contiguously.
func (m *musicDB) deletePlaylistTrackAt(playlistID int64, pos int) error {
	entries, err := m.playlistEntryIDs(playlistID)
	if err != nil {
		return err
	}
	if pos < 0 || pos >= len(entries) {
		return errPlaylistPosition
	}
	tx, err := m.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`DELETE FROM playlist_tracks WHERE id = ?`, entries[pos]); err != nil {
		return err
	}
	remaining := append(append([]int64{}, entries[:pos]...), entries[pos+1:]...)
	for index, entry := range remaining {
		if _, err := tx.Exec(`UPDATE playlist_tracks SET position = ? WHERE id = ?`,
			index+1, entry); err != nil {
			return err
		}
	}
	return tx.Commit()
}

// movePlaylistTrack moves the entry at 0-based from to 0-based to.
func (m *musicDB) movePlaylistTrack(playlistID int64, from, to int) error {
	entries, err := m.playlistEntryIDs(playlistID)
	if err != nil {
		return err
	}
	if from < 0 || from >= len(entries) || to < 0 || to >= len(entries) {
		return errPlaylistPosition
	}
	moved := entries[from]
	entries = append(entries[:from], entries[from+1:]...)
	entries = append(entries[:to], append([]int64{moved}, entries[to:]...)...)
	tx, err := m.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	for index, entry := range entries {
		if _, err := tx.Exec(`UPDATE playlist_tracks SET position = ? WHERE id = ?`,
			index+1, entry); err != nil {
			return err
		}
	}
	return tx.Commit()
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

func (m *musicDB) removeTracksNotIn(paths map[string]struct{}) error {
	rows, err := m.db.Query(`SELECT id, path FROM tracks`)
	if err != nil {
		return err
	}
	defer rows.Close()
	var toDelete []int64
	for rows.Next() {
		var id int64
		var path string
		if err := rows.Scan(&id, &path); err != nil {
			return err
		}
		if _, ok := paths[path]; !ok {
			toDelete = append(toDelete, id)
		}
	}
	if err := rows.Err(); err != nil {
		return err
	}
	for _, id := range toDelete {
		m.db.Exec(`DELETE FROM tracks_fts WHERE rowid = ?`, id)
		m.db.Exec(`DELETE FROM track_tags WHERE track_id = ?`, id)
		if _, err := m.db.Exec(`DELETE FROM tracks WHERE id = ?`, id); err != nil {
			return err
		}
	}
	// Clean up orphaned albums and artists
	_, _ = m.db.Exec(`DELETE FROM albums WHERE id NOT IN (SELECT DISTINCT album_id FROM tracks)`)
	_, _ = m.db.Exec(`DELETE FROM artists WHERE id NOT IN (SELECT DISTINCT artist_id FROM albums)`)
	return nil
}

// removeTracksUnderPrefixNotIn deletes tracks whose path begins with prefix but
// is not present in keep. Used by targeted subtree scans to prune files that
// vanished from a rescanned directory without touching the rest of the library.
// Pass an empty keep set to remove every track under the prefix (deleted dir).
func (m *musicDB) removeTracksUnderPrefixNotIn(prefix string, keep map[string]struct{}) error {
	rows, err := m.db.Query(`SELECT id, path FROM tracks`)
	if err != nil {
		return err
	}
	defer rows.Close()
	var toDelete []int64
	for rows.Next() {
		var id int64
		var path string
		if err := rows.Scan(&id, &path); err != nil {
			return err
		}
		if !strings.HasPrefix(path, prefix) {
			continue
		}
		if _, ok := keep[path]; ok {
			continue
		}
		toDelete = append(toDelete, id)
	}
	if err := rows.Err(); err != nil {
		return err
	}
	for _, id := range toDelete {
		m.db.Exec(`DELETE FROM tracks_fts WHERE rowid = ?`, id)
		m.db.Exec(`DELETE FROM track_tags WHERE track_id = ?`, id)
		if _, err := m.db.Exec(`DELETE FROM tracks WHERE id = ?`, id); err != nil {
			return err
		}
	}
	if len(toDelete) > 0 {
		// Clean up orphaned albums and artists
		_, _ = m.db.Exec(`DELETE FROM albums WHERE id NOT IN (SELECT DISTINCT album_id FROM tracks)`)
		_, _ = m.db.Exec(`DELETE FROM artists WHERE id NOT IN (SELECT DISTINCT artist_id FROM albums)`)
	}
	return nil
}

// deleteTrackByPath removes a single track (and its FTS row) by exact path,
// then prunes any artist/album left orphaned. No-op if the path is unknown.
func (m *musicDB) deleteTrackByPath(path string) error {
	var id int64
	err := m.db.QueryRow(`SELECT id FROM tracks WHERE path = ?`, path).Scan(&id)
	if err != nil {
		return nil // not present — nothing to do
	}
	m.db.Exec(`DELETE FROM tracks_fts WHERE rowid = ?`, id)
	m.db.Exec(`DELETE FROM track_tags WHERE track_id = ?`, id)
	if _, err := m.db.Exec(`DELETE FROM tracks WHERE id = ?`, id); err != nil {
		return err
	}
	_, _ = m.db.Exec(`DELETE FROM albums WHERE id NOT IN (SELECT DISTINCT album_id FROM tracks)`)
	_, _ = m.db.Exec(`DELETE FROM artists WHERE id NOT IN (SELECT DISTINCT artist_id FROM albums)`)
	return nil
}

func (m *musicDB) isFileUnchanged(path string, modTime int64) bool {
	var stored int64
	err := m.db.QueryRow(`SELECT file_modified FROM tracks WHERE path = ?`, path).Scan(&stored)
	if err != nil {
		return false
	}
	return stored == modTime
}

// allFileModTimes loads all (path → file_modified) pairs into a map for fast in-memory lookups.
// pathsMissingTechnicals lists tracks the scanner has not probed for stream
// properties yet, so an ordinary scan backfills them once after an upgrade.
func (m *musicDB) pathsMissingTechnicals() (map[string]bool, error) {
	rows, err := m.db.Query(`SELECT path FROM tracks WHERE codec = '' AND sample_rate = 0`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	result := map[string]bool{}
	for rows.Next() {
		var path string
		if err := rows.Scan(&path); err != nil {
			return nil, err
		}
		result[path] = true
	}
	return result, rows.Err()
}

func (m *musicDB) allFileModTimes() (map[string]int64, error) {
	rows, err := m.db.Query(`SELECT path, file_modified FROM tracks`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	result := make(map[string]int64, 4096)
	for rows.Next() {
		var path string
		var modTime int64
		if err := rows.Scan(&path, &modTime); err != nil {
			return nil, err
		}
		result[path] = modTime
	}
	return result, rows.Err()
}

func (m *musicDB) trackIDByPath(path string) (int64, error) {
	var id int64
	err := m.db.QueryRow(`SELECT id FROM tracks WHERE path = ?`, path).Scan(&id)
	return id, err
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

func (m *musicDB) scanTrackRow(row *sql.Row) (map[string]any, error) {
	var id, albumID, added, fileModified int64
	var artist, title, path, rating, ratingHash, albumArtist, albumTitle, albumDate string
	var codec string
	var trackNum, discNum, sampleRate, bitsPerSample, channels int
	var duration, rgTrack, rgAlbum, peakTrack, peakAlbum float64
	err := row.Scan(&id, &albumID, &artist, &title, &trackNum, &discNum,
		&duration, &path, &rgTrack, &rgAlbum, &peakTrack, &peakAlbum,
		&rating, &ratingHash, &added, &fileModified, &codec, &sampleRate,
		&bitsPerSample, &channels, &albumArtist, &albumTitle, &albumDate)
	if err != nil {
		return nil, err
	}
	t := m.buildTrackMap(id, albumID, added, fileModified, artist, title, path, trackNum, discNum,
		duration, rgTrack, rgAlbum, peakTrack, peakAlbum, rating, ratingHash,
		albumArtist, albumTitle, albumDate)
	t["codec"] = codec
	t["samplerate"] = sampleRate
	t["bitspersample"] = bitsPerSample
	t["channels"] = channels
	// Enrich single track with rating from ratings table
	if ratingHash != "" {
		if r, err := m.getRating(ratingHash); err == nil && r > 0 {
			t["rating"] = r
		}
	}
	m.enrichWithTags([]map[string]any{t})
	return t, nil
}

func (m *musicDB) scanTrackRows(rows *sql.Rows) ([]map[string]any, error) {
	var tracks []map[string]any
	for rows.Next() {
		var id, albumID, added, fileModified int64
		var artist, title, path, rating, ratingHash, albumArtist, albumTitle, albumDate string
		var codec string
		var trackNum, discNum, sampleRate, bitsPerSample, channels int
		var duration, rgTrack, rgAlbum, peakTrack, peakAlbum float64
		if err := rows.Scan(&id, &albumID, &artist, &title, &trackNum, &discNum,
			&duration, &path, &rgTrack, &rgAlbum, &peakTrack, &peakAlbum,
			&rating, &ratingHash, &added, &fileModified, &codec, &sampleRate,
			&bitsPerSample, &channels, &albumArtist, &albumTitle, &albumDate); err != nil {
			return nil, err
		}
		track := m.buildTrackMap(id, albumID, added, fileModified, artist, title, path, trackNum, discNum,
			duration, rgTrack, rgAlbum, peakTrack, peakAlbum, rating, ratingHash,
			albumArtist, albumTitle, albumDate)
		track["codec"] = codec
		track["samplerate"] = sampleRate
		track["bitspersample"] = bitsPerSample
		track["channels"] = channels
		tracks = append(tracks, track)
	}
	if tracks == nil {
		tracks = []map[string]any{}
	}
	m.enrichWithRatings(tracks)
	m.enrichWithTags(tracks)
	return tracks, rows.Err()
}

func (m *musicDB) buildTrackMap(id, albumID, added, fileModified int64, artist, title, path string, trackNum, discNum int,
	duration, rgTrack, rgAlbum, peakTrack, peakAlbum float64, rating, ratingHash,
	albumArtist, albumTitle, albumDate string) map[string]any {
	idStr := strconv.FormatInt(id, 10)
	albumIDStr := strconv.FormatInt(albumID, 10)
	result := map[string]any{
		"id":            idStr,
		"song_id":       idStr,
		"album_id":      albumIDStr,
		"artist":        artist,
		"albumartist":   albumArtist,
		"title":         title,
		"album":         albumTitle,
		"date":          albumDate,
		"track":         strconv.Itoa(trackNum),
		"tracknumber":   trackNum,
		"discnumber":    discNum,
		"duration":      duration,
		"path":          path,
		"rating_hash":   ratingHash,
		"rating":        nil,
		"added":         added,
		"file_modified": fileModified,
	}
	if rgTrack != 0 || rgAlbum != 0 || peakTrack != 0 || peakAlbum != 0 {
		result["replay_gain"] = map[string]any{
			"track_gain": rgTrack,
			"album_gain": rgAlbum,
			"track_peak": peakTrack,
			"album_peak": peakAlbum,
		}
	}
	return result
}

// replacePlaylistTracks publishes an ordered queue edit as one stored-list write.
func (m *musicDB) replacePlaylistTracks(id int64, tracks []int64) error {
	tx, err := m.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`DELETE FROM playlist_tracks WHERE playlist_id = ?`, id); err != nil {
		return err
	}
	stmt, err := tx.Prepare(`INSERT INTO playlist_tracks(playlist_id, track_id, position) VALUES (?, ?, ?)`)
	if err != nil {
		return err
	}
	defer stmt.Close()
	for pos, track := range tracks {
		if _, err := stmt.Exec(id, track, pos+1); err != nil {
			return err
		}
	}
	return tx.Commit()
}
