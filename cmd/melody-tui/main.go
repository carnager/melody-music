package main

import (
	"bufio"
	"bytes"
	"compress/zlib"
	"encoding/base64"
	"errors"
	"fmt"
	"image"
	"image/draw"
	_ "image/jpeg"
	_ "image/png"
	"io"
	"math"
	"math/rand"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	_ "golang.org/x/image/webp"

	"github.com/BurntSushi/toml"
	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/mattn/go-runewidth"
)

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

type tuiConfig struct {
	MPDHost string `toml:"mpd_host"`
	MPDPort int    `toml:"mpd_port"`
}

var cfg tuiConfig

func loadTUIConfig() tuiConfig {
	home, err := os.UserHomeDir()
	if err != nil {
		return tuiConfig{MPDHost: "localhost", MPDPort: 6600}
	}
	xdgConfig := os.Getenv("XDG_CONFIG_HOME")
	if xdgConfig == "" {
		xdgConfig = filepath.Join(home, ".config")
	}
	configPath := filepath.Join(xdgConfig, "melody", "melody-tui.toml")

	_ = os.MkdirAll(filepath.Dir(configPath), 0o755)

	if _, err := os.Stat(configPath); errors.Is(err, os.ErrNotExist) {
		_ = os.WriteFile(configPath, []byte("mpd_host = \"localhost\"\nmpd_port = 6600\n"), 0o644)
	}

	var c tuiConfig
	if _, err := toml.DecodeFile(configPath, &c); err != nil {
		fmt.Fprintf(os.Stderr, "warning: config: %v\n", err)
	}
	applyMPDEnv(&c)
	if c.MPDHost == "" {
		c.MPDHost = "localhost"
	}
	if c.MPDPort == 0 {
		c.MPDPort = 6600
	}
	return c
}

func applyMPDEnv(c *tuiConfig) {
	if h := os.Getenv("MPD_HOST"); h != "" {
		if host, port, ok := strings.Cut(h, ":"); ok {
			c.MPDHost = host
			fmt.Sscanf(port, "%d", &c.MPDPort)
		} else {
			c.MPDHost = h
		}
	}
	if p := os.Getenv("MPD_PORT"); p != "" {
		fmt.Sscanf(p, "%d", &c.MPDPort)
	}
}

// ---------------------------------------------------------------------------
// MPD client
// ---------------------------------------------------------------------------

type mpdClient struct {
	mu   sync.Mutex
	conn net.Conn
	r    *bufio.Reader
	w    *bufio.Writer
}

func newMPDClient(host string, port int) (*mpdClient, error) {
	conn, err := net.DialTimeout("tcp", fmt.Sprintf("%s:%d", host, port), 3*time.Second)
	if err != nil {
		return nil, err
	}
	c := &mpdClient{
		conn: conn,
		r:    bufio.NewReader(conn),
		w:    bufio.NewWriter(conn),
	}
	// Read greeting
	line, err := c.r.ReadString('\n')
	if err != nil {
		conn.Close()
		return nil, err
	}
	if !strings.HasPrefix(line, "OK MPD") {
		conn.Close()
		return nil, fmt.Errorf("unexpected MPD greeting: %s", line)
	}
	return c, nil
}

func (c *mpdClient) close() {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.conn != nil {
		c.conn.Close()
	}
}

// cmd sends a single MPD command and returns the response lines (without OK).
func (c *mpdClient) cmd(command string) ([]string, error) {
	if c == nil {
		return nil, fmt.Errorf("not connected")
	}
	c.mu.Lock()
	defer c.mu.Unlock()

	c.conn.SetDeadline(time.Now().Add(5 * time.Second))
	_, err := c.w.WriteString(command + "\n")
	if err != nil {
		return nil, err
	}
	if err := c.w.Flush(); err != nil {
		return nil, err
	}

	var lines []string
	for {
		line, err := c.r.ReadString('\n')
		if err != nil {
			return nil, err
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "OK" {
			return lines, nil
		}
		if strings.HasPrefix(line, "ACK ") {
			return nil, fmt.Errorf("mpd: %s", line)
		}
		lines = append(lines, line)
	}
}

// cmdBinary sends a command that returns binary data (albumart, readpicture).
// It reads key-value lines, then the binary chunk, reassembling across multiple
// requests if the data is larger than the server's chunk size.
func (c *mpdClient) cmdBinary(command string) ([]byte, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	var result []byte
	offset := 0
	totalSize := -1

	for {
		c.conn.SetDeadline(time.Now().Add(10 * time.Second))
		cmd := fmt.Sprintf("%s %d", command, offset)
		_, err := c.w.WriteString(cmd + "\n")
		if err != nil {
			return nil, err
		}
		if err := c.w.Flush(); err != nil {
			return nil, err
		}

		var binaryLen int
		for {
			line, err := c.r.ReadString('\n')
			if err != nil {
				return nil, err
			}
			line = strings.TrimRight(line, "\r\n")
			if line == "OK" {
				// No binary data (empty art)
				return result, nil
			}
			if strings.HasPrefix(line, "ACK ") {
				return nil, fmt.Errorf("mpd: %s", line)
			}
			if strings.HasPrefix(line, "size: ") {
				fmt.Sscanf(line, "size: %d", &totalSize)
			} else if strings.HasPrefix(line, "binary: ") {
				fmt.Sscanf(line, "binary: %d", &binaryLen)
				break
			}
		}

		if binaryLen == 0 {
			// Read trailing newline + OK
			c.r.ReadString('\n') // empty line after 0-length binary
			c.r.ReadString('\n') // OK
			return result, nil
		}

		chunk := make([]byte, binaryLen)
		_, err = io.ReadFull(c.r, chunk)
		if err != nil {
			return nil, err
		}
		// Read trailing newline after binary data
		c.r.ReadByte()
		// Read OK line
		okLine, _ := c.r.ReadString('\n')
		okLine = strings.TrimRight(okLine, "\r\n")
		if strings.HasPrefix(okLine, "ACK ") {
			return nil, fmt.Errorf("mpd: %s", okLine)
		}

		result = append(result, chunk...)
		offset += binaryLen

		if totalSize >= 0 && offset >= totalSize {
			return result, nil
		}
	}
}

// cmdBatch sends multiple commands in a command_list_ok_begin block.
func (c *mpdClient) cmdBatch(commands []string) ([][]string, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	c.conn.SetDeadline(time.Now().Add(5 * time.Second))
	c.w.WriteString("command_list_ok_begin\n")
	for _, cmd := range commands {
		c.w.WriteString(cmd + "\n")
	}
	c.w.WriteString("command_list_end\n")
	if err := c.w.Flush(); err != nil {
		return nil, err
	}

	var result [][]string
	var current []string
	for {
		line, err := c.r.ReadString('\n')
		if err != nil {
			return nil, err
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "OK" {
			result = append(result, current)
			return result, nil
		}
		if strings.HasPrefix(line, "ACK ") {
			return nil, fmt.Errorf("mpd: %s", line)
		}
		if line == "list_OK" {
			result = append(result, current)
			current = nil
			continue
		}
		current = append(current, line)
	}
}

// parseKV parses "Key: Value" lines into a map. Last value wins.
func parseKV(lines []string) map[string]string {
	m := make(map[string]string, len(lines))
	for _, l := range lines {
		if k, v, ok := strings.Cut(l, ": "); ok {
			m[k] = v
		}
	}
	return m
}

// parseGroups splits MPD response into groups, each starting with a line
// whose key matches groupKey. Handles grouped list responses where other
// tags (like Date) appear before the groupKey in each record.
func parseGroups(lines []string, groupKey string) []map[string]string {
	var groups []map[string]string
	cur := map[string]string{}
	for _, l := range lines {
		k, v, ok := strings.Cut(l, ": ")
		if !ok {
			continue
		}
		// If we see a key that already exists in cur, flush and start new group
		if _, exists := cur[k]; exists {
			if len(cur) > 0 {
				groups = append(groups, cur)
			}
			cur = map[string]string{}
		}
		cur[k] = v
	}
	if len(cur) > 0 {
		groups = append(groups, cur)
	}
	return groups
}

// parseList extracts all values for a given key.
func parseList(lines []string, key string) []string {
	var vals []string
	prefix := key + ": "
	for _, l := range lines {
		if strings.HasPrefix(l, prefix) {
			vals = append(vals, l[len(prefix):])
		}
	}
	return vals
}

// mpdEscape quotes a string for MPD protocol.
func mpdEscape(s string) string {
	s = strings.ReplaceAll(s, `\`, `\\`)
	s = strings.ReplaceAll(s, `"`, `\"`)
	return `"` + s + `"`
}

// mpdFilterEq builds a single MPD filter arg: "(Tag == \"value\")" properly
// escaped for the protocol. The result is a single quoted protocol arg.
func mpdFilterEq(tag, value string) string {
	// Inner value needs quotes for the filter syntax
	// Then the whole thing gets protocol-quoted
	inner := fmt.Sprintf(`(%s == %s)`, tag, mpdEscape(value))
	return mpdEscape(inner)
}

var mpd *mpdClient
var fetchConn *mpdClient // dedicated connection for album art, lyrics, etc.
var idleConn *mpdClient  // dedicated connection for MPD idle
var lastQueueVersion int // tracks MPD playlist version to skip redundant queue fetches
var forceQueueRefresh bool // set when ratings change to bypass version check

// Track last transmitted art to avoid re-transmitting on every render
var artTxFile string
var artTxCols, artTxRows int
var statusFetchTime time.Time // when status was last fetched (for elapsed interpolation)

func reconnectMPD() {
	if mpd != nil {
		mpd.close()
		mpd = nil
	}
	if fetchConn != nil {
		fetchConn.close()
		fetchConn = nil
	}
	c, err := newMPDClient(cfg.MPDHost, cfg.MPDPort)
	if err != nil {
		return
	}
	mpd = c
	fc, err := newMPDClient(cfg.MPDHost, cfg.MPDPort)
	if err != nil {
		return
	}
	fetchConn = fc
	lastQueueVersion = 0
	forceQueueRefresh = true
}

// ---------------------------------------------------------------------------
// API types (reused from old code, now populated from MPD responses)
// ---------------------------------------------------------------------------

type playbackStatus struct {
	State          string
	Title          string
	Artist         string
	AlbumArtist    string
	Album          string
	Date           string
	Track          int
	Disc           int
	File           string
	TimePos        float64
	Dur            float64
	Volume         int
	Rating         int
	SongID         string // X-SongId for rating commands
	SongPos        int    // current song position in queue
	ReplayGainMode string // "off", "track", "album"
	RGTrack        float64
	RGAlbum        float64
	Repeat         bool
	Random         bool
	Single         bool
	Consume        bool
}

type queueItem struct {
	Position int
	SongID   string
	XSongID  string // X-SongId (DB track ID, used for rating)
	Title    string
	Artist   string
	Album    string
	Duration float64
	Current  bool
	File     string
	Rating   int
	Priority int
}

type albumEntry struct {
	ID          string // composite: artist\x00album\x00date
	AlbumArtist string
	Album       string
	Date        string
	Rating      int
	Computed    float64
}

type deviceInfo struct {
	ID      string
	Name    string
	Enabled bool
}

type trackEntry struct {
	ID          string // file URI
	XSongID     string // X-SongId for rating
	Title       string
	Artist      string
	Album       string
	TrackNumber int
	Rating      int
	Duration    float64
}

type playlistEntry struct {
	Name      string
	SongCount int
	Duration  int
}

type searchResult struct {
	Albums []albumEntry
	Tracks []trackEntry
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

type tickMsg time.Time
type idleMsg []string // changed subsystems

type statusMsg struct {
	status       playbackStatus
	queue        []queueItem
	queueVersion int
	queueChanged bool
	reconnected  bool
}

type artistsMsg []string

type albumsMsg []albumEntry

type tracksMsg []trackEntry

type albumRatingMsg struct {
	rating   int
	computed float64
}

type ratingPopupMsg struct{ rating int }
type npAlbumRatingMsg struct{ rating int }

type searchMsg searchResult
type searchDebounceMsg struct{ gen int }

type playlistsMsg []playlistEntry

type playlistTracksMsg []trackEntry

type plPickerReadyMsg []playlistEntry

type devicesMsg struct {
	devices []deviceInfo
	active  int // output ID of enabled device, -1 if none
}

type albumArtMsg struct {
	data []byte
	file string
	w, h int
}

type artTransmittedMsg struct{}

type trackInfoLine struct {
	label, value string
}

type trackInfoMsg []trackInfoLine

type lyricsLine struct {
	time float64 // timestamp in seconds (-1 for plain text)
	text string
}

type lyricsMsg struct {
	lines      []lyricsLine
	lyricsType string
	file       string
}

type gotoMetaMsg struct {
	artist, albumArtist, album, date string
}

type errMsg string

// ---------------------------------------------------------------------------
// Focus / panel
// ---------------------------------------------------------------------------

type panel int

const (
	panelLibrary panel = iota
	panelQueue
)

type libView int

const (
	libArtists libView = iota
	libAlbums
	libTracks
	libPlaylists
	libPlaylistTracks
)

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

type model struct {
	width, height int

	focus panel

	// playback
	status playbackStatus
	queue  []queueItem

	// library
	libMode      libView
	libSortLatest bool
	artists      []string
	albums       []albumEntry
	tracks       []trackEntry
	curArtist          string
	curAlbum           *albumEntry
	albumRating        int
	npAlbumRating      int // album rating for currently playing track
	albumComputedRating float64
	libCursor    int
	libOffset    int
	libFiltering bool  // true when filter input is active
	libFilter    string // fzf-style filter text
	libFiltered  []int  // indices into the source list matching filter
	// saved positions for back navigation
	savedArtistCursor int
	savedArtistOffset int
	savedAlbumCursor  int
	savedAlbumOffset  int
	savedPlCursor     int
	savedPlOffset     int

	// queue
	qCursor      int
	qOffset      int
	qSelected    map[int]bool
	confirmClear bool
	qFirstSongID string
	queueVersion int // MPD playlist version, used to skip redundant queue fetches

	// playlists
	playlists        []playlistEntry
	playlistTracks   []trackEntry
	curPlaylist      string

	// search
	searching      bool
	searchInput    textinput.Model
	searchRes      searchResult
	srCursor       int
	srOffset       int
	srTotal        int
	searchPending  string // pending debounced query
	searchDebounce int    // debounce generation counter

	// action menu
	showMenu   bool
	menuCursor int
	menuSource string // "library" or "search"

	// playlist picker (add to playlist)
	showPlPicker    bool
	plPickerList    []playlistEntry
	plPickerCursor  int
	plPickerURI     string
	plPickerNewMode bool
	plPickerInput   textinput.Model

	// help
	showHelp bool

	// track info
	showTrackInfo bool
	trackInfo     []trackInfoLine // key-value pairs for display

	// lyrics sidebar + now playing screen
	showLyrics     bool
	showNowPlaying bool
	lyrics         []lyricsLine // parsed lyrics lines
	lyricsType     string       // "synced" or "plain"
	lyricsScroll   int          // scroll offset
	lyricsFile     string       // file URI whose lyrics are loaded

	// go-to menu
	showGoto      bool
	gotoCursor    int
	gotoArtist    string
	gotoAlbumArtist string
	gotoAlbum     string
	gotoDate      string

	// rating popup
	showRating    bool
	ratingCursor  int         // 0=unrate, 1-10 = rating values
	ratingIsAlbum bool        // true when rating an album
	ratingAlbum   *albumEntry // album being rated (from album list)

	// priority popup
	showPrioMenu  bool
	prioCursor    int    // 0=Low, 1=Medium, 2=High
	prioSourceURI string // file URI to add with priority

	// modes popup
	showModes   bool
	modesCursor int

	// devices (outputs)
	showDevices  bool
	devices      []deviceInfo
	activeDevice int // output ID
	devCursor    int

	// album art (kitty protocol)
	artData    []byte // raw image data for current track
	artFile    string // URI of track whose art is loaded
	artW, artH int    // pixel dimensions of the image
	artRGBA    []byte // zlib-compressed RGBA pixels (cached for re-transmit)

	err string
}

func newModel() model {
	ti := textinput.New()
	ti.Placeholder = "Search albums and tracks..."
	ti.CharLimit = 100

	pti := textinput.New()
	pti.Placeholder = "New playlist name..."
	pti.CharLimit = 100

	return model{
		focus:         panelLibrary,
		libMode:       libArtists,
		searchInput:   ti,
		plPickerInput: pti,
		activeDevice:  -1,
	}
}

func (m model) Init() tea.Cmd {
	return tea.Batch(
		fetchArtists,
		fetchStatus,
		tickCmd(),
		listenIdle,
	)
}

// ---------------------------------------------------------------------------
// Commands (MPD-backed)
// ---------------------------------------------------------------------------

func tickCmd() tea.Cmd {
	return tea.Tick(1*time.Second, func(t time.Time) tea.Msg {
		return tickMsg(t)
	})
}

func connectIdle() {
	if idleConn != nil {
		idleConn.close()
		idleConn = nil
	}
	c, err := newMPDClient(cfg.MPDHost, cfg.MPDPort)
	if err != nil {
		return
	}
	idleConn = c
}

func listenIdle() tea.Msg {
	if idleConn == nil {
		connectIdle()
		if idleConn == nil {
			time.Sleep(2 * time.Second)
			return idleMsg(nil)
		}
	}

	// Send idle command (no mutex needed — this connection is exclusively for idle)
	idleConn.conn.SetDeadline(time.Time{}) // no deadline for idle
	_, err := idleConn.w.WriteString("idle player playlist mixer options database stored_playlist rating\n")
	if err != nil {
		idleConn.close()
		idleConn = nil
		return idleMsg(nil)
	}
	if err := idleConn.w.Flush(); err != nil {
		idleConn.close()
		idleConn = nil
		return idleMsg(nil)
	}

	// Block until server responds with changed subsystems
	var changed []string
	for {
		line, err := idleConn.r.ReadString('\n')
		if err != nil {
			idleConn.close()
			idleConn = nil
			return idleMsg(nil)
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "OK" {
			break
		}
		if strings.HasPrefix(line, "changed: ") {
			changed = append(changed, line[9:])
		}
	}
	return idleMsg(changed)
}

func fetchStatus() tea.Msg {
	reconnected := false
	if mpd == nil {
		reconnectMPD()
		if mpd == nil {
			return statusMsg{}
		}
		reconnected = true
	}

	// First fetch status + currentsong + replay_gain_status
	results, err := mpd.cmdBatch([]string{"status", "currentsong", "replay_gain_status"})
	if err != nil || len(results) < 2 {
		reconnectMPD()
		return statusMsg{reconnected: mpd != nil}
	}

	st := parseKV(results[0])
	cs := parseKV(results[1])
	var rgs map[string]string
	if len(results) >= 3 {
		rgs = parseKV(results[2])
	}

	var ps playbackStatus
	ps.State = st["state"]
	ps.TimePos, _ = strconv.ParseFloat(st["elapsed"], 64)
	ps.Dur, _ = strconv.ParseFloat(st["duration"], 64)
	if ps.Dur == 0 {
		ps.Dur, _ = strconv.ParseFloat(cs["duration"], 64)
	}
	if ps.Dur == 0 {
		ps.Dur, _ = strconv.ParseFloat(cs["Time"], 64)
	}
	ps.Volume, _ = strconv.Atoi(st["volume"])
	ps.Title = cs["Title"]
	ps.Artist = cs["Artist"]
	ps.AlbumArtist = cs["AlbumArtist"]
	ps.Album = cs["Album"]
	ps.Date = cs["Date"]
	ps.Track, _ = strconv.Atoi(cs["Track"])
	ps.Disc, _ = strconv.Atoi(cs["Disc"])
	ps.File = cs["file"]
	ps.Rating, _ = strconv.Atoi(cs["X-Rating"])
	ps.SongID = cs["X-SongId"]
	ps.RGTrack, _ = strconv.ParseFloat(cs["X-ReplayGainTrack"], 64)
	ps.RGAlbum, _ = strconv.ParseFloat(cs["X-ReplayGainAlbum"], 64)
	ps.SongPos = -1
	if v, ok := st["song"]; ok {
		ps.SongPos, _ = strconv.Atoi(v)
	}
	ps.Repeat = st["repeat"] == "1"
	ps.Random = st["random"] == "1"
	ps.Single = st["single"] == "1"
	ps.Consume = st["consume"] == "1"
	if rgs != nil {
		ps.ReplayGainMode = rgs["replay_gain_mode"]
	}
	if ps.ReplayGainMode == "" {
		ps.ReplayGainMode = "off"
	}
	statusFetchTime = time.Now()

	curPos := ps.SongPos

	// Check if queue version changed
	qVersion, _ := strconv.Atoi(st["playlist"])
	if qVersion == lastQueueVersion && lastQueueVersion > 0 && !forceQueueRefresh {
		// Queue unchanged — return status only, update current position
		return statusMsg{status: ps, queueVersion: qVersion, queueChanged: false, reconnected: reconnected}
	}
	forceQueueRefresh = false

	// Queue changed — fetch it
	qResults, err := mpd.cmdBatch([]string{"playlistinfo"})
	if err != nil || len(qResults) < 1 {
		return statusMsg{status: ps, queueVersion: qVersion, queueChanged: false, reconnected: reconnected}
	}

	groups := parseGroups(qResults[0], "file")
	queue := make([]queueItem, 0, len(groups))
	for _, g := range groups {
		pos, _ := strconv.Atoi(g["Pos"])
		dur, _ := strconv.ParseFloat(g["duration"], 64)
		if dur == 0 {
			dur, _ = strconv.ParseFloat(g["Time"], 64)
		}
		r, _ := strconv.Atoi(g["X-Rating"])
		prio, _ := strconv.Atoi(g["Prio"])
		queue = append(queue, queueItem{
			Position: pos,
			SongID:   g["Id"],
			XSongID:  g["X-SongId"],
			Title:    g["Title"],
			Artist:   g["Artist"],
			Album:    g["Album"],
			Duration: dur,
			Current:  pos == curPos,
			File:     g["file"],
			Rating:   r,
			Priority: prio,
		})
	}

	lastQueueVersion = qVersion
	return statusMsg{status: ps, queue: queue, queueVersion: qVersion, queueChanged: true, reconnected: reconnected}
}

func fetchTrackInfo(file string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil || file == "" {
			return trackInfoMsg(nil)
		}
		lines, err := mpd.cmd(fmt.Sprintf("find file %s", mpdEscape(file)))
		if err != nil || len(lines) == 0 {
			return trackInfoMsg(nil)
		}
		kv := parseKV(lines)

		var info []trackInfoLine
		add := func(label, value string) {
			if value != "" && value != "0" {
				info = append(info, trackInfoLine{label, value})
			}
		}
		add("Title", kv["Title"])
		add("Artist", kv["Artist"])
		add("Album Artist", kv["AlbumArtist"])
		add("Album", kv["Album"])
		add("Date", kv["Date"])
		add("Track", kv["Track"])
		add("Disc", kv["Disc"])
		if dur, err := strconv.ParseFloat(kv["duration"], 64); err == nil && dur > 0 {
			add("Duration", fmtTime(dur))
		}
		add("Rating", kv["X-Rating"])
		if v := kv["X-ReplayGainTrack"]; v != "" && v != "0.00" {
			add("RG Track", v+" dB")
		}
		if v := kv["X-ReplayGainAlbum"]; v != "" && v != "0.00" {
			add("RG Album", v+" dB")
		}
		add("File", kv["file"])

		return trackInfoMsg(info)
	}
}

func fetchLyrics(file string) tea.Cmd {
	return func() tea.Msg {
		if fetchConn == nil || file == "" {
			return lyricsMsg{}
		}
		resp, err := fetchConn.cmd(fmt.Sprintf("readlyrics %s", mpdEscape(file)))
		if err != nil || len(resp) == 0 {
			return lyricsMsg{}
		}
		kv := parseKV(resp)
		lType := kv["X-Lyrics-Type"]
		raw := kv["X-Lyrics"]
		if raw == "" {
			return lyricsMsg{}
		}

		// Unescape: \\n → \n, \\\\ → \\
		text := unescapeLyrics(raw)

		var lines []lyricsLine
		if lType == "synced" {
			lines = parseLRC(text)
		} else {
			for _, l := range strings.Split(text, "\n") {
				lines = append(lines, lyricsLine{time: -1, text: l})
			}
		}
		return lyricsMsg{lines: lines, lyricsType: lType, file: file}
	}
}

func unescapeLyrics(s string) string {
	var b strings.Builder
	b.Grow(len(s))
	for i := 0; i < len(s); i++ {
		if s[i] == '\\' && i+1 < len(s) {
			switch s[i+1] {
			case 'n':
				b.WriteByte('\n')
				i++
			case '\\':
				b.WriteByte('\\')
				i++
			default:
				b.WriteByte(s[i])
			}
		} else {
			b.WriteByte(s[i])
		}
	}
	return b.String()
}

func parseLRC(text string) []lyricsLine {
	var lines []lyricsLine
	for _, raw := range strings.Split(text, "\n") {
		raw = strings.TrimSpace(raw)
		if raw == "" {
			lines = append(lines, lyricsLine{time: -1, text: ""})
			continue
		}
		// Parse [MM:SS.xx] or [MM:SS] timestamps
		if len(raw) >= 5 && raw[0] == '[' {
			end := strings.Index(raw, "]")
			if end > 0 {
				ts := raw[1:end]
				rest := raw[end+1:]
				if t, ok := parseLRCTime(ts); ok {
					lines = append(lines, lyricsLine{time: t, text: rest})
					continue
				}
			}
		}
		// Non-timestamped line (metadata tags like [ar:Artist])
		lines = append(lines, lyricsLine{time: -1, text: raw})
	}
	return lines
}

func parseLRCTime(s string) (float64, bool) {
	// MM:SS.xx or MM:SS
	parts := strings.SplitN(s, ":", 2)
	if len(parts) != 2 {
		return 0, false
	}
	min, err := strconv.ParseFloat(parts[0], 64)
	if err != nil {
		return 0, false
	}
	sec, err := strconv.ParseFloat(parts[1], 64)
	if err != nil {
		return 0, false
	}
	return min*60 + sec, true
}

func fetchGotoMeta(file string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil || file == "" {
			return gotoMetaMsg{}
		}
		lines, err := mpd.cmd(fmt.Sprintf("find file %s", mpdEscape(file)))
		if err != nil || len(lines) == 0 {
			return gotoMetaMsg{}
		}
		kv := parseKV(lines)
		return gotoMetaMsg{
			artist:      kv["Artist"],
			albumArtist: kv["AlbumArtist"],
			album:       kv["Album"],
			date:        kv["Date"],
		}
	}
}

func fetchAlbumArt(file string) tea.Cmd {
	return func() tea.Msg {
		if fetchConn == nil || file == "" {
			return albumArtMsg{}
		}
		data, err := fetchConn.cmdBinary(fmt.Sprintf(`albumart "%s"`, file))
		if err != nil || len(data) == 0 {
			return albumArtMsg{}
		}
		// Decode to get dimensions
		r := bytes.NewReader(data)
		cfg, _, err := image.DecodeConfig(r)
		if err != nil {
			return albumArtMsg{data: data, file: file, w: 300, h: 300}
		}
		return albumArtMsg{data: data, file: file, w: cfg.Width, h: cfg.Height}
	}
}

func fetchArtists() tea.Msg {
	if mpd == nil {
		return artistsMsg(nil)
	}
	lines, err := mpd.cmd("list AlbumArtist")
	if err != nil {
		return artistsMsg(nil)
	}
	artists := parseList(lines, "AlbumArtist")
	sort.Strings(artists)
	return artistsMsg(artists)
}

func fetchAlbums(artist string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return albumsMsg(nil)
		}
		lines, err := mpd.cmd(fmt.Sprintf("list Album AlbumArtist %s group Date", mpdEscape(artist)))
		if err != nil {
			return albumsMsg(nil)
		}
		groups := parseGroups(lines, "Album")
		var albums []albumEntry
		for _, g := range groups {
			a := albumEntry{
				AlbumArtist: artist,
				Album:       g["Album"],
				Date:        g["Date"],
			}
			a.ID = artist + "\x00" + a.Album + "\x00" + a.Date
			albums = append(albums, a)
		}
		// Fetch album ratings
		for i := range albums {
			a := &albums[i]
			rLines, err := mpd.cmd("getalbumrating " + mpdEscape(a.AlbumArtist) + " " + mpdEscape(a.Album) + " " + mpdEscape(a.Date))
			if err == nil {
				rKV := parseKV(rLines)
				if v, ok := rKV["rating"]; ok {
					a.Rating, _ = strconv.Atoi(v)
				}
				if v, ok := rKV["computed"]; ok {
					a.Computed, _ = strconv.ParseFloat(v, 64)
				}
			}
		}
		// Sort by date then album name
		sort.Slice(albums, func(i, j int) bool {
			if albums[i].Date != albums[j].Date {
				return albums[i].Date < albums[j].Date
			}
			return albums[i].Album < albums[j].Album
		})
		return albumsMsg(albums)
	}
}

func fetchAllAlbumsLatest() tea.Msg {
	if mpd == nil {
		return albumsMsg(nil)
	}
	lines, err := mpd.cmd("list Album group AlbumArtist group Date sort latest")
	if err != nil {
		return albumsMsg(nil)
	}
	groups := parseGroups(lines, "Album")
	var albums []albumEntry
	for _, g := range groups {
		a := albumEntry{
			AlbumArtist: g["AlbumArtist"],
			Album:       g["Album"],
			Date:        g["Date"],
		}
		a.ID = a.AlbumArtist + "\x00" + a.Album + "\x00" + a.Date
		albums = append(albums, a)
	}
	// Skip per-album rating fetch — too many round-trips for full library.
	// Ratings are shown when browsing a specific artist's albums.
	return albumsMsg(albums)
}

func fetchTracks(albumID string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return tracksMsg(nil)
		}
		parts := strings.SplitN(albumID, "\x00", 3)
		if len(parts) < 3 {
			return tracksMsg(nil)
		}
		artist, album, date := parts[0], parts[1], parts[2]

		cmd := "find " + mpdFilterEq("AlbumArtist", artist) + " " + mpdFilterEq("Album", album)
		if date != "" {
			cmd += " " + mpdFilterEq("Date", date)
		}
		lines, err := mpd.cmd(cmd)
		if err != nil {
			return tracksMsg(nil)
		}
		groups := parseGroups(lines, "file")
		var tracks []trackEntry
		for _, g := range groups {
			tn, _ := strconv.Atoi(g["Track"])
			r, _ := strconv.Atoi(g["X-Rating"])
			dur, _ := strconv.ParseFloat(g["duration"], 64)
			if dur == 0 {
				dur, _ = strconv.ParseFloat(g["Time"], 64)
			}
			tracks = append(tracks, trackEntry{
				ID:          g["file"],
				XSongID:     g["X-SongId"],
				Title:       g["Title"],
				Artist:      g["Artist"],
				Album:       g["Album"],
				TrackNumber: tn,
				Rating:      r,
				Duration:    dur,
			})
		}
		sort.Slice(tracks, func(i, j int) bool {
			return tracks[i].TrackNumber < tracks[j].TrackNumber
		})
		return tracksMsg(tracks)
	}
}

