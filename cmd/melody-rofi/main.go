package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/BurntSushi/toml"
	"github.com/carnager/melody/internal/shared"
)

const defaultLocalAPIAddress = shared.LocalAPIConfigValue

type config struct {
	API struct {
		Address string `toml:"address"`
		Secret  string `toml:"secret"`
	} `toml:"api"`
	Autostart struct {
		Enabled        bool     `toml:"enabled"`
		SystemdUnit    string   `toml:"systemd_unit"`
		Command        []string `toml:"command"`
		TimeoutSeconds float64  `toml:"timeout_seconds"`
	} `toml:"autostart"`
	UI struct {
		Menu []string `toml:"menu"`
	} `toml:"ui"`
	Columns struct {
		Artist      int `toml:"artist"`
		AlbumArtist int `toml:"album_artist"`
		Date        int `toml:"date"`
		Album       int `toml:"album"`
		ID          int `toml:"id"`
		Title       int `toml:"title"`
		Track       int `toml:"track"`
	} `toml:"columns"`
}

type album struct {
	ID          string `json:"id"`
	AlbumArtist string `json:"albumartist"`
	Album       string `json:"album"`
	Date        string `json:"date"`
	Rating      any    `json:"rating"`
}

type track struct {
	ID     string `json:"id"`
	Track  any    `json:"track"`
	Title  any    `json:"title"`
	Artist any    `json:"artist"`
	Album  any    `json:"album"`
	Date   any    `json:"date"`
	Rating any    `json:"rating"`
}

type currentAlbum struct {
	Rating      any    `json:"rating"`
	AlbumArtist string `json:"albumartist"`
	Album       string `json:"album"`
}

type currentTrack struct {
	Rating any `json:"rating"`
	Title  any `json:"title"`
	Artist any `json:"artist"`
}

type cacheStatus struct {
	Version              int64  `json:"version"`
	UpdatedAt            string `json:"updated_at"`
	Stale                bool   `json:"stale"`
	NavidromeConnected   bool   `json:"navidrome_connected"`
	NavidromeScanning    bool   `json:"navidrome_scanning"`
	NavidromeLastScanned string `json:"navidrome_last_scanned"`
}

type apiClient struct {
	baseURL              string
	secret               string
	autoStartLocalDaemon bool
	useLocalSocket       bool
	localServiceUnit     string
	localServiceCommand  []string
	startupTimeout       time.Duration
	httpClient           *http.Client
	autostartAttempted   bool
}

func main() {
	cfg, cfgPath, err := loadConfig()
	if err != nil {
		fatal(err)
	}

	var (
		optAlbums   = flag.Bool("a", false, "")
		optLatest   = flag.Bool("l", false, "")
		optTracks   = flag.Bool("t", false, "")
		optRandomA  = flag.Bool("A", false, "")
		optRandomT  = flag.Bool("T", false, "")
		optCurrent  = flag.Bool("c", false, "")
		optUpdate   = flag.Bool("u", false, "")
		optRegen    = flag.Bool("x", false, "")
		optHelp     = flag.Bool("h", false, "")
		apiAddress  = flag.String("api-address", "", "")
		noAutostart = flag.Bool("no-auto-start-local-daemon", false, "")
	)
	flag.Parse()

	displayAddress := apiAddressInput(cfg, *apiAddress)
	effectiveURL, implicitLocal, useLocalSocket, socketPath, err := resolveAPIAddress(cfg, *apiAddress)
	if err != nil {
		fatalWithUI(cfg, err)
	}
	if *optHelp || !(*optAlbums || *optLatest || *optTracks || *optRandomA || *optRandomT || *optCurrent || *optUpdate || *optRegen) {
		fmt.Print(helpText(displayAddress, implicitLocal))
		return
	}
	if *optRegen {
		if err := os.WriteFile(cfgPath, []byte(defaultConfigText()), 0o644); err != nil {
			fatalWithUI(cfg, err)
		}
		fmt.Printf("Wrote default config to %s\n", cfgPath)
		return
	}

	client := newAPIClient(cfg, effectiveURL, implicitLocal && !*noAutostart, useLocalSocket, socketPath)
	if err := client.ensureAvailable(); err != nil {
		fatalWithUI(cfg, err)
	}

	switch {
	case *optAlbums:
		if err := addAlbumUI(cfg, client, "album"); err != nil {
			fatalWithUI(cfg, err)
		}
	case *optLatest:
		if err := addAlbumUI(cfg, client, "latest"); err != nil {
			fatalWithUI(cfg, err)
		}
	case *optTracks:
		if err := addTrackUI(cfg, client); err != nil {
			fatalWithUI(cfg, err)
		}
	case *optRandomA:
		if err := client.post("playback/random/album", nil, nil); err != nil {
			fatalWithUI(cfg, err)
		}
	case *optRandomT:
		if err := client.post("playback/random/tracks", nil, nil); err != nil {
			fatalWithUI(cfg, err)
		}
	case *optCurrent:
		if err := currentTrackUI(cfg, client); err != nil {
			fatalWithUI(cfg, err)
		}
	case *optUpdate:
		if err := client.post("cache/update", nil, nil); err != nil {
			fatalWithUI(cfg, err)
		}
	}
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, err)
	os.Exit(1)
}

