//go:build !darwin

package main

func listCoreAudioOutputDevices() ([]coreAudioOutputDevice, error) {
	return nil, nil
}