func fetchAlbumRating(albumArtist, album, date string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return albumRatingMsg{}
		}
		lines, err := mpd.cmd("getalbumrating " + mpdEscape(albumArtist) + " " + mpdEscape(album) + " " + mpdEscape(date))
		if err != nil {
			return albumRatingMsg{}
		}
		var rating int
		var computed float64
		for _, l := range lines {
			if strings.HasPrefix(l, "rating: ") {
				rating, _ = strconv.Atoi(strings.TrimPrefix(l, "rating: "))
			}
			if strings.HasPrefix(l, "computed: ") {
				computed, _ = strconv.ParseFloat(strings.TrimPrefix(l, "computed: "), 64)
			}
		}
		return albumRatingMsg{rating: rating, computed: computed}
	}
}

func fetchNPAlbumRating(albumArtist, album, date string) tea.Cmd {
	return func() tea.Msg {
		if fetchConn == nil || albumArtist == "" || album == "" {
			return npAlbumRatingMsg{}
		}
		lines, err := fetchConn.cmd("getalbumrating " + mpdEscape(albumArtist) + " " + mpdEscape(album) + " " + mpdEscape(date))
		if err != nil {
			return npAlbumRatingMsg{}
		}
		kv := parseKV(lines)
		r, _ := strconv.Atoi(kv["rating"])
		if r == 0 {
			// Fall back to computed average (server already applies 70% threshold)
			if c, err := strconv.ParseFloat(kv["computed"], 64); err == nil && c > 0 {
				r = int(math.Round(c))
				if r < 1 {
					r = 1
				} else if r > 10 {
					r = 10
				}
			}
		}
		return npAlbumRatingMsg{rating: r}
	}
}

func fetchAlbumRatingForPopup(albumArtist, album, date string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return ratingPopupMsg{}
		}
		lines, err := mpd.cmd("getalbumrating " + mpdEscape(albumArtist) + " " + mpdEscape(album) + " " + mpdEscape(date))
		if err != nil {
			return ratingPopupMsg{}
		}
		var rating int
		for _, l := range lines {
			if strings.HasPrefix(l, "rating: ") {
				rating, _ = strconv.Atoi(strings.TrimPrefix(l, "rating: "))
			}
		}
		return ratingPopupMsg{rating: rating}
	}
}

func fetchPlaylists() tea.Msg {
	if mpd == nil {
		return playlistsMsg(nil)
	}
	lines, err := mpd.cmd("listplaylists")
	if err != nil {
		return playlistsMsg(nil)
	}
	groups := parseGroups(lines, "playlist")
	var pls []playlistEntry
	for _, g := range groups {
		name := g["playlist"]
		if name == "" {
			continue
		}
		sc, _ := strconv.Atoi(g["songs"])
		dur, _ := strconv.Atoi(g["playtime"])
		pls = append(pls, playlistEntry{Name: name, SongCount: sc, Duration: dur})
	}
	return playlistsMsg(pls)
}

func fetchPlaylistTracks(name string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return playlistTracksMsg(nil)
		}
		lines, err := mpd.cmd("listplaylistinfo " + mpdEscape(name))
		if err != nil {
			return playlistTracksMsg(nil)
		}
		groups := parseGroups(lines, "file")
		var tracks []trackEntry
		for i, g := range groups {
			r, _ := strconv.Atoi(g["X-Rating"])
			tracks = append(tracks, trackEntry{
				ID:          g["file"],
				XSongID:     g["X-SongId"],
				Title:       g["Title"],
				Artist:      g["Artist"],
				Album:       g["Album"],
				TrackNumber: i + 1,
				Rating:      r,
			})
		}
		return playlistTracksMsg(tracks)
	}
}

func loadPlaylist(name, mode string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return fetchStatus()
		}
		switch mode {
		case "replace":
			mpd.cmd("clear")
			mpd.cmd("load " + mpdEscape(name))
			mpd.cmd("play")
		default:
			mpd.cmd("load " + mpdEscape(name))
		}
		return fetchStatus()
	}
}

func fetchPlPickerPlaylists(uri string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return plPickerReadyMsg(nil)
		}
		lines, err := mpd.cmd("listplaylists")
		if err != nil {
			return plPickerReadyMsg(nil)
		}
		groups := parseGroups(lines, "playlist")
		var pls []playlistEntry
		for _, g := range groups {
			name := g["playlist"]
			if name == "" {
				continue
			}
			sc, _ := strconv.Atoi(g["songs"])
			pls = append(pls, playlistEntry{Name: name, SongCount: sc})
		}
		return plPickerReadyMsg(pls)
	}
}

func addToPlaylist(playlistName, uri string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return nil
		}
		mpd.cmd("playlistadd " + mpdEscape(playlistName) + " " + mpdEscape(uri))
		return nil
	}
}

func parseRatingFilter(w string) (tag, op, value string, ok bool) {
	wl := strings.ToLower(w)
	for _, prefix := range []string{"albumrating", "rating"} {
		if !strings.HasPrefix(wl, prefix) {
			continue
		}
		rest := w[len(prefix):]
		for _, op := range []string{">=", "<=", ">", "<", "="} {
			if strings.HasPrefix(rest, op) {
				val := rest[len(op):]
				if val != "" {
					mpdOp := op
					if mpdOp == "=" {
						mpdOp = "=="
					}
					return prefix, mpdOp, val, true
				}
			}
		}
	}
	return "", "", "", false
}

func buildSearchCmd(q string) string {
	words := strings.Fields(q)
	var textParts []string
	var filters []string
	for _, w := range words {
		if tag, op, val, ok := parseRatingFilter(w); ok {
			filters = append(filters, "("+tag+" "+op+" '"+val+"')")
		} else {
			textParts = append(textParts, w)
		}
	}
	if len(filters) == 0 {
		return "search any " + mpdEscape(strings.Join(textParts, " "))
	}
	var parts []string
	if len(textParts) > 0 {
		parts = append(parts, "\"(any contains '"+mpdEscapeFilter(strings.Join(textParts, " "))+"')\"")
	}
	for _, f := range filters {
		parts = append(parts, "\""+f+"\"")
	}
	return "search " + strings.Join(parts, " ")
}

func mpdEscapeFilter(s string) string {
	return strings.ReplaceAll(strings.ReplaceAll(s, "\\", "\\\\"), "'", "\\'")
}

func matchesAll(text string, terms []string) bool {
	for _, t := range terms {
		if !strings.Contains(text, t) {
			return false
		}
	}
	return true
}

func searchTextTerms(q string) []string {
	var terms []string
	for _, w := range strings.Fields(q) {
		if _, _, _, ok := parseRatingFilter(w); !ok {
			terms = append(terms, strings.ToLower(w))
		}
	}
	return terms
}

