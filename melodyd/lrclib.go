package main

import (
	"encoding/json"
	"fmt"
	"math"
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

// fetchAndCacheLyrics fetches lyrics from NetEase first (synced), then lrclib
// as fallback, caches the result, and notifies clients so they can re-request.
func (a *app) fetchAndCacheLyrics(absPath, artist, title, album string, dur float64) {
	// Try NetEase first (better synced lyrics)
	a.logger.Printf("lyrics: querying netease for %s - %s", artist, title)
	text, lyricsType := fetchNetease(artist, title, dur)
	source := "netease"

	// Fall back to lrclib
	if text == "" {
		a.logger.Printf("lyrics: querying lrclib for %s - %s", artist, title)
		text, lyricsType = fetchLrclib(artist, title, album, dur)
		source = "lrclib"
	}

	key := lrclibCacheKey(artist, title)
	lrclibCacheMu.Lock()
	lrclibCache[key] = lrclibCacheEntry{text: text, lyricsType: lyricsType, fetchedAt: time.Now()}
	lrclibCacheMu.Unlock()

	if text == "" {
		a.logger.Printf("lyrics: no results from netease or lrclib")
		return
	}

	a.logger.Printf("lyrics: %s returned %s lyrics (%d bytes)", source, lyricsType, len(text))
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

// ---------------------------------------------------------------------------
// NetEase Cloud Music API
// ---------------------------------------------------------------------------

type neteaseSearchResult struct {
	Result struct {
		Songs []struct {
			ID      int    `json:"id"`
			Name    string `json:"name"`
			Artists []struct {
				Name string `json:"name"`
			} `json:"artists"`
			Duration int `json:"duration"` // milliseconds
		} `json:"songs"`
	} `json:"result"`
}

type neteaseLyricResult struct {
	LRC struct {
		Lyric string `json:"lyric"`
	} `json:"lrc"`
}

func fetchNetease(artist, title string, durationSecs float64) (string, string) {
	if artist == "" || title == "" {
		return "", ""
	}

	query := artist + " " + title
	resp, err := lrclibClient.PostForm("https://music.163.com/api/search/get", url.Values{
		"s":      {query},
		"type":   {"1"},
		"limit":  {"5"},
		"offset": {"0"},
	})
	if err != nil {
		return "", ""
	}
	defer resp.Body.Close()

	var search neteaseSearchResult
	if err := json.NewDecoder(resp.Body).Decode(&search); err != nil {
		return "", ""
	}

	titleLower := strings.ToLower(title)
	titleBase := titleLower
	if idx := strings.Index(titleBase, " ("); idx > 0 {
		titleBase = titleBase[:idx]
	}
	artistLower := strings.ToLower(artist)

	type candidate struct {
		id    int
		score int
	}
	var best candidate
	for _, s := range search.Result.Songs {
		nameLower := strings.ToLower(s.Name)
		nameBase := nameLower
		if idx := strings.Index(nameBase, " ("); idx > 0 {
			nameBase = nameBase[:idx]
		}
		titleMatch := nameLower == titleLower ||
			nameBase == titleLower ||
			nameLower == titleBase ||
			nameBase == titleBase
		if !titleMatch {
			continue
		}

		sDur := float64(s.Duration) / 1000.0
		durDiff := math.Abs(sDur - durationSecs)

		artistMatch := false
		for _, a := range s.Artists {
			aLower := strings.ToLower(a.Name)
			if aLower == artistLower || strings.Contains(artistLower, aLower) || strings.Contains(aLower, artistLower) {
				artistMatch = true
				break
			}
		}

		score := 0
		if artistMatch {
			score += 1000
		}
		if durDiff < 5 {
			score += 100
		} else if durDiff < 15 {
			score += 50
		} else if durDiff < 30 {
			score += 20
		}
		if !artistMatch {
			continue
		}
		if durDiff > 30 {
			continue
		}
		if score > best.score {
			best = candidate{id: s.ID, score: score}
		}
	}
	if best.id == 0 {
		return "", ""
	}

	lyrResp, err := lrclibClient.Get(fmt.Sprintf("https://music.163.com/api/song/lyric?os=osx&id=%d&lv=-1&kv=-1&tv=-1", best.id))
	if err != nil {
		return "", ""
	}
	defer lyrResp.Body.Close()

	var lyric neteaseLyricResult
	if err := json.NewDecoder(lyrResp.Body).Decode(&lyric); err != nil {
		return "", ""
	}

	lrc := strings.TrimSpace(lyric.LRC.Lyric)
	if lrc == "" {
		return "", ""
	}

	// Strip metadata lines and check we have real lyrics
	var cleaned []string
	lyricLineCount := 0
	for _, line := range strings.Split(lrc, "\n") {
		trimmed := strings.TrimSpace(line)
		if trimmed == "" {
			cleaned = append(cleaned, line)
			continue
		}
		if !strings.HasPrefix(trimmed, "[") {
			cleaned = append(cleaned, line)
			continue
		}
		if len(trimmed) > 1 {
			inner := trimmed[1:]
			if idx := strings.Index(inner, "]"); idx > 0 {
				tag := inner[:idx]
				if strings.Contains(tag, ":") && !strings.ContainsAny(tag[:strings.Index(tag, ":")], "0123456789") {
					continue
				}
				content := strings.TrimSpace(inner[idx+1:])
				if neteaseIsMetadata(content) {
					continue
				}
			}
		}
		cleaned = append(cleaned, line)
		lyricLineCount++
	}
	if lyricLineCount < 3 {
		return "", ""
	}

	return strings.TrimSpace(strings.Join(cleaned, "\n")), "synced"
}

func neteaseIsMetadata(s string) bool {
	lower := strings.ToLower(s)
	if strings.Contains(s, " : ") || strings.Contains(s, "：") {
		return true
	}
	prefixes := []string{
		"作曲", "作词", "编曲", "制作", "混音", "录音", "母带", "出品", "监制",
		"composed by", "arranged by", "written by", "lyrics by", "music by",
		"produced by", "mixed by", "recorded by", "mastered by",
		"edited", "siwon",
	}
	for _, p := range prefixes {
		if strings.HasPrefix(s, p) || strings.HasPrefix(lower, p) {
			return true
		}
	}
	return false
}
