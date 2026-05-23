package main

import (
	"bufio"
	"errors"
	"fmt"
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
var idleConn *mpdClient      // dedicated connection for MPD idle
var lastQueueVersion int     // tracks MPD playlist version to skip redundant queue fetches
var statusFetchTime time.Time // when status was last fetched (for elapsed interpolation)

func reconnectMPD() {
	if mpd != nil {
		mpd.close()
		mpd = nil
	}
	c, err := newMPDClient(cfg.MPDHost, cfg.MPDPort)
	if err != nil {
		return
	}
	mpd = c
}

// ---------------------------------------------------------------------------
// API types (reused from old code, now populated from MPD responses)
// ---------------------------------------------------------------------------

type playbackStatus struct {
	State       string
	Title       string
	Artist      string
	AlbumArtist string
	Album       string
	Date        string
	TimePos     float64
	Dur         float64
	Volume      int
	Rating      int
	SongID      string // X-SongId for rating commands
	SongPos     int    // current song position in queue
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
}

type albumEntry struct {
	ID          string // composite: artist\x00album\x00date
	AlbumArtist string
	Album       string
	Date        string
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
}

type artistsMsg []string

type albumsMsg []albumEntry

type tracksMsg []trackEntry

type albumRatingMsg struct {
	rating   int
	computed float64
}

type ratingPopupMsg struct{ rating int }

type searchMsg searchResult
type searchDebounceMsg struct{ gen int }

type playlistsMsg []playlistEntry

type playlistTracksMsg []trackEntry

type plPickerReadyMsg []playlistEntry

type devicesMsg struct {
	devices []deviceInfo
	active  int // output ID of enabled device, -1 if none
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
	libSortMtime bool
	artists      []string
	albums       []albumEntry
	tracks       []trackEntry
	curArtist          string
	curAlbum           *albumEntry
	albumRating        int
	albumComputedRating float64
	libCursor          int
	libOffset    int
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

	// rating popup
	showRating    bool
	ratingCursor  int         // 0=unrate, 1-10 = rating values
	ratingIsAlbum bool        // true when rating an album
	ratingAlbum   *albumEntry // album being rated (from album list)

	// devices (outputs)
	showDevices  bool
	devices      []deviceInfo
	activeDevice int // output ID
	devCursor    int

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
	_, err := idleConn.w.WriteString("idle player playlist mixer options database stored_playlist\n")
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
	if mpd == nil {
		reconnectMPD()
		if mpd == nil {
			return statusMsg{}
		}
	}

	// First fetch status + currentsong
	results, err := mpd.cmdBatch([]string{"status", "currentsong"})
	if err != nil || len(results) < 2 {
		reconnectMPD()
		return statusMsg{}
	}

	st := parseKV(results[0])
	cs := parseKV(results[1])

	var ps playbackStatus
	ps.State = st["state"]
	ps.TimePos, _ = strconv.ParseFloat(st["elapsed"], 64)
	ps.Dur, _ = strconv.ParseFloat(st["duration"], 64)
	ps.Volume, _ = strconv.Atoi(st["volume"])
	ps.Title = cs["Title"]
	ps.Artist = cs["Artist"]
	ps.AlbumArtist = cs["AlbumArtist"]
	ps.Album = cs["Album"]
	ps.Date = cs["Date"]
	ps.Rating, _ = strconv.Atoi(cs["X-Rating"])
	ps.SongID = cs["X-SongId"]
	ps.SongPos = -1
	if v, ok := st["song"]; ok {
		ps.SongPos, _ = strconv.Atoi(v)
	}
	statusFetchTime = time.Now()

	curPos := ps.SongPos

	// Check if queue version changed
	qVersion, _ := strconv.Atoi(st["playlist"])
	if qVersion == lastQueueVersion && lastQueueVersion > 0 {
		// Queue unchanged — return status only, update current position
		return statusMsg{status: ps, queueVersion: qVersion, queueChanged: false}
	}

	// Queue changed — fetch it
	qResults, err := mpd.cmdBatch([]string{"playlistinfo"})
	if err != nil || len(qResults) < 1 {
		return statusMsg{status: ps, queueVersion: qVersion, queueChanged: false}
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
		})
	}

	lastQueueVersion = qVersion
	return statusMsg{status: ps, queue: queue, queueVersion: qVersion, queueChanged: true}
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
			tracks = append(tracks, trackEntry{
				ID:          g["file"],
				XSongID:     g["X-SongId"],
				Title:       g["Title"],
				Artist:      g["Artist"],
				Album:       g["Album"],
				TrackNumber: tn,
				Rating:      r,
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
			tracks = append(tracks, trackEntry{
				ID:          g["file"],
				XSongID:     g["X-SongId"],
				Title:       g["Title"],
				Artist:      g["Artist"],
				Album:       g["Album"],
				TrackNumber: tn,
				Rating:      r,
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

func rateTrack(songID, rating string) tea.Cmd {
	return func() tea.Msg {
		if mpd == nil {
			return fetchStatus()
		}
		mpd.cmd("rate " + songID + " " + rating)
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
		if m.status.State == "playing" {
			m.status.TimePos += time.Since(statusFetchTime).Seconds()
			statusFetchTime = time.Now()
			if m.status.TimePos > m.status.Dur && m.status.Dur > 0 {
				m.status.TimePos = m.status.Dur
			}
		}
		return m, tickCmd()

	case idleMsg:
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
		return m, nil

	case artistsMsg:
		m.artists = msg
		m.libCursor = 0
		m.libOffset = 0
		return m, nil

	case albumsMsg:
		m.albums = msg
		m.libMode = libAlbums
		m.libCursor = 0
		m.libOffset = 0
		return m, tea.ClearScreen

	case tracksMsg:
		m.tracks = msg
		m.libMode = libTracks
		m.libCursor = 0
		m.libOffset = 0
		return m, tea.ClearScreen

	case albumRatingMsg:
		m.albumRating = msg.rating
		m.albumComputedRating = msg.computed
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
		return m, tea.ClearScreen

	case playlistTracksMsg:
		m.playlistTracks = msg
		m.libMode = libPlaylistTracks
		m.libCursor = 0
		m.libOffset = 0
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
			seekY := m.height - 2
			if msg.Y == seekY && m.status.Dur > 0 {
				posStr := fmtTime(m.status.TimePos)
				durStr := fmtTime(m.status.Dur)
				barStart := len(posStr) + 1
				barW := m.width - len(posStr) - len(durStr) - 6
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
	if key == "q" && !m.searching && !m.showMenu && !m.showHelp && !m.showRating {
		return m, tea.Quit
	}

	if m.showHelp {
		m.showHelp = false
		return m, nil
	}

	if m.showRating {
		return m.handleRatingKey(key)
	}

	if m.showPlPicker {
		return m.handlePlPickerKey(msg, key)
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
		// Open rating popup — context-aware
		m.showRating = true
		m.ratingIsAlbum = false
		// Determine current rating to position cursor
		if m.focus == panelQueue && m.qCursor < len(m.queue) {
			m.ratingCursor = m.queue[m.qCursor].Rating
		} else if m.focus == panelLibrary && m.libMode == libAlbums {
			// Rating an album from the album list
			di := m.dataIndex()
			if di >= 0 && di < len(m.albums) {
				m.ratingIsAlbum = true
				m.ratingCursor = 0 // will be fetched
				a := m.albums[di]
				m.ratingAlbum = &a
				return m, fetchAlbumRatingForPopup(a.AlbumArtist, a.Album, a.Date)
			}
		} else if m.focus == panelLibrary && m.libMode == libTracks {
			di := m.dataIndex()
			if di >= 0 && di < len(m.tracks) && m.tracks[di].XSongID != "" {
				m.ratingCursor = m.tracks[di].Rating
			} else {
				// No track under cursor, default to current song
				m.ratingCursor = m.status.Rating
			}
		} else {
			m.ratingCursor = m.status.Rating
		}
		return m, nil
	case "u":
		return m, mpdCommand("update")
	case "?":
		m.showHelp = true
		return m, nil
	case "P":
		return m, fetchPlaylists
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

var menuOptions = []string{"Add to queue", "Insert after current", "Replace queue", "Browse into"}

func (m model) menuOptionCount() int {
	if m.menuSource == "search" {
		idx := m.srCursor
		nAlbums := len(m.searchRes.Albums)
		if idx < nAlbums {
			return 4
		}
		return 3
	}
	if m.libMode == libTracks || m.libMode == libPlaylistTracks {
		return 3
	}
	return len(menuOptions)
}

func (m model) handleMenuKey(key string) (tea.Model, tea.Cmd) {
	maxIdx := m.menuOptionCount() - 1
	switch key {
	case "esc", "q":
		m.showMenu = false
		return m, nil
	case "j", "down":
		if m.menuCursor < maxIdx {
			m.menuCursor++
		}
		return m, nil
	case "k", "up":
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
				return m.searchAction("insert")
			case 2:
				return m.searchAction("replace")
			case 3:
				return m.searchDrillIn()
			}
		} else {
			switch m.menuCursor {
			case 0:
				return m.libAction("add")
			case 1:
				return m.libAction("insert")
			case 2:
				return m.libAction("replace")
			case 3:
				return m.libDrillIn()
			}
		}
	}
	return m, nil
}

func (m model) handleLibKey(key string) (tea.Model, tea.Cmd) {
	listLen := m.libListLen()

	switch key {
	case "j", "down":
		if m.libCursor < listLen-1 {
			m.libCursor++
		}
		return m, nil
	case "k", "up":
		if m.libCursor > 0 {
			m.libCursor--
		}
		return m, nil
	case "g", "home":
		m.libCursor = 0
		return m, nil
	case "G", "end":
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
		di := m.dataIndex()
		if di < 0 {
			return m.libBack()
		}
		m.showMenu = true
		m.menuCursor = 0
		m.menuSource = "library"
		return m, nil
	case "l", "right":
		return m.libDrillIn()
	case "h", "left", "backspace":
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
		if m.libMode == libArtists {
			m.libSortMtime = !m.libSortMtime
			m.libCursor = 0
			m.libOffset = 0
			// MPD doesn't have mtime sort directly; just re-fetch
			return m, fetchArtists
		}
	}
	return m, nil
}

func (m model) libListLen() int {
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

func (m model) dataIndex() int {
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
		m.libMode = libArtists
		m.libCursor = m.savedArtistCursor
		m.libOffset = m.savedArtistOffset
	case libTracks:
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
	case "j", "down":
		if m.qCursor < qLen-1 {
			m.qCursor++
		}
	case "k", "up":
		if m.qCursor > 0 {
			m.qCursor--
		}
	case "g", "home":
		m.qCursor = 0
	case "G", "end":
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
	case "J":
		if len(m.qSelected) > 0 {
			positions := sortedSelected(m.qSelected)
			if positions[len(positions)-1] >= qLen-1 {
				return m, nil
			}
			cmds := make([]string, 0, len(positions))
			for i := len(positions) - 1; i >= 0; i-- {
				cmds = append(cmds, fmt.Sprintf("move %d %d", positions[i], positions[i]+1))
			}
			newSel := map[int]bool{}
			for _, p := range positions {
				newSel[p+1] = true
			}
			m.qSelected = newSel
			m.qCursor++
			return m, mpdCommand(cmds...)
		}
		if m.qCursor < qLen-1 {
			m.qCursor++
			return m, mpdCommand(fmt.Sprintf("move %d %d", m.qCursor-1, m.qCursor))
		}
	case "K":
		if len(m.qSelected) > 0 {
			positions := sortedSelected(m.qSelected)
			if positions[0] <= 0 {
				return m, nil
			}
			cmds := make([]string, 0, len(positions))
			for _, p := range positions {
				cmds = append(cmds, fmt.Sprintf("move %d %d", p, p-1))
			}
			newSel := map[int]bool{}
			for _, p := range positions {
				newSel[p-1] = true
			}
			m.qSelected = newSel
			m.qCursor--
			return m, mpdCommand(cmds...)
		}
		if m.qCursor > 0 {
			m.qCursor--
			return m, mpdCommand(fmt.Sprintf("move %d %d", m.qCursor+1, m.qCursor))
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
	case "j", "down":
		if m.plPickerCursor < len(m.plPickerList)-1 {
			m.plPickerCursor++
		}
	case "k", "up":
		if m.plPickerCursor > 0 {
			m.plPickerCursor--
		}
	case "enter":
		if m.plPickerCursor < len(m.plPickerList) {
			name := m.plPickerList[m.plPickerCursor].Name
			m.showPlPicker = false
			return m, addToPlaylist(name, m.plPickerURI)
		}
	case "n":
		m.plPickerNewMode = true
		m.plPickerInput.SetValue("")
		m.plPickerInput.Focus()
		return m, textinput.Blink
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
	case "j", "down":
		if m.devCursor < len(m.devices)-1 {
			m.devCursor++
		}
		return m, nil
	case "k", "up":
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

func (m model) handleRatingKey(key string) (tea.Model, tea.Cmd) {
	switch key {
	case "esc", "q", "*":
		m.showRating = false
		return m, nil
	case "j", "down":
		if m.ratingCursor < 10 {
			m.ratingCursor++
		}
		return m, nil
	case "k", "up":
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
		if m.focus == panelQueue && m.qCursor < len(m.queue) {
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
	if m.showRating {
		return m.ratingView()
	}
	if m.showPlPicker {
		return m.plPickerView()
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

	playerH := 3
	mainH := m.height - playerH
	if mainH < 3 {
		mainH = 3
	}

	libW := m.width * 35 / 100
	if libW < 20 {
		libW = 20
	}
	queueW := m.width - libW
	if queueW < 20 {
		queueW = 20
	}

	lib := m.libraryView(libW-2, mainH-2)
	que := m.queueView(queueW-2, mainH-2)

	libBorder := panelBorder
	queBorder := panelBorder
	if m.focus == panelLibrary {
		libBorder = focusBorder
	} else {
		queBorder = focusBorder
	}

	libPanel := libBorder.Width(libW - 2).Height(mainH - 2).Render(lib)
	quePanel := queBorder.Width(queueW - 2).Height(mainH - 2).Render(que)

	main := lipgloss.JoinHorizontal(lipgloss.Top, libPanel, quePanel)
	player := m.playerView()

	return main + "\n" + player
}

func (m model) libraryView(w, h int) string {
	var breadcrumbs []string
	var items []string

	switch m.libMode {
	case libArtists:
		breadcrumbs = []string{fmt.Sprintf("Artists (%d)", len(m.artists))}
		for i, a := range m.artists {
			items = append(items, m.libRow(i, a+" >", "", 0, w))
		}
	case libAlbums:
		breadcrumbs = []string{"Artists", m.curArtist, fmt.Sprintf("Albums (%d)", len(m.albums))}
		for i, a := range m.albums {
			label := a.Album
			if a.Date != "" && a.Date != "0000" {
				label = a.Date + " " + a.Album
			}
			items = append(items, m.libRow(i, label+" >", "", 0, w))
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
			num := fmt.Sprintf("%2d", t.TrackNumber)
			items = append(items, m.libRow(i, t.Title, num, t.Rating, w))
		}
	case libPlaylists:
		breadcrumbs = []string{fmt.Sprintf("Playlists (%d)", len(m.playlists))}
		for i, pl := range m.playlists {
			label := pl.Name
			if pl.SongCount > 0 {
				label += fmt.Sprintf(" (%d)", pl.SongCount)
			}
			items = append(items, m.libRow(i, label+" >", "", 0, w))
		}
	case libPlaylistTracks:
		breadcrumbs = []string{"Playlists", m.curPlaylist, fmt.Sprintf("Tracks (%d)", len(m.playlistTracks))}
		for i, t := range m.playlistTracks {
			num := fmt.Sprintf("%2d", i+1)
			label := t.Title
			if t.Artist != "" {
				label += " - " + t.Artist
			}
			items = append(items, m.libRow(i, label, num, t.Rating, w))
		}
	}

	// Build breadcrumb bar as plain text, truncate to fit.
	title := strings.Join(breadcrumbs, " > ")
	title = truncate(title, w-2)
	hdr := headerStyle.Width(w).Render(title)
	visH := h - 1
	if visH < 1 {
		visH = 1
	}

	// Context hint — always reserve the line to keep layout stable
	var hint string
	switch m.libMode {
	case libArtists:
		hint = "[enter]browse  [/]search  [?]help"
	case libAlbums:
		hint = "[enter]browse  [bksp]back"
	case libTracks, libPlaylistTracks:
		hint = "[enter]add  [p]playlist  [bksp]back"
	case libPlaylists:
		hint = "[enter]browse  [bksp]back"
	}

	bodyH := visH - 1
	if bodyH < 1 {
		bodyH = 1
	}

	m.libOffset = scrollOffset(m.libCursor, m.libOffset, bodyH, len(items))

	end := m.libOffset + bodyH
	if end > len(items) {
		end = len(items)
	}
	visible := items[m.libOffset:end]

	body := strings.Join(visible, "\n")
	emptyRow := strings.Repeat(" ", w)
	for len(visible) < bodyH {
		body += "\n" + emptyRow
		visible = append(visible, "")
	}

	hintLine := dimStyle.Width(w).Render(hint)
	return hdr + "\n" + body + "\n" + hintLine
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
		stars := padRight(renderRating(q.Rating), ratingW)
		ratingStyle := lipgloss.NewStyle().Foreground(lipgloss.Color("#e6b422"))
		var row string
		if isCursor {
			row = marker + num + " " + artist + " " + title + " " + album + " " + stars + dur
		} else {
			row = marker + dimStyle.Render(num) + " " + artist + " " + title + " " + dimStyle.Render(album) + " " + ratingStyle.Render(stars) + dimStyle.Render(dur)
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

func (m model) playerView() string {
	w := m.width
	if w < 10 {
		w = 10
	}

	np := "\u2014"
	if m.status.Title != "" {
		np = m.status.Title
		if m.status.Artist != "" {
			np += " \u2014 " + m.status.Artist
		}
		if m.status.Album != "" {
			np += " \u2014 " + m.status.Album
		}
	}

	stateIcon := "\u25b6"
	if m.status.State == "play" {
		stateIcon = "\u23f8"
	} else if m.status.State == "stop" {
		stateIcon = "\u25a0"
	}

	posStr := fmtTime(m.status.TimePos)
	durStr := fmtTime(m.status.Dur)
	barW := w - len(posStr) - len(durStr) - 6
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

	ratingStr := ""
	if m.status.Rating > 0 {
		ratingStr = " " + lipgloss.NewStyle().Foreground(lipgloss.Color("#e6b422")).Render(renderRating(m.status.Rating))
	}
	line1 := titleStyle.Render(stateIcon) + " " + truncate(np, w-4-12) + ratingStr
	line2 := timeL + " " + bar + " " + timeR

	hints := dimStyle.Render("[/]search [?]help [space]play [<>]prev/next [s]stop [r]album [R]tracks [P]playlists [D]devices [*]rate [q]quit")

	return line1 + "\n" + line2 + "\n" + hints
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
			"  r          Random album",
			"  R          Random tracks",
			"  u          Update library",
			"  P          Playlists",
			"  D          Device picker",
			"  *          Rate track/album",
			"  Tab        Switch panel focus",
			"  q          Quit",
		}, "\n")},
		{"Library", strings.Join([]string{
			"  j/k        Navigate up/down",
			"  Enter      Action menu (Add/Insert/Replace/Browse)",
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

func (m model) plPickerView() string {
	header := titleStyle.Render("Add to Playlist") + "\n\n"

	if len(m.plPickerList) == 0 && !m.plPickerNewMode {
		header += dimStyle.Render("No playlists found")
	}

	var items []string
	for i, pl := range m.plPickerList {
		name := pl.Name
		if pl.SongCount > 0 {
			name += dimStyle.Render(fmt.Sprintf(" (%d)", pl.SongCount))
		}
		if i == m.plPickerCursor && !m.plPickerNewMode {
			s := lipgloss.NewStyle().Background(selectedBg).Foreground(lipgloss.Color("#ffffff")).Bold(true)
			items = append(items, s.Render(" "+pl.Name+" "))
		} else {
			items = append(items, " "+name)
		}
	}

	var newLine string
	if m.plPickerNewMode {
		newLine = "\n\n" + m.plPickerInput.View()
	}

	hints := "\n\n" + dimStyle.Render("[↑↓]navigate [enter]add [n]new playlist [esc]close")
	content := header + strings.Join(items, "\n") + newLine + hints

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
				label = a.Album
				if a.Date != "" && a.Date != "0000" {
					label = a.Date + " " + a.Album
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
		for i, t := range m.searchRes.Tracks {
			if nAlbums+i == m.srCursor {
				cursorVisual = len(items)
			}
			label := t.Title + " \u2014 " + t.Artist
			items = append(items, m.srRow(nAlbums+i, label, w))
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