func fatalWithUI(cfg config, err error) {
	if err == nil {
		return
	}
	_ = showErrorMenu(cfg, err)
	fatal(err)
}

func loadConfig() (config, string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return config{}, "", err
	}
	xdgConfig := getenv("XDG_CONFIG_HOME", filepath.Join(home, ".config"))
	confDir := filepath.Join(xdgConfig, "melody")
	confPath := filepath.Join(confDir, "melody-rofi.toml")
	if err := os.MkdirAll(confDir, 0o755); err != nil {
		return config{}, "", err
	}
	if _, err := os.Stat(confPath); errors.Is(err, os.ErrNotExist) {
		if err := os.WriteFile(confPath, []byte(defaultConfigText()), 0o644); err != nil {
			return config{}, "", err
		}
	}

	var raw map[string]any
	if _, err := toml.DecodeFile(confPath, &raw); err != nil {
		return config{}, "", err
	}
	var cfg config
	api, _ := raw["api"].(map[string]any)
	autostart, _ := raw["autostart"].(map[string]any)
	ui, _ := raw["ui"].(map[string]any)
	columns, _ := raw["columns"].(map[string]any)
	cfg.API.Address = stringify(api["address"])
	cfg.Autostart.Enabled = boolFromAny(autostart["enabled"], true)
	cfg.Autostart.SystemdUnit = stringify(autostart["systemd_unit"])
	cfg.Autostart.Command = stringSlice(autostart["command"])
	cfg.Autostart.TimeoutSeconds = floatFromAny(autostart["timeout_seconds"], 5.0)
	cfg.UI.Menu = stringSlice(ui["menu"])
	cfg.Columns.Artist = intFromAny(columns["artist"], 30)
	cfg.Columns.AlbumArtist = intFromAny(columns["album_artist"], 30)
	cfg.Columns.Date = intFromAny(columns["date"], 6)
	cfg.Columns.Album = intFromAny(columns["album"], 40)
	cfg.Columns.ID = intFromAny(columns["id"], 5)
	cfg.Columns.Title = intFromAny(columns["title"], 40)
	cfg.Columns.Track = intFromAny(columns["track"], 5)
	applyDefaults(&cfg)
	return cfg, confPath, nil
}

func defaultConfigText() string {
	return `[api]
address = "local"

[autostart]
enabled = true
systemd_unit = "melodyd.service"
command = ["melodyd"]
timeout_seconds = 5.0

[ui]
menu = ["rofi", "-dmenu", "-p", "Melody"]

[columns]
artist = 30
album_artist = 30
date = 6
album = 40
id = 5
title = 40
track = 5
`
}

func applyDefaults(cfg *config) {
	if cfg.API.Address == "" {
		cfg.API.Address = defaultLocalAPIAddress
	}
	if cfg.Autostart.SystemdUnit == "" {
		cfg.Autostart.SystemdUnit = "melodyd.service"
	}
	if cfg.Autostart.TimeoutSeconds <= 0 {
		cfg.Autostart.TimeoutSeconds = 5
	}
	if len(cfg.Autostart.Command) == 0 {
		cfg.Autostart.Command = []string{"melodyd"}
	}
	if len(cfg.UI.Menu) == 0 {
		cfg.UI.Menu = []string{"rofi", "-dmenu", "-p", "Melody"}
	}
	if cfg.Columns.Artist == 0 {
		cfg.Columns.Artist = 30
	}
	if cfg.Columns.AlbumArtist == 0 {
		cfg.Columns.AlbumArtist = 30
	}
	if cfg.Columns.Date == 0 {
		cfg.Columns.Date = 6
	}
	if cfg.Columns.Album == 0 {
		cfg.Columns.Album = 40
	}
	if cfg.Columns.ID == 0 {
		cfg.Columns.ID = 5
	}
	if cfg.Columns.Title == 0 {
		cfg.Columns.Title = 40
	}
	if cfg.Columns.Track == 0 {
		cfg.Columns.Track = 5
	}
}