func doSearch(q string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return searchMsg(searchResult{})
		}
		cmd := buildSearchCmd(q)
		lines, err := mpd.cmd(cmd)
		if err != nil {
			return searchMsg(searchResult{})
		}
		groups := parseGroups(lines, "file")
		terms := searchTextTerms(q)

		// Deduplicate albums and collect tracks
		albumSeen := map[string]bool{}
		var albums []albumEntry
		var tracks []trackEntry
		for _, g := range groups {
			tn, _ := strconv.Atoi(g["Track"])
			r, _ := strconv.Atoi(g["X-Rating"])
			dur, _ := strconv.ParseFloat(g["duration"], 64)
			if dur == 0 {
				dur, _ = strconv.ParseFloat(g["Time"], 64)
			}
			tracks = append(tracks, trackEntry{
				ID:          g["file"],
				XSongID:     g["X-SongId"],
				Title:       g["Title"],
				Artist:      g["Artist"],
				Album:       g["Album"],
				TrackNumber: tn,
				Rating:      r,
				Duration:    dur,
			})
			aa := g["AlbumArtist"]
			if aa == "" {
				aa = g["Artist"]
			}
			key := aa + "\x00" + g["Album"] + "\x00" + g["Date"]
			if !albumSeen[key] && g["Album"] != "" {
				// Only show album if artist or album name matches the text query
				albumText := strings.ToLower(aa + " " + g["Album"])
				if matchesAll(albumText, terms) {
					albumSeen[key] = true
					albums = append(albums, albumEntry{
						ID:          key,
						AlbumArtist: aa,
						Album:       g["Album"],
						Date:        g["Date"],
					})
				}
			}
		}
		sort.Slice(tracks, func(i, j int) bool {
			if tracks[i].Album != tracks[j].Album {
				return tracks[i].Album < tracks[j].Album
			}
			return tracks[i].TrackNumber < tracks[j].TrackNumber
		})
		return searchMsg(searchResult{Albums: albums, Tracks: tracks})
	}
}

func fetchDevices() tea.Msg {
	if mpd == nil {
		return devicesMsg{}
	}
	lines, err := mpd.cmd("outputs")
	if err != nil {
		return devicesMsg{}
	}
	groups := parseGroups(lines, "outputid")
	var devs []deviceInfo
	active := -1
	for _, g := range groups {
		d := deviceInfo{
			ID:      g["outputid"],
			Name:    g["outputname"],
			Enabled: g["outputenabled"] == "1",
		}
		if d.Enabled {
			id, _ := strconv.Atoi(d.ID)
			active = id
		}
		devs = append(devs, d)
	}
	return devicesMsg{devices: devs, active: active}
}

func setActiveDevice(id string) tea.Cmd {
	return func() tea.Msg {
		if mpd != nil {
			mpd.cmd("enableoutput " + id)
		}
		return fetchDevices()
	}
}

func mpdCommand(cmds ...string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return fetchStatus()
		}
		for _, c := range cmds {
			mpd.cmd(c)
		}
		return fetchStatus()
	}
}

func volumeDeltaKey(msg tea.KeyMsg, key string) (int, bool) {
	switch key {
	case "+", "=", "plus", "kp+", "shift+=":
		return 5, true
	case "-", "_", "minus", "kp-":
		return -5, true
	}

	if msg.Type == tea.KeyRunes && len(msg.Runes) == 1 {
		switch msg.Runes[0] {
		case '+', '=':
			return 5, true
		case '-', '_':
			return -5, true
		}
	}

	return 0, false
}

func rateTrack(songID, rating string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return fetchStatus()
		}
		mpd.cmd("rate " + songID + " " + rating)
		return fetchStatus()
	}
}

func rateTracks(songIDs []string, rating string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return fetchStatus()
		}
		for _, id := range songIDs {
			mpd.cmd("rate " + id + " " + rating)
		}
		return fetchStatus()
	}
}

func rateAlbum(albumArtist, album, date, rating string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return albumRatingMsg{}
		}
		mpd.cmd("albumrate " + mpdEscape(albumArtist) + " " + mpdEscape(album) + " " + mpdEscape(date) + " " + rating)
		// Re-fetch the album rating
		lines, err := mpd.cmd("getalbumrating " + mpdEscape(albumArtist) + " " + mpdEscape(album) + " " + mpdEscape(date))
		if err != nil {
			return albumRatingMsg{}
		}
		var r int
		var c float64
		for _, l := range lines {
			if strings.HasPrefix(l, "rating: ") {
				r, _ = strconv.Atoi(strings.TrimPrefix(l, "rating: "))
			}
			if strings.HasPrefix(l, "computed: ") {
				c, _ = strconv.ParseFloat(strings.TrimPrefix(l, "computed: "), 64)
			}
		}
		return albumRatingMsg{rating: r, computed: c}
	}
}

func doRandomAlbum() tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return fetchStatus()
		}
		// Get all albums
		lines, err := mpd.cmd("list Album group AlbumArtist group Date")
		if err != nil {
			return fetchStatus()
		}
		groups := parseGroups(lines, "Album")
		if len(groups) == 0 {
			return fetchStatus()
		}
		pick := groups[rand.Intn(len(groups))]
		artist := pick["AlbumArtist"]
		album := pick["Album"]

		mpd.cmd("clear")
		mpd.cmd("findadd " + mpdFilterEq("AlbumArtist", artist) + " " + mpdFilterEq("Album", album))
		mpd.cmd("play")
		return fetchStatus()
	}
}

func doRandomTracks() tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return fetchStatus()
		}
		// Get all files
		lines, err := mpd.cmd("listall")
		if err != nil {
			return fetchStatus()
		}
		var files []string
		for _, l := range lines {
			if k, v, ok := strings.Cut(l, ": "); ok && k == "file" {
				files = append(files, v)
			}
		}
		if len(files) == 0 {
			return fetchStatus()
		}
		// Shuffle and pick up to 50
		rand.Shuffle(len(files), func(i, j int) { files[i], files[j] = files[j], files[i] })
		n := 50
		if n > len(files) {
			n = len(files)
		}
		mpd.cmd("clear")
		for _, f := range files[:n] {
			mpd.cmd("add " + mpdEscape(f))
		}
		mpd.cmd("play")
		return fetchStatus()
	}
}

func addToQueue(uri, mode string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return fetchStatus()
		}
		switch mode {
		case "replace":
			mpd.cmd("clear")
			mpd.cmd("add " + mpdEscape(uri))
			mpd.cmd("play")
		case "insert":
			// Insert after current song
			lines, _ := mpd.cmd("status")
			st := parseKV(lines)
			pos := -1
			if v, ok := st["song"]; ok {
				pos, _ = strconv.Atoi(v)
			}
			result, _ := mpd.cmd("addid " + mpdEscape(uri))
			if pos >= 0 && len(result) > 0 {
				kv := parseKV(result)
				if id, ok := kv["Id"]; ok {
					mpd.cmd(fmt.Sprintf("moveid %s %d", id, pos+1))
				}
			}
		default: // "add"
			mpd.cmd("add " + mpdEscape(uri))
		}
		return fetchStatus()
	}
}

func addAlbumToQueue(albumID, mode string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return fetchStatus()
		}
		parts := strings.SplitN(albumID, "\x00", 3)
		if len(parts) < 3 {
			return fetchStatus()
		}
		artist, album, date := parts[0], parts[1], parts[2]
		filterArgs := mpdFilterEq("AlbumArtist", artist) + " " + mpdFilterEq("Album", album)
		if date != "" {
			filterArgs += " " + mpdFilterEq("Date", date)
		}

		switch mode {
		case "replace":
			mpd.cmd("clear")
			mpd.cmd("findadd " + filterArgs)
			mpd.cmd("play")
		case "insert":
			// findadd doesn't support position, so find + addid each
			lines, _ := mpd.cmd("find " + filterArgs)
			groups := parseGroups(lines, "file")
			stLines, _ := mpd.cmd("status")
			st := parseKV(stLines)
			pos := -1
			if v, ok := st["song"]; ok {
				pos, _ = strconv.Atoi(v)
			}
			for i, g := range groups {
				result, _ := mpd.cmd("addid " + mpdEscape(g["file"]))
				if pos >= 0 && len(result) > 0 {
					kv := parseKV(result)
					if id, ok := kv["Id"]; ok {
						mpd.cmd(fmt.Sprintf("moveid %s %d", id, pos+1+i))
					}
				}
			}
		default:
			mpd.cmd("findadd " + filterArgs)
		}
		return fetchStatus()
	}
}

func addArtistToQueue(artist, mode string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return fetchStatus()
		}
		filterArg := mpdFilterEq("AlbumArtist", artist)
		switch mode {
		case "replace":
			mpd.cmd("clear")
			mpd.cmd("findadd " + filterArg)
			mpd.cmd("play")
		case "insert":
			lines, _ := mpd.cmd("find " + filterArg)
			groups := parseGroups(lines, "file")
			stLines, _ := mpd.cmd("status")
			st := parseKV(stLines)
			pos := -1
			if v, ok := st["song"]; ok {
				pos, _ = strconv.Atoi(v)
			}
			for i, g := range groups {
				result, _ := mpd.cmd("addid " + mpdEscape(g["file"]))
				if pos >= 0 && len(result) > 0 {
					kv := parseKV(result)
					if id, ok := kv["Id"]; ok {
						mpd.cmd(fmt.Sprintf("moveid %s %d", id, pos+1+i))
					}
				}
			}
		default:
			mpd.cmd("findadd " + filterArg)
		}
		return fetchStatus()
	}
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.width = msg.Width
		m.height = msg.Height
		return m, nil

	case tickMsg:
		// Tick is only for elapsed time interpolation redraws — no status fetch
		if m.status.State == "play" {
			m.status.TimePos += time.Since(statusFetchTime).Seconds()
			statusFetchTime = time.Now()
			if m.status.TimePos > m.status.Dur && m.status.Dur > 0 {
				m.status.TimePos = m.status.Dur
			}
		}
		return m, tickCmd()

	case idleMsg:
		// On rating changes, force queue refetch to pick up new X-Rating values
		for _, sub := range []string(msg) {
			if sub == "rating" {
				forceQueueRefresh = true
				break
			}
		}
		// Server notified us of changes — fetch status immediately
		return m, tea.Batch(tea.Cmd(fetchStatus), tea.Cmd(listenIdle))

	case statusMsg:
		m.status = msg.status
		m.queueVersion = msg.queueVersion
		if msg.queueChanged {
			newFirstID := ""
			if len(msg.queue) > 0 {
				newFirstID = msg.queue[0].SongID
			}
			if newFirstID != m.qFirstSongID {
				m.qCursor = 0
				m.qOffset = 0
				m.qSelected = nil
				m.qFirstSongID = newFirstID
			}
			m.queue = msg.queue
		} else if len(m.queue) > 0 {
			// Update current position markers without refetching queue
			for i := range m.queue {
				m.queue[i].Current = m.queue[i].Position == m.status.SongPos
			}
		}
		// On reconnect, refetch library data
		if msg.reconnected {
			return m, tea.Batch(
				tea.Cmd(fetchArtists),
				fetchNPAlbumRating(m.status.AlbumArtist, m.status.Album, m.status.Date),
			)
		}
		// Fetch album art if track changed
		curFile := ""
		for _, q := range m.queue {
			if q.Current {
				curFile = q.File
				break
			}
		}
		if curFile != "" && curFile != m.artFile {
			cmds := []tea.Cmd{
				fetchAlbumArt(curFile),
				fetchNPAlbumRating(m.status.AlbumArtist, m.status.Album, m.status.Date),
			}
			if (m.showLyrics || m.showNowPlaying) && curFile != m.lyricsFile {
				m.lyrics = nil
				m.lyricsFile = curFile
				m.lyricsScroll = 0
				cmds = append(cmds, fetchLyrics(curFile))
			}
			return m, tea.Batch(cmds...)
		}
		return m, nil

	case albumArtMsg:
		m.artFile = msg.file
		m.artW = msg.w
		m.artH = msg.h
		artTxFile = ""
		artTxCols = 0
		artTxRows = 0
		if len(msg.data) > 0 {
			m.artData = msg.data
			m.artRGBA, m.artW, m.artH = prepareArtRGBA(msg.data, m.npAlbumRating)
		} else {
			m.artData = nil
			m.artRGBA = nil
		}
		return m, nil

	case artistsMsg:
		m.artists = msg
		m.libCursor = 0
		m.libOffset = 0
		m.libFiltering = false
		m.libFilter = ""
		m.libFiltered = nil
		return m, nil

	case albumsMsg:
		m.albums = msg
		m.libMode = libAlbums
		m.libCursor = 0
		m.libOffset = 0
		m.libFiltering = false
		m.libFilter = ""
		m.libFiltered = nil
		return m, tea.ClearScreen

	case tracksMsg:
		m.tracks = msg
		m.libMode = libTracks
		m.libCursor = 0
		m.libOffset = 0
		m.libFiltering = false
		m.libFilter = ""
		m.libFiltered = nil
		return m, tea.ClearScreen

	case trackInfoMsg:
		m.trackInfo = msg
		return m, nil

	case lyricsMsg:
		m.lyrics = msg.lines
		m.lyricsType = msg.lyricsType
		m.lyricsFile = msg.file
		return m, nil

	case gotoMetaMsg:
		if msg.albumArtist == "" && msg.artist == "" {
			m.showGoto = false
			return m, nil
		}
		m.gotoArtist = msg.artist
		m.gotoAlbumArtist = msg.albumArtist
		m.gotoAlbum = msg.album
		m.gotoDate = msg.date
		return m, nil

	case npAlbumRatingMsg:
		m.npAlbumRating = msg.rating
		// Re-prepare art with updated rating burned in
		if m.artData != nil {
			artTxFile = ""
			artTxCols = 0
			artTxRows = 0
			m.artRGBA, m.artW, m.artH = prepareArtRGBA(m.artData, m.npAlbumRating)
		}
		return m, nil

	case albumRatingMsg:
		m.albumRating = msg.rating
		m.albumComputedRating = msg.computed
		// If this is the now-playing album, update art with new rating
		isNP := false
		if m.ratingAlbum != nil {
			isNP = m.ratingAlbum.AlbumArtist == m.status.AlbumArtist && m.ratingAlbum.Album == m.status.Album && m.ratingAlbum.Date == m.status.Date
		} else if m.curAlbum != nil {
			isNP = m.curAlbum.AlbumArtist == m.status.AlbumArtist && m.curAlbum.Album == m.status.Album && m.curAlbum.Date == m.status.Date
		}
		if isNP {
			m.npAlbumRating = msg.rating
			if m.artData != nil {
				artTxFile = ""
				artTxCols = 0
				artTxRows = 0
				m.artRGBA, m.artW, m.artH = prepareArtRGBA(m.artData, m.npAlbumRating)
			}
		}
		// Update the album entry in the albums list so the view refreshes
		if m.ratingAlbum != nil {
			for i := range m.albums {
				if m.albums[i].AlbumArtist == m.ratingAlbum.AlbumArtist &&
					m.albums[i].Album == m.ratingAlbum.Album &&
					m.albums[i].Date == m.ratingAlbum.Date {
					m.albums[i].Rating = msg.rating
					m.albums[i].Computed = msg.computed
					break
				}
			}
		} else if m.curAlbum != nil {
			for i := range m.albums {
				if m.albums[i].AlbumArtist == m.curAlbum.AlbumArtist &&
					m.albums[i].Album == m.curAlbum.Album &&
					m.albums[i].Date == m.curAlbum.Date {
					m.albums[i].Rating = msg.rating
					m.albums[i].Computed = msg.computed
					break
				}
			}
		}
		return m, nil

	case ratingPopupMsg:
		if m.showRating {
			m.ratingCursor = msg.rating
		}
		return m, nil

	case playlistsMsg:
		m.playlists = msg
		m.libMode = libPlaylists
		m.libCursor = 0
		m.libOffset = 0
		m.libFiltering = false
		m.libFilter = ""
		m.libFiltered = nil
		return m, tea.ClearScreen

	case playlistTracksMsg:
		m.playlistTracks = msg
		m.libMode = libPlaylistTracks
		m.libCursor = 0
		m.libOffset = 0
		m.libFiltering = false
		m.libFilter = ""
		m.libFiltered = nil
		return m, tea.ClearScreen

	case plPickerReadyMsg:
		m.plPickerList = msg
		m.showPlPicker = true
		m.plPickerCursor = 0
		m.plPickerNewMode = false
		return m, nil

	case searchMsg:
		m.searchRes = searchResult(msg)
		m.srTotal = len(m.searchRes.Albums) + len(m.searchRes.Tracks)
		m.srCursor = 0
		return m, nil

	case searchDebounceMsg:
		if msg.gen == m.searchDebounce && m.searchPending != "" {
			q := m.searchPending
			m.searchPending = ""
			return m, doSearch(q)
		}
		return m, nil

	case devicesMsg:
		m.devices = msg.devices
		m.activeDevice = msg.active
		return m, nil

	case tea.KeyMsg:
		return m.handleKey(msg)

	case tea.MouseMsg:
		if msg.Action == tea.MouseActionPress && msg.Button == tea.MouseButtonLeft {
			seekY := m.height - 1
			if (msg.Y >= seekY-3 && msg.Y <= seekY+1) && m.status.Dur > 0 {
				// Offset for album art on the left
				artOffset := 0
				if len(m.artRGBA) > 0 {
					artOffset = 4*2 + 1 // artCols (rows*2) + gap
				}
				posStr := fmtTime(m.status.TimePos)
				durStr := fmtTime(m.status.Dur)
				infoW := m.width - artOffset
				if infoW < 20 {
					infoW = 20
				}
				barStart := artOffset + len(posStr) + 1
				barW := infoW - len(posStr) - len(durStr) - 6
				if barW < 5 {
					barW = 5
				}
				x := msg.X - barStart
				if x >= 0 && x <= barW {
					pos := float64(x) / float64(barW) * m.status.Dur
					return m, mpdCommand(fmt.Sprintf("seekcur %.1f", pos))
				}
			}
		}
	}

	if m.searching {
		var cmd tea.Cmd
		m.searchInput, cmd = m.searchInput.Update(msg)
		return m, cmd
	}
	return m, nil
}

