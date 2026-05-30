package main

import "strings"

func sanitizeDeviceID(s string) string {
	s = strings.TrimPrefix(s, "uuid:")
	var b strings.Builder
	for _, r := range strings.ToLower(s) {
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '-' || r == '_' {
			b.WriteRune(r)
		}
	}
	if b.Len() == 0 {
		return "device"
	}
	return b.String()
}
