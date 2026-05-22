package main

import (
	"fmt"
	"io"
	"log"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/dhowden/tag"
	"github.com/fsnotify/fsnotify"
)

var audioExtensions = map[string]bool{
	".flac": true,
	".mp3":  true,
	".m4a":  true,
	".ogg":  true,
	".opus": true,
}

type scanner struct {
	musicDir          string
	db                *musicDB
	logger            *log.Logger
	transcodeCacheDir string
	scanning          bool
	scanMu            sync.Mutex
	onScanComplete    func() // called after a successful full scan
}

func newScanner(musicDir string, db *musicDB, logger *log.Logger, transcodeCacheDir string) *scanner {
	return &scanner{
		musicDir:          musicDir,
		db:                db,
		logger:            logger,
		transcodeCacheDir: transcodeCacheDir,
	}
}

func (s *scanner) isScanning() bool {
	s.scanMu.Lock()
	defer s.scanMu.Unlock()
	return s.scanning
}

func (s *scanner) fullScan() error {
	s.scanMu.Lock()
	if s.scanning {
		s.scanMu.Unlock()
		return fmt.Errorf("scan already in progress")
	}
	s.scanning = true
	s.scanMu.Unlock()
	defer func() {
		s.scanMu.Lock()
		s.scanning = false
		s.scanMu.Unlock()
	}()

	start := time.Now()
	s.logger.Printf("scanner: starting full scan of %s", s.musicDir)

	// Load all known file mod times in one query for fast skip checks
	modTimes, err := s.db.allFileModTimes()
	if err != nil {
		s.logger.Printf("scanner: warning: could not load mod times: %v", err)
		modTimes = map[string]int64{}
	}

	// Collect all audio files
	var files []string
	err = filepath.WalkDir(s.musicDir, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return nil // skip errors
		}
		if d.IsDir() {
			return nil
		}
		ext := strings.ToLower(filepath.Ext(path))
		if audioExtensions[ext] {
			files = append(files, path)
		}
		return nil
	})
	if err != nil {
		return fmt.Errorf("walk dir: %w", err)
	}

	s.logger.Printf("scanner: found %d audio files", len(files))

	// Phase 1: read metadata concurrently (CPU-bound tag parsing + ffprobe for duration)
	type scanResult struct {
		meta *trackMeta
		path string
		err  error
		skip bool
	}

	workers := runtime.NumCPU()
	if workers < 4 {
		workers = 4
	}
	results := make(chan scanResult, len(files))
	sem := make(chan struct{}, workers)

	for _, path := range files {
		go func(p string) {
			sem <- struct{}{}
			defer func() { <-sem }()

			info, err := os.Stat(p)
			if err != nil {
				results <- scanResult{path: p, err: err}
				return
			}
			modTime := info.ModTime().UnixMilli()

			// Fast skip: check in-memory mod time map instead of per-file DB query
			if stored, ok := modTimes[p]; ok && stored == modTime {
				results <- scanResult{path: p, skip: true}
				return
			}

			meta, err := s.readFileMeta(p, modTime)
			results <- scanResult{meta: meta, path: p, err: err}
		}(path)
	}

	// Phase 2: collect results, then batch-write to DB in a transaction
	var toUpsert []*trackMeta
	var scanned, skipped, scanErrors int
	for i := 0; i < len(files); i++ {
		r := <-results
		if r.skip {
			skipped++
		} else if r.err != nil {
			scanErrors++
			if scanErrors <= 10 {
				s.logger.Printf("scanner: error reading %s: %v", r.path, r.err)
			}
		} else {
			toUpsert = append(toUpsert, r.meta)
		}
		if (i+1)%500 == 0 {
			s.logger.Printf("scanner: read progress %d/%d (new=%d skipped=%d errors=%d)",
				i+1, len(files), len(toUpsert), skipped, scanErrors)
		}
	}

	s.logger.Printf("scanner: read complete (new/changed=%d skipped=%d errors=%d), writing to database...",
		len(toUpsert), skipped, scanErrors)

	// Batch upsert in a single transaction
	if len(toUpsert) > 0 {
		tx, err := s.db.db.Begin()
		if err != nil {
			return fmt.Errorf("begin tx: %w", err)
		}
		// Use ON CONFLICT DO UPDATE ... RETURNING id to always get the row ID in one statement
		stmtArtist, err := tx.Prepare(`INSERT INTO artists(name) VALUES(?)
			ON CONFLICT(name) DO UPDATE SET name = excluded.name
			RETURNING id`)
		if err != nil {
			tx.Rollback()
			return fmt.Errorf("prepare artist stmt: %w", err)
		}
		stmtAlbum, err := tx.Prepare(`INSERT INTO albums(artist_id, title, date) VALUES(?, ?, ?)
			ON CONFLICT(artist_id, title, date) DO UPDATE SET title = excluded.title
			RETURNING id`)
		if err != nil {
			tx.Rollback()
			return fmt.Errorf("prepare album stmt: %w", err)
		}
		stmtTrack, err := tx.Prepare(`INSERT INTO tracks(album_id, artist, title, track_number, disc_number,
			duration, path, file_modified, replay_gain_track, replay_gain_album, peak_track, peak_album)
			VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
				peak_album = excluded.peak_album`)
		if err != nil {
			tx.Rollback()
			return fmt.Errorf("prepare track stmt: %w", err)
		}
		defer stmtArtist.Close()
		defer stmtAlbum.Close()
		defer stmtTrack.Close()

		for _, t := range toUpsert {
			var artistID int64
			if err := stmtArtist.QueryRow(t.albumArtist).Scan(&artistID); err != nil {
				scanErrors++
				if scanErrors <= 10 {
					s.logger.Printf("scanner: artist upsert error for %s (%q): %v", t.Path, t.albumArtist, err)
				}
				continue
			}

			var albumID int64
			if err := stmtAlbum.QueryRow(artistID, t.album, t.date).Scan(&albumID); err != nil {
				scanErrors++
				if scanErrors <= 10 {
					s.logger.Printf("scanner: album upsert error for %s (%q): %v", t.Path, t.album, err)
				}
				continue
			}

			_, err := stmtTrack.Exec(albumID, t.Artist, t.Title, t.TrackNumber, t.DiscNumber,
				t.Duration, t.Path, t.FileModified,
				t.ReplayGainTrack, t.ReplayGainAlbum, t.PeakTrack, t.PeakAlbum)
			if err != nil {
				scanErrors++
				if scanErrors <= 10 {
					s.logger.Printf("scanner: track upsert error for %s: %v", t.Path, err)
				}
			} else {
				scanned++
			}
		}

		if err := tx.Commit(); err != nil {
			return fmt.Errorf("commit tx: %w", err)
		}

		// Invalidate transcode cache for changed tracks
		if s.transcodeCacheDir != "" {
			s.invalidateTranscodeCache(toUpsert)
		}
	}

	// Remove tracks for files that no longer exist
	pathSet := make(map[string]struct{}, len(files))
	for _, f := range files {
		pathSet[f] = struct{}{}
	}
	if err := s.db.removeTracksNotIn(pathSet); err != nil {
		s.logger.Printf("scanner: cleanup error: %v", err)
	}

	elapsed := time.Since(start)
	s.logger.Printf("scanner: full scan complete in %s (scanned=%d skipped=%d errors=%d total=%d)",
		elapsed.Round(time.Millisecond), scanned, skipped, scanErrors, len(files))

	if s.onScanComplete != nil {
		s.onScanComplete()
	}
	return nil
}