func (m model) handleKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	key := msg.String()

	if key == "ctrl+c" {
		return m, tea.Quit
	}
	if key == "q" && !m.searching && !m.showMenu && !m.showHelp && !m.showRating && !m.showTrackInfo && !m.showNowPlaying && !m.showModes && !m.showPrioMenu && !m.libFiltering {
		return m, tea.Quit
	}

	if !m.searching && !m.libFiltering && !m.showPlPicker {
		if delta, ok := volumeDeltaKey(msg, key); ok {
			return m, mpdCommand(fmt.Sprintf("volume %+d", delta))
		}
	}

	if m.showHelp {
		m.showHelp = false
		return m, nil
	}

	if m.showTrackInfo {
		m.showTrackInfo = false
		return m, nil
	}

	if m.showNowPlaying {
		switch key {
		case "down":
			if m.lyricsScroll < len(m.lyrics)-1 {
				m.lyricsScroll++
			}
		case "up":
			if m.lyricsScroll > 0 {
				m.lyricsScroll--
			}
		case "pgdown":
			m.lyricsScroll += 20
			if m.lyricsScroll >= len(m.lyrics) {
				m.lyricsScroll = len(m.lyrics) - 1
			}
		case "pgup":
			m.lyricsScroll -= 20
			if m.lyricsScroll < 0 {
				m.lyricsScroll = 0
			}
		case "f":
			if m.lyricsType == "synced" {
				m.lyricsScroll = m.currentLyricsLine()
			}
		default:
			m.showNowPlaying = false
		}
		return m, nil
	}

	if m.showGoto {
		return m.handleGotoKey(key)
	}

	if m.showRating {
		return m.handleRatingKey(key)
	}

	if m.showPlPicker {
		return m.handlePlPickerKey(msg, key)
	}

	if m.showPrioMenu {
		return m.handlePrioKey(key)
	}

	if m.showModes {
		return m.handleModesKey(key)
	}

	if m.showDevices {
		return m.handleDeviceKey(key)
	}

	if m.showMenu {
		return m.handleMenuKey(key)
	}

	if m.searching {
		return m.handleSearchKey(msg, key)
	}

	// Library filter mode — intercept all keys
	if m.libFiltering {
		return m.handleLibFilterKey(msg, key)
	}

	// Global hotkeys
	switch key {
	case "/":
		m.searching = true
		m.searchInput.SetValue("")
		m.searchInput.Focus()
		m.searchRes = searchResult{}
		m.srCursor = 0
		m.srTotal = 0
		return m, textinput.Blink
	case " ":
		return m, mpdCommand("pause")
	case ">":
		return m, mpdCommand("next")
	case "<":
		return m, mpdCommand("previous")
	case "s":
		return m, mpdCommand("stop")
	case "r":
		return m, doRandomAlbum()
	case "R":
		return m, doRandomTracks()
	case "*":
		m.showRating = true
		m.ratingIsAlbum = false
		if m.focus == panelQueue && m.qCursor < len(m.queue) {
			m.ratingCursor = m.queue[m.qCursor].Rating
		} else if m.focus == panelLibrary && m.libMode == libAlbums {
			di := m.dataIndex()
			if di >= 0 && di < len(m.albums) {
				m.ratingIsAlbum = true
				m.ratingCursor = 0
				a := m.albums[di]
				m.ratingAlbum = &a
				return m, fetchAlbumRatingForPopup(a.AlbumArtist, a.Album, a.Date)
			}
		} else if m.focus == panelLibrary && m.libMode == libTracks {
			di := m.dataIndex()
			if di >= 0 && di < len(m.tracks) && m.tracks[di].XSongID != "" {
				m.ratingCursor = m.tracks[di].Rating
			} else {
				m.ratingCursor = m.status.Rating
			}
		} else {
			m.ratingCursor = m.status.Rating
		}
		return m, nil
	case "u":
		return m, mpdCommand("update")
	case "ctrl+g":
		// Cycle ReplayGain mode: off → track → album → off
		var nextMode string
		switch m.status.ReplayGainMode {
		case "track":
			nextMode = "album"
		case "album":
			nextMode = "off"
		default:
			nextMode = "track"
		}
		return m, mpdCommand("replay_gain_mode " + nextMode)
	case "i":
		file := m.focusedTrackFile()
		if file != "" {
			m.showTrackInfo = true
			m.trackInfo = nil
			return m, fetchTrackInfo(file)
		}
		return m, nil
		return m, nil
	case "l":
		m.showLyrics = !m.showLyrics
		if m.showLyrics {
			file := m.status.File
			if file != "" && m.lyricsFile != file {
				m.lyrics = nil
				m.lyricsFile = file
				m.lyricsScroll = 0
				return m, fetchLyrics(file)
			}
		}
		return m, nil
	case "L":
		file := m.status.File
		if file == "" {
			return m, nil
		}
		if m.showNowPlaying {
			m.showNowPlaying = false
			return m, nil
		}
		m.showNowPlaying = true
		// Also fetch lyrics if not already loaded for this track
		if m.lyricsFile != file {
			m.lyrics = nil
			m.lyricsFile = file
			m.lyricsScroll = 0
			return m, fetchLyrics(file)
		}
		return m, nil
	case "o":
		file := m.focusedTrackFile()
		if file == "" {
			return m, nil
		}
		m.showGoto = true
		m.gotoCursor = 0
		// If we're in library tracks view, we already have the metadata
		if m.focus == panelLibrary && m.libMode == libTracks && m.curAlbum != nil {
			m.gotoArtist = m.curAlbum.AlbumArtist
			m.gotoAlbumArtist = m.curAlbum.AlbumArtist
			m.gotoAlbum = m.curAlbum.Album
			m.gotoDate = m.curAlbum.Date
			return m, nil
		}
		// Otherwise fetch metadata from server
		m.gotoArtist = ""
		m.gotoAlbumArtist = ""
		m.gotoAlbum = ""
		m.gotoDate = ""
		return m, fetchGotoMeta(file)
	case "?":
		m.showHelp = true
		return m, nil
	case "P":
		return m, fetchPlaylists
	case "M":
		m.showModes = true
		m.modesCursor = 0
		return m, nil
	case "D":
		m.showDevices = true
		m.devCursor = 0
		return m, tea.Cmd(fetchDevices)
	case "tab":
		if m.focus == panelLibrary {
			m.focus = panelQueue
		} else {
			m.focus = panelLibrary
		}
		return m, nil
	}

	if m.focus == panelLibrary {
		return m.handleLibKey(key)
	}
	return m.handleQueueKey(key)
}

var menuOptions = []string{"Add to queue", "Add with priority", "Insert after current", "Replace queue", "Browse into"}

func (m model) menuOptionCount() int {
	if m.menuSource == "search" {
		idx := m.srCursor
		nAlbums := len(m.searchRes.Albums)
		if idx < nAlbums {
			return 5 // all options including Browse into
		}
		return 4 // tracks: no Browse into
	}
	if m.libMode == libTracks || m.libMode == libPlaylistTracks {
		return 4 // tracks: no Browse into
	}
	return len(menuOptions)
}

func (m model) handleMenuKey(key string) (tea.Model, tea.Cmd) {
	maxIdx := m.menuOptionCount() - 1
	switch key {
	case "esc", "q":
		m.showMenu = false
		return m, nil
	case "down":
		if m.menuCursor < maxIdx {
			m.menuCursor++
		}
		return m, nil
	case "up":
		if m.menuCursor > 0 {
			m.menuCursor--
		}
		return m, nil
	case "enter":
		m.showMenu = false
		if m.menuSource == "search" {
			switch m.menuCursor {
			case 0:
				return m.searchAction("add")
			case 1:
				return m.searchPrioAction()
			case 2:
				return m.searchAction("insert")
			case 3:
				return m.searchAction("replace")
			case 4:
				return m.searchDrillIn()
			}
		} else {
			switch m.menuCursor {
			case 0:
				return m.libAction("add")
			case 1:
				return m.libPrioAction()
			case 2:
				return m.libAction("insert")
			case 3:
				return m.libAction("replace")
			case 4:
				return m.libDrillIn()
			}
		}
	}
	return m, nil
}

func (m model) handleLibKey(key string) (tea.Model, tea.Cmd) {
	listLen := m.libListLen()

	switch key {
	case "down":
		if m.libCursor < listLen-1 {
			m.libCursor++
		}
		return m, nil
	case "up":
		if m.libCursor > 0 {
			m.libCursor--
		}
		return m, nil
	case "home":
		m.libCursor = 0
		return m, nil
	case "end":
		if listLen > 0 {
			m.libCursor = listLen - 1
		}
		return m, nil
	case "pgdown":
		m.libCursor += 20
		if m.libCursor >= listLen {
			m.libCursor = listLen - 1
		}
		return m, nil
	case "pgup":
		m.libCursor -= 20
		if m.libCursor < 0 {
			m.libCursor = 0
		}
		return m, nil
	case "f":
		m.libFiltering = true
		m.libFilter = ""
		m.libFiltered = nil
		m.libCursor = 0
		m.libOffset = 0
		return m, nil
	case "enter":
		di := m.dataIndex()
		if di < 0 {
			return m.libBack()
		}
		m.showMenu = true
		m.menuCursor = 0
		m.menuSource = "library"
		return m, nil
	case "right":
		return m.libDrillIn()
	case "left", "backspace":
		return m.libBack()
	case "a":
		return m.libAction("add")
	case "A":
		return m.libAction("replace")
	case "i":
		return m.libAction("insert")
	case "p":
		di := m.dataIndex()
		if di >= 0 {
			var uri string
			switch m.libMode {
			case libTracks:
				if di < len(m.tracks) {
					uri = m.tracks[di].ID
				}
			case libPlaylistTracks:
				if di < len(m.playlistTracks) {
					uri = m.playlistTracks[di].ID
				}
			}
			if uri != "" {
				m.plPickerURI = uri
				return m, fetchPlPickerPlaylists(uri)
			}
		}
	case "S":
		if m.libMode == libArtists && !m.libSortLatest {
			// Toggle on: switch to latest-sorted album list
			m.libSortLatest = true
			m.savedArtistCursor = m.libCursor
			m.savedArtistOffset = m.libOffset
			m.libCursor = 0
			m.libOffset = 0
			m.libFiltering = false
			m.libFilter = ""
			m.libFiltered = nil
			return m, fetchAllAlbumsLatest
		} else if m.libMode == libAlbums && m.libSortLatest {
			// Toggle off: go back to artist list
			m.libSortLatest = false
			m.libMode = libArtists
			m.libCursor = m.savedArtistCursor
			m.libOffset = m.savedArtistOffset
			m.libFiltering = false
			m.libFilter = ""
			m.libFiltered = nil
			return m, tea.ClearScreen
		}
	}
	return m, nil
}

func (m model) handleLibFilterKey(msg tea.KeyMsg, key string) (tea.Model, tea.Cmd) {
	listLen := m.libListLen()

	switch key {
	case "esc":
		m.libFiltering = false
		m.libFilter = ""
		m.libFiltered = nil
		m.libCursor = 0
		m.libOffset = 0
		return m, nil
	case "backspace":
		if len(m.libFilter) > 0 {
			m.libFilter = m.libFilter[:len(m.libFilter)-1]
			m.libCursor = 0
			m.libOffset = 0
			m.rebuildLibFilter()
		} else {
			// Empty filter + backspace exits filter mode
			m.libFiltering = false
			m.libCursor = 0
			m.libOffset = 0
		}
		return m, nil
	case "down", "ctrl+n":
		if m.libCursor < listLen-1 {
			m.libCursor++
		}
		return m, nil
	case "up", "ctrl+p":
		if m.libCursor > 0 {
			m.libCursor--
		}
		return m, nil
	case "home":
		m.libCursor = 0
		return m, nil
	case "end":
		if listLen > 0 {
			m.libCursor = listLen - 1
		}
		return m, nil
	case "pgdown":
		m.libCursor += 20
		if m.libCursor >= listLen {
			m.libCursor = listLen - 1
		}
		return m, nil
	case "pgup":
		m.libCursor -= 20
		if m.libCursor < 0 {
			m.libCursor = 0
		}
		return m, nil
	case "enter":
		// Show popup menu (same as normal mode)
		di := m.dataIndex()
		if di < 0 {
			return m, nil
		}
		m.showMenu = true
		m.menuCursor = 0
		m.menuSource = "library"
		return m, nil
	case "right":
		di := m.dataIndex()
		if di < 0 {
			return m, nil
		}
		m.libFiltering = false
		m.libFilter = ""
		m.libFiltered = nil
		m.libCursor = di
		return m.libDrillIn()
	case "left":
		m.libFiltering = false
		m.libFilter = ""
		m.libFiltered = nil
		return m.libBack()
	default:
		// Printable chars extend filter
		if len(key) == 1 && key[0] >= 32 && key[0] < 127 {
			m.libFilter += key
			m.libCursor = 0
			m.libOffset = 0
			m.rebuildLibFilter()
			return m, nil
		}
	}
	return m, nil
}

func (m *model) rebuildLibFilter() {
	if m.libFilter == "" {
		m.libFiltered = nil
		return
	}
	query := strings.ToLower(m.libFilter)
	var indices []int
	switch m.libMode {
	case libArtists:
		for i, a := range m.artists {
			if strings.Contains(strings.ToLower(a), query) {
				indices = append(indices, i)
			}
		}
	case libAlbums:
		for i, a := range m.albums {
			var label string
			if m.libSortLatest {
				label = a.AlbumArtist + " - " + a.Album
				if a.Date != "" {
					label = a.Date + " " + label
				}
			} else {
				label = a.Album
				if a.Date != "" {
					label = a.Date + " " + a.Album
				}
			}
			if strings.Contains(strings.ToLower(label), query) {
				indices = append(indices, i)
			}
		}
	case libTracks:
		for i, t := range m.tracks {
			if strings.Contains(strings.ToLower(t.Title), query) {
				indices = append(indices, i)
			}
		}
	case libPlaylists:
		for i, pl := range m.playlists {
			if strings.Contains(strings.ToLower(pl.Name), query) {
				indices = append(indices, i)
			}
		}
	case libPlaylistTracks:
		for i, t := range m.playlistTracks {
			label := t.Title + " " + t.Artist
			if strings.Contains(strings.ToLower(label), query) {
				indices = append(indices, i)
			}
		}
	}
	m.libFiltered = indices
	if m.libCursor >= len(indices) {
		m.libCursor = 0
	}
}

func (m model) libListLen() int {
	if m.libFilter != "" {
		return len(m.libFiltered)
	}
	switch m.libMode {
	case libArtists:
		return len(m.artists)
	case libAlbums:
		return len(m.albums)
	case libTracks:
		return len(m.tracks)
	case libPlaylists:
		return len(m.playlists)
	case libPlaylistTracks:
		return len(m.playlistTracks)
	}
	return 0
}

func (m model) focusedTrackFile() string {
	if m.focus == panelQueue && m.qCursor < len(m.queue) {
		return m.queue[m.qCursor].File
	}
	if m.focus == panelLibrary {
		switch m.libMode {
		case libTracks:
			di := m.dataIndex()
			if di >= 0 && di < len(m.tracks) {
				return m.tracks[di].ID
			}
		case libPlaylistTracks:
			di := m.dataIndex()
			if di >= 0 && di < len(m.playlistTracks) {
				return m.playlistTracks[di].ID
			}
		}
	}
	return ""
}

func (m model) dataIndex() int {
	if m.libFilter != "" && len(m.libFiltered) > 0 {
		if m.libCursor < len(m.libFiltered) {
			return m.libFiltered[m.libCursor]
		}
		return -1
	}
	return m.libCursor
}

func (m model) libDrillIn() (tea.Model, tea.Cmd) {
	di := m.dataIndex()
	if di < 0 {
		return m.libBack()
	}
	switch m.libMode {
	case libArtists:
		if di < len(m.artists) {
			m.savedArtistCursor = m.libCursor
			m.savedArtistOffset = m.libOffset
			m.curArtist = m.artists[di]
			return m, fetchAlbums(m.curArtist)
		}
	case libAlbums:
		if di < len(m.albums) {
			m.savedAlbumCursor = m.libCursor
			m.savedAlbumOffset = m.libOffset
			a := m.albums[di]
			m.curAlbum = &a
			m.curArtist = a.AlbumArtist
			m.albumRating = 0
			m.albumComputedRating = 0
			return m, tea.Batch(fetchTracks(a.ID), fetchAlbumRating(a.AlbumArtist, a.Album, a.Date))
		}
	case libTracks:
		if di < len(m.tracks) {
			return m, addToQueue(m.tracks[di].ID, "add")
		}
	case libPlaylists:
		if di < len(m.playlists) {
			m.savedPlCursor = m.libCursor
			m.savedPlOffset = m.libOffset
			m.curPlaylist = m.playlists[di].Name
			return m, fetchPlaylistTracks(m.curPlaylist)
		}
	case libPlaylistTracks:
		if di < len(m.playlistTracks) {
			return m, addToQueue(m.playlistTracks[di].ID, "add")
		}
	}
	return m, nil
}

func (m model) libBack() (tea.Model, tea.Cmd) {
	switch m.libMode {
	case libAlbums:
		if m.libSortLatest {
			// Back from latest-sorted albums returns to artist list with sort off
			m.libSortLatest = false
		}
		m.libMode = libArtists
		m.libCursor = m.savedArtistCursor
		m.libOffset = m.savedArtistOffset
	case libTracks:
		if m.libSortLatest {
			// Back from tracks in latest mode returns to the latest album list
			m.libMode = libAlbums
			m.libCursor = m.savedAlbumCursor
			m.libOffset = m.savedAlbumOffset
			return m, tea.ClearScreen
		}
		m.libMode = libAlbums
		m.libCursor = m.savedAlbumCursor
		m.libOffset = m.savedAlbumOffset
	case libPlaylists:
		m.libMode = libArtists
		m.libCursor = m.savedArtistCursor
		m.libOffset = m.savedArtistOffset
	case libPlaylistTracks:
		m.libMode = libPlaylists
		m.libCursor = m.savedPlCursor
		m.libOffset = m.savedPlOffset
		m.curAlbum = nil
		m.albumRating = 0
		m.albumComputedRating = 0
	}
	return m, tea.ClearScreen
}

func (m model) libAction(mode string) (tea.Model, tea.Cmd) {
	di := m.dataIndex()
	if di < 0 {
		return m, nil
	}
	switch m.libMode {
	case libArtists:
		if di < len(m.artists) {
			return m, addArtistToQueue(m.artists[di], mode)
		}
	case libAlbums:
		if di < len(m.albums) {
			return m, addAlbumToQueue(m.albums[di].ID, mode)
		}
	case libTracks:
		if di < len(m.tracks) {
			return m, addToQueue(m.tracks[di].ID, mode)
		}
	case libPlaylists:
		if di < len(m.playlists) {
			return m, loadPlaylist(m.playlists[di].Name, mode)
		}
	case libPlaylistTracks:
		if di < len(m.playlistTracks) {
			return m, addToQueue(m.playlistTracks[di].ID, mode)
		}
	}
	return m, nil
}

