//go:build linux

package main

import (
	"os"
	"syscall"
	"unsafe"
)

// stdinIsTerminal reports whether stdin is an interactive terminal. The
// termios ioctl succeeds only on ttys — not on /dev/null, pipes, or files.
func stdinIsTerminal() bool {
	var termios syscall.Termios
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, os.Stdin.Fd(),
		syscall.TCGETS, uintptr(unsafe.Pointer(&termios)))
	return errno == 0
}
