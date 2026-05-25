package main

import (
	"database/sql"
	"flag"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"

	_ "modernc.org/sqlite"
)

func main() {
	melodyDB := flag.String("db", "", "path to melodyd's melody.db")
	lrclibDB := flag.String("lrclib", "", "path to lrclib SQLite dump")
	dryRun := flag.Bool("dry-run", false, "print matches without writing files")
	flag.Parse()

	if *melodyDB == "" || *lrclibDB == "" {
		fmt.Fprintln(os.Stderr, "Usage: melody-lrcmatch -db <melody.db> -lrclib <lrclib.sqlite3> [-dry-run]")
		os.Exit(1)
	}

	mdb, err := sql.Open("sqlite", *melodyDB+"?mode=ro")
	if err != nil {
		log.Fatalf("open melody db: %v", err)
	}
	defer mdb.Close()

	ldb, err := sql.Open("sqlite", *lrclibDB+"?mode=ro")
	if err != nil {
		log.Fatalf("open lrclib db: %v", err)
	}
	defer ldb.Close()

	// Prepare lrclib lookup query — strict match on artist, title, album, duration (±2s)
	lrclibStmt, err := ldb.Prepare(`
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

	rows, err := mdb.Query(`SELECT t.path, t.artist, t.title, t.duration, a.title AS album_title
		FROM tracks t
		JOIN albums a ON a.id = t.album_id`)
	if err != nil {
		log.Fatalf("query melody tracks: %v", err)
	}
	defer rows.Close()

	var total, matched, skipped, written int

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

		// Query lrclib dump
		var syncedLyrics, plainLyrics sql.NullString
		err := lrclibStmt.QueryRow(
			strings.ToLower(artist),
			strings.ToLower(title),
			strings.ToLower(album),
			duration,
		).Scan(&syncedLyrics, &plainLyrics)
		if err != nil {
			continue // no match
		}

		// Prefer synced lyrics
		var lyrics, kind string
		if syncedLyrics.String != "" {
			lyrics = syncedLyrics.String
			kind = "synced"
		} else if plainLyrics.String != "" {
			lyrics = plainLyrics.String
			kind = "plain"
		}
		if lyrics == "" {
			continue
		}

		matched++

		if *dryRun {
			fmt.Printf("[%s] %s - %s (%s)\n", kind, artist, title, album)
			continue
		}

		if err := os.WriteFile(lrcPath, []byte(lyrics), 0o644); err != nil {
			log.Printf("write %s: %v", lrcPath, err)
			continue
		}
		written++

		if written%100 == 0 {
			fmt.Printf("progress: %d written...\n", written)
		}
	}

	if err := rows.Err(); err != nil {
		log.Fatalf("rows: %v", err)
	}

	fmt.Printf("total: %d, matched: %d, skipped (already have .lrc): %d, written: %d\n",
		total, matched, skipped, written)
}
