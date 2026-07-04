// melody-rofi is a rofi/dmenu front-end for adding music to a (possibly remote)
// melodyd queue. It lists albums, latest albums, or tracks, lets you mark
// several with rofi's multi-select, then offers an Add / Insert / Replace
// submenu:
//
//	Add      append the selection to the end of the queue
//	Insert   place it right after the current track
//	Replace  clear the queue, load the selection, start playing
//
// Speed: the lists come pre-built and cached from melodyd (melody_albums /
// melody_albums_latest / melody_tracks), so the menu pops up instantly even for
// large libraries. Enqueueing uses melodyd's `enqueue` extension by album/track
// id, so there's no ambiguity or per-track round trips.
//
// Configure the target like the other melody tools — config file,
// MPD_HOST/MPD_PORT, or flags:
//
//	melody-rofi albums                 # all albums
//	melody-rofi latest                 # newest albums first
//	melody-rofi tracks                 # individual tracks
//	melody-rofi --api-address host:6601 albums
//
// The menu command defaults to `rofi -dmenu` and can be overridden in the config
// file (menu_cmd) or via the MELODY_MENU env var (e.g. "dmenu -l 20").
package main

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/BurntSushi/toml"
)

type config struct {
	MPDHost string `toml:"mpd_host"`
	MPDPort int    `toml:"mpd_port"`
	MenuCmd string `toml:"menu_cmd"`
}

// entry is one selectable item: the enqueue filter that identifies it
// ("albumid" 42 or "trackid" 1337) plus the raw display columns from the server.
type entry struct {
	field string // "albumid" or "trackid"
	id    string
	cols  []string // e.g. [artist, album, date] or [artist, title, album]
}

func main() {
	cfg := loadConfig()
	mode := parseMode(os.Args[1:], &cfg)

	if mode == "" {
		choice, ok := menu(cfg, "Browse", []string{"Albums", "Latest Albums", "Tracks"})
		if !ok {
			return
		}
		switch strings.ToLower(choice) {
		case "tracks":
			mode = "tracks"
		case "latest albums", "latest":
			mode = "latest"
		default:
			mode = "albums"
		}
	}

	var command, field, prompt string
	switch mode {
	case "albums":
		command, field, prompt = "melody_albums", "albumid", "Albums"
	case "latest":
		command, field, prompt = "melody_albums_latest", "albumid", "Latest"
	case "tracks":
		command, field, prompt = "melody_tracks", "trackid", "Tracks"
	default:
		fatalf("unknown mode %q", mode)
	}

	entries, err := fetchList(cfg, command, field)
	if err != nil {
		fatalf("%s: %v", command, err)
	}
	if len(entries) == 0 {
		fatalf("nothing to show")
	}

	var labels []string
	if mode == "tracks" {
		labels = trackLabels(entries)
	} else {
		labels = albumLabels(entries)
	}
	run(cfg, prompt, entries, labels)
}

func run(cfg config, prompt string, entries []entry, labels []string) {
	idxs, ok := menuIndices(cfg, prompt, labels)
	if !ok || len(idxs) == 0 {
		return
	}

	mode, ok := actionMenu(cfg)
	if !ok {
		return
	}

	selected := make([]entry, len(idxs))
	for i, idx := range idxs {
		selected[i] = entries[idx]
	}
	applyEnqueue(cfg, mode, selected)
}

// parseMode extracts the picker mode and connection flags from the args.
func parseMode(args []string, cfg *config) string {
	mode := ""
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "-a", "--albums", "albums":
			mode = "albums"
		case "-l", "--latest", "latest":
			mode = "latest"
		case "-t", "--tracks", "tracks":
			mode = "tracks"
		case "--api-address", "-H":
			if i+1 < len(args) {
				applyAddress(cfg, args[i+1])
				i++
			}
		case "-h", "--help":
			fmt.Println("Usage: melody-rofi [albums|latest|tracks] [--api-address host:port]")
			os.Exit(0)
		default:
			if strings.HasPrefix(args[i], "--api-address=") {
				applyAddress(cfg, strings.TrimPrefix(args[i], "--api-address="))
			}
		}
	}
	return mode
}

// actionMenu asks for the queue action and maps it to an enqueue mode.
func actionMenu(cfg config) (string, bool) {
	choice, ok := menu(cfg, "Action", []string{"Add", "Insert", "Replace"})
	if !ok {
		return "", false
	}
	switch strings.ToLower(choice) {
	case "add", "insert", "replace":
		return strings.ToLower(choice), true
	}
	return "", false
}

