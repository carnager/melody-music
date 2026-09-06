package main

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// storageCheck captures the mounts required for a scan. Check again immediately
// before pruning: storage can disappear while metadata is being read.
type storageCheck struct {
	s        *scanner
	root     string
	expected map[string]struct{}
}

func pathUnder(path, root string) bool {
	rel, err := filepath.Rel(root, path)
	return err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(os.PathSeparator))
}

func (s *scanner) mountedPaths() (map[string]struct{}, error) {
	read := s.readMounts
	if read == nil {
		read = mountedPaths
	}
	paths, err := read()
	if err != nil {
		return nil, fmt.Errorf("read mount table (no cleanup): %w", err)
	}
	set := make(map[string]struct{}, len(paths))
	for _, path := range paths {
		set[filepath.Clean(path)] = struct{}{}
	}
	return set, nil
}

func (s *scanner) checkStorage() (*storageCheck, error) {
	root, err := filepath.Abs(s.musicDir)
	if err != nil {
		return nil, err
	}
	// Resolve symlinks so the mount table and library paths use the same names.
	root, err = filepath.EvalSymlinks(root)
	if err != nil {
		return nil, fmt.Errorf("music storage unavailable (no cleanup): %w", err)
	}
	c := &storageCheck{s: s, root: root, expected: make(map[string]struct{})}
	if len(s.requiredMounts) > 0 {
		for _, path := range s.requiredMounts {
			if !filepath.IsAbs(path) {
				return nil, fmt.Errorf("required mount must be absolute: %q", path)
			}
			c.expected[filepath.Clean(path)] = struct{}{}
		}
	} else {
		rows, err := s.db.db.Query(`SELECT mount_path FROM library_mounts WHERE music_dir = ?`, root)
		if err != nil {
			return nil, err
		}
		for rows.Next() {
			var path string
			if err := rows.Scan(&path); err != nil {
				rows.Close()
				return nil, err
			}
			c.expected[path] = struct{}{}
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return nil, err
		}
	}

	// Opening and reading the root also triggers an automount before inspecting
	// the mount table. On first upgrade, an empty mount point with an existing
	// library is ambiguous; do not accept it as our initial storage baseline.
	f, err := os.Open(root)
	if err != nil {
		return nil, fmt.Errorf("open music root (no cleanup): %w", err)
	}
	_, err = f.ReadDir(1)
	f.Close()
	if err != nil && err != io.EOF {
		return nil, fmt.Errorf("read music root (no cleanup): %w", err)
	}
	if err == io.EOF && len(c.expected) == 0 {
		n, err := s.db.trackCount()
		if err != nil {
			return nil, err
		}
		if n > 0 {
			return nil, fmt.Errorf("music root is empty and storage has not been verified; mount the share or set library.required_mounts (no cleanup)")
		}
	}
	// Access nested mounts too, so an available but dormant automount gets a
	// chance to attach before we reject its placeholder in the mount table.
	if err := c.probeMounts(); err != nil {
		return nil, err
	}
	current, err := s.mountedPaths()
	if err != nil {
		return nil, err
	}
	if err := c.checkMounts(current); err != nil {
		return nil, err
	}
	if len(s.requiredMounts) == 0 {
		// Remember the closest enclosing mount and every mount inside the library.
		// Keeping previous mounts protects an unavailable nested share too.
		var enclosing string
		for path := range current {
			if pathUnder(root, path) && len(path) > len(enclosing) {
				enclosing = path
			}
			if pathUnder(path, root) {
				c.expected[path] = struct{}{}
			}
		}
		if enclosing != "" {
			c.expected[enclosing] = struct{}{}
		}
	}
	return c, nil
}

func (c *storageCheck) checkMounts(current map[string]struct{}) error {
	for path := range c.expected {
		if _, ok := current[path]; !ok {
			return fmt.Errorf("music storage mount %q is unavailable (no cleanup)", path)
		}
	}
	return nil
}

func (c *storageCheck) verify() error {
	if err := c.probeMounts(); err != nil {
		return err
	}
	current, err := c.s.mountedPaths()
	if err != nil {
		return err
	}
	if err := c.checkMounts(current); err != nil {
		return err
	}
	// A missing root on the same filesystem must also abort deletion.
	f, err := os.Open(c.root)
	if err != nil {
		return fmt.Errorf("music storage unavailable before cleanup: %w", err)
	}
	defer f.Close()
	if _, err := f.ReadDir(1); err != nil && err != io.EOF {
		return fmt.Errorf("read music root before cleanup: %w", err)
	}
	return nil
}

func (c *storageCheck) probeMounts() error {
	for path := range c.expected {
		f, err := os.Open(path)
		if err != nil {
			return fmt.Errorf("open music mount %q (no cleanup): %w", path, err)
		}
		_, err = f.ReadDir(1)
		f.Close()
		if err != nil && err != io.EOF {
			return fmt.Errorf("read music mount %q (no cleanup): %w", path, err)
		}
	}
	return nil
}

func (c *storageCheck) remember() error {
	if len(c.s.requiredMounts) > 0 {
		return nil // Explicit configuration overrides automatic mount discovery.
	}
	for path := range c.expected {
		if _, err := c.s.db.db.Exec(`INSERT OR IGNORE INTO library_mounts(music_dir, mount_path) VALUES(?, ?)`, c.root, path); err != nil {
			return fmt.Errorf("remember library mount (no cleanup): %w", err)
		}
	}
	return nil
}
