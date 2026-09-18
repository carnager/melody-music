package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestExpandTilde(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	cases := map[string]string{
		"~":          home,
		"~/Music":    filepath.Join(home, "Music"),
		"/abs/path":  "/abs/path",
		"relative":   "relative",
		"~elsewhere": "~elsewhere",
		"":           "",
	}
	for input, want := range cases {
		if got := expandTilde(input); got != want {
			t.Fatalf("expandTilde(%q) = %q, want %q", input, got, want)
		}
	}
}

func TestWizardFreshDefaults(t *testing.T) {
	musicDir := t.TempDir()
	var out strings.Builder
	// Answer the music directory, accept every other default.
	raw, err := runWizard(strings.NewReader(musicDir+"\n\n\n\n\n"), &out, nil)
	if err != nil {
		t.Fatalf("runWizard: %v", err)
	}
	library := configSection(raw, "library")
	if got := stringify(library["music_dir"]); got != musicDir {
		t.Fatalf("music_dir = %q, want %q", got, musicDir)
	}
	if got := intFromAny(configSection(raw, "mpd")["port"], 0); got != 6600 {
		t.Fatalf("mpd port = %d, want 6600", got)
	}
	server := configSection(raw, "server")
	binds := stringSlice(server["bind_to_address"])
	if len(binds) != 2 || binds[0] != "0.0.0.0:6701" || !strings.HasPrefix(binds[1], "/") {
		t.Fatalf("bind_to_address = %v", binds)
	}
	if got := stringify(server["web_secret"]); got != "" {
		t.Fatalf("web_secret = %q, want empty", got)
	}
	hostname, _ := os.Hostname()
	if got := stringify(server["name"]); got != hostname || got == "" {
		t.Fatalf("server name = %q, want hostname %q", got, hostname)
	}
	// The untouched skeleton keys survive the seed.
	if _, ok := configSection(raw, "random")["tracks"]; !ok {
		t.Fatalf("skeleton [random] section lost: %v", raw)
	}
}

func TestWizardRepromptsOnMissingDir(t *testing.T) {
	musicDir := t.TempDir()
	missing := filepath.Join(musicDir, "does-not-exist")
	var out strings.Builder
	raw, err := runWizard(strings.NewReader(missing+"\n"+musicDir+"\n\n\n\n\n"), &out, nil)
	if err != nil {
		t.Fatalf("runWizard: %v", err)
	}
	if !strings.Contains(out.String(), "does not exist or is not a directory") {
		t.Fatalf("expected re-prompt message, got:\n%s", out.String())
	}
	if got := stringify(configSection(raw, "library")["music_dir"]); got != musicDir {
		t.Fatalf("music_dir = %q, want %q", got, musicDir)
	}
}

func TestWizardEOFAborts(t *testing.T) {
	var out strings.Builder
	if _, err := runWizard(strings.NewReader(""), &out, nil); err == nil {
		t.Fatalf("closed stdin must abort the wizard")
	}
}

func TestWizardReRunPreservesUnknownKeys(t *testing.T) {
	musicDir := t.TempDir()
	existing := map[string]any{
		"server": map[string]any{
			"name":     "Old Name",
			"base_url": "https://music.example",
		},
		"library": map[string]any{
			"music_dir":       musicDir,
			"required_mounts": []any{"/mnt/nas"},
		},
		"transcode": map[string]any{"cache_max_mb": int64(1024)},
	}
	var out strings.Builder
	// Accept every default: existing values must round-trip unchanged.
	raw, err := runWizard(strings.NewReader("\n\n\n\n\n"), &out, existing)
	if err != nil {
		t.Fatalf("runWizard: %v", err)
	}
	if got := stringify(configSection(raw, "server")["base_url"]); got != "https://music.example" {
		t.Fatalf("base_url lost: %q", got)
	}
	if got := stringify(configSection(raw, "server")["name"]); got != "Old Name" {
		t.Fatalf("name = %q, want Old Name", got)
	}
	if mounts := stringSlice(configSection(raw, "library")["required_mounts"]); len(mounts) != 1 ||
		mounts[0] != "/mnt/nas" {
		t.Fatalf("required_mounts lost: %v", mounts)
	}

	// And they survive the write→read round trip on disk.
	configPath := filepath.Join(t.TempDir(), "melody", "melodyd.toml")
	if err := writeSetupConfig(configPath, raw); err != nil {
		t.Fatalf("writeSetupConfig: %v", err)
	}
	cfg, reread, exists, err := readConfigFile(configPath)
	if err != nil || !exists {
		t.Fatalf("readConfigFile: exists=%v err=%v", exists, err)
	}
	if cfg.Library.MusicDir != musicDir || cfg.Server.BaseURL != "https://music.example" ||
		len(cfg.Library.RequiredMounts) != 1 {
		t.Fatalf("round-tripped config = %+v", cfg)
	}
	if got := intFromAny(configSection(reread, "transcode")["cache_max_mb"], 0); got != 1024 {
		t.Fatalf("transcode cache_max_mb lost: %d", got)
	}
}

func TestWriteSetupConfigBacksUpExisting(t *testing.T) {
	dir := t.TempDir()
	configPath := filepath.Join(dir, "melodyd.toml")
	if err := os.WriteFile(configPath, []byte("# old\n[library]\nmusic_dir = \"/x\"\n"),
		0o644); err != nil {
		t.Fatalf("seed config: %v", err)
	}
	raw := map[string]any{"library": map[string]any{"music_dir": "/y"}}
	if err := writeSetupConfig(configPath, raw); err != nil {
		t.Fatalf("writeSetupConfig: %v", err)
	}
	backup, err := os.ReadFile(configPath + ".bak")
	if err != nil || !strings.Contains(string(backup), "# old") {
		t.Fatalf("backup missing or wrong: %v %q", err, backup)
	}
	written, _ := os.ReadFile(configPath)
	if !strings.Contains(string(written), "melodyd setup") ||
		!strings.Contains(string(written), "music_dir = \"/y\"") {
		t.Fatalf("written config unexpected:\n%s", written)
	}
}

func TestReadConfigFileMissing(t *testing.T) {
	configPath := filepath.Join(t.TempDir(), "melodyd.toml")
	cfg, raw, exists, err := readConfigFile(configPath)
	if err != nil {
		t.Fatalf("missing file must not error: %v", err)
	}
	if exists || raw != nil {
		t.Fatalf("exists=%v raw=%v, want false/nil", exists, raw)
	}
	// No file is silently created anymore, and defaults still apply.
	if _, statErr := os.Stat(configPath); statErr == nil {
		t.Fatalf("readConfigFile must not write a config file")
	}
	if cfg.MPD.Port != 6600 || cfg.Player.MPVPath != "mpv" {
		t.Fatalf("defaults not applied: %+v", cfg)
	}
}

func TestReadConfigFileExpandsTilde(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	configPath := filepath.Join(t.TempDir(), "melodyd.toml")
	if err := os.WriteFile(configPath, []byte("[library]\nmusic_dir = \"~/Music\"\n"),
		0o644); err != nil {
		t.Fatalf("write config: %v", err)
	}
	cfg, _, _, err := readConfigFile(configPath)
	if err != nil {
		t.Fatalf("readConfigFile: %v", err)
	}
	if want := filepath.Join(home, "Music"); cfg.Library.MusicDir != want {
		t.Fatalf("music_dir = %q, want %q", cfg.Library.MusicDir, want)
	}
}