// applyEnqueue issues enqueue commands for the selected entries in an order that
// preserves the user's selection order in the resulting queue.
func applyEnqueue(cfg config, mode string, sel []entry) {
	conn, r, w, err := dial(cfg)
	if err != nil {
		fatalf("connect: %v", err)
	}
	defer conn.Close()

	switch mode {
	case "add":
		for _, e := range sel {
			enqueue(conn, r, w, "add", e)
		}
	case "replace":
		// First selection replaces and starts playback; the rest append in
		// order so the whole selection ends up queued.
		enqueue(conn, r, w, "replace", sel[0])
		for _, e := range sel[1:] {
			enqueue(conn, r, w, "add", e)
		}
	case "insert":
		// Insert places tracks right after the current one. Inserting in
		// reverse leaves the selection in its original order after the current
		// track.
		for i := len(sel) - 1; i >= 0; i-- {
			enqueue(conn, r, w, "insert", sel[i])
		}
	}
}

// ---------------------------------------------------------------------------
// MPD client
// ---------------------------------------------------------------------------

func dial(cfg config) (net.Conn, *bufio.Reader, *bufio.Writer, error) {
	conn, err := net.DialTimeout("tcp", fmt.Sprintf("%s:%d", cfg.MPDHost, cfg.MPDPort), 5*time.Second)
	if err != nil {
		return nil, nil, nil, err
	}
	r := bufio.NewReader(conn)
	w := bufio.NewWriter(conn)
	greeting, err := r.ReadString('\n')
	if err != nil || !strings.HasPrefix(greeting, "OK MPD") {
		conn.Close()
		return nil, nil, nil, fmt.Errorf("not an MPD server at %s:%d", cfg.MPDHost, cfg.MPDPort)
	}
	return conn, r, w, nil
}

// mpdCmd sends one command and returns the response lines (excluding OK).
func mpdCmd(conn net.Conn, r *bufio.Reader, w *bufio.Writer, cmd string) ([]string, error) {
	conn.SetDeadline(time.Now().Add(20 * time.Second))
	if _, err := w.WriteString(cmd + "\n"); err != nil {
		return nil, err
	}
	if err := w.Flush(); err != nil {
		return nil, err
	}
	var lines []string
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			return lines, err
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "OK" {
			return lines, nil
		}
		if strings.HasPrefix(line, "ACK") {
			return lines, fmt.Errorf("%s", line)
		}
		lines = append(lines, line)
	}
}

// fetchList retrieves a pre-built launcher list. Each response line looks like
// "X-Album: <id>\t<col1>\t<col2>\t..." (or X-Track:); the value after the key is
// split on tabs into the id and the raw display columns.
func fetchList(cfg config, command, field string) ([]entry, error) {
	conn, r, w, err := dial(cfg)
	if err != nil {
		return nil, err
	}
	defer conn.Close()

	lines, err := mpdCmd(conn, r, w, command)
	if err != nil {
		return nil, err
	}

	entries := make([]entry, 0, len(lines))
	for _, line := range lines {
		_, val, ok := splitKV(line)
		if !ok {
			continue
		}
		parts := strings.Split(val, "\t")
		if len(parts) < 2 || parts[0] == "" {
			continue
		}
		entries = append(entries, entry{field: field, id: parts[0], cols: parts[1:]})
	}
	return entries, nil
}

// The server sends raw fields per row. For albums: col0=artist, col1=album,
// col2=date. For tracks: col0=artist, col1=title, col2=album.

// albumLabels renders three aligned columns: artist, date, then album — i.e. the
// year is prepended to the album, two spaces apart. An unknown date ("0000") is
// shown blank so the album column still lines up.
func albumLabels(entries []entry) []string {
	rows := make([][]string, len(entries))
	for i, e := range entries {
		date := col(e.cols, 2)
		if date == "0000" {
			date = ""
		}
		rows[i] = []string{col(e.cols, 0), date, col(e.cols, 1)}
	}
	return alignColumns(rows, []int{30, 4, 0})
}

// trackLabels renders artist, title, album as aligned columns.
func trackLabels(entries []entry) []string {
	rows := make([][]string, len(entries))
	for i, e := range entries {
		rows[i] = []string{col(e.cols, 0), col(e.cols, 1), col(e.cols, 2)}
	}
	return alignColumns(rows, []int{28, 45, 0})
}

func alignColumns(rows [][]string, caps []int) []string {
	width := make([]int, len(caps))
	for _, r := range rows {
		for i := range caps {
			if caps[i] == 0 {
				continue
			}
			if w := runeLen(r[i]); w > width[i] {
				width[i] = w
			}
		}
	}
	for i := range width {
		if caps[i] > 0 && width[i] > caps[i] {
			width[i] = caps[i]
		}
	}

	out := make([]string, len(rows))
	for ri, r := range rows {
		var b strings.Builder
		for i := range caps {
			if i > 0 {
				b.WriteString("  ")
			}
			if caps[i] == 0 {
				b.WriteString(r[i])
			} else {
				b.WriteString(padTrunc(r[i], width[i]))
			}
		}
		out[ri] = strings.TrimRight(b.String(), " ")
	}
	return out
}