func apiAddressInput(cfg config, override string) string {
	address := strings.TrimSpace(override)
	if address == "" {
		address = strings.TrimSpace(cfg.API.Address)
	}
	if address == "" {
		address = defaultLocalAPIAddress
	}
	return address
}

func resolveAPIAddress(cfg config, override string) (string, bool, bool, string, error) {
	address := apiAddressInput(cfg, override)
	baseURL, useLocalSocket, socketPath, err := shared.APIBaseURLFromAddress(address)
	if err != nil {
		return "", false, false, "", err
	}
	implicitLocal := useLocalSocket || shared.IsLoopbackTCPAddress(address)
	return baseURL, implicitLocal, useLocalSocket, socketPath, nil
}

func helpText(apiBaseURL string, autoStart bool) string {
	auto := "no"
	if autoStart {
		auto = "yes"
	}
	return fmt.Sprintf(`Usage: melody-rofi [option] [--api-address ADDRESS] [--no-auto-start-local-daemon]
 -a  Add Albums
 -l  Add Latest Albums
 -t  Add Tracks
 -A  Random Album
 -T  Random Tracks
 -c  Rate Current Track/Album
 -u  Rebuild Caches
 -x  Regenerate UI Config
 -h  Show This Help

Defaults:
 api.address = %s
 autostart.enabled = %s
`, apiBaseURL, auto)
}

func newAPIClient(cfg config, baseURL string, autoStart bool, useLocalSocket bool, socketPath string) *apiClient {
	httpClient := &http.Client{Timeout: 10 * time.Second}
	if useLocalSocket {
		httpClient = shared.NewLocalHTTPClient(10*time.Second, socketPath)
	}
	return &apiClient{
		baseURL:              baseURL,
		secret:               cfg.API.Secret,
		autoStartLocalDaemon: autoStart && cfg.Autostart.Enabled,
		useLocalSocket:       useLocalSocket,
		localServiceUnit:     cfg.Autostart.SystemdUnit,
		localServiceCommand:  cfg.Autostart.Command,
		startupTimeout:       time.Duration(cfg.Autostart.TimeoutSeconds * float64(time.Second)),
		httpClient:           httpClient,
	}
}

func (c *apiClient) ensureAvailable() error {
	if !c.autoStartLocalDaemon {
		return nil
	}
	if c.healthcheck() {
		return nil
	}
	if err := c.ensureLocalService(); err != nil {
		return fmt.Errorf("local Melody daemon is not reachable at %s and could not be started: %w", c.baseURL, err)
	}
	return nil
}

func (c *apiClient) healthcheck() bool {
	req, _ := http.NewRequest(http.MethodGet, c.baseURL+"/health", nil)
	if c.secret != "" {
		req.Header.Set("Authorization", "Bearer "+c.secret)
	}
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	return resp.StatusCode >= 200 && resp.StatusCode < 300
}

func (c *apiClient) ensureLocalService() error {
	if c.autostartAttempted {
		return errors.New("daemon start already attempted")
	}
	c.autostartAttempted = true
	if err := c.startWithSystemd(); err != nil {
		if err := c.startWithCommand(); err != nil {
			return err
		}
	}
	deadline := time.Now().Add(c.startupTimeout)
	for time.Now().Before(deadline) {
		if c.healthcheck() {
			return nil
		}
		time.Sleep(200 * time.Millisecond)
	}
	return errors.New("startup timed out")
}

func (c *apiClient) startWithSystemd() error {
	if c.localServiceUnit == "" {
		return errors.New("no systemd unit configured")
	}
	if _, err := exec.LookPath("systemctl"); err != nil {
		return err
	}
	cmd := exec.Command("systemctl", "--user", "start", c.localServiceUnit)
	cmd.Stdout = io.Discard
	cmd.Stderr = io.Discard
	return cmd.Run()
}

