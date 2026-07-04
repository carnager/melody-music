// melody-watcher watches a music library on the host where the files physically
// live and pushes targeted `update` commands to a remote melodyd over the MPD
// protocol.
//
// melodyd's built-in fsnotify watcher only sees changes made through the local
// kernel VFS, so it is blind to a library served from a network share. Run this
// binary ON THE FILE SERVER (e.g. TrueNAS), where inotify works, pointed at the
// local library path and the remote melodyd address. When music is added,
// changed, or removed it sends `update <subdir>` so melodyd rescans just that
// subtree — new albums appear without restarting melodyd.
//
// It is self-contained (uses fsnotify, no inotify-tools needed). Configure via
// flags or environment variables:
//
//	-music    MUSIC_DIR     local path to the music library      (required)
//	-host     MELODYD_HOST  melodyd host/IP        (default 127.0.0.1)
//	-port     MELODYD_PORT  melodyd MPD port       (default 6600)
//	-debounce DEBOUNCE      coalesce window         (default 3s)
package main

import (
	"bufio"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/fsnotify/fsnotify"
)

var audioExtensions = map[string]bool{
	".flac": true,
	".mp3":  true,
	".m4a":  true,
	".ogg":  true,
	".opus": true,
}

func main() {
	musicDir := flag.String("music", envOr("MUSIC_DIR", ""), "local path to the music library (required)")
	host := flag.String("host", envOr("MELODYD_HOST", "127.0.0.1"), "melodyd host/IP")
	port := flag.Int("port", envInt("MELODYD_PORT", 6600), "melodyd MPD port")
	debounce := flag.Duration("debounce", envDuration("DEBOUNCE", 3*time.Second), "event coalesce window")
	flag.Parse()

	logger := log.New(os.Stderr, "melody-watcher: ", log.LstdFlags)

	if *musicDir == "" {
		logger.Fatal("music directory is required (-music or MUSIC_DIR)")
	}
	root, err := filepath.Abs(filepath.Clean(*musicDir))
	if err != nil {
		logger.Fatalf("resolve music dir: %v", err)
	}
	if info, err := os.Stat(root); err != nil || !info.IsDir() {
		logger.Fatalf("music dir %q is not a directory: %v", root, err)
	}

	w := &watcher{
		root:     root,
		addr:     net.JoinHostPort(*host, strconv.Itoa(*port)),
		debounce: *debounce,
		logger:   logger,
		pending:  make(map[string]struct{}),
	}
	w.run()
}

type watcher struct {
	root     string
	addr     string
	debounce time.Duration
	logger   *log.Logger

	mu      sync.Mutex
	pending map[string]struct{} // music-root-relative URIs awaiting flush
	timer   *time.Timer
}

func (w *watcher) run() {
	fsw, err := fsnotify.NewWatcher()
	if err != nil {
		w.logger.Fatalf("fsnotify: %v", err)
	}
	defer fsw.Close()

	// Watch the whole tree (fsnotify is not recursive — add every directory).
	count := 0
	filepath.WalkDir(w.root, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if d.IsDir() {
			if addErr := fsw.Add(path); addErr == nil {
				count++
			}
		}
		return nil
	})
	w.logger.Printf("watching %s (%d dirs) → %s (debounce %s)", w.root, count, w.addr, w.debounce)

	// Verify melodyd is reachable up front (non-fatal — it may start later).
	if err := w.sendUpdate(""); err != nil {
		w.logger.Printf("warning: initial connectivity check failed: %v", err)
	} else {
		w.logger.Printf("triggered initial full update")
	}

	for {
		select {
		case event, ok := <-fsw.Events:
			if !ok {
				return
			}
			w.handleEvent(fsw, event)
		case err, ok := <-fsw.Errors:
			if !ok {
				return
			}
			w.logger.Printf("watch error: %v", err)
		}
	}
}