func padTrunc(s string, w int) string {
	r := []rune(s)
	if len(r) > w {
		if w <= 1 {
			return string(r[:w])
		}
		return string(r[:w-1]) + "…"
	}
	return s + strings.Repeat(" ", w-len(r))
}

func runeLen(s string) int { return len([]rune(s)) }

func col(cols []string, i int) string {
	if i < len(cols) {
		return cols[i]
	}
	return ""
}

func enqueue(conn net.Conn, r *bufio.Reader, w *bufio.Writer, mode string, e entry) {
	cmd := fmt.Sprintf("enqueue %s %s %s", mode, e.field, e.id)
	if _, err := mpdCmd(conn, r, w, cmd); err != nil {
		fmt.Fprintf(os.Stderr, "melody-rofi: enqueue failed: %v\n", err)
	}
}

// ---------------------------------------------------------------------------
// rofi / dmenu
// ---------------------------------------------------------------------------

// menuIndices shows a multi-select menu and returns the selected row indices.
func menuIndices(cfg config, prompt string, items []string) ([]int, bool) {
	base := menuArgs(cfg)
	args := append([]string{}, base[1:]...)
	args = append(args, "-p", prompt, "-i", "-multi-select", "-format", "i")
	out, ok := runMenu(base[0], args, strings.Join(items, "\n"))
	if !ok {
		return nil, false
	}
	var idxs []int
	for _, line := range strings.Split(strings.TrimSpace(out), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		if n, err := strconv.Atoi(line); err == nil && n >= 0 && n < len(items) {
			idxs = append(idxs, n)
		}
	}
	return idxs, len(idxs) > 0
}

// menu shows a single-select menu and returns the chosen entry text.
func menu(cfg config, prompt string, items []string) (string, bool) {
	base := menuArgs(cfg)
	args := append([]string{}, base[1:]...)
	args = append(args, "-p", prompt, "-i")
	out, ok := runMenu(base[0], args, strings.Join(items, "\n"))
	if !ok {
		return "", false
	}
	choice := strings.TrimRight(out, "\n")
	if choice == "" {
		return "", false
	}
	return choice, true
}

func runMenu(name string, args []string, stdin string) (string, bool) {
	cmd := exec.Command(name, args...)
	cmd.Stdin = strings.NewReader(stdin)
	cmd.Stderr = os.Stderr
	out, err := cmd.Output()
	if err != nil {
		// Non-zero exit means the user cancelled (Esc) — not a hard error.
		return "", false
	}
	return string(out), true
}

func menuArgs(cfg config) []string {
	cmd := cfg.MenuCmd
	if env := os.Getenv("MELODY_MENU"); env != "" {
		cmd = env
	}
	if cmd == "" {
		cmd = "rofi -dmenu"
	}
	return strings.Fields(cmd)
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

func splitKV(line string) (string, string, bool) {
	idx := strings.Index(line, ": ")
	if idx < 0 {
		return "", "", false
	}
	return line[:idx], line[idx+2:], true
}

func fatalf(format string, a ...any) {
	fmt.Fprintf(os.Stderr, "melody-rofi: "+format+"\n", a...)
	os.Exit(1)
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

func loadConfig() config {
	home, _ := os.UserHomeDir()
	xdgConfig := os.Getenv("XDG_CONFIG_HOME")
	if xdgConfig == "" {
		xdgConfig = filepath.Join(home, ".config")
	}
	configPath := filepath.Join(xdgConfig, "melody", "melody-rofi.toml")

	_ = os.MkdirAll(filepath.Dir(configPath), 0o755)
	if _, err := os.Stat(configPath); os.IsNotExist(err) {
		_ = os.WriteFile(configPath, []byte(defaultConfig()), 0o644)
	}

	var c config
	_, _ = toml.DecodeFile(configPath, &c)

	// Env overrides (shared with the other melody tools).
	if h := os.Getenv("MPD_HOST"); h != "" {
		applyAddress(&c, h)
	}
	if p := os.Getenv("MPD_PORT"); p != "" {
		fmt.Sscanf(p, "%d", &c.MPDPort)
	}
	if c.MPDHost == "" {
		c.MPDHost = "localhost"
	}
	if c.MPDPort == 0 {
		c.MPDPort = 6600
	}
	return c
}

// applyAddress accepts "host" or "host:port".
func applyAddress(c *config, addr string) {
	if host, port, ok := strings.Cut(addr, ":"); ok {
		c.MPDHost = host
		fmt.Sscanf(port, "%d", &c.MPDPort)
	} else {
		c.MPDHost = addr
	}
}

func defaultConfig() string {
	return `# melodyd MPD server to add music to.
# MPD_HOST and MPD_PORT environment variables override these values.
mpd_host = "localhost"
mpd_port = 6600

# Launcher used for menus. Must read newline-separated entries on stdin and
# print the selection on stdout (rofi -dmenu / dmenu compatible).
menu_cmd = "rofi -dmenu"
`
}