func (c *apiClient) startWithCommand() error {
	if len(c.localServiceCommand) == 0 {
		return errors.New("no local service command configured")
	}
	cmd := exec.Command(c.localServiceCommand[0], c.localServiceCommand[1:]...)
	cmd.Stdout = io.Discard
	cmd.Stderr = io.Discard
	cmd.Stdin = nil
	return cmd.Start()
}

func (c *apiClient) get(path string, out any) error {
	req, _ := http.NewRequest(http.MethodGet, c.baseURL+"/"+strings.TrimLeft(path, "/"), nil)
	return c.do(req, out, true)
}

func (c *apiClient) post(path string, body any, out any) error {
	var reader io.Reader
	if body != nil {
		data, err := json.Marshal(body)
		if err != nil {
			return err
		}
		reader = bytes.NewReader(data)
	}
	req, _ := http.NewRequest(http.MethodPost, c.baseURL+"/"+strings.TrimLeft(path, "/"), reader)
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	return c.do(req, out, true)
}

func (c *apiClient) cacheStatus() (cacheStatus, error) {
	var status cacheStatus
	if err := c.get("cache/status", &status); err != nil {
		return cacheStatus{}, err
	}
	return status, nil
}

func (c *apiClient) ensureFreshCache() error {
	status, err := c.cacheStatus()
	if err != nil {
		return nil
	}
	if !status.Stale || status.NavidromeScanning {
		return nil
	}
	return c.post("cache/update", nil, nil)
}

func (c *apiClient) do(req *http.Request, out any, retryOnConnectError bool) error {
	if c.secret != "" {
		req.Header.Set("Authorization", "Bearer "+c.secret)
	}
	resp, err := c.httpClient.Do(req)
	if err != nil {
		if retryOnConnectError && c.autoStartLocalDaemon {
			if startErr := c.ensureLocalService(); startErr == nil {
				clone := req.Clone(req.Context())
				if req.Body != nil && req.GetBody != nil {
					body, _ := req.GetBody()
					clone.Body = body
				}
				return c.do(clone, out, false)
			}
		}
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		body, _ := io.ReadAll(resp.Body)
		return errors.New(apiErrorMessage(req, resp.StatusCode, body))
	}
	if out == nil {
		io.Copy(io.Discard, resp.Body)
		return nil
	}
	return json.NewDecoder(resp.Body).Decode(out)
}

func apiErrorMessage(req *http.Request, status int, body []byte) string {
	if message := decodeAPIMessage(body); message != "" {
		return message
	}
	text := strings.TrimSpace(string(body))
	if text == "" {
		return fmt.Sprintf("http %d for %s", status, req.URL.String())
	}
	return fmt.Sprintf("http %d for %s - %s", status, req.URL.String(), text)
}

func decodeAPIMessage(body []byte) string {
	var payload map[string]any
	if err := json.Unmarshal(body, &payload); err != nil {
		return ""
	}
	if message := textOr(payload["error"], ""); message != "" {
		return message
	}
	if message := textOr(payload["message"], ""); message != "" {
		return message
	}
	return ""
}

func addAlbumUI(cfg config, client *apiClient, mode string) error {
	var albums []album
	endpoint := "albums"
	if mode == "latest" {
		endpoint = "latest_albums"
	}
	if err := client.ensureFreshCache(); err != nil {
		return err
	}
	if err := client.get(endpoint, &albums); err != nil {
		return err
	}
	if len(albums) == 0 {
		return errors.New("no albums available")
	}
	lines := make([]string, 0, len(albums))
	lineIDs := make(map[string]string, len(albums))
	for _, album := range albums {
		line := formatAlbumLine(cfg, album)
		lines = append(lines, line)
		lineIDs[menuSelectionKey(line)] = album.ID
	}
	selectedLines, err := runMenu(cfg, lines)
	if err != nil || len(selectedLines) == 0 {
		return err
	}
	selectedIDs, err := mapSelectedLinesToIDs(selectedLines, lineIDs)
	if err != nil || len(selectedIDs) == 0 {
		return err
	}
	action, err := runSingleMenu(cfg, []string{"Add", "Insert", "Replace", "Rate"})
	if err != nil || action == "" {
		return err
	}
	selected := filterAlbums(albums, selectedIDs)
	if action == "Rate" {
		for _, album := range selected {
			rating, err := inputRating(cfg)
			if err != nil {
				return err
			}
			if err := client.post("albums/"+album.ID+"/rating", map[string]string{
				"rating":    rating,
				"list_mode": mode,
			}, nil); err != nil {
				return err
			}
		}
		return nil
	}
	payload := map[string]any{
		"mode":      strings.ToLower(action),
		"list_mode": mode,
	}
	if len(selected) == 1 {
		return client.post("playlist/add/album/"+selected[0].ID, payload, nil)
	}
	ids := make([]string, 0, len(selected))
	for _, album := range selected {
		ids = append(ids, album.ID)
	}
	payload["album_ids"] = ids
	return client.post("playlist/add/albums", payload, nil)
}

