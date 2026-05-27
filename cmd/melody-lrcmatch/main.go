package main

import (
	"database/sql"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"math"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"

	_ "modernc.org/sqlite"
)

func main() {
	melodyDB := flag.String("db", "", "path to melodyd's melody.db")
	lrclibDB := flag.String("lrclib", "", "path to lrclib SQLite dump (optional with -netease)")
	netease := flag.Bool("netease", false, "fetch missing lyrics from NetEase Cloud Music")
	dryRun := flag.Bool("dry-run", false, "print matches without writing files")
	flag.Parse()

	if *melodyDB == "" {
		fmt.Fprintln(os.Stderr, "Usage: melody-lrcmatch -db <melody.db> [-lrclib <lrclib.sqlite3>] [-netease] [-dry-run]")
		os.Exit(1)
	}
	if *lrclibDB == "" && !*netease {
		fmt.Fprintln(os.Stderr, "Specify at least one of -lrclib or -netease")
		os.Exit(1)
	}

	mdb, err := sql.Open("sqlite", *melodyDB+"?mode=ro")
	if err != nil {
		log.Fatalf("open melody db: %v", err)
	}
	defer mdb.Close()

	var lrclibStmt *sql.Stmt
	if *lrclibDB != "" {
		ldb, err := sql.Open("sqlite", *lrclibDB+"?mode=ro")
		if err != nil {
			log.Fatalf("open lrclib db: %v", err)
		}
		defer ldb.Close()

		lrclibStmt, err = ldb.Prepare(`
			SELECT l.synced_lyrics, l.plain_lyrics
			FROM tracks t
			JOIN lyrics l ON l.id = t.last_lyrics_id
			WHERE t.artist_name_lower = ?
			  AND t.name_lower = ?
			  AND t.album_name_lower = ?
			  AND ABS(t.duration - ?) < 2
			  AND (l.synced_lyrics IS NOT NULL OR l.plain_lyrics IS NOT NULL)
			LIMIT 1
		`)
		if err != nil {
			log.Fatalf("prepare lrclib query: %v", err)
		}
		defer lrclibStmt.Close()
	}

	rows, err := mdb.Query(`SELECT t.path, t.artist, t.title, t.duration, a.title AS album_title
		FROM tracks t
		JOIN albums a ON a.id = t.album_id`)
	if err != nil {
		log.Fatalf("query melody tracks: %v", err)
	}
	defer rows.Close()

	var total, matched, skipped, written, neteaseHits int

	for rows.Next() {
		var path, artist, title string
		var duration float64
		var album string
		if err := rows.Scan(&path, &artist, &title, &duration, &album); err != nil {
			log.Printf("scan: %v", err)
			continue
		}
		total++

		// Check if .lrc already exists
		lrcPath := strings.TrimSuffix(path, filepath.Ext(path)) + ".lrc"
		if _, err := os.Stat(lrcPath); err == nil {
			skipped++
			continue
		}

		// Try lrclib first
		var lyrics, kind string
		if lrclibStmt != nil {
			var syncedLyrics, plainLyrics sql.NullString
			err := lrclibStmt.QueryRow(
				strings.ToLower(artist),
				strings.ToLower(title),
				strings.ToLower(album),
				duration,
			).Scan(&syncedLyrics, &plainLyrics)
			if err == nil {
				if syncedLyrics.String != "" {
					lyrics = syncedLyrics.String
					kind = "synced"
				} else if plainLyrics.String != "" {
					lyrics = plainLyrics.String
					kind = "plain"
				}
			}
		}

		// Fall back to NetEase
		if lyrics == "" && *netease {
			if lrc, err := fetchNeteaseLyrics(artist, title, duration); err == nil && lrc != "" {
				lyrics = lrc
				kind = "netease"
				neteaseHits++
			}
		}

		if lyrics == "" {
			continue
		}

		matched++
		fmt.Printf("[%s] %s - %s (%s)\n", kind, artist, title, album)

		if *dryRun {
			continue
		}

		if err := os.WriteFile(lrcPath, []byte(lyrics), 0o644); err != nil {
			log.Printf("write %s: %v", lrcPath, err)
			continue
		}
		written++
	}

	if err := rows.Err(); err != nil {
		log.Fatalf("rows: %v", err)
	}

	fmt.Printf("total: %d, matched: %d (netease: %d), skipped (already have .lrc): %d, written: %d\n",
		total, matched, neteaseHits, skipped, written)
}

// ---------------------------------------------------------------------------
// NetEase Cloud Music API
// ---------------------------------------------------------------------------

var httpClient = &http.Client{Timeout: 10 * time.Second}

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

func fetchNeteaseLyrics(artist, title string, durationSecs float64) (string, error) {
	// Rate limit: be polite to the API
	time.Sleep(200 * time.Millisecond)

	// Search for the song
	query := artist + " " + title
	resp, err := httpClient.PostForm("https://music.163.com/api/search/get", url.Values{
		"s":      {query},
		"type":   {"1"},
		"limit":  {"5"},
		"offset": {"0"},
	})
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()

	var search neteaseSearchResult
	if err := json.NewDecoder(resp.Body).Decode(&search); err != nil {
		return "", err
	}

	// Find best match: title must match (case-insensitive), duration within 3s
	var songID int
	titleLower := strings.ToLower(title)
	for _, s := range search.Result.Songs {
		if strings.ToLower(s.Name) != titleLower {
			continue
		}
		sDur := float64(s.Duration) / 1000.0
		if math.Abs(sDur-durationSecs) < 3 {
			songID = s.ID
			break
		}
	}
	if songID == 0 {
		return "", fmt.Errorf("no match")
	}

	// Fetch lyrics
	lyrResp, err := httpClient.Get(fmt.Sprintf("https://music.163.com/api/song/lyric?os=osx&id=%d&lv=-1&kv=-1&tv=-1", songID))
	if err != nil {
		return "", err
	}
	defer lyrResp.Body.Close()

	var lyric neteaseLyricResult
	if err := json.NewDecoder(lyrResp.Body).Decode(&lyric); err != nil {
		return "", err
	}

	lrc := strings.TrimSpace(lyric.LRC.Lyric)
	if lrc == "" {
		return "", fmt.Errorf("no lyrics")
	}

	// Filter out metadata-only "lyrics" (just [ti:] [ar:] tags with no timed lines)
	hasTimedLine := false
	for _, line := range strings.Split(lrc, "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		if !strings.HasPrefix(line, "[") {
			continue
		}
		// Skip metadata tags like [ti:], [ar:], [al:], [by:], [offset:]
		if len(line) > 1 {
			inner := line[1:]
			if idx := strings.Index(inner, "]"); idx > 0 {
				tag := inner[:idx]
				if strings.Contains(tag, ":") && !strings.ContainsAny(tag[:strings.Index(tag, ":")], "0123456789") {
					continue
				}
			}
		}
		hasTimedLine = true
		break
	}
	if !hasTimedLine {
		return "", fmt.Errorf("metadata only")
	}

	return lrc, nil
}