func (m model) handleQueueKey(key string) (tea.Model, tea.Cmd) {
	if m.confirmClear {
		m.confirmClear = false
		if key == "y" || key == "Y" {
			return m, mpdCommand("clear")
		}
		return m, nil
	}
	qLen := len(m.queue)
	switch key {
	case "down":
		if m.qCursor < qLen-1 {
			m.qCursor++
		}
	case "up":
		if m.qCursor > 0 {
			m.qCursor--
		}
	case "home":
		m.qCursor = 0
	case "end":
		if qLen > 0 {
			m.qCursor = qLen - 1
		}
	case "pgdown":
		m.qCursor += 20
		if m.qCursor >= qLen {
			m.qCursor = qLen - 1
		}
		if m.qCursor < 0 {
			m.qCursor = 0
		}
	case "pgup":
		m.qCursor -= 20
		if m.qCursor < 0 {
			m.qCursor = 0
		}
	case "enter":
		if m.qCursor < qLen {
			m.qSelected = nil
			return m, mpdCommand(fmt.Sprintf("play %d", m.qCursor))
		}
	case "v":
		if m.qCursor < qLen {
			if m.qSelected == nil {
				m.qSelected = map[int]bool{}
			}
			if m.qSelected[m.qCursor] {
				delete(m.qSelected, m.qCursor)
			} else {
				m.qSelected[m.qCursor] = true
			}
			if m.qCursor < qLen-1 {
				m.qCursor++
			}
		}
	case "V":
		if m.qCursor < qLen {
			if m.qSelected == nil {
				m.qSelected = map[int]bool{}
			}
			from := m.qCursor
			for i := m.qCursor - 1; i >= 0; i-- {
				if m.qSelected[i] {
					from = i
					break
				}
			}
			lo, hi := from, m.qCursor
			if lo > hi {
				lo, hi = hi, lo
			}
			for i := lo; i <= hi; i++ {
				m.qSelected[i] = true
			}
		}
	case "escape", "esc":
		m.qSelected = nil
	case "d", "delete", "x":
		if len(m.qSelected) > 0 {
			positions := make([]int, 0, len(m.qSelected))
			for pos := range m.qSelected {
				positions = append(positions, pos)
			}
			sort.Sort(sort.Reverse(sort.IntSlice(positions)))
			m.qSelected = nil
			cmds := make([]string, len(positions))
			for i, pos := range positions {
				cmds[i] = fmt.Sprintf("delete %d", pos)
			}
			return m, mpdCommand(cmds...)
		}
		if m.qCursor < qLen {
			return m, mpdCommand(fmt.Sprintf("delete %d", m.qCursor))
		}
	case "p":
		if m.qCursor < qLen {
			uri := m.queue[m.qCursor].File
			if uri != "" {
				m.plPickerURI = uri
				return m, fetchPlPickerPlaylists(uri)
			}
		}
	case "c":
		m.confirmClear = true
		return m, nil
	}
	return m, nil
}

func (m model) handlePlPickerKey(msg tea.KeyMsg, key string) (tea.Model, tea.Cmd) {
	plTotal := len(m.plPickerList) + 1 // +1 for "New Playlist..." entry at index 0

	if m.plPickerNewMode {
		switch key {
		case "esc":
			m.plPickerNewMode = false
			m.plPickerInput.Blur()
			return m, nil
		case "enter":
			name := strings.TrimSpace(m.plPickerInput.Value())
			if name != "" {
				m.showPlPicker = false
				m.plPickerNewMode = false
				m.plPickerInput.Blur()
				return m, addToPlaylist(name, m.plPickerURI)
			}
			return m, nil
		default:
			var cmd tea.Cmd
			m.plPickerInput, cmd = m.plPickerInput.Update(msg)
			return m, cmd
		}
	}

	switch key {
	case "esc", "q":
		m.showPlPicker = false
		return m, nil
	case "down":
		if m.plPickerCursor < plTotal-1 {
			m.plPickerCursor++
		}
	case "up":
		if m.plPickerCursor > 0 {
			m.plPickerCursor--
		}
	case "home":
		m.plPickerCursor = 0
	case "end":
		m.plPickerCursor = plTotal - 1
	case "pgdown":
		m.plPickerCursor += 20
		if m.plPickerCursor >= plTotal {
			m.plPickerCursor = plTotal - 1
		}
	case "pgup":
		m.plPickerCursor -= 20
		if m.plPickerCursor < 0 {
			m.plPickerCursor = 0
		}
	case "enter":
		if m.plPickerCursor == 0 {
			// "New Playlist..." selected
			m.plPickerNewMode = true
			m.plPickerInput.SetValue("")
			m.plPickerInput.Focus()
			return m, textinput.Blink
		}
		plIdx := m.plPickerCursor - 1
		if plIdx < len(m.plPickerList) {
			name := m.plPickerList[plIdx].Name
			m.showPlPicker = false
			return m, addToPlaylist(name, m.plPickerURI)
		}
	}
	return m, nil
}

func (m model) handleSearchKey(msg tea.KeyMsg, key string) (tea.Model, tea.Cmd) {
	switch key {
	case "esc":
		m.searching = false
		return m, nil
	case "up":
		if m.srCursor > 0 {
			m.srCursor--
		}
		return m, nil
	case "down":
		if m.srCursor < m.srTotal-1 {
			m.srCursor++
		}
		return m, nil
	case "home":
		m.srCursor = 0
		return m, nil
	case "end":
		if m.srTotal > 0 {
			m.srCursor = m.srTotal - 1
		}
		return m, nil
	case "pgdown":
		m.srCursor += 20
		if m.srCursor >= m.srTotal {
			m.srCursor = m.srTotal - 1
		}
		if m.srCursor < 0 {
			m.srCursor = 0
		}
		return m, nil
	case "pgup":
		m.srCursor -= 20
		if m.srCursor < 0 {
			m.srCursor = 0
		}
		return m, nil
	case "enter":
		if m.srTotal > 0 {
			m.showMenu = true
			m.menuCursor = 0
			m.menuSource = "search"
		}
		return m, nil
	}

	var cmd tea.Cmd
	prev := m.searchInput.Value()
	m.searchInput, cmd = m.searchInput.Update(msg)
	cur := m.searchInput.Value()
	if cur != prev {
		q := strings.TrimSpace(cur)
		if q != "" {
			m.searchDebounce++
			m.searchPending = q
			gen := m.searchDebounce
			debounceCmd := tea.Tick(300*time.Millisecond, func(time.Time) tea.Msg {
				return searchDebounceMsg{gen: gen}
			})
			return m, tea.Batch(cmd, debounceCmd)
		}
		// Cleared input — reset results
		m.searchRes = searchResult{}
		m.srTotal = 0
		m.srCursor = 0
	}
	return m, cmd
}

func (m model) searchAction(mode string) (tea.Model, tea.Cmd) {
	if m.srTotal == 0 {
		return m, nil
	}
	idx := m.srCursor
	nAlbums := len(m.searchRes.Albums)
	var cmd tea.Cmd
	if idx < nAlbums {
		a := m.searchRes.Albums[idx]
		cmd = addAlbumToQueue(a.ID, mode)
	} else {
		t := m.searchRes.Tracks[idx-nAlbums]
		cmd = addToQueue(t.ID, mode)
	}
	m.searching = false
	return m, cmd
}

func (m model) searchDrillIn() (tea.Model, tea.Cmd) {
	if m.srTotal == 0 {
		return m, nil
	}
	idx := m.srCursor
	nAlbums := len(m.searchRes.Albums)
	if idx < nAlbums {
		a := m.searchRes.Albums[idx]
		m.searching = false
		m.curArtist = a.AlbumArtist
		m.curAlbum = &a
		m.albumRating = 0
		m.albumComputedRating = 0
		return m, tea.Batch(fetchTracks(a.ID), fetchAlbumRating(a.AlbumArtist, a.Album, a.Date))
	}
	return m, nil
}

func (m model) handleDeviceKey(key string) (tea.Model, tea.Cmd) {
	switch key {
	case "esc", "q", "D":
		m.showDevices = false
		return m, nil
	case "down":
		if m.devCursor < len(m.devices)-1 {
			m.devCursor++
		}
		return m, nil
	case "up":
		if m.devCursor > 0 {
			m.devCursor--
		}
		return m, nil
	case "enter":
		if m.devCursor < len(m.devices) {
			dev := m.devices[m.devCursor]
			m.showDevices = false
			return m, setActiveDevice(dev.ID)
		}
	}
	return m, nil
}

func (m model) libPrioAction() (tea.Model, tea.Cmd) {
	di := m.dataIndex()
	if di < 0 {
		return m, nil
	}
	var uri string
	switch m.libMode {
	case libTracks:
		if di < len(m.tracks) {
			uri = m.tracks[di].ID
		}
	case libPlaylistTracks:
		if di < len(m.playlistTracks) {
			uri = m.playlistTracks[di].ID
		}
	}
	if uri != "" {
		m.showPrioMenu = true
		m.prioCursor = 1
		m.prioSourceURI = uri
	}
	return m, nil
}

func (m model) searchPrioAction() (tea.Model, tea.Cmd) {
	if m.srTotal == 0 {
		return m, nil
	}
	idx := m.srCursor
	nAlbums := len(m.searchRes.Albums)
	if idx >= nAlbums {
		t := m.searchRes.Tracks[idx-nAlbums]
		m.showPrioMenu = true
		m.prioCursor = 1
		m.prioSourceURI = t.ID
	}
	return m, nil
}

func (m model) handlePrioKey(key string) (tea.Model, tea.Cmd) {
	switch key {
	case "esc", "q":
		m.showPrioMenu = false
		return m, nil
	case "down":
		if m.prioCursor < 2 {
			m.prioCursor++
		}
		return m, nil
	case "up":
		if m.prioCursor > 0 {
			m.prioCursor--
		}
		return m, nil
	case "enter":
		prio := 10 // Low
		switch m.prioCursor {
		case 1:
			prio = 20 // Medium
		case 2:
			prio = 30 // High
		}
		m.showPrioMenu = false
		uri := m.prioSourceURI
		return m, addWithPriority(uri, prio)
	}
	return m, nil
}

func addWithPriority(uri string, prio int) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return fetchStatus()
		}
		mpd.cmd(fmt.Sprintf("addidprio %s %d", mpdEscape(uri), prio))
		return fetchStatus()
	}
}

func (m model) handleModesKey(key string) (tea.Model, tea.Cmd) {
	switch key {
	case "esc", "q", "M":
		m.showModes = false
		return m, nil
	case "down":
		if m.modesCursor < 4 {
			m.modesCursor++
		}
		return m, nil
	case "up":
		if m.modesCursor > 0 {
			m.modesCursor--
		}
		return m, nil
	case "enter", " ":
		switch m.modesCursor {
		case 0: // ReplayGain — cycle
			var nextMode string
			switch m.status.ReplayGainMode {
			case "track":
				nextMode = "album"
			case "album":
				nextMode = "off"
			default:
				nextMode = "track"
			}
			return m, mpdCommand("replay_gain_mode " + nextMode)
		case 1: // Repeat
			v := "1"
			if m.status.Repeat {
				v = "0"
			}
			return m, mpdCommand("repeat " + v)
		case 2: // Random
			v := "1"
			if m.status.Random {
				v = "0"
			}
			return m, mpdCommand("random " + v)
		case 3: // Single
			v := "1"
			if m.status.Single {
				v = "0"
			}
			return m, mpdCommand("single " + v)
		case 4: // Consume
			v := "1"
			if m.status.Consume {
				v = "0"
			}
			return m, mpdCommand("consume " + v)
		}
	}
	return m, nil
}

func (m model) handleGotoKey(key string) (tea.Model, tea.Cmd) {
	switch key {
	case "esc", "q", "o":
		m.showGoto = false
		return m, nil
	case "down":
		if m.gotoCursor < 2 {
			m.gotoCursor++
		}
		return m, nil
	case "up":
		if m.gotoCursor > 0 {
			m.gotoCursor--
		}
		return m, nil
	case "enter":
		if m.gotoAlbumArtist == "" {
			return m, nil
		}
		m.showGoto = false
		m.libFilter = ""
		m.libFiltered = nil
		m.libFiltering = false
		m.focus = panelLibrary
		switch m.gotoCursor {
		case 0: // Go to Artist
			m.curArtist = m.gotoAlbumArtist
			m.libCursor = 0
			m.libOffset = 0
			return m, fetchAlbums(m.gotoAlbumArtist)
		case 1: // Go to Album
			m.curArtist = m.gotoAlbumArtist
			albumID := m.gotoAlbumArtist + "\x00" + m.gotoAlbum + "\x00" + m.gotoDate
			a := albumEntry{
				AlbumArtist: m.gotoAlbumArtist,
				Album:       m.gotoAlbum,
				Date:        m.gotoDate,
				ID:          albumID,
			}
			m.curAlbum = &a
			m.albumRating = 0
			m.albumComputedRating = 0
			m.libCursor = 0
			m.libOffset = 0
			return m, tea.Batch(fetchTracks(albumID), fetchAlbumRating(m.gotoAlbumArtist, m.gotoAlbum, m.gotoDate))
		case 2: // Search Artist
			m.searching = true
			m.searchInput.SetValue(m.gotoAlbumArtist)
			m.searchInput.Focus()
			m.srCursor = 0
			return m, tea.Batch(textinput.Blink, doSearch(m.gotoAlbumArtist))
		}
	}
	return m, nil
}

func (m model) handleRatingKey(key string) (tea.Model, tea.Cmd) {
	switch key {
	case "esc", "q", "*":
		m.showRating = false
		return m, nil
	case "down":
		if m.ratingCursor < 10 {
			m.ratingCursor++
		}
		return m, nil
	case "up":
		if m.ratingCursor > 0 {
			m.ratingCursor--
		}
		return m, nil
	case "tab":
		// Toggle track/album mode
		m.ratingIsAlbum = !m.ratingIsAlbum
		if m.ratingIsAlbum {
			// Determine album context and fetch its rating
			var aa, al, dt string
			if m.ratingAlbum != nil {
				aa, al, dt = m.ratingAlbum.AlbumArtist, m.ratingAlbum.Album, m.ratingAlbum.Date
			} else if m.curAlbum != nil {
				aa, al, dt = m.curAlbum.AlbumArtist, m.curAlbum.Album, m.curAlbum.Date
				m.ratingCursor = m.albumRating
				return m, nil
			} else if m.status.Album != "" {
				aa, al, dt = m.status.AlbumArtist, m.status.Album, m.status.Date
			}
			if aa != "" {
				m.ratingAlbum = &albumEntry{AlbumArtist: aa, Album: al, Date: dt}
				return m, fetchAlbumRatingForPopup(aa, al, dt)
			}
			// No album context, revert
			m.ratingIsAlbum = false
		} else {
			// Switch back to track
			m.ratingAlbum = nil
			if m.focus == panelQueue && m.qCursor < len(m.queue) {
				m.ratingCursor = m.queue[m.qCursor].Rating
			} else if m.focus == panelLibrary && m.libMode == libTracks {
				di := m.dataIndex()
				if di >= 0 && di < len(m.tracks) {
					m.ratingCursor = m.tracks[di].Rating
				} else {
					m.ratingCursor = m.status.Rating
				}
			} else {
				m.ratingCursor = m.status.Rating
			}
		}
		return m, nil
	case "enter":
		m.showRating = false
		ratingStr := strconv.Itoa(m.ratingCursor)
		if m.ratingIsAlbum {
			// Album rating — from album list or track view
			if m.ratingAlbum != nil {
				return m, rateAlbum(m.ratingAlbum.AlbumArtist, m.ratingAlbum.Album, m.ratingAlbum.Date, ratingStr)
			}
			if m.curAlbum != nil {
				return m, rateAlbum(m.curAlbum.AlbumArtist, m.curAlbum.Album, m.curAlbum.Date, ratingStr)
			}
			return m, nil
		}
		// Track rating — same context logic as before
		if m.focus == panelQueue && len(m.qSelected) > 0 {
			// Batch rate all selected queue items
			var ids []string
			for pos := range m.qSelected {
				if pos < len(m.queue) && m.queue[pos].XSongID != "" {
					ids = append(ids, m.queue[pos].XSongID)
				}
			}
			m.qSelected = nil
			if len(ids) > 0 {
				return m, rateTracks(ids, ratingStr)
			}
			return m, nil
		} else if m.focus == panelQueue && m.qCursor < len(m.queue) {
			q := m.queue[m.qCursor]
			if q.XSongID != "" {
				return m, rateTrack(q.XSongID, ratingStr)
			}
		} else if m.focus == panelLibrary && m.libMode == libTracks {
			di := m.dataIndex()
			if di >= 0 && di < len(m.tracks) && m.tracks[di].XSongID != "" {
				return m, rateTrack(m.tracks[di].XSongID, ratingStr)
			}
		} else if m.status.SongID != "" {
			return m, rateTrack(m.status.SongID, ratingStr)
		}
		return m, nil
	case "0":
		m.ratingCursor = 0
		return m, nil
	case "1", "2", "3", "4", "5":
		n, _ := strconv.Atoi(key)
		m.ratingCursor = n * 2 // Jump to the full star value
		return m, nil
	}
	return m, nil
}

func sortedSelected(sel map[int]bool) []int {
	positions := make([]int, 0, len(sel))
	for p := range sel {
		positions = append(positions, p)
	}
	sort.Ints(positions)
	return positions
}

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------

var (
	accentColor = lipgloss.Color("#3b82f6")
	dimColor    = lipgloss.Color("#6b7280")
	dangerColor = lipgloss.Color("#ef4444")
	borderColor = lipgloss.Color("#374151")
	selectedBg  = lipgloss.Color("#1e3a5f")
	playingBg   = lipgloss.Color("#1a2744")

	titleStyle  = lipgloss.NewStyle().Bold(true).Foreground(accentColor)
	dimStyle    = lipgloss.NewStyle().Foreground(dimColor)
	headerStyle = lipgloss.NewStyle().
			Bold(true).
			Foreground(lipgloss.Color("#9ca3af")).
			Background(lipgloss.Color("#1f2937")).
			Padding(0, 1)
	panelBorder = lipgloss.NewStyle().
			Border(lipgloss.RoundedBorder()).
			BorderForeground(borderColor)
	focusBorder = lipgloss.NewStyle().
			Border(lipgloss.RoundedBorder()).
			BorderForeground(accentColor)
)

func (m model) View() string {
	if m.width == 0 {
		return "Loading..."
	}

	if m.showHelp {
		return m.helpView()
	}
	if m.showTrackInfo {
		return m.trackInfoView()
	}
	if m.showNowPlaying {
		return m.nowPlayingView()
	}
	if m.showGoto {
		return m.gotoView()
	}
	if m.showRating {
		return m.ratingView()
	}
	if m.showPlPicker {
		return m.plPickerView()
	}
	if m.showPrioMenu {
		return m.prioView()
	}
	if m.showModes {
		return m.modesView()
	}
	if m.showDevices {
		return m.deviceView()
	}
	if m.showMenu {
		return m.menuView()
	}
	if m.searching {
		return m.searchView()
	}

	playerH := 5
	mainH := m.height - playerH
	if mainH < 3 {
		mainH = 3
	}

	lyricsW := 0
	if m.showLyrics {
		lyricsW = m.width * 30 / 100
		if lyricsW < 25 {
			lyricsW = 25
		}
		if lyricsW > 60 {
			lyricsW = 60
		}
	}

	remainW := m.width - lyricsW
	libW := remainW * 25 / 100
	if libW < 25 {
		libW = 25
	}
	if libW > 55 {
		libW = 55
	}
	queueW := remainW - libW
	if queueW < 20 {
		queueW = 20
	}

	libH := mainH - 2
	if m.libMode == libPlaylists || m.libMode == libPlaylistTracks {
		libH--
	}

	lib := m.libraryView(libW-2, libH)
	que := m.queueView(queueW-2, mainH-2)

	libBorder := panelBorder
	queBorder := panelBorder
	if m.focus == panelLibrary {
		libBorder = focusBorder
	} else {
		queBorder = focusBorder
	}

	leftPanel := libBorder.Width(libW - 2).Height(libH).Render(lib)
	quePanel := queBorder.Width(queueW - 2).Height(mainH - 2).Render(que)

	var main string
	if m.showLyrics {
		lyricsContent := m.lyricsSidebarView(lyricsW-4, mainH-2)
		lyricsPanel := panelBorder.Width(lyricsW - 4).Height(mainH - 2).
			BorderForeground(accentColor).Render(lyricsContent)
		main = lipgloss.JoinHorizontal(lipgloss.Top, leftPanel, quePanel, lyricsPanel)
	} else {
		main = lipgloss.JoinHorizontal(lipgloss.Top, leftPanel, quePanel)
	}
	player := m.playerView()

	return main + "\n" + player
}

