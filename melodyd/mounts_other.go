//go:build !linux

package main

// Automatic mount discovery is currently Linux-only. Directory access checks
// still apply elsewhere; configured required mounts fail closed.
func mountedPaths() ([]string, error) {
	return nil, nil
}
