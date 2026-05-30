package main

import (
	"testing"
	"time"
)

func TestPlChangesNeedsFull(t *testing.T) {
	tests := []struct {
		name       string
		clientVer  int
		currentVer int
		want       bool
	}{
		{name: "zero requests full playlist", clientVer: 0, currentVer: 7, want: true},
		{name: "older client version requests full playlist", clientVer: 6, currentVer: 7, want: true},
		{name: "same version has no changes", clientVer: 7, currentVer: 7, want: false},
		{name: "newer client version requests full playlist", clientVer: 8, currentVer: 7, want: true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := plChangesNeedsFull(tt.clientVer, tt.currentVer); got != tt.want {
				t.Fatalf("plChangesNeedsFull(%d, %d) = %v, want %v", tt.clientVer, tt.currentVer, got, tt.want)
			}
		})
	}
}

func TestBumpQueueVersionLocked(t *testing.T) {
	a := &app{}
	before := int(time.Now().Unix())
	a.bumpQueueVersionLocked()
	after := int(time.Now().Unix())

	if a.queueVersion < before || a.queueVersion > after {
		t.Fatalf("queueVersion = %d, want between %d and %d", a.queueVersion, before, after)
	}

	a.queueVersion = after + 100
	a.bumpQueueVersionLocked()
	if a.queueVersion != after+101 {
		t.Fatalf("future queueVersion = %d, want %d", a.queueVersion, after+101)
	}
}