func (m model) libraryView(w, h int) string {
	var breadcrumbs []string

	// Build all items, then select visible subset based on filter
	type libItem struct {
		text   string
		prefix string
		rating int
		srcIdx int // index into source data
	}
	var allItems []libItem

	switch m.libMode {
	case libArtists:
		breadcrumbs = []string{fmt.Sprintf("Artists (%d)", len(m.artists))}
		for i, a := range m.artists {
			allItems = append(allItems, libItem{text: a, srcIdx: i})
		}
	case libAlbums:
		if m.libSortLatest {
			breadcrumbs = []string{fmt.Sprintf("Latest Albums (%d)", len(m.albums))}
		} else {
			breadcrumbs = []string{"Artists", m.curArtist, fmt.Sprintf("Albums (%d)", len(m.albums))}
		}
		for i, a := range m.albums {
			var label string
			if m.libSortLatest {
				if a.Date != "" && a.Date != "0000" {
					label = a.Date + " " + a.AlbumArtist + " - " + a.Album
				} else {
					label = a.AlbumArtist + " - " + a.Album
				}
			} else {
				label = a.Album
				if a.Date != "" && a.Date != "0000" {
					label = a.Date + " " + a.Album
				}
			}
			displayRating := a.Rating
			if displayRating == 0 && a.Computed > 0 {
				displayRating = int(math.Round(a.Computed))
				if displayRating < 1 {
					displayRating = 1
				} else if displayRating > 10 {
					displayRating = 10
				}
			}
			allItems = append(allItems, libItem{text: label, rating: displayRating, srcIdx: i})
		}
	case libTracks:
		albumName := ""
		if len(m.tracks) > 0 {
			albumName = m.tracks[0].Album
		}
		displayRating := m.albumRating
		if displayRating == 0 && m.albumComputedRating > 0 {
			displayRating = int(math.Round(m.albumComputedRating))
			if displayRating < 1 {
				displayRating = 1
			} else if displayRating > 10 {
				displayRating = 10
			}
		}
		if displayRating > 0 {
			albumName += " " + renderRating(displayRating)
		}
		breadcrumbs = []string{"Artists", m.curArtist, albumName}
		for i, t := range m.tracks {
			allItems = append(allItems, libItem{
				text: t.Title, prefix: fmt.Sprintf("%2d", t.TrackNumber),
				rating: t.Rating, srcIdx: i,
			})
		}
	case libPlaylists:
		breadcrumbs = []string{fmt.Sprintf("Playlists (%d)", len(m.playlists))}
		for i, pl := range m.playlists {
			label := pl.Name
			if pl.SongCount > 0 {
				label += fmt.Sprintf(" (%d)", pl.SongCount)
			}
			allItems = append(allItems, libItem{text: label, srcIdx: i})
		}
	case libPlaylistTracks:
		breadcrumbs = []string{"Playlists", m.curPlaylist, fmt.Sprintf("Tracks (%d)", len(m.playlistTracks))}
		for i, t := range m.playlistTracks {
			label := t.Title
			if t.Artist != "" {
				label += " - " + t.Artist
			}
			allItems = append(allItems, libItem{
				text: label, prefix: fmt.Sprintf("%2d", i+1),
				rating: t.Rating, srcIdx: i,
			})
		}
	}

	// Apply filter
	var items []libItem
	if m.libFilter != "" && len(m.libFiltered) > 0 {
		for _, idx := range m.libFiltered {
			if idx < len(allItems) {
				items = append(items, allItems[idx])
			}
		}
	} else if m.libFilter == "" {
		items = allItems
	}

	// Build breadcrumb bar
	title := strings.Join(breadcrumbs, " > ")
	title = truncate(title, w-2)
	hdr := headerStyle.Width(w).Render(title)
	visH := h - 1
	if visH < 1 {
		visH = 1
	}

	// Filter bar (only shown when actively filtering)
	hintLine := ""
	if m.libFiltering {
		filterText := fmt.Sprintf("> %s_ %d/%d", m.libFilter, len(items), len(allItems))
		hintLine = lipgloss.NewStyle().Foreground(accentColor).Width(w).Render(truncate(filterText, w))
	}

	bodyH := visH
	if m.libFiltering {
		bodyH-- // reserve a line for the filter bar
	}
	if bodyH < 1 {
		bodyH = 1
	}

	// Render visible items
	var rows []string
	for i, it := range items {
		rows = append(rows, m.libRow(i, it.text, it.prefix, it.rating, w))
	}

	m.libOffset = scrollOffset(m.libCursor, m.libOffset, bodyH, len(rows))

	end := m.libOffset + bodyH
	if end > len(rows) {
		end = len(rows)
	}
	visible := rows[m.libOffset:end]

	body := strings.Join(visible, "\n")
	emptyRow := strings.Repeat(" ", w)
	for len(visible) < bodyH {
		body += "\n" + emptyRow
		visible = append(visible, "")
	}

	if hintLine != "" {
		return hdr + "\n" + body + "\n" + hintLine
	}
	return hdr + "\n" + body
}

func (m model) libRow(idx int, text, prefix string, rating int, w int) string {
	selected := m.focus == panelLibrary && idx == m.libCursor
	label := text
	if prefix != "" {
		label = prefix + " " + text
	}
	stars := ""
	if rating > 0 {
		stars = renderRating(rating)
	}
	starsW := runewidth.StringWidth(stars)
	if starsW > 0 {
		label = truncate(label, w-starsW-1)
		label = padRight(label, w-starsW) + stars
	} else {
		label = truncate(label, w)
	}
	s := lipgloss.NewStyle().Width(w)
	if selected {
		s = s.Background(selectedBg).Foreground(lipgloss.Color("#ffffff")).Bold(true)
	}
	return s.Render(label)
}

func (m model) backRow(w int, label string) string {
	selected := m.focus == panelLibrary && m.libCursor == 0
	s := lipgloss.NewStyle().Width(w).Foreground(accentColor)
	if selected {
		s = s.Background(selectedBg).Bold(true)
	}
	return s.Render(label)
}

func (m model) queueView(w, h int) string {
	var title string
	if m.confirmClear {
		title = lipgloss.NewStyle().Bold(true).Foreground(dangerColor).Render("Clear queue? [y/N]")
	} else {
		title = fmt.Sprintf("Queue (%d)", len(m.queue))
	}
	hdr := headerStyle.Width(w).Render(title)
	visH := h - 1
	if visH < 1 {
		visH = 1
	}

	if len(m.queue) == 0 {
		return hdr + "\n" + dimStyle.Render("  Empty queue")
	}

	numW := 4
	timeW := 6
	ratingW := 6
	innerW := w - numW - timeW - ratingW - 5
	artistW := innerW * 30 / 100
	titleW := innerW * 40 / 100
	albumW := innerW - artistW - titleW
	if artistW < 5 {
		artistW = 5
	}
	if titleW < 5 {
		titleW = 5
	}
	if albumW < 5 {
		albumW = 5
	}

	var items []string
	for i, q := range m.queue {
		num := fmt.Sprintf("%3d", q.Position+1)
		dur := fmtTime(q.Duration)
		dur = strings.Repeat(" ", timeW-len(dur)) + dur

		artist := truncate(q.Artist, artistW)
		title := truncate(q.Title, titleW)
		album := truncate(q.Album, albumW)

		artist = padRight(artist, artistW)
		title = padRight(title, titleW)
		album = padRight(album, albumW)

		isCursor := m.focus == panelQueue && i == m.qCursor
		isSelected := m.qSelected[i]
		s := lipgloss.NewStyle().Width(w)
		if q.Current {
			s = s.Background(playingBg)
		}
		if isSelected {
			s = s.Background(lipgloss.Color("#2d1f4e"))
		}
		if isCursor {
			s = s.Background(selectedBg).Foreground(lipgloss.Color("#ffffff")).Bold(true)
		}
		marker := " "
		if q.Current && isCursor {
			marker = "\u25b6"
		} else if q.Current {
			marker = lipgloss.NewStyle().Foreground(accentColor).Render("\u25b6")
		} else if isSelected && !isCursor {
			marker = lipgloss.NewStyle().Foreground(accentColor).Render("*")
		} else if isSelected {
			marker = "*"
		}
		// Priority indicator
		prioDot := " "
		if q.Priority >= 30 {
			prioDot = lipgloss.NewStyle().Foreground(lipgloss.Color("#ff6600")).Render("\u25cf")
		} else if q.Priority >= 20 {
			prioDot = lipgloss.NewStyle().Foreground(lipgloss.Color("#ff9933")).Render("\u25cf")
		} else if q.Priority > 0 {
			prioDot = lipgloss.NewStyle().Foreground(lipgloss.Color("#ffcc66")).Render("\u25cf")
		}

		stars := padRight(renderRating(q.Rating), ratingW)
		ratingStyle := lipgloss.NewStyle().Foreground(lipgloss.Color("#e6b422"))
		var row string
		if isCursor {
			row = marker + prioDot + num + " " + artist + " " + title + " " + album + " " + stars + dur
		} else {
			row = marker + prioDot + dimStyle.Render(num) + " " + artist + " " + title + " " + dimStyle.Render(album) + " " + ratingStyle.Render(stars) + dimStyle.Render(dur)
		}
		items = append(items, s.Render(row))
	}

	m.qOffset = scrollOffset(m.qCursor, m.qOffset, visH, len(items))

	end := m.qOffset + visH
	if end > len(items) {
		end = len(items)
	}
	visible := items[m.qOffset:end]
	body := strings.Join(visible, "\n")
	emptyRow := strings.Repeat(" ", w)
	for len(visible) < visH {
		body += "\n" + emptyRow
		visible = append(visible, "")
	}

	return hdr + "\n" + body
}

// prepareArtRGBA decodes image data to RGBA and zlib-compresses it.
// Returns the compressed bytes and pixel dimensions.
func prepareArtRGBA(data []byte, albumRating int) ([]byte, int, int) {
	img, _, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		return nil, 0, 0
	}
	bounds := img.Bounds()
	rgba := image.NewRGBA(bounds)
	draw.Draw(rgba, rgba.Bounds(), img, bounds.Min, draw.Src)

	drawStarsOnImage(rgba, albumRating)

	var compressed bytes.Buffer
	zw, _ := zlib.NewWriterLevel(&compressed, 6)
	zw.Write(rgba.Pix)
	zw.Close()

	return compressed.Bytes(), bounds.Dx(), bounds.Dy()
}

// drawStarsOnImage renders filled/empty star shapes at the bottom of the image.
func drawStarsOnImage(rgba *image.RGBA, rating int) {
	w := rgba.Bounds().Dx()
	h := rgba.Bounds().Dy()

	// Star size scales with image width
	starSize := w / 10
	if starSize < 8 {
		starSize = 8
	}
	gap := starSize / 3
	totalW := 5*starSize + 4*gap
	startX := (w - totalW) / 2
	startY := h - starSize - starSize/2 // some padding from bottom

	// Semi-transparent dark background strip
	for py := startY - starSize/3; py < h; py++ {
		for px := 0; px < w; px++ {
			idx := (py*w + px) * 4
			if idx < 0 || idx+3 >= len(rgba.Pix) {
				continue
			}
			// Darken: blend with 60% black
			rgba.Pix[idx+0] = uint8(float64(rgba.Pix[idx+0]) * 0.4)
			rgba.Pix[idx+1] = uint8(float64(rgba.Pix[idx+1]) * 0.4)
			rgba.Pix[idx+2] = uint8(float64(rgba.Pix[idx+2]) * 0.4)
		}
	}

	// Draw 5 stars (rating is 0-10, so divide by 2 for full stars)
	fullStars := rating / 2
	for i := 0; i < 5; i++ {
		cx := startX + i*(starSize+gap) + starSize/2
		cy := startY + starSize/2
		filled := i < fullStars
		drawStar(rgba, cx, cy, starSize/2, filled)
	}
}

// drawStar renders a 5-pointed star centered at (cx, cy) with given radius.
func drawStar(rgba *image.RGBA, cx, cy, radius int, filled bool) {
	w := rgba.Bounds().Dx()
	// Gold color for filled, dim gray for empty
	var r, g, b uint8
	if filled {
		r, g, b = 230, 180, 34 // gold
	} else {
		r, g, b = 80, 80, 80 // dim gray
	}

	// Generate star polygon points (5 outer, 5 inner)
	innerR := radius * 38 / 100 // inner radius ~38% of outer
	var points [10][2]float64
	for i := 0; i < 10; i++ {
		angle := float64(i)*math.Pi/5 - math.Pi/2
		rad := float64(radius)
		if i%2 == 1 {
			rad = float64(innerR)
		}
		points[i] = [2]float64{
			float64(cx) + rad*math.Cos(angle),
			float64(cy) + rad*math.Sin(angle),
		}
	}

	// Rasterize: for each pixel in bounding box, check if inside polygon
	for py := cy - radius - 1; py <= cy+radius+1; py++ {
		for px := cx - radius - 1; px <= cx+radius+1; px++ {
			if px < 0 || px >= w || py < 0 || py >= rgba.Bounds().Dy() {
				continue
			}
			inside := pointInPolygon(float64(px)+0.5, float64(py)+0.5, points[:])
			if !inside {
				continue
			}
			if filled {
				idx := (py*w + px) * 4
				rgba.Pix[idx+0] = r
				rgba.Pix[idx+1] = g
				rgba.Pix[idx+2] = b
				rgba.Pix[idx+3] = 255
			} else {
				// Outline only: check if any neighbor is outside the polygon
				isEdge := false
				for _, d := range [][2]int{{-1, 0}, {1, 0}, {0, -1}, {0, 1}} {
					if !pointInPolygon(float64(px+d[0])+0.5, float64(py+d[1])+0.5, points[:]) {
						isEdge = true
						break
					}
				}
				if isEdge {
					idx := (py*w + px) * 4
					rgba.Pix[idx+0] = r
					rgba.Pix[idx+1] = g
					rgba.Pix[idx+2] = b
					rgba.Pix[idx+3] = 255
				}
			}
		}
	}
}

// pointInPolygon uses ray casting to test if point (x,y) is inside polygon.
func pointInPolygon(x, y float64, poly [][2]float64) bool {
	n := len(poly)
	inside := false
	j := n - 1
	for i := 0; i < n; i++ {
		yi, xi := poly[i][1], poly[i][0]
		yj, xj := poly[j][1], poly[j][0]
		if ((yi > y) != (yj > y)) && (x < (xj-xi)*(y-yi)/(yj-yi)+xi) {
			inside = !inside
		}
		j = i
	}
	return inside
}

// transmitArtToTerminal writes the Kitty graphics protocol escape sequence
// directly to stdout, specifying cell dimensions c/r so the image scales.
func transmitArtToTerminal(rgbaData []byte, pixW, pixH, cols, rows int) {
	b64 := base64.StdEncoding.EncodeToString(rgbaData)

	var buf bytes.Buffer
	fmt.Fprintf(&buf, "\033_Ga=d,d=A,q=2\033\\")
	const chunkSize = 4096
	for i := 0; i < len(b64); i += chunkSize {
		end := i + chunkSize
		if end > len(b64) {
			end = len(b64)
		}
		more := 1
		if end >= len(b64) {
			more = 0
		}
		if i == 0 {
			fmt.Fprintf(&buf, "\033_Gi=1,f=32,U=1,t=d,a=T,q=2,o=z,s=%d,v=%d,c=%d,r=%d,m=%d;%s\033\\",
				pixW, pixH, cols, rows, more, b64[i:end])
		} else {
			fmt.Fprintf(&buf, "\033_Gm=%d;%s\033\\", more, b64[i:end])
		}
	}
	os.Stdout.Write(buf.Bytes())
}

// kittyDiacritics is the table of combining characters used by the Kitty
// graphics protocol for Unicode placeholder row/column encoding.
// Derived from kitty's rowcolumn-diacritics.txt (Unicode 6.0.0, class 230).
var kittyDiacritics = [...]rune{
	0x0305, 0x030D, 0x030E, 0x0310, 0x0312, 0x033D, 0x033E, 0x033F,
	0x0346, 0x034A, 0x034B, 0x034C, 0x0350, 0x0351, 0x0352, 0x0357,
	0x035B, 0x0363, 0x0364, 0x0365, 0x0366, 0x0367, 0x0368, 0x0369,
	0x036A, 0x036B, 0x036C, 0x036D, 0x036E, 0x036F, 0x0483, 0x0484,
	0x0485, 0x0486, 0x0487, 0x0592, 0x0593, 0x0594, 0x0595, 0x0597,
	0x0598, 0x0599, 0x059C, 0x059D, 0x059E, 0x059F, 0x05A0, 0x05A1,
	0x05A8, 0x05A9, 0x05AB, 0x05AC, 0x05AF, 0x05C4, 0x0610, 0x0611,
	0x0612, 0x0613, 0x0614, 0x0615, 0x0616, 0x0617, 0x0657, 0x0658,
	0x0659, 0x065A, 0x065B, 0x065D, 0x065E, 0x06D6, 0x06D7, 0x06D8,
	0x06D9, 0x06DA, 0x06DB, 0x06DC, 0x06DF, 0x06E0, 0x06E1, 0x06E2,
	0x06E4, 0x06E7, 0x06E8, 0x06EB, 0x06EC, 0x0730, 0x0732, 0x0733,
	0x0735, 0x0736, 0x073A, 0x073D, 0x073F, 0x0740, 0x0741, 0x0743,
	0x0745, 0x0747, 0x0749, 0x074A, 0x07EB, 0x07EC, 0x07ED, 0x07EE,
	0x07EF, 0x07F0, 0x07F1, 0x07F3, 0x0816, 0x0817, 0x0818, 0x0819,
	0x081B, 0x081C, 0x081D, 0x081E, 0x081F, 0x0820, 0x0821, 0x0822,
	0x0823, 0x0825, 0x0826, 0x0827, 0x0829, 0x082A, 0x082B, 0x082C,
	0x082D, 0x0951, 0x0953, 0x0954, 0x0F82, 0x0F83, 0x0F86, 0x0F87,
	0x135D, 0x135E, 0x135F, 0x17DD, 0x193A, 0x1A17, 0x1A75, 0x1A76,
	0x1A77, 0x1A78, 0x1A79, 0x1A7A, 0x1A7B, 0x1A7C, 0x1B6B, 0x1B6D,
	0x1B6E, 0x1B6F, 0x1B70, 0x1B71, 0x1B72, 0x1B73, 0x1CD0, 0x1CD1,
	0x1CD2, 0x1CDA, 0x1CDB, 0x1CE0, 0x1DC0, 0x1DC1, 0x1DC3, 0x1DC4,
	0x1DC5, 0x1DC6, 0x1DC7, 0x1DC8, 0x1DC9, 0x1DCB, 0x1DCC, 0x1DD1,
	0x1DD2, 0x1DD3, 0x1DD4, 0x1DD5, 0x1DD6, 0x1DD7, 0x1DD8, 0x1DD9,
	0x1DDA, 0x1DDB, 0x1DDC, 0x1DDD, 0x1DDE, 0x1DDF, 0x1DE0, 0x1DE1,
	0x1DE2, 0x1DE3, 0x1DE4, 0x1DE5, 0x1DE6, 0x1DFE, 0x20D0, 0x20D1,
	0x20D4, 0x20D5, 0x20D6, 0x20D7, 0x20DB, 0x20DC, 0x20E1, 0x20E7,
	0x20E9, 0x20F0, 0x2CEF, 0x2CF0, 0x2CF1, 0x2DE0, 0x2DE1, 0x2DE2,
	0x2DE3, 0x2DE4, 0x2DE5, 0x2DE6, 0x2DE7, 0x2DE8, 0x2DE9, 0x2DEA,
	0x2DEB, 0x2DEC, 0x2DED, 0x2DEE, 0x2DEF, 0x2DF0, 0x2DF1, 0x2DF2,
	0x2DF3, 0x2DF4, 0x2DF5, 0x2DF6, 0x2DF7, 0x2DF8, 0x2DF9, 0x2DFA,
	0x2DFB, 0x2DFC, 0x2DFD, 0x2DFE, 0x2DFF, 0xA66F, 0xA67C, 0xA67D,
	0xA6F0, 0xA6F1, 0xA8E0, 0xA8E1, 0xA8E2, 0xA8E3, 0xA8E4, 0xA8E5,
	0xA8E6, 0xA8E7, 0xA8E8, 0xA8E9, 0xA8EA, 0xA8EB, 0xA8EC, 0xA8ED,
	0xA8EE, 0xA8EF, 0xA8F0, 0xA8F1, 0xAAB0, 0xAAB2, 0xAAB3, 0xAAB7,
	0xAAB8, 0xAABE, 0xAABF, 0xAAC1, 0xFE20, 0xFE21, 0xFE22, 0xFE23,
	0xFE24, 0xFE25, 0xFE26, 0x10A0F, 0x10A38, 0x1D185, 0x1D186,
	0x1D187, 0x1D188, 0x1D189, 0x1D1AA, 0x1D1AB, 0x1D1AC, 0x1D1AD,
	0x1D242, 0x1D243, 0x1D244,
}