// readFileMeta reads tags and duration from a file without touching the database.
func (s *scanner) readFileMeta(path string, modTime int64) (*trackMeta, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	metadata, err := tag.ReadFrom(f)
	if err != nil {
		// Tag reading failed — use filename-based metadata
		return s.readFileMetaMinimal(path, modTime), nil
	}

	artist := strings.TrimSpace(metadata.Artist())
	albumArtist := strings.TrimSpace(metadata.AlbumArtist())
	if albumArtist == "" {
		albumArtist = artist
	}
	if albumArtist == "" {
		albumArtist = "Unknown Artist"
	}
	album := strings.TrimSpace(metadata.Album())
	if album == "" {
		album = filepath.Base(filepath.Dir(path))
	}
	title := strings.TrimSpace(metadata.Title())
	if title == "" {
		title = strings.TrimSuffix(filepath.Base(path), filepath.Ext(path))
	}
	trackNum, _ := metadata.Track()
	discNum, _ := metadata.Disc()
	if discNum == 0 {
		discNum = 1
	}
	year := metadata.Year()
	date := "0000"
	if year > 0 {
		date = strconv.Itoa(year)
	}

	rgTrack, rgAlbum, peakTrack, peakAlbum := readReplayGain(metadata)

	// Get duration from tags if available, otherwise read from audio stream headers.
	duration := durationFromTags(metadata)
	if duration == 0 {
		duration = durationFromFile(path)
	}

	return &trackMeta{
		Artist:          firstNonEmpty(artist, albumArtist),
		Title:           title,
		TrackNumber:     trackNum,
		DiscNumber:      discNum,
		Duration:        duration,
		Path:            path,
		FileModified:    modTime,
		ReplayGainTrack: rgTrack,
		ReplayGainAlbum: rgAlbum,
		PeakTrack:       peakTrack,
		PeakAlbum:       peakAlbum,
		albumArtist:     albumArtist,
		album:           album,
		date:            date,
	}, nil
}

