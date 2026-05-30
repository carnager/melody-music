//go:build !linux

package player

import "os/exec"

func configureCommand(cmd *exec.Cmd) {
}
