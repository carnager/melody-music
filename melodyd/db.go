package main

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"fmt"
	"strconv"
	"strings"
	"sync"

	_ "modernc.org/sqlite"
)

type musicDB struct {
	db *sql.DB

	// Cached results for expensive queries, invalidated on scan.
	cacheMu                  sync.Mutex
	cachedAlbumsLatest       []map[string]any
	cachedAlbumsLatestFormatted string // pre-formatted MPD response lines
}

func openMusicDB(path string) (*musicDB, error) {
	db, err := sql.Open("sqlite", path+"?_journal_mode=WAL&_busy_timeout=5000")
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

func (m *musicDB) migrate() error {
	_, err := m.db.Exec(`
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
			created_at TEXT NOT NULL DEFAULT (datetime('now'))
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
	// Performance indexes
	m.db.Exec(`CREATE INDEX IF NOT EXISTS idx_tracks_album_id ON tracks(album_id)`)
	m.db.Exec(`CREATE INDEX IF NOT EXISTS idx_albums_created_at ON albums(created_at)`)

	// FTS5 for fast search
	m.db.Exec(`CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5(
		artist, albumartist, title, album
	)`)

	// Migration: add rating_hash column if missing
	m.db.Exec(`ALTER TABLE tracks ADD COLUMN rating_hash TEXT NOT NULL DEFAULT ''`)
	m.db.Exec(`CREATE INDEX IF NOT EXISTS idx_tracks_rating_hash ON tracks(rating_hash)`)
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
	res, err := m.db.Exec(`INSERT INTO artists(name) VALUES(?) ON CONFLICT(name) DO NOTHING`, name)
	if err != nil {
		return 0, err
	}
	id, err := res.LastInsertId()
	if err != nil || id == 0 {
		row := m.db.QueryRow(`SELECT id FROM artists WHERE name = ?`, name)
		if err := row.Scan(&id); err != nil {
			return 0, err
		}
	}
	return id, nil
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
	res, err := m.db.Exec(`INSERT INTO albums(artist_id, title, date)
		VALUES(?, ?, ?)
		ON CONFLICT(artist_id, title, date) DO NOTHING`,
		artistID, title, date)
	if err != nil {
		return 0, err
	}
	id, err := res.LastInsertId()
	if err != nil || id == 0 {
		row := m.db.QueryRow(`SELECT id FROM albums WHERE artist_id = ? AND title = ? AND date = ?`,
			artistID, title, date)
		if err := row.Scan(&id); err != nil {
			return 0, err
		}
	}
	return id, nil
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
		order = "max_mtime DESC"
	}
	query := fmt.Sprintf(`SELECT al.id, a.name, al.title, al.date, COALESCE(MAX(t.file_modified), 0) AS max_mtime
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
		var id, maxMtime int64
		var artist, title, date string
		if err := rows.Scan(&id, &artist, &title, &date, &maxMtime); err != nil {
			return nil, err
		}
		albums = append(albums, map[string]any{
			"id":          strconv.FormatInt(id, 10),
			"albumartist": artist,
			"album":       title,
			"date":        date,
			"album_id":    strconv.FormatInt(id, 10),
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
	m.cacheMu.Unlock()
}

func (m *musicDB) warmCache() {
	m.allAlbums(true)
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

	// Resolved during scan, used by batch writer to look up artist/album IDs
	albumArtist string
	album       string
	date        string
}

func (m *musicDB) upsertTrack(t *trackMeta) (int64, error) {
	rHash := trackRatingHash(t.albumArtist, t.album, t.Title, t.TrackNumber)
	res, err := m.db.Exec(`INSERT INTO tracks(album_id, artist, title, track_number, disc_number,
			duration, path, file_modified, replay_gain_track, replay_gain_album, peak_track, peak_album, rating_hash)
		VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
			rating_hash = excluded.rating_hash`,
		t.AlbumID, t.Artist, t.Title, t.TrackNumber, t.DiscNumber,
		t.Duration, t.Path, t.FileModified, t.ReplayGainTrack, t.ReplayGainAlbum, t.PeakTrack, t.PeakAlbum, rHash)
	if err != nil {
		return 0, err
	}
	id, err := res.LastInsertId()
	if err != nil || id == 0 {
		row := m.db.QueryRow(`SELECT id FROM tracks WHERE path = ?`, t.Path)
		if err := row.Scan(&id); err != nil {
			return 0, err
		}
	}
	// Update FTS index for this track
	m.db.Exec(`DELETE FROM tracks_fts WHERE rowid = ?`, id)
	m.db.Exec(`INSERT INTO tracks_fts(rowid, artist, albumartist, title, album) VALUES(?, ?, ?, ?, ?)`,
		id, t.Artist, t.albumArtist, t.Title, t.album)
	return id, nil
}

func (m *musicDB) trackByID(id int64) (map[string]any, error) {
	return m.scanTrackRow(m.db.QueryRow(`SELECT t.id, t.album_id, t.artist, t.title,
		t.track_number, t.disc_number, t.duration, t.path,
		t.replay_gain_track, t.replay_gain_album, t.peak_track, t.peak_album,
		t.rating, t.rating_hash, a.name, al.title, al.date
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

func (m *musicDB) trackByPath(path string) (map[string]any, error) {
	return m.scanTrackRow(m.db.QueryRow(`SELECT t.id, t.album_id, t.artist, t.title,
		t.track_number, t.disc_number, t.duration, t.path,
		t.replay_gain_track, t.replay_gain_album, t.peak_track, t.peak_album,
		t.rating, t.rating_hash, a.name, al.title, al.date
		FROM tracks t
		INNER JOIN albums al ON al.id = t.album_id
		INNER JOIN artists a ON a.id = al.artist_id
		WHERE t.path = ?`, path))
}

func (m *musicDB) tracksByPathPrefix(prefix string) ([]map[string]any, error) {
	rows, err := m.db.Query(`SELECT t.id, t.album_id, t.artist, t.title,
		t.track_number, t.disc_number, t.duration, t.path,
		t.replay_gain_track, t.replay_gain_album, t.peak_track, t.peak_album,
		t.rating, t.rating_hash, a.name, al.title, al.date
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
		t.rating, t.rating_hash, a.name, al.title, al.date
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
		t.rating, t.rating_hash, a.name, al.title, al.date
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
		t.rating, t.rating_hash, a.name, al.title, al.date
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
		t.rating, t.rating_hash, a.name, al.title, al.date
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
		t.rating, t.rating_hash, a.name, al.title, al.date
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
	_, err := m.db.Exec(`DELETE FROM playlists WHERE id = ?`, id)
	return err
}

func (m *musicDB) addTrackToPlaylist(playlistID, trackID int64) error {
	_, err := m.db.Exec(`INSERT INTO playlist_tracks(playlist_id, track_id, position)
		VALUES(?, ?, (SELECT COALESCE(MAX(position), 0) + 1 FROM playlist_tracks WHERE playlist_id = ?))`,
		playlistID, trackID, playlistID)
	return err
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
		if _, err := m.db.Exec(`DELETE FROM tracks WHERE id = ?`, id); err != nil {
			return err
		}
	}
	// Clean up orphaned albums and artists
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
	var id, albumID int64
	var artist, title, path, rating, ratingHash, albumArtist, albumTitle, albumDate string
	var trackNum, discNum int
	var duration, rgTrack, rgAlbum, peakTrack, peakAlbum float64
	err := row.Scan(&id, &albumID, &artist, &title, &trackNum, &discNum,
		&duration, &path, &rgTrack, &rgAlbum, &peakTrack, &peakAlbum,
		&rating, &ratingHash, &albumArtist, &albumTitle, &albumDate)
	if err != nil {
		return nil, err
	}
	t := m.buildTrackMap(id, albumID, artist, title, path, trackNum, discNum,
		duration, rgTrack, rgAlbum, peakTrack, peakAlbum, rating, ratingHash,
		albumArtist, albumTitle, albumDate)
	// Enrich single track with rating from ratings table
	if ratingHash != "" {
		if r, err := m.getRating(ratingHash); err == nil && r > 0 {
			t["rating"] = r
		}
	}
	return t, nil
}

func (m *musicDB) scanTrackRows(rows *sql.Rows) ([]map[string]any, error) {
	var tracks []map[string]any
	for rows.Next() {
		var id, albumID int64
		var artist, title, path, rating, ratingHash, albumArtist, albumTitle, albumDate string
		var trackNum, discNum int
		var duration, rgTrack, rgAlbum, peakTrack, peakAlbum float64
		if err := rows.Scan(&id, &albumID, &artist, &title, &trackNum, &discNum,
			&duration, &path, &rgTrack, &rgAlbum, &peakTrack, &peakAlbum,
			&rating, &ratingHash, &albumArtist, &albumTitle, &albumDate); err != nil {
			return nil, err
		}
		tracks = append(tracks, m.buildTrackMap(id, albumID, artist, title, path, trackNum, discNum,
			duration, rgTrack, rgAlbum, peakTrack, peakAlbum, rating, ratingHash,
			albumArtist, albumTitle, albumDate))
	}
	if tracks == nil {
		tracks = []map[string]any{}
	}
	m.enrichWithRatings(tracks)
	return tracks, rows.Err()
}

func (m *musicDB) buildTrackMap(id, albumID int64, artist, title, path string, trackNum, discNum int,
	duration, rgTrack, rgAlbum, peakTrack, peakAlbum float64, rating, ratingHash,
	albumArtist, albumTitle, albumDate string) map[string]any {
	idStr := strconv.FormatInt(id, 10)
	albumIDStr := strconv.FormatInt(albumID, 10)
	result := map[string]any{
		"id":          idStr,
		"song_id":     idStr,
		"album_id":    albumIDStr,
		"artist":      artist,
		"albumartist": albumArtist,
		"title":       title,
		"album":       albumTitle,
		"date":        albumDate,
		"track":       strconv.Itoa(trackNum),
		"tracknumber": trackNum,
		"discnumber":  discNum,
		"duration":    duration,
		"path":        path,
		"rating_hash": ratingHash,
		"rating":      nil,
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
