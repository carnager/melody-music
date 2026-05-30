//go:build darwin

package main

/*
#cgo darwin LDFLAGS: -framework CoreAudio -framework CoreFoundation
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>

static OSStatus stringProperty(AudioDeviceID id, AudioObjectPropertySelector selector, char *buf, UInt32 bufSize) {
	AudioObjectPropertyAddress addr = {
		selector,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain,
	};
	CFStringRef value = NULL;
	UInt32 size = sizeof(value);
	OSStatus st = AudioObjectGetPropertyData(id, &addr, 0, NULL, &size, &value);
	if (st != noErr || value == NULL) {
		return st;
	}
	Boolean ok = CFStringGetCString(value, buf, bufSize, kCFStringEncodingUTF8);
	CFRelease(value);
	return ok ? noErr : -1;
}

static OSStatus deviceName(AudioDeviceID id, char *buf, UInt32 bufSize) {
	return stringProperty(id, kAudioObjectPropertyName, buf, bufSize);
}

static OSStatus deviceUID(AudioDeviceID id, char *buf, UInt32 bufSize) {
	return stringProperty(id, kAudioDevicePropertyDeviceUID, buf, bufSize);
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
*/
import "C"

import (
	"fmt"
	"unsafe"
)

func listCoreAudioOutputDevices() ([]coreAudioOutputDevice, error) {
	addr := C.AudioObjectPropertyAddress{
		mSelector: C.kAudioHardwarePropertyDevices,
		mScope:    C.kAudioObjectPropertyScopeGlobal,
		mElement:  C.kAudioObjectPropertyElementMain,
	}
	var size C.UInt32
	if st := C.AudioObjectGetPropertyDataSize(C.kAudioObjectSystemObject, &addr, 0, nil, &size); st != C.noErr {
		return nil, fmt.Errorf("list devices: CoreAudio status %d", int(st))
	}

	count := int(size) / int(unsafe.Sizeof(C.AudioDeviceID(0)))
	if count == 0 {
		return nil, nil
	}

	devices := make([]C.AudioDeviceID, count)
	if st := C.AudioObjectGetPropertyData(C.kAudioObjectSystemObject, &addr, 0, nil, &size, unsafe.Pointer(&devices[0])); st != C.noErr {
		return nil, fmt.Errorf("read devices: CoreAudio status %d", int(st))
	}

	outputs := make([]coreAudioOutputDevice, 0, count)
	for _, id := range devices {
		if C.hasOutput(id) == 0 {
			continue
		}

		nameBuf := make([]C.char, 512)
		if st := C.deviceName(id, &nameBuf[0], C.UInt32(len(nameBuf))); st != C.noErr {
			continue
		}
		name := C.GoString(&nameBuf[0])
		if name == "" {
			continue
		}

		uidBuf := make([]C.char, 1024)
		if st := C.deviceUID(id, &uidBuf[0], C.UInt32(len(uidBuf))); st != C.noErr {
			continue
		}
		uid := C.GoString(&uidBuf[0])
		if uid == "" {
			continue
		}

		outputs = append(outputs, coreAudioOutputDevice{
			ID:        coreAudioDeviceID(uid, name),
			Name:      name,
			MPVDevice: "coreaudio/" + uid,
		})
	}
	return outputs, nil
}