func addTrackUI(cfg config, client *apiClient) error {
	var tracks []track
	if err := client.ensureFreshCache(); err != nil {
		return err
	}
	if err := client.get("tracks", &tracks); err != nil {
		return err
	}
	if len(tracks) == 0 {
		return errors.New("no tracks available")
	}
	lines := make([]string, 0, len(tracks))
	lineIDs := make(map[string]string, len(tracks))
	for _, track := range tracks {
		line := formatTrackLine(cfg, track)
		lines = append(lines, line)
		lineIDs[menuSelectionKey(line)] = track.ID
	}
	selectedLines, err := runMenu(cfg, lines)
	if err != nil || len(selectedLines) == 0 {
		return err
	}
	selectedIDs, err := mapSelectedLinesToIDs(selectedLines, lineIDs)
	if err != nil || len(selectedIDs) == 0 {
		return err
	}
	action, err := runSingleMenu(cfg, []string{"Add", "Insert", "Replace", "Rate Track"})
	if err != nil || action == "" {
		return err
	}
	selected := filterTracks(tracks, selectedIDs)
	if action == "Rate Track" {
		for _, track := range selected {
			rating, err := inputRating(cfg)
			if err != nil {
				return err
			}
			if err := client.post("tracks/"+track.ID+"/rating", map[string]string{"rating": rating}, nil); err != nil {
				return err
			}
		}
		return nil
	}
	payload := map[string]any{"mode": strings.ToLower(action)}
	if len(selected) == 1 {
		return client.post("playlist/add/track/"+selected[0].ID, payload, nil)
	}
	ids := make([]string, 0, len(selected))
	for _, track := range selected {
		ids = append(ids, track.ID)
	}
	payload["track_ids"] = ids
	return client.post("playlist/add/tracks", payload, nil)
}

func currentTrackUI(cfg config, client *apiClient) error {
	action, err := runSingleMenu(cfg, []string{"Rate Album", "Rate Track"})
	if err != nil || action == "" {
		return err
	}
	switch action {
	case "Rate Album":
		var current currentAlbum
		if err := client.get("current_album/rating", &current); err != nil {
			return err
		}
		rating, err := inputRating(cfg)
		if err != nil {
			return err
		}
		return client.post("current_album/rating", map[string]string{"rating": rating}, nil)
	case "Rate Track":
		var current currentTrack
		if err := client.get("current_track/rating", &current); err != nil {
			return err
		}
		rating, err := inputRating(cfg)
		if err != nil {
			return err
		}
		return client.post("current_track/rating", map[string]string{"rating": rating}, nil)
	default:
		return nil
	}
}

func runMenu(cfg config, lines []string) ([]string, error) {
	cmdArgs := make([]string, len(cfg.UI.Menu))
	copy(cmdArgs, cfg.UI.Menu)
	if len(cmdArgs) == 0 {
		return nil, errors.New("ui.menu is empty")
	}
	cmd := exec.Command(cmdArgs[0], cmdArgs[1:]...)
	cmd.Stderr = io.Discard
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	var stdout bytes.Buffer
	cmd.Stdout = &stdout
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	_, _ = io.WriteString(stdin, strings.Join(lines, "\n"))
	_ = stdin.Close()
	err = cmd.Wait()
	if err != nil {
		var exitErr *exec.ExitError
		if errors.As(err, &exitErr) {
			return nil, nil
		}
		return nil, err
	}
	outLines := strings.Split(strings.TrimSpace(stdout.String()), "\n")
	if len(outLines) == 1 && outLines[0] == "" {
		return nil, nil
	}
	return outLines, nil
}

