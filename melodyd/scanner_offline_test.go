package main

import (
	"fmt"
	"io"
	"log"
	"os"
	"path/filepath"
	"testing"
)

func offlineTestScanner(t *testing.T) (*scanner, string) {
	t.Helper()
	root := filepath.Join(t.TempDir(), "music")
	writeFile(t, filepath.Join(root, "Artist", "Album", "01.flac"))
	writeFile(t, filepath.Join(root, "Artist", "Album", "02.flac"))
	db, err := openMusicDB(filepath.Join(t.TempDir(), "music.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.close() })
	s := newScanner(root, db, log.New(io.Discard, "", 0), "")
	if err := s.fullScan(); err != nil {
		t.Fatal(err)
	}
	return s, root
}

func assertOfflineTracks(t *testing.T, s *scanner, want int) {
	t.Helper()
	n, err := s.db.trackCount()
	if err != nil || n != want {
		t.Fatalf("track count = %d, err = %v; want %d", n, err, want)
	}
}

func TestFullScanMissingRootPreservesLibrary(t *testing.T) {
	s, root := offlineTestScanner(t)
	if err := os.Rename(root, root+"-offline"); err != nil {
		t.Fatal(err)
	}
	updated := s.lastUpdateTime()
	s.onScanComplete = func(bool) { t.Error("failed scan notified completion") }
	if err := s.fullScan(); err == nil {
		t.Fatal("missing root scan succeeded")
	}
	assertOfflineTracks(t, s, 2)
	if s.isScanning() || s.lastUpdateTime() != updated {
		t.Fatal("failed scan left incorrect update state")
	}
}

func TestScanUnreadableDirectoryPreservesLibrary(t *testing.T) {
	for _, uri := range []string{"", "Artist", "Artist/Album", "Artist/Album/01.flac"} {
		t.Run("update_"+uri, func(t *testing.T) {
			s, root := offlineTestScanner(t)
			album := filepath.Join(root, "Artist", "Album")
			if err := os.Chmod(album, 0); err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { os.Chmod(album, 0o755) })
			if _, err := os.ReadDir(album); err == nil {
				t.Skip("directory permissions are not enforced for this user")
			}
			scan := func() error {
				if uri == "" {
					return s.fullScan()
				}
				return s.scanPath(uri)
			}
			if err := scan(); err == nil {
				t.Fatal("unreadable directory scan succeeded")
			}
			assertOfflineTracks(t, s, 2)
			if err := os.Chmod(album, 0o755); err != nil {
				t.Fatal(err)
			}
			if err := os.Remove(filepath.Join(album, "01.flac")); err != nil {
				t.Fatal(err)
			}
			if err := scan(); err != nil {
				t.Fatalf("scan after recovery: %v", err)
			}
			assertOfflineTracks(t, s, 1)
		})
	}
}

func TestOfflineMountPreservesLibraryAcrossRestart(t *testing.T) {
	for _, uri := range []string{"", ".", "Artist", "Artist/Album"} {
		t.Run("update_"+uri, func(t *testing.T) {
			s, root := offlineTestScanner(t)
			// Model the mount table without requiring a real mount or privileges.
			if _, err := s.db.db.Exec(`DELETE FROM library_mounts`); err != nil {
				t.Fatal(err)
			}
			s.readMounts = func() ([]string, error) { return []string{"/", root}, nil }
			if err := s.fullScan(); err != nil {
				t.Fatal(err)
			}
			originalID, err := s.db.trackIDByPath(filepath.Join(root, "Artist", "Album", "01.flac"))
			if err != nil {
				t.Fatal(err)
			}
			if err := os.Rename(root, root+"-offline"); err != nil {
				t.Fatal(err)
			}
			if err := os.Mkdir(root, 0o755); err != nil {
				t.Fatal(err)
			}
			// A new scanner has no in-memory knowledge of the missing mount.
			s = newScanner(root, s.db, s.logger, "")
			s.readMounts = func() ([]string, error) { return []string{"/"}, nil }
			s.onScanComplete = func(bool) { t.Error("offline scan notified completion") }
			scan := func() error {
				if uri == "" {
					return s.fullScan()
				}
				return s.scanPath(uri)
			}
			if err := scan(); err == nil {
				t.Fatal("offline scan succeeded")
			}
			assertOfflineTracks(t, s, 2)
			if s.isScanning() || s.lastUpdateTime() != 0 {
				t.Fatal("offline scan left incorrect update state")
			}
			// Remount and delete one real file: cleanup should work again and
			// preserve the surviving track's identity.
			if err := os.Remove(root); err != nil {
				t.Fatal(err)
			}
			if err := os.Rename(root+"-offline", root); err != nil {
				t.Fatal(err)
			}
			if err := os.Remove(filepath.Join(root, "Artist", "Album", "02.flac")); err != nil {
				t.Fatal(err)
			}
			s.readMounts = func() ([]string, error) { return []string{"/", root}, nil }
			s.onScanComplete = nil
			if err := scan(); err != nil {
				t.Fatalf("scan after remount: %v", err)
			}
			assertOfflineTracks(t, s, 1)
			id, err := s.db.trackIDByPath(filepath.Join(root, "Artist", "Album", "01.flac"))
			if err != nil || id != originalID {
				t.Fatalf("surviving track changed identity: id=%d, err=%v", id, err)
			}
		})
	}
}

func TestNestedMountDisappearsDuringScan(t *testing.T) {
	for _, uri := range []string{"", "Artist", "Artist/Album"} {
		t.Run("update_"+uri, func(t *testing.T) {
			s, root := offlineTestScanner(t)
			if _, err := s.db.db.Exec(`DELETE FROM library_mounts`); err != nil {
				t.Fatal(err)
			}
			mount := filepath.Join(root, "Artist")
			s.readMounts = func() ([]string, error) { return []string{"/", mount}, nil }
			if err := s.fullScan(); err != nil {
				t.Fatal(err)
			}
			if err := os.Remove(filepath.Join(mount, "Album", "02.flac")); err != nil {
				t.Fatal(err)
			}
			calls := 0
			s.readMounts = func() ([]string, error) {
				calls++
				if calls == 1 {
					return []string{"/", mount}, nil
				}
				return []string{"/"}, nil
			}
			var err error
			if uri == "" {
				err = s.fullScan()
			} else {
				err = s.scanPath(uri)
			}
			if err == nil {
				t.Fatal("scan succeeded after nested mount disappeared")
			}
			assertOfflineTracks(t, s, 2)
		})
	}
}

func TestEmptyRootWithoutMountHistoryPreservesLibrary(t *testing.T) {
	for _, uri := range []string{"", "Artist"} {
		t.Run("update_"+uri, func(t *testing.T) {
			s, root := offlineTestScanner(t)
			if _, err := s.db.db.Exec(`DELETE FROM library_mounts`); err != nil {
				t.Fatal(err)
			}
			if err := os.Rename(root, root+"-offline"); err != nil {
				t.Fatal(err)
			}
			if err := os.Mkdir(root, 0o755); err != nil {
				t.Fatal(err)
			}
			var err error
			if uri == "" {
				err = s.fullScan()
			} else {
				err = s.scanPath(uri)
			}
			if err == nil {
				t.Fatal("empty root without mount history was accepted")
			}
			assertOfflineTracks(t, s, 2)
		})
	}
}

func TestRequiredMountProtectsFirstScan(t *testing.T) {
	s, root := offlineTestScanner(t)
	s.requiredMounts = []string{root}
	s.readMounts = func() ([]string, error) { return []string{"/"}, nil }
	if err := os.RemoveAll(filepath.Join(root, "Artist")); err != nil {
		t.Fatal(err)
	}
	if err := s.fullScan(); err == nil {
		t.Fatal("missing required mount was accepted")
	}
	assertOfflineTracks(t, s, 2)
	// An explicitly verified, genuinely empty share may remove its last tracks.
	s.readMounts = func() ([]string, error) { return []string{"/", root}, nil }
	if err := s.fullScan(); err != nil {
		t.Fatal(err)
	}
	assertOfflineTracks(t, s, 0)
}

func TestMountTableFailurePreservesLibrary(t *testing.T) {
	s, root := offlineTestScanner(t)
	if err := os.Remove(filepath.Join(root, "Artist", "Album", "02.flac")); err != nil {
		t.Fatal(err)
	}
	s.readMounts = func() ([]string, error) { return nil, fmt.Errorf("mount table unavailable") }
	if err := s.fullScan(); err == nil {
		t.Fatal("mount table failure was ignored")
	}
	assertOfflineTracks(t, s, 2)
}

func TestRequiredMountConfig(t *testing.T) {
	configDir := t.TempDir()
	t.Setenv("XDG_CONFIG_HOME", configDir)
	t.Setenv("XDG_DATA_HOME", t.TempDir())
	path := filepath.Join(configDir, "melody", "melodyd.toml")
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("[library]\nmusic_dir = '/mnt/music/flac'\nrequired_mounts = ['/mnt/music']\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	cfg, _, err := loadConfig()
	if err != nil {
		t.Fatal(err)
	}
	if len(cfg.Library.RequiredMounts) != 1 || cfg.Library.RequiredMounts[0] != "/mnt/music" {
		t.Fatalf("required_mounts not loaded: %v", cfg.Library.RequiredMounts)
	}
}
