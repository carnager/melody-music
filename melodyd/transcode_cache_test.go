package main

import (
	"log"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// TestTranscodeCacheEvictsLRU verifies the size cap deletes least-recently-used
// files (by access time) down to the 90% low-water mark, and leaves the cache
// alone when under the limit.
func TestTranscodeCacheEvictsLRU(t *testing.T) {
	dir := t.TempDir()
	a := &app{logger: log.New(os.Stderr, "", 0)}
	a.paths.TranscodeCacheDir = dir
	a.cfg.Transcode.CacheMaxMB = 10 // 10 MB cap

	// Six 2 MB files = 12 MB, over the 10 MB cap. atimes ascending by index, so
	// f0 is the least-recently-used.
	base := time.Now().Add(-6 * time.Hour)
	for i := 0; i < 6; i++ {
		p := filepath.Join(dir, mkName(i))
		writeSized(t, p, 2*1024*1024)
		at := base.Add(time.Duration(i) * time.Hour)
		if err := os.Chtimes(p, at, at); err != nil {
			t.Fatalf("chtimes: %v", err)
		}
	}

	a.maybeEvictTranscodeCache()

	// Target low-water mark is 90% of 10 MB = 9 MB, so it must drop to <= 9 MB.
	// Evicting f0 and f1 (2 MB each) leaves 8 MB (4 files).
	if got := cacheSizeMB(t, dir); got > 9 {
		t.Fatalf("cache not trimmed: %d MB (want <= 9)", got)
	}
	// The two oldest-accessed files must be gone; the four newest must remain.
	if exists(filepath.Join(dir, mkName(0))) || exists(filepath.Join(dir, mkName(1))) {
		t.Fatalf("LRU files were not the ones evicted")
	}
	for i := 2; i < 6; i++ {
		if !exists(filepath.Join(dir, mkName(i))) {
			t.Fatalf("recently-used file %d was wrongly evicted", i)
		}
	}
}

func TestTranscodeCacheUnlimitedAndUnderLimit(t *testing.T) {
	dir := t.TempDir()
	a := &app{logger: log.New(os.Stderr, "", 0)}
	a.paths.TranscodeCacheDir = dir

	writeSized(t, filepath.Join(dir, mkName(0)), 3*1024*1024)

	// cache_max_mb = 0 → unlimited, nothing removed.
	a.cfg.Transcode.CacheMaxMB = 0
	a.maybeEvictTranscodeCache()
	if !exists(filepath.Join(dir, mkName(0))) {
		t.Fatalf("file removed despite unlimited cache")
	}

	// Under the limit → nothing removed.
	a.cfg.Transcode.CacheMaxMB = 100
	a.maybeEvictTranscodeCache()
	if !exists(filepath.Join(dir, mkName(0))) {
		t.Fatalf("file removed despite being under the limit")
	}
}

func mkName(i int) string { return string(rune('a'+i)) + "_opus_128.opus" }

func writeSized(t *testing.T, path string, size int) {
	t.Helper()
	if err := os.WriteFile(path, make([]byte, size), 0o644); err != nil {
		t.Fatalf("write %s: %v", path, err)
	}
}

func cacheSizeMB(t *testing.T, dir string) int64 {
	t.Helper()
	entries, _ := os.ReadDir(dir)
	var total int64
	for _, e := range entries {
		if info, err := e.Info(); err == nil {
			total += info.Size()
		}
	}
	return total / (1024 * 1024)
}

func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}