func (w *watcher) handleEvent(fsw *fsnotify.Watcher, event fsnotify.Event) {
	if event.Op&(fsnotify.Create|fsnotify.Write|fsnotify.Remove|fsnotify.Rename) == 0 {
		return
	}

	// Start watching newly created subdirectories so deeper changes are seen.
	if event.Op&fsnotify.Create != 0 {
		if info, err := os.Stat(event.Name); err == nil && info.IsDir() {
			_ = fsw.Add(event.Name)
		}
	}

	// Only react to audio files (or directory ops, which have no extension).
	ext := strings.ToLower(filepath.Ext(event.Name))
	isDirOp := ext == ""
	if !isDirOp && !audioExtensions[ext] {
		return
	}

	uri := w.toURI(event.Name)
	w.mu.Lock()
	w.pending[uri] = struct{}{}
	if w.timer != nil {
		w.timer.Stop()
	}
	w.timer = time.AfterFunc(w.debounce, w.flush)
	w.mu.Unlock()
}

// flush sends an update for each pending subtree once the event stream is quiet.
func (w *watcher) flush() {
	w.mu.Lock()
	uris := make([]string, 0, len(w.pending))
	for u := range w.pending {
		uris = append(uris, u)
	}
	w.pending = make(map[string]struct{})
	w.mu.Unlock()

	for _, uri := range uris {
		if err := w.sendUpdate(uri); err != nil {
			w.logger.Printf("update %q failed: %v", display(uri), err)
		} else {
			w.logger.Printf("updated %s", display(uri))
		}
	}
}

// toURI maps an absolute path to the music-root-relative directory to rescan.
// Files map to their containing directory so a whole album updates at once.
func (w *watcher) toURI(path string) string {
	abs, err := filepath.Abs(path)
	if err != nil {
		return ""
	}
	// A file event maps to its parent dir; a (possibly deleted) directory maps
	// to itself. We can't stat deleted paths, so treat anything with an audio
	// extension as a file and everything else as a directory.
	if audioExtensions[strings.ToLower(filepath.Ext(abs))] {
		abs = filepath.Dir(abs)
	}
	rel, err := filepath.Rel(w.root, abs)
	if err != nil || rel == "." || rel == ".." || strings.HasPrefix(rel, ".."+string(os.PathSeparator)) {
		return "" // outside root → full scan
	}
	return filepath.ToSlash(rel)
}

// sendUpdate issues `update <uri>` to melodyd over the MPD protocol. An empty
// uri triggers a full scan.
func (w *watcher) sendUpdate(uri string) error {
	conn, err := net.DialTimeout("tcp", w.addr, 5*time.Second)
	if err != nil {
		return err
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(10 * time.Second))

	r := bufio.NewReader(conn)
	greeting, err := r.ReadString('\n')
	if err != nil {
		return fmt.Errorf("read greeting: %w", err)
	}
	if !strings.HasPrefix(greeting, "OK MPD") {
		return fmt.Errorf("unexpected greeting: %s", strings.TrimSpace(greeting))
	}

	cmd := "update\n"
	if uri != "" {
		cmd = fmt.Sprintf("update %s\n", mpdQuote(uri))
	}
	if _, err := conn.Write([]byte(cmd)); err != nil {
		return fmt.Errorf("write command: %w", err)
	}

	// Read until OK / ACK.
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			return fmt.Errorf("read response: %w", err)
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "OK" {
			return nil
		}
		if strings.HasPrefix(line, "ACK") {
			return fmt.Errorf("melodyd: %s", line)
		}
	}
}

func mpdQuote(s string) string {
	s = strings.ReplaceAll(s, `\`, `\\`)
	s = strings.ReplaceAll(s, `"`, `\"`)
	return `"` + s + `"`
}

func display(uri string) string {
	if uri == "" {
		return "<root>"
	}
	return uri
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func envInt(key string, def int) int {
	if v := os.Getenv(key); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}

func envDuration(key string, def time.Duration) time.Duration {
	if v := os.Getenv(key); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			return d
		}
	}
	return def
}
