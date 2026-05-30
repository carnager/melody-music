//go:build darwin

package main

/*
#cgo darwin LDFLAGS: -framework CoreAudio -framework CoreFoundation
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <string.h>

static OSStatus deviceName(AudioDeviceID id, char *buf, UInt32 bufSize) {
	AudioObjectPropertyAddress addr = {
		kAudioObjectPropertyName,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain,
	};
	CFStringRef name = NULL;
	UInt32 size = sizeof(name);
	OSStatus st = AudioObjectGetPropertyData(id, &addr, 0, NULL, &size, &name);
	if (st != noErr || name == NULL) {
		return st;
	}
	Boolean ok = CFStringGetCString(name, buf, bufSize, kCFStringEncodingUTF8);
	CFRelease(name);
	return ok ? noErr : -1;
}

static int hasOutput(AudioDeviceID id) {
	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyStreams,
		kAudioDevicePropertyScopeOutput,
		kAudioObjectPropertyElementMain,
	};
	UInt32 size = 0;
	OSStatus st = AudioObjectGetPropertyDataSize(id, &addr, 0, NULL, &size);
	return st == noErr && size > 0;
}

static OSStatus setDefaultOutput(AudioDeviceID id) {
	AudioObjectPropertyAddress addr = {
		kAudioHardwarePropertyDefaultOutputDevice,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain,
	};
	UInt32 size = sizeof(id);
	OSStatus st = AudioObjectSetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, size, &id);
	if (st != noErr) {
		return st;
	}
	addr.mSelector = kAudioHardwarePropertyDefaultSystemOutputDevice;
	return AudioObjectSetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, size, &id);
}
*/
import "C"

import (
	"fmt"
	"strings"
	"unsafe"
)

func selectOutputDevice(name string) error {
	name = strings.TrimSpace(name)
	if name == "" {
		return nil
	}

	addr := C.AudioObjectPropertyAddress{
		mSelector: C.kAudioHardwarePropertyDevices,
		mScope:    C.kAudioObjectPropertyScopeGlobal,
		mElement:  C.kAudioObjectPropertyElementMain,
	}
	var size C.UInt32
	if st := C.AudioObjectGetPropertyDataSize(C.kAudioObjectSystemObject, &addr, 0, nil, &size); st != C.noErr {
		return fmt.Errorf("list devices: CoreAudio status %d", int(st))
	}

	count := int(size) / int(unsafe.Sizeof(C.AudioDeviceID(0)))
	devices := make([]C.AudioDeviceID, count)
	if count == 0 {
		return fmt.Errorf("no CoreAudio output devices found")
	}
	if st := C.AudioObjectGetPropertyData(C.kAudioObjectSystemObject, &addr, 0, nil, &size, unsafe.Pointer(&devices[0])); st != C.noErr {
		return fmt.Errorf("read devices: CoreAudio status %d", int(st))
	}

	var names []string
	for _, id := range devices {
		if C.hasOutput(id) == 0 {
			continue
		}
		buf := make([]C.char, 512)
		if st := C.deviceName(id, &buf[0], C.UInt32(len(buf))); st != C.noErr {
			continue
		}
		devName := C.GoString(&buf[0])
		names = append(names, devName)
		if strings.EqualFold(devName, name) || strings.Contains(strings.ToLower(devName), strings.ToLower(name)) {
			if st := C.setDefaultOutput(id); st != C.noErr {
				return fmt.Errorf("set default output: CoreAudio status %d", int(st))
			}
			return nil
		}
	}
	return fmt.Errorf("output device %q not found; available outputs: %s", name, strings.Join(names, ", "))
}
