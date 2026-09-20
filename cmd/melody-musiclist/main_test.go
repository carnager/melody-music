package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestUploadRequiresExplicitDestination(t *testing.T) {
	for _, tc := range []struct {
		name, upload string
		valid        bool
	}{
		{"first run", "", false},
		{"host only", "[upload]\nhost = 'web'\n", false},
		{"path only", "[upload]\npath = '/srv/list'\n", false},
		{"blank host", "[upload]\nhost = '  '\npath = '/srv/list'\n", false},
		{"configured", "[upload]\nhost = 'my-web-host'\npath = '/srv/list'\n", true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			root := t.TempDir()
			t.Setenv("XDG_CONFIG_HOME", root)
			t.Setenv("MPD_HOST", "")
			t.Setenv("MPD_PORT", "")
			path := filepath.Join(root, "melody", "melody-musiclist.toml")
			if tc.upload != "" {
				if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(path, []byte(tc.upload), 0600); err != nil {
					t.Fatal(err)
				}
			}
			cfg, err := loadConfig()
			if tc.valid {
				if err != nil {
					t.Fatal(err)
				}
				if cfg.Upload.Host != "my-web-host" || cfg.Upload.Path != "/srv/list" {
					t.Fatalf("destination changed: %+v", cfg.Upload)
				}
			} else if err == nil || !strings.Contains(err.Error(), path) || !strings.Contains(err.Error(), "[upload]") {
				t.Fatalf("expected actionable configuration error, got %v", err)
			}
			data, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			if tc.upload != "" && string(data) != tc.upload {
				t.Fatal("existing config was rewritten")
			}
			if tc.upload == "" && (strings.Contains(string(data), "proteus") || strings.Contains(string(data), "/srv/http/list")) {
				t.Fatal("personal destination in generated config")
			}
		})
	}
}
