//go:build !linux && !darwin

package main

import "os"

// stdinIsTerminal falls back to the character-device heuristic on
// platforms without a known termios ioctl. Imperfect (/dev/null also
// matches), but those platforms do not run melodyd under systemd.
func stdinIsTerminal() bool {
	info, err := os.Stdin.Stat()
	return err == nil && info.Mode()&os.ModeCharDevice != 0
}
