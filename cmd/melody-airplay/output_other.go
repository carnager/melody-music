//go:build !darwin

package main

import "fmt"

func selectOutputDevice(name string) error {
	if name == "" {
		return nil
	}
	return fmt.Errorf("selecting AirPlay/CoreAudio output devices is only supported on macOS")
}