func (s *scanner) readFileMetaMinimal(path string, modTime int64) *trackMeta {
	title := strings.TrimSuffix(filepath.Base(path), filepath.Ext(path))
	dir := filepath.Base(filepath.Dir(path))

	return &trackMeta{
		Artist:       "Unknown Artist",
		Title:        title,
		Duration:     0,
		Path:         path,
		FileModified: modTime,
		albumArtist:  "Unknown Artist",
		album:        dir,
		date:         "0000",
	}
}

// scanFile scans a single file (used by the filesystem watcher for incremental updates).
func (s *scanner) scanFile(path string) error {
	info, err := os.Stat(path)
	if err != nil {
		return err
	}
	modTime := info.ModTime().UnixMilli()

	if s.db.isFileUnchanged(path, modTime) {
		return fmt.Errorf("unchanged")
	}

	meta, err := s.readFileMeta(path, modTime)
	if err != nil {
		return err
	}

	artistID, err := s.db.upsertArtist(meta.albumArtist)
	if err != nil {
		return fmt.Errorf("upsert artist: %w", err)
	}
	albumID, err := s.db.upsertAlbum(artistID, meta.album, meta.date)
	if err != nil {
		return fmt.Errorf("upsert album: %w", err)
	}
	meta.AlbumID = albumID

	_, err = s.db.upsertTrack(meta)
	return err
}

// watchForChanges monitors the music directory for file changes.
func (s *scanner) watchForChanges() {
	watcher, err := fsnotify.NewWatcher()
	if err != nil {
		s.logger.Printf("scanner: fsnotify error: %v", err)
		return
	}
	defer watcher.Close()

	// Add all directories
	filepath.WalkDir(s.musicDir, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if d.IsDir() {
			_ = watcher.Add(path)
		}
		return nil
	})

	s.logger.Printf("scanner: watching %s for changes", s.musicDir)

	// Debounce: accumulate changes, then trigger scan
	var debounceTimer *time.Timer
	var pendingMu sync.Mutex
	pending := make(map[string]struct{})

	processPending := func() {
		pendingMu.Lock()
		paths := make([]string, 0, len(pending))
		for p := range pending {
			paths = append(paths, p)
		}
		pending = make(map[string]struct{})
		pendingMu.Unlock()

		for _, p := range paths {
			ext := strings.ToLower(filepath.Ext(p))
			if !audioExtensions[ext] {
				continue
			}
			if _, err := os.Stat(p); os.IsNotExist(err) {
				// File deleted — full scan to clean up
				s.logger.Printf("scanner: file deleted: %s, triggering cleanup", p)
				go s.fullScan()
				return
			}
			if err := s.scanFile(p); err != nil && err.Error() != "unchanged" {
				s.logger.Printf("scanner: error scanning %s: %v", p, err)
			} else if err == nil {
				s.logger.Printf("scanner: updated %s", filepath.Base(p))
			}
		}
	}

	for {
		select {
		case event, ok := <-watcher.Events:
			if !ok {
				return
			}
			if event.Op&(fsnotify.Create|fsnotify.Write|fsnotify.Remove|fsnotify.Rename) != 0 {
				pendingMu.Lock()
				pending[event.Name] = struct{}{}
				pendingMu.Unlock()

				if debounceTimer != nil {
					debounceTimer.Stop()
				}
				debounceTimer = time.AfterFunc(2*time.Second, processPending)

				// Watch newly created directories
				if event.Op&fsnotify.Create != 0 {
					if info, err := os.Stat(event.Name); err == nil && info.IsDir() {
						_ = watcher.Add(event.Name)
					}
				}
			}
		case err, ok := <-watcher.Errors:
			if !ok {
				return
			}
			s.logger.Printf("scanner: watcher error: %v", err)
		}
	}
}

