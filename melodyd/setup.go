package main

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/BurntSushi/toml"
	"github.com/carnager/melody/internal/shared"
)

// First-run setup: an interactive wizard that writes melodyd.toml so nobody
// has to hand-author TOML before hearing music. `melodyd setup` runs it
// explicitly (and re-runs it over an existing config); a plain `melodyd`
// start with no usable config drops into it automatically when attached to
// a terminal.

func printUsage(w io.Writer) {
	fmt.Fprint(w, `Usage: melodyd [command]

Without a command, melodyd starts the music server. On a first run without
a configuration it walks through interactive setup when run in a terminal.

Commands:
  setup      configure melodyd interactively (music folder, ports, password);
             safe to re-run — existing settings become the defaults
  version    print the melodyd version
  help       show this help
`)
}

// stdinIsTerminal lives in the setup_tty_*.go files: a real isatty check,
// because systemd hands daemons /dev/null — a character device — as stdin,
// so os.ModeCharDevice alone cannot tell a service from a terminal.

// expandTilde resolves a leading "~" so music_dir works the way people
// type it in a shell.
func expandTilde(path string) string {
	if path == "~" || strings.HasPrefix(path, "~/") {
		if home, err := os.UserHomeDir(); err == nil && home != "" {
			return filepath.Join(home, strings.TrimPrefix(path, "~"))
		}
	}
	return path
}

// configSection returns the named table of the raw config map, creating it
// so answers always have somewhere to land.
func configSection(raw map[string]any, name string) map[string]any {
	if section, ok := raw[name].(map[string]any); ok {
		return section
	}
	section := map[string]any{}
	raw[name] = section
	return section
}

// firstTCPBind picks the host:port entry of a bind list, skipping unix
// socket paths.
func firstTCPBind(values []string) string {
	for _, value := range values {
		if !strings.HasPrefix(value, "/") {
			return value
		}
	}
	return ""
}

// promptLine shows "label [display]: " and returns the trimmed answer;
// empty input means "accept the default" and is returned as "". A closed
// stdin (Ctrl-D) is an abort error so nothing half-answered gets written.
func promptLine(in *bufio.Reader, out io.Writer, label, display string) (string, error) {
	if display != "" {
		fmt.Fprintf(out, "%s [%s]: ", label, display)
	} else {
		fmt.Fprintf(out, "%s: ", label)
	}
	line, err := in.ReadString('\n')
	line = strings.TrimSpace(line)
	if err != nil {
		if errors.Is(err, io.EOF) && line != "" {
			return line, nil
		}
		return "", fmt.Errorf("setup aborted: %w", err)
	}
	return line, nil
}

// runWizard asks the handful of questions a first start needs and returns
// the raw config map to write. existing (a decoded melodyd.toml) seeds the
// defaults on re-runs and keeps every key the wizard does not ask about;
// nil starts from the default skeleton.
func runWizard(in io.Reader, out io.Writer, existing map[string]any) (map[string]any, error) {
	raw := existing
	if len(raw) == 0 {
		raw = map[string]any{}
		if _, err := toml.Decode(defaultDaemonConfig(), &raw); err != nil {
			return nil, fmt.Errorf("default config template: %w", err)
		}
	}
	server := configSection(raw, "server")
	library := configSection(raw, "library")
	mpdSection := configSection(raw, "mpd")
	reader := bufio.NewReader(in)

	fmt.Fprintln(out, "Melody setup — press Enter to accept the value in brackets.")
	fmt.Fprintln(out)

	// Music directory: the one thing melodyd cannot run without.
	musicDefault := strings.TrimSpace(stringify(library["music_dir"]))
	for {
		answer, err := promptLine(reader, out, "Music directory", musicDefault)
		if err != nil {
			return nil, err
		}
		if answer == "" {
			answer = musicDefault
		}
		answer = expandTilde(answer)
		if answer == "" {
			fmt.Fprintln(out, "A music directory is required.")
			continue
		}
		info, statErr := os.Stat(answer)
		if statErr != nil || !info.IsDir() {
			fmt.Fprintf(out, "%s does not exist or is not a directory.\n", answer)
			continue
		}
		library["music_dir"] = answer
		break
	}

	// MPD client port.
	portDefault := intFromAny(mpdSection["port"], 6600)
	for {
		answer, err := promptLine(reader, out, "MPD client port (0 disables)",
			strconv.Itoa(portDefault))
		if err != nil {
			return nil, err
		}
		if answer == "" {
			mpdSection["port"] = int64(portDefault)
			break
		}
		port, parseErr := strconv.Atoi(answer)
		if parseErr != nil || port < 0 || port > 65535 {
			fmt.Fprintln(out, "The port must be a number between 0 and 65535.")
			continue
		}
		mpdSection["port"] = int64(port)
		break
	}

	// HTTP bind address (web UI, streams, cover art).
	bindDefault := firstTCPBind(stringSlice(server["bind_to_address"]))
	if bindDefault == "" {
		bindDefault = "0.0.0.0:6701"
	}
	for {
		answer, err := promptLine(reader, out, "HTTP address (web UI and streams)",
			bindDefault)
		if err != nil {
			return nil, err
		}
		if answer == "" {
			answer = bindDefault
		}
		if _, _, splitErr := net.SplitHostPort(answer); splitErr != nil {
			fmt.Fprintln(out, "Use host:port form, for example 0.0.0.0:6701.")
			continue
		}
		server["bind_to_address"] = []string{answer, shared.DefaultSocketPath()}
		break
	}

	// Web password: optional, one honest sentence about what empty means.
	secretDefault := stringify(server["web_secret"])
	secretDisplay := "none — anyone on the network can control playback"
	if secretDefault != "" {
		secretDisplay = "keep current password"
	}
	secret, err := promptLine(reader, out, "Web password", secretDisplay)
	if err != nil {
		return nil, err
	}
	if secret == "" {
		secret = secretDefault
	}
	server["web_secret"] = secret

	// Display name.
	nameDefault := strings.TrimSpace(stringify(server["name"]))
	if nameDefault == "" {
		if hostname, hostErr := os.Hostname(); hostErr == nil && hostname != "" {
			nameDefault = hostname
		} else {
			nameDefault = "Server"
		}
	}
	name, err := promptLine(reader, out, "Server name", nameDefault)
	if err != nil {
		return nil, err
	}
	if name == "" {
		name = nameDefault
	}
	server["name"] = name

	// Not fatal — melodyd serves other outputs fine — but say it now
	// rather than as a cryptic player error later.
	mpvPath := stringify(configSection(raw, "player")["mpv_path"])
	if mpvPath == "" {
		mpvPath = "mpv"
	}
	if _, lookErr := exec.LookPath(mpvPath); lookErr != nil {
		fmt.Fprintf(out, "\nNote: %s was not found in PATH — playback on this machine "+
			"will not work until mpv is installed.\n", mpvPath)
	}

	return raw, nil
}

