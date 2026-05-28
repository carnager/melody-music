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
	"sync"
	"sync/atomic"
	"time"

	_ "modernc.org/sqlite"
)

func main() {
	melodyDB := flag.String("db", "", "path to melodyd's melody.db")
	lrclibDB := flag.String("lrclib", "", "path to lrclib SQLite dump (optional with -netease)")
	netease := flag.Bool("netease", false, "fetch missing lyrics from NetEase Cloud Music")
	dryRun := flag.Bool("dry-run", false, "print matches without writing files")
	verbose := flag.Bool("verbose", false, "show detailed match/mismatch info for NetEase queries")
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

	type trackInfo struct {
		path, artist, title, album string
		duration                   float64
		lrcPath                    string
	}

	var total, skipped int
	var matched, written, neteaseHits atomic.Int64

	// First pass: lrclib lookups (fast, local) and collect netease work
	var neteaseWork []trackInfo
	for rows.Next() {
		var t trackInfo
		if err := rows.Scan(&t.path, &t.artist, &t.title, &t.duration, &t.album); err != nil {
			log.Printf("scan: %v", err)
			continue
		}
		total++

		t.lrcPath = strings.TrimSuffix(t.path, filepath.Ext(t.path)) + ".lrc"
		if _, err := os.Stat(t.lrcPath); err == nil {
			skipped++
			continue
		}

		// Try lrclib first
		var lyrics, kind string
		if lrclibStmt != nil {
			var syncedLyrics, plainLyrics sql.NullString
			err := lrclibStmt.QueryRow(
				strings.ToLower(t.artist),
				strings.ToLower(t.title),
				strings.ToLower(t.album),
				t.duration,
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

		if lyrics != "" {
			matched.Add(1)
			fmt.Printf("[%s] %s - %s (%s)\n", kind, t.artist, t.title, t.album)
			if !*dryRun {
				if err := os.WriteFile(t.lrcPath, []byte(lyrics), 0o644); err != nil {
					log.Printf("write %s: %v", t.lrcPath, err)
				} else {
					written.Add(1)
				}
			}
			continue
		}

		if *netease {
			neteaseWork = append(neteaseWork, t)
		}
	}
	if err := rows.Err(); err != nil {
		log.Fatalf("rows: %v", err)
	}

	fmt.Printf("lrclib pass done: %d total, %d matched, %d skipped, %d pending netease\n",
		total, matched.Load(), skipped, len(neteaseWork))

	// Second pass: NetEase lookups with worker pool
	if len(neteaseWork) > 0 {
		const numWorkers = 10
		work := make(chan trackInfo, numWorkers*2)
		var wg sync.WaitGroup

		var processed atomic.Int64

		for i := 0; i < numWorkers; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				for t := range work {
					n := processed.Add(1)
					if n%100 == 0 {
						fmt.Printf("  [netease] progress: %d/%d queried, %d matched\n",
							n, len(neteaseWork), neteaseHits.Load())
					}
					lrc, err := fetchNeteaseLyrics(t.artist, t.title, t.duration, *verbose)
					if err != nil || lrc == "" {
						continue
					}
					neteaseHits.Add(1)
					matched.Add(1)
					fmt.Printf("[netease] %s - %s (%s)\n", t.artist, t.title, t.album)
					if !*dryRun {
						if err := os.WriteFile(t.lrcPath, []byte(lrc), 0o644); err != nil {
							log.Printf("write %s: %v", t.lrcPath, err)
						} else {
							written.Add(1)
						}
					}
				}
			}()
		}

		for _, t := range neteaseWork {
			work <- t
		}
		close(work)
		wg.Wait()
	}

	fmt.Printf("total: %d, matched: %d (netease: %d), skipped (already have .lrc): %d, written: %d\n",
		total, matched.Load(), neteaseHits.Load(), skipped, written.Load())
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

// isMetadataContent checks if a lyric line's content is just credits/metadata
// rather than actual lyrics (e.g. "作曲 : Someone", "编曲:Someone").
func isMetadataContent(s string) bool {
	lower := strings.ToLower(s)
	// Check for "Name : Value" credit pattern (common in NetEase)
	if strings.Contains(s, " : ") || strings.Contains(s, "：") {
		return true
	}
	prefixes := []string{
		// Chinese
		"作曲", "作词", "编曲", "制作", "混音", "录音", "母带", "出品", "监制",
		// English
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

func fetchNeteaseLyrics(artist, title string, durationSecs float64, verbose bool) (string, error) {
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

	if verbose {
		fmt.Printf("\n  [netease] search %q → %d results\n", query, len(search.Result.Songs))
	}

	if len(search.Result.Songs) == 0 {
		if verbose {
			fmt.Printf("  [netease] no results returned\n")
		}
		return "", fmt.Errorf("no results")
	}

	// Find best match by scoring: title must match, prefer artist match + close duration
	titleLower := strings.ToLower(title)
	titleBase := titleLower
	if idx := strings.Index(titleBase, " ("); idx > 0 {
		titleBase = titleBase[:idx]
	}
	artistLower := strings.ToLower(artist)

	type candidate struct {
		id    int
		score int // higher is better
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
		sDur := float64(s.Duration) / 1000.0
		durDiff := math.Abs(sDur - durationSecs)

		// Check if any result artist contains or is contained in our artist
		artistMatch := false
		for _, a := range s.Artists {
			aLower := strings.ToLower(a.Name)
			if aLower == artistLower || strings.Contains(artistLower, aLower) || strings.Contains(aLower, artistLower) {
				artistMatch = true
				break
			}
		}

		if verbose {
			artistStr := ""
			if len(s.Artists) > 0 {
				artistStr = s.Artists[0].Name
			}
			fmt.Printf("  [netease]   candidate: %q by %q (dur=%.1fs) title=%v artist=%v dur_diff=%.1fs\n",
				s.Name, artistStr, sDur, titleMatch, artistMatch, durDiff)
		}
		if !titleMatch {
			continue
		}

		// Score: artist match is most important, then duration closeness
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
		// Reject if no artist match and duration is way off
		if !artistMatch {
			continue
		}
		// Reject if duration is absurdly off even with artist match
		if durDiff > 30 {
			continue
		}
		if score > best.score {
			best = candidate{id: s.ID, score: score}
		}
	}
	if best.id == 0 {
		return "", fmt.Errorf("no match")
	}
	songID := best.id

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
		// Skip metadata tags like [ti:], [ar:], [al:], [by:], [offset:]
		if len(trimmed) > 1 {
			inner := trimmed[1:]
			if idx := strings.Index(inner, "]"); idx > 0 {
				tag := inner[:idx]
				if strings.Contains(tag, ":") && !strings.ContainsAny(tag[:strings.Index(tag, ":")], "0123456789") {
					continue
				}
				content := strings.TrimSpace(inner[idx+1:])
				if isMetadataContent(content) {
					continue
				}
			}
		}
		cleaned = append(cleaned, line)
		lyricLineCount++
	}
	if lyricLineCount < 3 {
		if verbose {
			fmt.Printf("  [netease]   lyrics rejected: only %d content lines (%d bytes)\n", lyricLineCount, len(lrc))
		}
		return "", fmt.Errorf("metadata only")
	}

	return strings.TrimSpace(strings.Join(cleaned, "\n")), nil
}