// ---------------------------------------------------------------------------
// ReplayGain parsing
// ---------------------------------------------------------------------------

func readReplayGain(m tag.Metadata) (trackGain, albumGain, peakTrack, peakAlbum float64) {
	raw := m.Raw()
	if raw == nil {
		return
	}

	get := func(keys ...string) float64 {
		for _, key := range keys {
			if v, ok := raw[key]; ok {
				return parseRGValue(v)
			}
			// Try case-insensitive
			for k, v := range raw {
				if strings.EqualFold(k, key) {
					return parseRGValue(v)
				}
			}
		}
		return 0
	}

	trackGain = get("REPLAYGAIN_TRACK_GAIN", "replaygain_track_gain")
	albumGain = get("REPLAYGAIN_ALBUM_GAIN", "replaygain_album_gain")
	peakTrack = get("REPLAYGAIN_TRACK_PEAK", "replaygain_track_peak")
	peakAlbum = get("REPLAYGAIN_ALBUM_PEAK", "replaygain_album_peak")
	return
}

func parseRGValue(v any) float64 {
	var s string
	switch val := v.(type) {
	case string:
		s = val
	case []string:
		if len(val) > 0 {
			s = val[0]
		}
	default:
		s = fmt.Sprintf("%v", val)
	}
	s = strings.TrimSpace(s)
	s = strings.TrimSuffix(s, " dB")
	s = strings.TrimSuffix(s, "dB")
	s = strings.TrimSpace(s)
	f, _ := strconv.ParseFloat(s, 64)
	return f
}

// ---------------------------------------------------------------------------
// Duration from tags (cheap, no subprocess)
// ---------------------------------------------------------------------------

// durationFromTags tries to extract duration from tag metadata.
// Many taggers write LENGTH (milliseconds) or TDUR fields.
func durationFromTags(m tag.Metadata) float64 {
	raw := m.Raw()
	if raw == nil {
		return 0
	}

	// Vorbis comments: LENGTH (in milliseconds, written by some taggers)
	for _, key := range []string{"LENGTH", "length"} {
		if v, ok := raw[key]; ok {
			if ms := parseTagFloat(v); ms > 0 {
				return ms / 1000.0
			}
		}
	}

	// ID3v2: TLEN (duration in milliseconds)
	for _, key := range []string{"TLEN", "TDUR"} {
		if v, ok := raw[key]; ok {
			if ms := parseTagFloat(v); ms > 0 {
				return ms / 1000.0
			}
		}
	}

	return 0
}

// durationFromFile reads duration directly from audio file headers.
// Supports FLAC (STREAMINFO), MP4/M4A (mvhd atom), OGG/Opus (last page granule).
func durationFromFile(path string) float64 {
	f, err := os.Open(path)
	if err != nil {
		return 0
	}
	defer f.Close()

	header := make([]byte, 4)
	if _, err := io.ReadFull(f, header); err != nil {
		return 0
	}

	switch {
	case string(header) == "fLaC":
		return flacDuration(f)
	case string(header) == "OggS":
		f.Seek(0, io.SeekStart)
		return oggDuration(f)
	case header[0] == 0xFF && (header[1]&0xE0) == 0xE0:
		// MP3 sync word
		f.Seek(0, io.SeekStart)
		return mp3Duration(f)
	default:
		// Try MP4 — check for ftyp box
		f.Seek(0, io.SeekStart)
		buf := make([]byte, 8)
		if _, err := io.ReadFull(f, buf); err != nil {
			return 0
		}
		if string(buf[4:8]) == "ftyp" {
			f.Seek(0, io.SeekStart)
			return mp4Duration(f)
		}
	}
	return 0
}

