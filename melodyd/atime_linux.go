//go:build linux

package main

import (
	"os"
	"syscall"
	"time"
)

// fileATime returns a file's last-access time, used for LRU eviction of the
// transcode cache. Access time is read-only (unlike bumping mtime, it doesn't
// interfere with the cache freshness check against the source file).
func fileATime(info os.FileInfo) time.Time {
	if st, ok := info.Sys().(*syscall.Stat_t); ok {
		return time.Unix(st.Atim.Sec, st.Atim.Nsec)
	}
	return info.ModTime()
}
