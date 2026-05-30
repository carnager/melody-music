package player

import "testing"

func TestNewClose(t *testing.T) {
	p := New()
	p.Close()
}
