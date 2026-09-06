//go:build linux

package main

import (
	"reflect"
	"strings"
	"testing"
)

func TestParseMountPaths(t *testing.T) {
	input := "1 0 0:1 / / rw - ext4 /dev/root rw\n" +
		`2 1 0:2 / /mnt/music\040share rw shared:1 - nfs4 nas:/music rw` + "\n" +
		"3 1 0:3 / /mnt/pending rw - autofs systemd-1 rw\n" +
		`4 1 0:4 / /mnt/back\134slash rw - nfs nas:/other rw` + "\n"
	got, err := parseMountPaths(strings.NewReader(input))
	want := []string{"/", "/mnt/music share", `/mnt/back\slash`}
	if err != nil || !reflect.DeepEqual(got, want) {
		t.Fatalf("mounts = %q, err = %v; want %q", got, err, want)
	}
	if _, err := parseMountPaths(strings.NewReader("invalid\n")); err == nil {
		t.Fatal("malformed mount table accepted")
	}
}