// writeSetupConfig writes the raw config map as TOML, backing up an
// existing file first. One atomic-enough WriteFile at the end: an aborted
// wizard never leaves a partial config behind.
func writeSetupConfig(configPath string, raw map[string]any) error {
	if err := os.MkdirAll(filepath.Dir(configPath), 0o755); err != nil {
		return err
	}
	if previous, err := os.ReadFile(configPath); err == nil {
		if err := os.WriteFile(configPath+".bak", previous, 0o644); err != nil {
			return fmt.Errorf("back up existing config: %w", err)
		}
	}
	var buf bytes.Buffer
	buf.WriteString("# melodyd configuration — settings reference: docs/melodyd.md\n")
	buf.WriteString("# Re-run \"melodyd setup\" to change these interactively.\n\n")
	if err := toml.NewEncoder(&buf).Encode(raw); err != nil {
		return fmt.Errorf("encode config: %w", err)
	}
	return os.WriteFile(configPath, buf.Bytes(), 0o644)
}

func printSetupEpilogue(out io.Writer, configPath string, raw map[string]any) {
	musicDir := stringify(configSection(raw, "library")["music_dir"])
	mpdPort := intFromAny(configSection(raw, "mpd")["port"], 6600)
	httpBind := firstTCPBind(stringSlice(configSection(raw, "server")["bind_to_address"]))
	httpPort := "6701"
	if _, port, err := net.SplitHostPort(httpBind); err == nil {
		httpPort = port
	}
	fmt.Fprintf(out, "\nConfiguration written to %s.\n\nNext steps:\n", configPath)
	fmt.Fprintf(out, "  - melodyd scans %s automatically on every start\n", musicDir)
	fmt.Fprintf(out, "  - Web UI: http://localhost:%s/web/\n", httpPort)
	if mpdPort != 0 {
		fmt.Fprintf(out, "  - MPD clients connect on port %d\n", mpdPort)
	}
	fmt.Fprintf(out, "  - Start at login: systemctl --user enable --now melodyd\n\n")
}

// runSetupCommand is the `melodyd setup` subcommand.
func runSetupCommand() error {
	if !stdinIsTerminal() {
		return errors.New("melodyd setup is interactive; run it in a terminal")
	}
	pathCfg, err := resolvePaths()
	if err != nil {
		return err
	}
	var existing map[string]any
	if _, statErr := os.Stat(pathCfg.ConfigPath); statErr == nil {
		if _, decodeErr := toml.DecodeFile(pathCfg.ConfigPath, &existing); decodeErr != nil {
			fmt.Printf("The existing config at %s does not parse:\n  %v\n",
				pathCfg.ConfigPath, decodeErr)
			answer, promptErr := promptLine(bufio.NewReader(os.Stdin), os.Stdout,
				"Overwrite it with fresh settings? (the old file is kept as .bak)", "y/N")
			if promptErr != nil {
				return promptErr
			}
			if !strings.EqualFold(answer, "y") && !strings.EqualFold(answer, "yes") {
				return errors.New("keeping the existing file; fix it by hand or re-run setup")
			}
			existing = nil
		}
	}
	if len(existing) > 0 {
		fmt.Printf("Editing %s — comments in the old file are not preserved "+
			"(backup written to %s.bak).\n\n", pathCfg.ConfigPath, pathCfg.ConfigPath)
	}
	raw, err := runWizard(os.Stdin, os.Stdout, existing)
	if err != nil {
		return err
	}
	if err := writeSetupConfig(pathCfg.ConfigPath, raw); err != nil {
		return err
	}
	printSetupEpilogue(os.Stdout, pathCfg.ConfigPath, raw)
	return nil
}
