package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

var lrclibClient = &http.Client{Timeout: 3 * time.Second}

// lrclib result cache: stores both positive and negative results to avoid
// repeated HTTP requests. Negative results expire after 24 hours.
type lrclibCacheEntry struct {
	text       string
	lyricsType string
	fetchedAt  time.Time
}

var (
	lrclibCache   = make(map[string]lrclibCacheEntry)
	lrclibCacheMu sync.Mutex
)

func lrclibCacheKey(artist, title string) string {
	return strings.ToLower(artist) + "\x00" + strings.ToLower(title)
}

// getCachedLrclib returns cached lyrics if available.
func getCachedLrclib(artist, title string) (string, string) {
	key := lrclibCacheKey(artist, title)
	lrclibCacheMu.Lock()
	defer lrclibCacheMu.Unlock()
	if e, ok := lrclibCache[key]; ok {
		// Negative results expire after 24 hours
		if e.text == "" && time.Since(e.fetchedAt) > 24*time.Hour {
			delete(lrclibCache, key)
			return "", ""
		}
		return e.text, e.lyricsType
	}
	return "", ""
}

// fetchAndCacheLrclib fetches lyrics from lrclib in the background, caches the
// result, and notifies clients so they can re-request.
func (a *app) fetchAndCacheLrclib(absPath, artist, title, album string, dur float64) {
	a.logger.Printf("lyrics: querying lrclib for %s - %s", artist, title)
	text, lyricsType := fetchLrclib(artist, title, album, dur)

	key := lrclibCacheKey(artist, title)
	lrclibCacheMu.Lock()
	lrclibCache[key] = lrclibCacheEntry{text: text, lyricsType: lyricsType, fetchedAt: time.Now()}
	lrclibCacheMu.Unlock()

	if text == "" {
		a.logger.Printf("lyrics: lrclib returned no results")
		return
	}

	a.logger.Printf("lyrics: lrclib returned %s lyrics (%d bytes)", lyricsType, len(text))
	if a.cfg.Library.SaveLRC {
		if err := saveLRC(absPath, text); err != nil {
			a.logger.Printf("lyrics: failed to save .lrc for %s: %v", absPath, err)
		} else {
			a.logger.Printf("lyrics: saved .lrc for %s", absPath)
		}
	}
	if a.cfg.Library.EmbedLyrics {
		if err := embedLyrics(absPath, text, lyricsType); err != nil {
			a.logger.Printf("lyrics: failed to embed in %s: %v", absPath, err)
		} else {
			a.logger.Printf("lyrics: embedded in %s", absPath)
		}
	}

	// Notify clients that lyrics are now available
	a.mpdHub.notify(SubPlayer)
}

type lrclibResponse struct {
	SyncedLyrics string `json:"syncedLyrics"`
	PlainLyrics  string `json:"plainLyrics"`
}

// fetchLrclib queries lrclib.net for lyrics matching the given track metadata.
// Returns lyrics text and type ("synced" or "plain"), or empty strings if not found.
func fetchLrclib(artist, title, album string, duration float64) (string, string) {
	if artist == "" || title == "" {
		return "", ""
	}

	params := url.Values{}
	params.Set("artist_name", artist)
	params.Set("track_name", title)
	if album != "" {
		params.Set("album_name", album)
	}
	if duration > 0 {
		params.Set("duration", fmt.Sprintf("%d", int(duration)))
	}

	req, err := http.NewRequest("GET", "https://lrclib.net/api/get?"+params.Encode(), nil)
	if err != nil {
		return "", ""
	}
	req.Header.Set("User-Agent", "melodyd/1.0 (https://github.com/carnager/melody-music)")

	resp, err := lrclibClient.Do(req)
	if err != nil || resp.StatusCode != 200 {
		if resp != nil {
			resp.Body.Close()
		}
		return "", ""
	}
	defer resp.Body.Close()

	var result lrclibResponse
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		return "", ""
	}

	if result.SyncedLyrics != "" {
		return result.SyncedLyrics, "synced"
	}
	if result.PlainLyrics != "" {
		return result.PlainLyrics, "plain"
	}

	return "", ""
}

// saveLRC writes lyrics to a .lrc sidecar file next to the audio file.
func saveLRC(trackPath, lyrics string) error {
	lrcPath := strings.TrimSuffix(trackPath, filepath.Ext(trackPath)) + ".lrc"
	return os.WriteFile(lrcPath, []byte(lyrics), 0o644)
}

// embedLyrics writes lyrics into the audio file's tags using external tools.
// Currently supports FLAC (via metaflac). Note: updates file mtime.
func embedLyrics(trackPath, lyrics, lyricsType string) error {
	ext := strings.ToLower(filepath.Ext(trackPath))
	switch ext {
	case ".flac":
		// Write lyrics to temp file — metaflac --set-tag truncates at newlines
		tmp, err := os.CreateTemp("", "lyrics-*.txt")
		if err != nil {
			return fmt.Errorf("create temp file: %w", err)
		}
		tmpPath := tmp.Name()
		defer os.Remove(tmpPath)

		if _, err := tmp.WriteString(lyrics); err != nil {
			tmp.Close()
			return fmt.Errorf("write temp file: %w", err)
		}
		tmp.Close()

		// Remove existing tag first (ignore error if tag doesn't exist)
		exec.Command("metaflac", "--remove-tag=LYRICS", trackPath).Run()

		out, err := exec.Command("metaflac",
			"--set-tag-from-file=LYRICS="+tmpPath,
			trackPath).CombinedOutput()
		if err != nil {
			return fmt.Errorf("metaflac: %w: %s", err, out)
		}

		return nil
	default:
		return fmt.Errorf("unsupported format: %s", ext)
	}
}