// flacDuration reads the STREAMINFO block from a FLAC file.
// File position must be right after the "fLaC" magic.
func flacDuration(f *os.File) float64 {
	// METADATA_BLOCK_HEADER: 1 byte (last-block flag + type) + 3 bytes (length)
	blockHeader := make([]byte, 4)
	if _, err := io.ReadFull(f, blockHeader); err != nil {
		return 0
	}
	blockType := blockHeader[0] & 0x7F
	if blockType != 0 { // must be STREAMINFO
		return 0
	}
	// STREAMINFO is 34 bytes
	si := make([]byte, 34)
	if _, err := io.ReadFull(f, si); err != nil {
		return 0
	}
	// Sample rate: 20 bits at offset 10
	sampleRate := uint32(si[10])<<12 | uint32(si[11])<<4 | uint32(si[12])>>4
	// Total samples: 36 bits at offset 13 (4 bits) + 14-17 (32 bits)
	totalSamples := uint64(si[13]&0x0F)<<32 | uint64(si[14])<<24 | uint64(si[15])<<16 | uint64(si[16])<<8 | uint64(si[17])
	if sampleRate == 0 {
		return 0
	}
	return float64(totalSamples) / float64(sampleRate)
}

// oggDuration reads the last OGG page to get the granule position.
func oggDuration(f *os.File) float64 {
	// We need the sample rate from the first page and the granule from the last page.
	// Read first page to get Vorbis/Opus sample rate.
	buf := make([]byte, 128)
	if _, err := io.ReadFull(f, buf); err != nil {
		return 0
	}
	if string(buf[:4]) != "OggS" {
		return 0
	}
	// Segment table starts at byte 27, number of segments at byte 26
	nSegments := int(buf[26])
	segTableEnd := 27 + nSegments
	if segTableEnd > len(buf) {
		return 0
	}
	dataStart := segTableEnd
	var sampleRate uint32
	// Check for Vorbis identification header
	if dataStart+30 < len(buf) && string(buf[dataStart+1:dataStart+7]) == "vorbis" {
		sampleRate = uint32(buf[dataStart+12]) | uint32(buf[dataStart+13])<<8 |
			uint32(buf[dataStart+14])<<16 | uint32(buf[dataStart+15])<<24
	}
	// Check for OpusHead
	if dataStart+19 < len(buf) && string(buf[dataStart:dataStart+8]) == "OpusHead" {
		// Opus always uses 48000 for granule position
		sampleRate = 48000
	}
	if sampleRate == 0 {
		return 0
	}

	// Seek to end and scan backward for last OggS page
	fi, err := f.Stat()
	if err != nil {
		return 0
	}
	searchSize := int64(65536)
	if searchSize > fi.Size() {
		searchSize = fi.Size()
	}
	tail := make([]byte, searchSize)
	f.Seek(fi.Size()-searchSize, io.SeekStart)
	if _, err := io.ReadFull(f, tail); err != nil {
		return 0
	}
	// Find last "OggS" in tail
	lastOgg := -1
	for i := len(tail) - 4; i >= 0; i-- {
		if string(tail[i:i+4]) == "OggS" {
			lastOgg = i
			break
		}
	}
	if lastOgg < 0 || lastOgg+14 > len(tail) {
		return 0
	}
	// Granule position is at offset 6 (8 bytes, little-endian)
	gp := uint64(tail[lastOgg+6]) | uint64(tail[lastOgg+7])<<8 |
		uint64(tail[lastOgg+8])<<16 | uint64(tail[lastOgg+9])<<24 |
		uint64(tail[lastOgg+10])<<32 | uint64(tail[lastOgg+11])<<40 |
		uint64(tail[lastOgg+12])<<48 | uint64(tail[lastOgg+13])<<56
	if gp == 0 || gp == ^uint64(0) {
		return 0
	}
	return float64(gp) / float64(sampleRate)
}

