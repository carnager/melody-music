//go:build linux

package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"strings"
)

func mountedPaths() ([]string, error) {
	f, err := os.Open("/proc/self/mountinfo")
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return parseMountPaths(f)
}

func parseMountPaths(r io.Reader) ([]string, error) {
	var paths []string
	s := bufio.NewScanner(r)
	s.Buffer(make([]byte, 4096), 1024*1024)
	unescape := strings.NewReplacer(`\040`, " ", `\011`, "\t", `\012`, "\n", `\134`, `\`)
	for s.Scan() {
		parts := strings.SplitN(s.Text(), " - ", 2)
		if len(parts) != 2 {
			return nil, fmt.Errorf("invalid mountinfo record")
		}
		fields, fs := strings.Fields(parts[0]), strings.Fields(parts[1])
		if len(fields) < 6 || len(fs) < 3 {
			return nil, fmt.Errorf("invalid mountinfo fields")
		}
		if fs[0] == "autofs" {
			continue // An automount placeholder is not an available filesystem.
		}
		paths = append(paths, unescape.Replace(fields[4]))
	}
	return paths, s.Err()
}