// kittyPlaceholders returns Unicode placeholder characters (U+10EEEE) with
// diacritics encoding row/column positions. Kitty renders these as the
// transmitted image (id=1). These survive bubbletea redraws.
func kittyPlaceholders(cols, rows int) string {
	var sb strings.Builder
	maxIdx := len(kittyDiacritics)
	for r := 0; r < rows && r < maxIdx; r++ {
		if r > 0 {
			sb.WriteRune('\n')
		}
		// Foreground color encodes the image ID (1)
		sb.WriteString("\033[38;5;1m")
		for c := 0; c < cols && c < maxIdx; c++ {
			sb.WriteRune('\U0010EEEE')
			sb.WriteRune(kittyDiacritics[r]) // row
			sb.WriteRune(kittyDiacritics[c]) // column
		}
		sb.WriteString("\033[39m")
	}
	return sb.String()
}

func (m model) playerView() string {
	w := m.width
	if w < 10 {
		w = 10
	}

	// Album art: 3 rows tall, cols = rows * 2 (cells are ~2:1)
	artCols := 0
	artRows := 4
	if len(m.artRGBA) > 0 {
		artCols = artRows * 2
		if m.artFile != artTxFile || artCols != artTxCols || artRows != artTxRows {
			artTxFile = m.artFile
			artTxCols = artCols
			artTxRows = artRows
			transmitArtToTerminal(m.artRGBA, m.artW, m.artH, artCols, artRows)
		}
	}

	// Width available for player text (subtract art + gap)
	infoW := w
	if artCols > 0 {
		infoW = w - artCols - 1
	}
	if infoW < 20 {
		infoW = 20
	}

	// Track info
	trackTitle := "\u2014"
	if m.status.Title != "" {
		trackTitle = m.status.Title
		if m.status.Artist != "" {
			trackTitle += " \u2014 " + m.status.Artist
		}
	}

	// Album info
	albumInfo := ""
	if m.status.Album != "" {
		albumInfo = m.status.Album
		if m.status.Date != "" {
			albumInfo += " (" + m.status.Date + ")"
		}
	}

	posStr := fmtTime(m.status.TimePos)
	durStr := fmtTime(m.status.Dur)

	// Track rating stars
	trackRatingRendered := ""
	trackRatingLen := 0
	if m.status.Rating > 0 {
		trackRatingRendered = " " + lipgloss.NewStyle().Foreground(lipgloss.Color("#e6b422")).Render(renderRating(m.status.Rating))
		trackRatingLen = 1 + lipgloss.Width(renderRating(m.status.Rating))
	}

	// Seekbar
	barW := infoW - len(posStr) - len(durStr) - 3
	if barW < 5 {
		barW = 5
	}
	filled := 0
	if m.status.Dur > 0 {
		filled = int(m.status.TimePos / m.status.Dur * float64(barW))
	}
	if filled > barW {
		filled = barW
	}
	if filled < 0 {
		filled = 0
	}

	bar := lipgloss.NewStyle().Foreground(accentColor).Render(strings.Repeat("\u2501", filled))
	bar += lipgloss.NewStyle().Foreground(accentColor).Render("\u25cf")
	bar += dimStyle.Render(strings.Repeat("\u2500", barW-filled))

	timeL := dimStyle.Render(posStr)
	timeR := dimStyle.Render(durStr)

	// RG label for line 1
	activeFlag := lipgloss.NewStyle().Foreground(lipgloss.Color("#ffffff"))
	rgLabel := m.status.ReplayGainMode
	if rgLabel == "" {
		rgLabel = "off"
	}
	rgStr := ""
	if rgLabel != "off" {
		rgStr = activeFlag.Render("[RG:" + rgLabel + "]")
	} else {
		rgStr = dimStyle.Render("[RG:off]")
	}
	rgLen := lipgloss.Width(rgStr)

	// Mode flags for line 2
	var flags []string
	if m.status.Repeat {
		flags = append(flags, activeFlag.Render("r"))
	} else {
		flags = append(flags, dimStyle.Render("r"))
	}
	if m.status.Random {
		flags = append(flags, activeFlag.Render("z"))
	} else {
		flags = append(flags, dimStyle.Render("z"))
	}
	if m.status.Single {
		flags = append(flags, activeFlag.Render("s"))
	} else {
		flags = append(flags, dimStyle.Render("s"))
	}
	if m.status.Consume {
		flags = append(flags, activeFlag.Render("c"))
	} else {
		flags = append(flags, dimStyle.Render("c"))
	}
	flagsStr := strings.Join(flags, " ")
	flagsLen := lipgloss.Width(flagsStr)

	// Line 1: state icon + track + rating, right-aligned RG
	trackMaxW := infoW - trackRatingLen - rgLen - 2
	if trackMaxW < 10 {
		trackMaxW = 10
	}
	trackStr := truncate(trackTitle, trackMaxW) + trackRatingRendered
	trackLen := lipgloss.Width(trackStr)
	pad := infoW - trackLen - rgLen
	if pad < 1 {
		pad = 1
	}
	line1 := trackStr + strings.Repeat(" ", pad) + rgStr

	// Line 2: album info, right-aligned mode flags
	line2 := ""
	if albumInfo != "" {
		albumMaxW := infoW - flagsLen - 2
		if albumMaxW < 10 {
			albumMaxW = 10
		}
		albumStr := dimStyle.Render(truncate(albumInfo, albumMaxW))
		albumLen := lipgloss.Width(albumStr)
		pad2 := infoW - albumLen - flagsLen
		if pad2 < 1 {
			pad2 = 1
		}
		line2 = albumStr + strings.Repeat(" ", pad2) + flagsStr
	} else {
		pad2 := infoW - flagsLen
		if pad2 < 1 {
			pad2 = 1
		}
		line2 = strings.Repeat(" ", pad2) + flagsStr
	}

	// Line 3: empty
	// Line 4: seekbar
	line4 := timeL + " " + bar + " " + timeR

	playerRight := line1 + "\n" + line2 + "\n\n" + line4

	if artCols > 0 {
		artStr := kittyPlaceholders(artCols, artRows)
		// Pad art to 3 lines
		artLines := strings.Split(artStr, "\n")
		for len(artLines) < artRows {
			artLines = append(artLines, strings.Repeat(" ", artCols))
		}
		artBlock := strings.Join(artLines, "\n")
		return lipgloss.JoinHorizontal(lipgloss.Top, artBlock, " ", playerRight)
	}
	return playerRight
}

func (m model) helpView() string {
	title := titleStyle.Render("Hotkeys")
	sections := []struct{ header, body string }{
		{"Global", strings.Join([]string{
			"  /          Search",
			"  ?          This help screen",
			"  Space      Play / Pause",
			"  >          Next track",
			"  <          Previous track",
			"  s          Stop",
			"  +/-        Volume up/down",
			"  r          Random album",
			"  R          Random tracks",
			"  u          Update library",
			"  P          Playlists",
			"  D          Device picker",
			"  i          Track info (library tracks / queue)",
			"  l          Toggle lyrics sidebar",
			"  L          Now playing (art + info + lyrics)",
			"  o          Go to artist/album/search",
			"  *          Rate track/album",
			"  Ctrl+G     Cycle ReplayGain (off/track/album)",
			"  Tab        Switch panel focus",
			"  q          Quit",
		}, "\n")},
		{"Library", strings.Join([]string{
			"  j/k        Navigate up/down",
			"  Enter      Action menu (Add/Priority/Insert/Replace)",
			"  p          Add track to playlist",
			"  PgUp/PgDn  Jump 20 items",
			"  g/G        Go to first/last",
		}, "\n")},
		{"Queue", strings.Join([]string{
			"  j/k        Navigate up/down",
			"  Enter      Play selected track",
			"  p          Add track to playlist",
			"  d/x/Del    Delete track (or selection)",
			"  v          Toggle select",
			"  V          Select range",
			"  Esc        Clear selection",
			"  J/K        Move track down/up",
			"  c          Clear queue (confirm)",
			"  PgUp/PgDn  Jump 20 items",
			"  g/G        Go to first/last",
		}, "\n")},
		{"Search", strings.Join([]string{
			"  j/k        Navigate results",
			"  Enter      Action menu",
			"  Esc        Close search",
		}, "\n")},
		{"Seekbar", strings.Join([]string{
			"  Click      Seek to position",
		}, "\n")},
	}

	var lines []string
	lines = append(lines, title, "")
	for _, s := range sections {
		lines = append(lines, titleStyle.Render(s.header))
		lines = append(lines, s.body, "")
	}
	lines = append(lines, dimStyle.Render("Press any key to close"))

	box := lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(accentColor).
		Padding(1, 2).
		Render(strings.Join(lines, "\n"))

	return lipgloss.Place(m.width, m.height, lipgloss.Center, lipgloss.Center, box)
}

// displayLine represents a single rendered line with a reference back to its source lyrics line.
type displayLine struct {
	text    string
	srcLine int // index into m.lyrics
}

// wrapLyrics wraps and centers lyrics lines to fit within maxW, tracking source line indices.
func wrapLyrics(lyrics []lyricsLine, maxW int) []displayLine {
	var result []displayLine
	for i, l := range lyrics {
		text := strings.TrimSpace(l.text)
		if text == "" {
			result = append(result, displayLine{text: "", srcLine: i})
			continue
		}
		sw := runewidth.StringWidth(text)
		if sw <= maxW {
			pad := (maxW - sw) / 2
			centered := strings.Repeat(" ", pad) + text
			result = append(result, displayLine{text: centered, srcLine: i})
			continue
		}
		// Word-wrap
		wrapped := wordWrapLine(text, maxW)
		for _, wl := range wrapped {
			ww := runewidth.StringWidth(wl)
			pad := (maxW - ww) / 2
			if pad < 0 {
				pad = 0
			}
			centered := strings.Repeat(" ", pad) + wl
			result = append(result, displayLine{text: centered, srcLine: i})
		}
	}
	return result
}

// wordWrapLine breaks a line into multiple lines at word boundaries to fit within maxW.
func wordWrapLine(text string, maxW int) []string {
	words := strings.Fields(text)
	if len(words) == 0 {
		return []string{""}
	}
	var lines []string
	cur := words[0]
	curW := runewidth.StringWidth(cur)
	for _, word := range words[1:] {
		ww := runewidth.StringWidth(word)
		if curW+1+ww <= maxW {
			cur += " " + word
			curW += 1 + ww
		} else {
			lines = append(lines, cur)
			cur = word
			curW = ww
		}
	}
	lines = append(lines, cur)
	return lines
}

func (m model) lyricsSidebarView(w, h int) string {
	var lines []string
	lines = append(lines, titleStyle.Render("Lyrics"), "")

	if len(m.lyrics) == 0 {
		if m.lyricsFile != "" {
			lines = append(lines, dimStyle.Render("No lyrics available"))
		} else {
			lines = append(lines, dimStyle.Render("Loading..."))
		}
		return strings.Join(lines, "\n")
	}

	dLines := wrapLyrics(m.lyrics, w)

	currentSrc := -1
	if m.lyricsType == "synced" {
		currentSrc = m.currentLyricsLine()
		// Auto-scroll: find first display line for currentSrc
		currentDisplay := 0
		for j, dl := range dLines {
			if dl.srcLine == currentSrc {
				currentDisplay = j
				break
			}
		}
		visH := h - 4
		if visH < 3 {
			visH = 3
		}
		if currentDisplay >= m.lyricsScroll+visH || currentDisplay < m.lyricsScroll {
			m.lyricsScroll = currentDisplay - visH/3
			if m.lyricsScroll < 0 {
				m.lyricsScroll = 0
			}
		}
	}

	visH := h - 4
	if visH < 3 {
		visH = 3
	}
	start := m.lyricsScroll
	end := start + visH
	if end > len(dLines) {
		end = len(dLines)
	}

	activeStyle := lipgloss.NewStyle().Foreground(accentColor).Bold(true)
	for i := start; i < end; i++ {
		if dLines[i].srcLine == currentSrc {
			lines = append(lines, activeStyle.Render(dLines[i].text))
		} else {
			lines = append(lines, dLines[i].text)
		}
	}

	return strings.Join(lines, "\n")
}

func (m model) currentLyricsLine() int {
	if m.lyricsType != "synced" || len(m.lyrics) == 0 {
		return 0
	}
	pos := m.status.TimePos
	best := 0
	for i, l := range m.lyrics {
		if l.time >= 0 && l.time <= pos {
			best = i
		}
	}
	return best
}

func (m model) nowPlayingView() string {
	w := m.width
	h := m.height

	// Layout modes based on terminal width:
	// wide (>=100): side-by-side (art+info left, lyrics right)
	// medium (>=60): vertical (art+info top, lyrics bottom)
	// narrow (<60): no art, info + lyrics stacked
	horizontal := w >= 100
	showArt := w >= 60 && len(m.artRGBA) > 0

	// --- Track info lines (shared across layouts) ---
	infoW := w - 4
	var infoLines []string
	if m.status.Title != "" {
		infoLines = append(infoLines, titleStyle.Render(truncate(m.status.Title, infoW)))
	}
	if m.status.Artist != "" {
		infoLines = append(infoLines, truncate(m.status.Artist, infoW))
	}
	if m.status.Album != "" {
		albumLine := m.status.Album
		if m.status.Date != "" {
			albumLine += " (" + m.status.Date + ")"
		}
		infoLines = append(infoLines, dimStyle.Render(truncate(albumLine, infoW)))
	}
	if m.status.Rating > 0 {
		infoLines = append(infoLines, lipgloss.NewStyle().Foreground(lipgloss.Color("#e6b422")).Render(renderRating(m.status.Rating)))
	}

	// --- Seekbar ---
	seekW := infoW
	posStr := fmtTime(m.status.TimePos)
	durStr := fmtTime(m.status.Dur)
	barW := seekW - len(posStr) - len(durStr) - 3
	if barW < 5 {
		barW = 5
	}
	filled := 0
	if m.status.Dur > 0 {
		filled = int(m.status.TimePos / m.status.Dur * float64(barW))
	}
	if filled > barW {
		filled = barW
	}
	if filled < 0 {
		filled = 0
	}
	seekBar := lipgloss.NewStyle().Foreground(accentColor).Render(strings.Repeat("\u2501", filled))
	seekBar += lipgloss.NewStyle().Foreground(accentColor).Render("\u25cf")
	seekBar += dimStyle.Render(strings.Repeat("\u2500", barW-filled))
	seekLine := dimStyle.Render(posStr) + " " + seekBar + " " + dimStyle.Render(durStr)

	// --- Lyrics hint ---
	hint := "j/k scroll, any other key to close"
	if m.lyricsType == "synced" {
		hint = "j/k scroll, f follow, any other key to close"
	}

	if horizontal {
		return m.npHorizontal(w, h, showArt, infoLines, seekLine, hint)
	}
	return m.npVertical(w, h, showArt, infoLines, seekLine, hint)
}

func (m model) npHorizontal(w, h int, showArt bool, infoLines []string, seekLine, hint string) string {
	// Info block takes ~6 lines (title, artist, album, rating, blank, seekbar)
	infoH := len(infoLines) + 2 // +blank +seekbar

	// Art fills the left column, leaving room for info below
	artRows := h - infoH - 2
	if artRows < 4 {
		artRows = 4
	}
	artCols := artRows * 2
	maxLeftW := w/2 - 2
	if artCols > maxLeftW {
		artCols = maxLeftW
		artRows = artCols / 2
	}

	leftW := artCols
	if leftW < 30 {
		leftW = 30
	}

	artStr := ""
	if showArt {
		if artCols != artTxCols || artRows != artTxRows {
			artTxCols = artCols
			artTxRows = artRows
			transmitArtToTerminal(m.artRGBA, m.artW, m.artH, artCols, artRows)
		}
		artStr = kittyPlaceholders(artCols, artRows)
	}

	// Rebuild seekbar at left column width
	posStr := fmtTime(m.status.TimePos)
	durStr := fmtTime(m.status.Dur)
	barW := leftW - len(posStr) - len(durStr) - 3
	if barW < 5 {
		barW = 5
	}
	filled := 0
	if m.status.Dur > 0 {
		filled = int(m.status.TimePos / m.status.Dur * float64(barW))
	}
	if filled > barW {
		filled = barW
	}
	if filled < 0 {
		filled = 0
	}
	bar := lipgloss.NewStyle().Foreground(accentColor).Render(strings.Repeat("\u2501", filled))
	bar += lipgloss.NewStyle().Foreground(accentColor).Render("\u25cf")
	bar += dimStyle.Render(strings.Repeat("\u2500", barW-filled))
	seekLine = dimStyle.Render(posStr) + " " + bar + " " + dimStyle.Render(durStr)

	// Truncate info to left column width
	var leftInfo []string
	for _, l := range infoLines {
		leftInfo = append(leftInfo, truncate(l, leftW))
	}

	var leftParts []string
	if artStr != "" {
		leftParts = append(leftParts, artStr)
	}
	leftParts = append(leftParts, "")
	leftParts = append(leftParts, leftInfo...)
	leftParts = append(leftParts, "", seekLine)

	leftBlock := lipgloss.NewStyle().Width(leftW + 2).Height(h).Render(strings.Join(leftParts, "\n"))

	// Right column: lyrics fills full height
	rightW := w - leftW - 6
	if rightW < 20 {
		rightW = 20
	}
	lyricsBlock := m.renderLyrics(rightW, h-2, hint)

	rightBlock := lipgloss.NewStyle().
		Width(rightW).
		Height(h).
		BorderLeft(true).
		BorderStyle(lipgloss.NormalBorder()).
		BorderForeground(lipgloss.Color("#444")).
		PaddingLeft(2).
		Render(lyricsBlock)

	return lipgloss.JoinHorizontal(lipgloss.Top, leftBlock, rightBlock)
}