// mp3Duration estimates MP3 duration from file size and first frame bitrate,
// or from Xing/VBRI header if present.
func mp3Duration(f *os.File) float64 {
	fi, err := f.Stat()
	if err != nil {
		return 0
	}
	fileSize := fi.Size()

	buf := make([]byte, 256)
	if _, err := io.ReadFull(f, buf); err != nil {
		return 0
	}

	// Find first sync
	off := 0
	for off < len(buf)-4 {
		if buf[off] == 0xFF && (buf[off+1]&0xE0) == 0xE0 {
			break
		}
		off++
	}
	if off >= len(buf)-4 {
		return 0
	}

	b1 := buf[off+1]
	b2 := buf[off+2]
	version := (b1 >> 3) & 0x03    // 0=2.5, 2=2, 3=1
	layer := (b1 >> 1) & 0x03      // 1=III, 2=II, 3=I
	bitrateIdx := (b2 >> 4) & 0x0F
	sampleIdx := (b2 >> 2) & 0x03

	if version == 1 || layer == 0 || bitrateIdx == 0 || bitrateIdx == 15 || sampleIdx == 3 {
		return 0
	}

	// Bitrate table for MPEG1 Layer III
	bitrateTable := [16]int{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}
	sampleRateTable := [4]int{44100, 48000, 32000, 0}

	bitrate := 0
	sampleRate := 0
	if version == 3 && layer == 1 { // MPEG1 Layer III
		bitrate = bitrateTable[bitrateIdx] * 1000
		sampleRate = sampleRateTable[sampleIdx]
	} else {
		// Simplified — just use MPEG1 L3 table as estimate
		bitrate = bitrateTable[bitrateIdx] * 1000
		sampleRate = sampleRateTable[sampleIdx]
		if version != 3 {
			sampleRate /= 2
		}
	}

	if bitrate == 0 || sampleRate == 0 {
		return 0
	}

	// Check for Xing header (more accurate for VBR)
	frameSize := 144*bitrate/sampleRate + int((b2>>1)&1)
	xingOff := off + 4
	if version == 3 { // MPEG1
		xingOff += 32 // side info
	} else {
		xingOff += 17
	}
	if xingOff+8 < len(buf) {
		tag := string(buf[xingOff : xingOff+4])
		if tag == "Xing" || tag == "Info" {
			flags := uint32(buf[xingOff+4])<<24 | uint32(buf[xingOff+5])<<16 |
				uint32(buf[xingOff+6])<<8 | uint32(buf[xingOff+7])
			if flags&1 != 0 && xingOff+12 < len(buf) { // frames field present
				frames := uint32(buf[xingOff+8])<<24 | uint32(buf[xingOff+9])<<16 |
					uint32(buf[xingOff+10])<<8 | uint32(buf[xingOff+11])
				samplesPerFrame := 1152
				return float64(frames) * float64(samplesPerFrame) / float64(sampleRate)
			}
		}
	}

	// Fallback: estimate from file size and CBR bitrate
	_ = frameSize
	return float64(fileSize) * 8 / float64(bitrate)
}

// mp4Duration reads duration from MP4/M4A mvhd atom.
func mp4Duration(f *os.File) float64 {
	// Walk top-level atoms looking for moov
	fi, err := f.Stat()
	if err != nil {
		return 0
	}
	fileSize := fi.Size()

	var pos int64
	for pos < fileSize {
		f.Seek(pos, io.SeekStart)
		header := make([]byte, 8)
		if _, err := io.ReadFull(f, header); err != nil {
			return 0
		}
		size := int64(header[0])<<24 | int64(header[1])<<16 | int64(header[2])<<8 | int64(header[3])
		name := string(header[4:8])

		if size < 8 {
			if size == 1 { // 64-bit extended size
				ext := make([]byte, 8)
				if _, err := io.ReadFull(f, ext); err != nil {
					return 0
				}
				size = int64(ext[0])<<56 | int64(ext[1])<<48 | int64(ext[2])<<40 | int64(ext[3])<<32 |
					int64(ext[4])<<24 | int64(ext[5])<<16 | int64(ext[6])<<8 | int64(ext[7])
			} else {
				return 0
			}
		}

		if name == "moov" {
			return mp4FindMvhd(f, pos+8, pos+size)
		}
		pos += size
	}
	return 0
}

