package main

import "testing"

func TestCastInfoFields(t *testing.T) {
	info := castInfoFields([]string{
		"id=abc123",
		"fn=Living\\032Room",
		"md=SHIELD Android TV",
		"bad",
	})

	if info["id"] != "abc123" {
		t.Fatalf("id = %q", info["id"])
	}
	if info["fn"] != "Living Room" {
		t.Fatalf("fn = %q", info["fn"])
	}
	if info["md"] != "SHIELD Android TV" {
		t.Fatalf("md = %q", info["md"])
	}
	if _, ok := info["bad"]; ok {
		t.Fatalf("malformed field should be ignored")
	}
}