func (m model) npVertical(w, h int, showArt bool, infoLines []string, seekLine, hint string) string {
	// Info takes ~4-6 lines, seekbar 1, blanks 2 = ~8 lines for non-art/lyrics
	infoH := len(infoLines) + 3 // blanks + seekbar
	artRowsUsed := 0

	var sections []string

	if showArt {
		// Art gets ~40% of remaining height
		artRows := (h - infoH) * 2 / 5
		if artRows < 4 {
			artRows = 4
		}
		artCols := artRows * 2
		if artCols > w-4 {
			artCols = w - 4
			artRows = artCols / 2
		}
		artRowsUsed = artRows

		if artCols != artTxCols || artRows != artTxRows {
			artTxCols = artCols
			artTxRows = artRows
			transmitArtToTerminal(m.artRGBA, m.artW, m.artH, artCols, artRows)
		}
		sections = append(sections, kittyPlaceholders(artCols, artRows))
	}

	// Track info
	sections = append(sections, "")
	for _, l := range infoLines {
		sections = append(sections, truncate(l, w-4))
	}
	sections = append(sections, seekLine, "")

	// Lyrics fills remaining height
	lyricsH := h - artRowsUsed - infoH - 2
	if lyricsH < 3 {
		lyricsH = 3
	}
	sections = append(sections, m.renderLyrics(w-4, lyricsH, hint))

	content := strings.Join(sections, "\n")
	return lipgloss.NewStyle().Width(w).Height(h).Render(content)
}

func (m model) renderLyrics(maxW, maxH int, hint string) string {
	var lines []string
	lines = append(lines, titleStyle.Render("Lyrics"), "")

	if len(m.lyrics) == 0 {
		if m.lyricsFile != "" {
			lines = append(lines, dimStyle.Render("No lyrics available"))
		} else {
			lines = append(lines, dimStyle.Render("Loading..."))
		}
	} else {
		dLines := wrapLyrics(m.lyrics, maxW)

		currentSrc := -1
		if m.lyricsType == "synced" {
			currentSrc = m.currentLyricsLine()
		}

		lyricsH := maxH - 4
		if lyricsH < 3 {
			lyricsH = 3
		}

		start := m.lyricsScroll
		end := start + lyricsH
		if end > len(dLines) {
			end = len(dLines)
		}

		activeStyle := lipgloss.NewStyle().Foreground(accentColor).Bold(true)
		for i := start; i < end; i++ {
			if dLines[i].srcLine == currentSrc {
				lines = append(lines, activeStyle.Render(dLines[i].text))
			} else {
				lines = append(lines, dLines[i].text)
			}
		}
	}

	lines = append(lines, "", dimStyle.Render(hint))
	return strings.Join(lines, "\n")
}

func (m model) trackInfoView() string {
	title := titleStyle.Render("Track Info")

	var lines []string
	lines = append(lines, title, "")

	if len(m.trackInfo) == 0 {
		lines = append(lines, dimStyle.Render("Loading..."))
	} else {
		maxLabel := 0
		for _, ti := range m.trackInfo {
			if len(ti.label) > maxLabel {
				maxLabel = len(ti.label)
			}
		}
		for _, ti := range m.trackInfo {
			pad := strings.Repeat(" ", maxLabel-len(ti.label))
			lines = append(lines, titleStyle.Render(ti.label+pad)+"  "+ti.value)
		}
	}

	lines = append(lines, "", dimStyle.Render("Press any key to close"))

	box := lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(accentColor).
		Padding(1, 2).
		Render(strings.Join(lines, "\n"))

	return lipgloss.Place(m.width, m.height, lipgloss.Center, lipgloss.Center, box)
}

func (m model) gotoView() string {
	title := titleStyle.Render("Go to...")

	var lines []string
	lines = append(lines, title, "")

	if m.gotoAlbumArtist == "" {
		lines = append(lines, dimStyle.Render("Loading..."))
	} else {
		options := []string{
			"Go to Artist: " + m.gotoAlbumArtist,
			"Go to Album: " + m.gotoAlbum,
			"Search: " + m.gotoAlbumArtist,
		}
		for i, opt := range options {
			if i == m.gotoCursor {
				lines = append(lines, lipgloss.NewStyle().Foreground(accentColor).Render("> "+opt))
			} else {
				lines = append(lines, "  "+opt)
			}
		}
	}

	lines = append(lines, "", dimStyle.Render("Enter to select, Esc to close"))

	box := lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(accentColor).
		Padding(1, 2).
		Render(strings.Join(lines, "\n"))

	return lipgloss.Place(m.width, m.height, lipgloss.Center, lipgloss.Center, box)
}

func (m model) plPickerView() string {
	header := titleStyle.Render("Add to Playlist") + "\n\n"

	var items []string

	// First entry: "New Playlist..."
	if m.plPickerNewMode {
		items = append(items, " New Playlist...\n "+m.plPickerInput.View())
	} else if m.plPickerCursor == 0 {
		s := lipgloss.NewStyle().Background(selectedBg).Foreground(lipgloss.Color("#ffffff")).Bold(true)
		items = append(items, s.Render(" New Playlist... "))
	} else {
		items = append(items, " "+dimStyle.Render("New Playlist..."))
	}

	// Existing playlists
	for i, pl := range m.plPickerList {
		name := pl.Name
		info := ""
		if pl.SongCount > 0 {
			info = dimStyle.Render(fmt.Sprintf(" (%d)", pl.SongCount))
		}
		cursorIdx := i + 1 // offset by 1 for "New Playlist..." entry
		if cursorIdx == m.plPickerCursor && !m.plPickerNewMode {
			s := lipgloss.NewStyle().Background(selectedBg).Foreground(lipgloss.Color("#ffffff")).Bold(true)
			items = append(items, s.Render(" "+name+" "))
		} else {
			items = append(items, " "+name+info)
		}
	}

	hints := "\n\n" + dimStyle.Render("[↑↓]navigate [enter]select [esc]close")
	content := header + strings.Join(items, "\n") + hints

	box := lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(accentColor).
		Padding(1, 3).
		Render(content)

	return lipgloss.Place(m.width, m.height, lipgloss.Center, lipgloss.Center, box)
}

func (m model) prioView() string {
	header := titleStyle.Render("Add Prioritized") + "\n\n"

	labels := []string{"Low", "Medium", "High"}
	colors := []string{"#ffcc66", "#ff9933", "#ff6600"}

	var lines []string
	for i, label := range labels {
		dot := lipgloss.NewStyle().Foreground(lipgloss.Color(colors[i])).Render("\u25cf")
		if i == m.prioCursor {
			s := lipgloss.NewStyle().Background(selectedBg).Foreground(lipgloss.Color("#ffffff")).Bold(true)
			lines = append(lines, s.Render(" "+dot+" "+label+" "))
		} else {
			lines = append(lines, " "+dot+" "+label)
		}
	}

	content := header + strings.Join(lines, "\n")

	box := lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(accentColor).
		Padding(1, 3).
		Render(content)

	return lipgloss.Place(m.width, m.height, lipgloss.Center, lipgloss.Center, box)
}

func (m model) modesView() string {
	header := titleStyle.Render("Playback Modes") + "\n\n"

	type modeItem struct {
		label string
		value string
		on    bool
	}

	rgLabel := m.status.ReplayGainMode
	if rgLabel == "" {
		rgLabel = "off"
	}

	items := []modeItem{
		{"ReplayGain", rgLabel, rgLabel != "off"},
		{"Repeat", "", m.status.Repeat},
		{"Random", "", m.status.Random},
		{"Single", "", m.status.Single},
		{"Consume", "", m.status.Consume},
	}

	var lines []string
	for i, it := range items {
		indicator := dimStyle.Render("\u25cb")
		if it.on {
			indicator = lipgloss.NewStyle().Foreground(lipgloss.Color("#22c55e")).Render("\u25cf")
		}
		label := it.label
		if it.value != "" {
			label += ": " + it.value
		}
		if i == m.modesCursor {
			s := lipgloss.NewStyle().Background(selectedBg).Foreground(lipgloss.Color("#ffffff")).Bold(true)
			lines = append(lines, s.Render(" "+indicator+" "+label+" "))
		} else {
			lines = append(lines, " "+indicator+" "+label)
		}
	}

	content := header + strings.Join(lines, "\n")

	box := lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(accentColor).
		Padding(1, 3).
		Render(content)

	return lipgloss.Place(m.width, m.height, lipgloss.Center, lipgloss.Center, box)
}

func (m model) deviceView() string {
	header := titleStyle.Render("Outputs") + "\n\n"

	if len(m.devices) == 0 {
		header += dimStyle.Render("No outputs found")
	}

	var items []string
	for i, d := range m.devices {
		status := dimStyle.Render("\u25cb")
		if d.Enabled {
			status = lipgloss.NewStyle().Foreground(lipgloss.Color("#22c55e")).Render("\u25cf")
		}
		active := "  "
		id, _ := strconv.Atoi(d.ID)
		if id == m.activeDevice {
			active = lipgloss.NewStyle().Foreground(accentColor).Render("\u25b6 ")
		}

		name := d.Name

		isCursor := i == m.devCursor
		s := lipgloss.NewStyle()
		if isCursor {
			s = s.Background(selectedBg).Foreground(lipgloss.Color("#ffffff")).Bold(true)
			line := " " + active + status + " " + d.Name
			items = append(items, s.Render(line))
		} else {
			items = append(items, " "+active+status+" "+name)
		}
	}

	hints := "\n\n" + dimStyle.Render("[\u2191\u2193]navigate [enter]switch [esc]close")
	content := header + strings.Join(items, "\n") + hints

	box := lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(accentColor).
		Padding(1, 3).
		Render(content)

	return lipgloss.Place(m.width, m.height, lipgloss.Center, lipgloss.Center, box)
}

func (m model) ratingView() string {
	label := "Track Rating"
	if m.ratingIsAlbum {
		label = "Album Rating"
	}
	header := titleStyle.Render(label) + "\n\n"

	var items []string
	// 0 = unrate
	for i := 0; i <= 10; i++ {
		var line string
		if i == 0 {
			line = "  No rating"
		} else {
			line = "  " + renderRating(i)
			// Add numeric label
			if i%2 == 0 {
				line += fmt.Sprintf("  %d", i/2)
			} else {
				line += fmt.Sprintf("  %d.5", i/2)
			}
		}

		s := lipgloss.NewStyle()
		if i == m.ratingCursor {
			s = s.Background(selectedBg).Foreground(lipgloss.Color("#ffffff")).Bold(true)
		}
		items = append(items, s.Render(line))
	}

	hintStr := "[↑↓]navigate [1-5]jump [tab]track/album [enter]confirm [esc]cancel"
	hints := "\n\n" + dimStyle.Render(hintStr)
	content := header + strings.Join(items, "\n") + hints

	box := lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(accentColor).
		Padding(1, 3).
		Render(content)

	return lipgloss.Place(m.width, m.height, lipgloss.Center, lipgloss.Center, box)
}

func (m model) menuView() string {
	var label string
	optCount := m.menuOptionCount()

	if m.menuSource == "search" {
		idx := m.srCursor
		nAlbums := len(m.searchRes.Albums)
		if idx < nAlbums {
			a := m.searchRes.Albums[idx]
			label = a.AlbumArtist + " - " + a.Album
			if a.Date != "" && a.Date != "0000" {
				label = a.AlbumArtist + " - " + a.Date + " " + a.Album
			}
		} else if idx-nAlbums < len(m.searchRes.Tracks) {
			t := m.searchRes.Tracks[idx-nAlbums]
			label = t.Artist + " - " + t.Title
		}
	} else {
		di := m.dataIndex()
		switch m.libMode {
		case libArtists:
			if di >= 0 && di < len(m.artists) {
				label = m.artists[di]
			}
		case libAlbums:
			if di >= 0 && di < len(m.albums) {
				a := m.albums[di]
				if m.libSortLatest {
					if a.Date != "" && a.Date != "0000" {
						label = a.Date + " " + a.AlbumArtist + " - " + a.Album
					} else {
						label = a.AlbumArtist + " - " + a.Album
					}
				} else {
					label = a.Album
					if a.Date != "" && a.Date != "0000" {
						label = a.Date + " " + a.Album
					}
				}
			}
		case libTracks:
			if di >= 0 && di < len(m.tracks) {
				label = m.tracks[di].Title
			}
		}
	}

	header := titleStyle.Render("Action") + "  " + label + "\n\n"
	var items []string
	for i := 0; i < optCount; i++ {
		prefix := "  "
		if i == m.menuCursor {
			prefix = "\u25b8 "
			items = append(items, lipgloss.NewStyle().Background(selectedBg).Foreground(lipgloss.Color("#ffffff")).Bold(true).Render(prefix+menuOptions[i]))
		} else {
			items = append(items, prefix+menuOptions[i])
		}
	}

	hints := "\n\n" + dimStyle.Render("[\u2191\u2193]navigate [enter]confirm [esc]cancel")
	content := header + strings.Join(items, "\n") + hints

	box := lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(accentColor).
		Padding(1, 3).
		Render(content)

	return lipgloss.Place(m.width, m.height, lipgloss.Center, lipgloss.Center, box)
}

func (m model) searchView() string {
	w := m.width
	h := m.height

	prompt := titleStyle.Render("> ") + m.searchInput.View()
	hints := dimStyle.Render("[esc]close [\u2191\u2193]navigate [enter]action menu")

	resH := h - 2
	if resH < 1 {
		resH = 1
	}

	var items []string
	cursorVisual := 0
	nAlbums := len(m.searchRes.Albums)
	if nAlbums > 0 {
		items = append(items, dimStyle.Render(fmt.Sprintf(" Albums (%d)", nAlbums)))
		for i, a := range m.searchRes.Albums {
			if i == m.srCursor {
				cursorVisual = len(items)
			}
			label := a.AlbumArtist + " \u2014 " + a.Album
			if a.Date != "" {
				label += " (" + a.Date + ")"
			}
			items = append(items, m.srRow(i, label, w))
		}
	}
	if len(m.searchRes.Tracks) > 0 {
		items = append(items, dimStyle.Render(fmt.Sprintf(" Tracks (%d)", len(m.searchRes.Tracks))))
		timeW := 6
		ratingW := 6
		innerW := w - timeW - ratingW - 4
		artistW := innerW * 25 / 100
		titleW := innerW * 35 / 100
		albumW := innerW - artistW - titleW
		if artistW < 5 {
			artistW = 5
		}
		if titleW < 5 {
			titleW = 5
		}
		if albumW < 5 {
			albumW = 5
		}
		ratingStyle := lipgloss.NewStyle().Foreground(lipgloss.Color("#e6b422"))
		for i, t := range m.searchRes.Tracks {
			if nAlbums+i == m.srCursor {
				cursorVisual = len(items)
			}
			artist := padRight(truncate(t.Artist, artistW), artistW)
			title := padRight(truncate(t.Title, titleW), titleW)
			album := padRight(truncate(t.Album, albumW), albumW)
			stars := padRight(renderRating(t.Rating), ratingW)
			dur := fmtTime(t.Duration)
			dur = strings.Repeat(" ", timeW-len(dur)) + dur

			isCursor := nAlbums+i == m.srCursor
			s := lipgloss.NewStyle().Width(w)
			if isCursor {
				s = s.Background(selectedBg).Foreground(lipgloss.Color("#ffffff")).Bold(true)
				items = append(items, s.Render(" "+artist+" "+title+" "+album+" "+stars+dur))
			} else {
				items = append(items, s.Render(" "+artist+" "+title+" "+dimStyle.Render(album)+" "+ratingStyle.Render(stars)+dimStyle.Render(dur)))
			}
		}
	}

	m.srOffset = scrollOffset(cursorVisual, m.srOffset, resH, len(items))
	end := m.srOffset + resH
	if end > len(items) {
		end = len(items)
	}

	var body string
	if m.srTotal == 0 && strings.TrimSpace(m.searchInput.Value()) != "" {
		body = dimStyle.Render(" No results")
		for i := 1; i < resH; i++ {
			body += "\n"
		}
	} else if len(items) > 0 {
		visible := items[m.srOffset:end]
		body = strings.Join(visible, "\n")
		for i := len(visible); i < resH; i++ {
			body += "\n"
		}
	} else {
		for i := 0; i < resH; i++ {
			if i > 0 {
				body += "\n"
			}
		}
	}

	return prompt + "\n" + body + "\n" + hints
}

func (m model) srRow(idx int, text string, w int) string {
	s := lipgloss.NewStyle().Width(w)
	if idx == m.srCursor {
		s = s.Background(selectedBg).Foreground(lipgloss.Color("#ffffff")).Bold(true)
		return s.Render(" " + truncate(text, w-2))
	}
	return s.Render(" " + truncate(text, w-2))
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

func renderRating(r int) string {
	if r <= 0 {
		return ""
	}
	full := r / 2
	half := r % 2
	empty := 5 - full - half
	return strings.Repeat("★", full) + strings.Repeat("⯪", half) + strings.Repeat("☆", empty)
}

func fmtTime(s float64) string {
	if s < 0 {
		s = 0
	}
	m := int(s) / 60
	sec := int(s) % 60
	return fmt.Sprintf("%d:%02d", m, sec)
}

func truncate(s string, max int) string {
	if max < 1 {
		return ""
	}
	if runewidth.StringWidth(s) <= max {
		return s
	}
	if max <= 1 {
		return "\u2026"
	}
	return runewidth.Truncate(s, max-1, "") + "\u2026"
}

func padRight(s string, w int) string {
	sw := runewidth.StringWidth(s)
	if sw >= w {
		return s
	}
	return s + strings.Repeat(" ", w-sw)
}

func scrollOffset(cursor, offset, visible, total int) int {
	if total <= visible {
		return 0
	}
	o := cursor - visible/2
	if o < 0 {
		o = 0
	}
	if o > total-visible {
		o = total - visible
	}
	return o
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

func main() {
	cfg = loadTUIConfig()

	mpd, _ = newMPDClient(cfg.MPDHost, cfg.MPDPort)
	defer func() {
		if mpd != nil {
			mpd.close()
		}
	}()

	p := tea.NewProgram(newModel(), tea.WithAltScreen(), tea.WithMouseCellMotion())
	if _, err := p.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
}
