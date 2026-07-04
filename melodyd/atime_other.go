//go:build !linux

package main

import (
	"os"
	"time"
)

// fileATime falls back to modification time on platforms where access time is
// not readily available; melodyd's server runs on Linux in practice.
func fileATime(info os.FileInfo) time.Time {
	return info.ModTime()
}
