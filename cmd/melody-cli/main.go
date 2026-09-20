package main

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/BurntSushi/toml"
)

type config struct {
	MPDHost string `toml:"mpd_host"`
	MPDPort int    `toml:"mpd_port"`
}

func loadConfig() config {
	home, _ := os.UserHomeDir()
	xdgConfig := os.Getenv("XDG_CONFIG_HOME")
	if xdgConfig == "" {
		xdgConfig = filepath.Join(home, ".config")
	}
	configPath := filepath.Join(xdgConfig, "melody", "melody-cli.toml")

	_ = os.MkdirAll(filepath.Dir(configPath), 0o755)
	if _, err := os.Stat(configPath); os.IsNotExist(err) {
		_ = os.WriteFile(configPath, []byte(defaultCLIConfig()), 0o644)
	}

	var c config
	toml.DecodeFile(configPath, &c)
	applyMPDEnv(&c)
	if c.MPDHost == "" {
		c.MPDHost = "localhost"
	}
	if c.MPDPort == 0 {
		c.MPDPort = 6600
	}
	return c
}

func defaultCLIConfig() string {
	return `# MPD server exposed by melodyd.
# MPD_HOST and MPD_PORT environment variables override these values.
mpd_host = "localhost"
mpd_port = 6600
`
}

func applyMPDEnv(c *config) {
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

func main() {
	if len(os.Args) < 2 {
		fmt.Fprint(os.Stderr, cliHelp)
		os.Exit(1)
	}
	if os.Args[1] == "help" || os.Args[1] == "--help" || os.Args[1] == "-h" {
		fmt.Print(cliHelp)
		return
	}
	command, args, err := prepareCommand(os.Args[1], os.Args[2:])
	if err != nil {
		fmt.Fprintln(os.Stderr, "Error:", err)
		os.Exit(1)
	}
	cfg := loadConfig()
	address := net.JoinHostPort(cfg.MPDHost, fmt.Sprint(cfg.MPDPort))
	conn, err := net.DialTimeout("tcp", address, 3*time.Second)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: cannot connect to %s: %v\n", address, err)
		os.Exit(1)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(10 * time.Second))
	reader := bufio.NewReader(conn)
	greeting, err := reader.ReadString('\n')
	if err != nil || !strings.HasPrefix(greeting, "OK MPD") {
		fmt.Fprintln(os.Stderr, "Error: not an MPD server")
		os.Exit(1)
	}
	client := mpdClient{conn: conn, reader: reader}
	if err := client.run(command, args, os.Stdout); err != nil {
		fmt.Fprintln(os.Stderr, "Error:", err)
		os.Exit(1)
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

func quote(s string) string {
	s = strings.ReplaceAll(s, `\`, `\\`)
	s = strings.ReplaceAll(s, `"`, `\"`)
	return `"` + s + `"`
}
