package main

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/BurntSushi/toml"
	"github.com/carnager/melody/internal/shared"
)

const usage = `Usage: melodyc <command>

Commands:
  prev      Previous track
  toggle    Toggle play/pause
  stop      Stop playback
  next      Next track
  update    Rebuild library cache
  status    Show current playback status
  devices   List connected devices
  device    Switch active device: device <name>
`

type cliConfig struct {
	Master string `toml:"master"`
	Secret string `toml:"secret"`
}

var (
	cfg     cliConfig
	baseURL string
	client  *http.Client
)

func loadConfig() cliConfig {
	home, err := os.UserHomeDir()
	if err != nil {
		return cliConfig{}
	}
	xdgConfig := shared.Getenv("XDG_CONFIG_HOME", filepath.Join(home, ".config"))
	configPath := filepath.Join(xdgConfig, "melody", "melodyc.toml")
	var c cliConfig
	if _, err := toml.DecodeFile(configPath, &c); err != nil {
		return cliConfig{}
	}
	return c
}

func initClient() {
	cfg = loadConfig()
	address := cfg.Master
	if address == "" {
		address = "local"
	}
	var useLocal bool
	var socketPath string
	var err error
	baseURL, useLocal, socketPath, err = shared.APIBaseURLFromAddress(address)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
	if useLocal {
		client = shared.NewLocalHTTPClient(5*time.Second, socketPath)
	} else {
		client = &http.Client{Timeout: 5 * time.Second}
	}
}

func apiDo(method, path string, body string) ([]byte, error) {
	var bodyReader io.Reader
	if body != "" {
		bodyReader = strings.NewReader(body)
	}
	req, err := http.NewRequest(method, baseURL+"/"+path, bodyReader)
	if err != nil {
		return nil, err
	}
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	if cfg.Secret != "" {
		req.Header.Set("Authorization", "Bearer "+cfg.Secret)
	}
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	return io.ReadAll(resp.Body)
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(1)
	}

	initClient()
	cmd := os.Args[1]

	switch cmd {
	case "prev":
		apiDo("POST", "playback/prev", "")
	case "toggle":
		apiDo("POST", "playback/play", "")
	case "stop":
		apiDo("POST", "playback/stop", "")
	case "next":
		apiDo("POST", "playback/next", "")
	case "update":
		apiDo("POST", "cache/update", "")
	case "status":
		data, err := apiDo("GET", "playback/status", "")
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
			os.Exit(1)
		}
		fmt.Print(string(data))
	case "devices":
		cmdDevices()
	case "device":
		if len(os.Args) < 3 {
			fmt.Fprintln(os.Stderr, "usage: melodyc device <name>")
			os.Exit(1)
		}
		cmdDeviceSwitch(strings.Join(os.Args[2:], " "))
	default:
		fmt.Fprintf(os.Stderr, "unknown command: %s\n", cmd)
		fmt.Fprint(os.Stderr, usage)
		os.Exit(1)
	}
}

type deviceInfo struct {
	ID      string `json:"id"`
	Name    string `json:"name"`
	IsLocal bool   `json:"is_local"`
	Online  bool   `json:"online"`
	Format  string `json:"format"`
	BitRate int    `json:"max_bitrate"`
}

type activeDevice struct {
	DeviceID   string `json:"device_id"`
	DeviceName string `json:"device_name"`
}

func cmdDevices() {
	data, err := apiDo("GET", "devices", "")
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
	var devs []deviceInfo
	if err := json.Unmarshal(data, &devs); err != nil {
		fmt.Fprintf(os.Stderr, "error parsing devices: %v\n", err)
		os.Exit(1)
	}

	actData, _ := apiDo("GET", "devices/active", "")
	var act activeDevice
	json.Unmarshal(actData, &act)

	if len(devs) == 0 {
		fmt.Println("No devices connected")
		return
	}

	for _, d := range devs {
		status := "offline"
		if d.Online {
			status = "online"
		}
		active := " "
		if d.ID == act.DeviceID {
			active = "*"
		}
		local := ""
		if d.IsLocal {
			local = " (local)"
		}
		detail := ""
		if d.Format != "" {
			detail = " [" + d.Format
			if d.BitRate > 0 {
				detail += fmt.Sprintf(" %dk", d.BitRate)
			}
			detail += "]"
		}
		fmt.Printf(" %s %-20s %-8s%s%s\n", active, d.Name, status, local, detail)
	}
}

func cmdDeviceSwitch(name string) {
	data, err := apiDo("GET", "devices", "")
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
	var devs []deviceInfo
	json.Unmarshal(data, &devs)

	name = strings.ToLower(name)
	for _, d := range devs {
		if strings.ToLower(d.Name) == name {
			_, err := apiDo("POST", "devices/active", fmt.Sprintf(`{"device_id":"%s"}`, d.ID))
			if err != nil {
				fmt.Fprintf(os.Stderr, "error: %v\n", err)
				os.Exit(1)
			}
			fmt.Printf("Switched to: %s\n", d.Name)
			return
		}
	}
	fmt.Fprintf(os.Stderr, "device not found: %s\n", name)
	os.Exit(1)
}