func showErrorMenu(cfg config, err error) error {
	message := strings.TrimSpace(err.Error())
	if message == "" {
		return nil
	}
	_, runErr := runMenu(cfg, []string{"Error: " + message, "Press Enter or Escape to dismiss"})
	return runErr
}

func runSingleMenu(cfg config, lines []string) (string, error) {
	selected, err := runMenu(cfg, lines)
	if err != nil || len(selected) == 0 {
		return "", err
	}
	return strings.TrimSpace(selected[0]), nil
}

func inputRating(cfg config) (string, error) {
	result, err := runMenu(cfg, []string{"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "---", "Delete"})
	if err != nil || len(result) == 0 || strings.TrimSpace(result[0]) == "" {
		return "---", err
	}
	return strings.TrimSpace(result[0]), nil
}

func mapSelectedLinesToIDs(lines []string, lineIDs map[string]string) ([]string, error) {
	ids := make([]string, 0, len(lines))
	for _, line := range lines {
		id, ok := lineIDs[menuSelectionKey(line)]
		if !ok {
			return nil, fmt.Errorf("could not resolve menu selection %q", line)
		}
		ids = append(ids, id)
	}
	return ids, nil
}

func menuSelectionKey(line string) string {
	return strings.TrimRight(line, " \t")
}

func formatAlbumLine(cfg config, a album) string {
	return fmt.Sprintf("%-*s %-*s %-*s %-*s r=%s",
		cfg.Columns.AlbumArtist, a.AlbumArtist,
		cfg.Columns.Date, fallback(a.Date, "0000"),
		cfg.Columns.Album, a.Album,
		cfg.Columns.ID, a.ID,
		ratingString(a.Rating),
	)
}

func formatTrackLine(cfg config, t track) string {
	return fmt.Sprintf("%-*s %-*s %-*s %-*s %-*s %-*s r=%s",
		cfg.Columns.Track, trackNumberString(t.Track),
		cfg.Columns.Title, textOr(t.Title, ""),
		cfg.Columns.Artist, textOr(t.Artist, ""),
		cfg.Columns.Album, textOr(t.Album, ""),
		cfg.Columns.Date, textOr(t.Date, "0000"),
		cfg.Columns.ID, t.ID,
		ratingString(t.Rating),
	)
}

func filterAlbums(items []album, ids []string) []album {
	index := make(map[string]album, len(items))
	for _, item := range items {
		index[item.ID] = item
	}
	out := make([]album, 0, len(ids))
	for _, id := range ids {
		if item, ok := index[id]; ok {
			out = append(out, item)
		}
	}
	return out
}

func filterTracks(items []track, ids []string) []track {
	index := make(map[string]track, len(items))
	for _, item := range items {
		index[item.ID] = item
	}
	out := make([]track, 0, len(ids))
	for _, id := range ids {
		if item, ok := index[id]; ok {
			out = append(out, item)
		}
	}
	return out
}

func ratingString(value any) string {
	switch v := value.(type) {
	case nil:
		return "-"
	case string:
		if v == "" {
			return "-"
		}
		return v
	case float64:
		return strconv.Itoa(int(v))
	default:
		return fmt.Sprintf("%v", v)
	}
}

func trackNumberString(value any) string {
	switch v := value.(type) {
	case string:
		return v
	case float64:
		return strconv.Itoa(int(v))
	default:
		return fmt.Sprintf("%v", v)
	}
}

func stringify(value any) string {
	return shared.Stringify(value)
}

func stringSlice(value any) []string {
	switch v := value.(type) {
	case string:
		if strings.TrimSpace(v) == "" {
			return nil
		}
		return []string{v}
	case []string:
		return append([]string(nil), v...)
	case []any:
		out := make([]string, 0, len(v))
		for _, item := range v {
			out = append(out, stringify(item))
		}
		return out
	default:
		return nil
	}
}

func intFromAny(value any, fallback int) int {
	return shared.IntFromAny(value, fallback)
}

func floatFromAny(value any, fallback float64) float64 {
	return shared.FloatFromAny(value, fallback)
}

func boolFromAny(value any, fallback bool) bool {
	return shared.BoolFromAny(value, fallback)
}

func fallback(value, alt string) string {
	return shared.Fallback(value, alt)
}

func textOr(value any, alt string) string {
	text := strings.TrimSpace(stringify(value))
	if text == "" {
		return alt
	}
	return text
}

func getenv(key, fallback string) string {
	return shared.Getenv(key, fallback)
}