func mp4FindMvhd(f *os.File, start, end int64) float64 {
	pos := start
	for pos < end {
		f.Seek(pos, io.SeekStart)
		header := make([]byte, 8)
		if _, err := io.ReadFull(f, header); err != nil {
			return 0
		}
		size := int64(header[0])<<24 | int64(header[1])<<16 | int64(header[2])<<8 | int64(header[3])
		name := string(header[4:8])

		if size < 8 {
			return 0
		}

		if name == "mvhd" {
			data := make([]byte, size-8)
			if _, err := io.ReadFull(f, data); err != nil {
				return 0
			}
			version := data[0]
			if version == 0 {
				// 4 bytes timescale at offset 12, 4 bytes duration at offset 16
				timeScale := uint32(data[12])<<24 | uint32(data[13])<<16 | uint32(data[14])<<8 | uint32(data[15])
				duration := uint32(data[16])<<24 | uint32(data[17])<<16 | uint32(data[18])<<8 | uint32(data[19])
				if timeScale > 0 {
					return float64(duration) / float64(timeScale)
				}
			} else {
				// 4 bytes timescale at offset 20, 8 bytes duration at offset 24
				timeScale := uint32(data[20])<<24 | uint32(data[21])<<16 | uint32(data[22])<<8 | uint32(data[23])
				duration := uint64(data[24])<<56 | uint64(data[25])<<48 | uint64(data[26])<<40 | uint64(data[27])<<32 |
					uint64(data[28])<<24 | uint64(data[29])<<16 | uint64(data[30])<<8 | uint64(data[31])
				if timeScale > 0 {
					return float64(duration) / float64(timeScale)
				}
			}
		}
		pos += size
	}
	return 0
}

func parseTagFloat(v any) float64 {
	var s string
	switch val := v.(type) {
	case string:
		s = val
	case []string:
		if len(val) > 0 {
			s = val[0]
		}
	default:
		s = fmt.Sprintf("%v", val)
	}
	f, _ := strconv.ParseFloat(strings.TrimSpace(s), 64)
	return f
}


// ---------------------------------------------------------------------------
// Cover art extraction
// ---------------------------------------------------------------------------

func extractCoverArt(path string) (data []byte, mimeType string) {
	f, err := os.Open(path)
	if err != nil {
		return nil, ""
	}
	defer f.Close()

	m, err := tag.ReadFrom(f)
	if err != nil {
		return nil, ""
	}
	if pic := m.Picture(); pic != nil {
		return pic.Data, pic.MIMEType
	}
	return nil, ""
}

// findFolderArt looks for cover art images in the directory.
func findFolderArt(dir string) string {
	candidates := []string{
		"cover.jpg", "cover.png", "Cover.jpg", "Cover.png",
		"folder.jpg", "folder.png", "Folder.jpg", "Folder.png",
		"front.jpg", "front.png", "Front.jpg", "Front.png",
		"album.jpg", "album.png", "Album.jpg", "Album.png",
	}
	for _, name := range candidates {
		p := filepath.Join(dir, name)
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	return ""
}

func (s *scanner) invalidateTranscodeCache(changed []*trackMeta) {
	for _, t := range changed {
		id, err := s.db.trackIDByPath(t.Path)
		if err != nil || id == 0 {
			continue
		}
		pattern := filepath.Join(s.transcodeCacheDir, fmt.Sprintf("%d_*", id))
		matches, _ := filepath.Glob(pattern)
		for _, m := range matches {
			os.Remove(m)
		}
	}
	if len(changed) > 0 {
		s.logger.Printf("scanner: invalidated transcode cache for %d changed tracks", len(changed))
	}
}
