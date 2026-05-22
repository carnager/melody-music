# melody

Music queue and playback manager for [Navidrome](https://www.navidrome.org/) with multi-device support.

Spiritual successor to [clerk](https://github.com/carnager/clerk-modular), replacing MPD with Navidrome's Subsonic API.

## Architecture

Melody uses a hub-and-spoke model inspired by Spotify Connect. The server owns all state; devices are playback endpoints; clients are remote controls.

```
  [Clients: TUI / Web UI / Android / CLI]
           |
           | HTTP API
           v
  [Server: melodyd]  -----> [Navidrome]
      |          |
      | local    | HTTP/SSE
      | mpv IPC  |
      v          v
  [local mpv]   [Agent / Android / Browser]
```

**Server (melodyd)** is the central hub:
- Connects to Navidrome and caches the music library
- Manages the play queue, ratings, and scrobbling
- Runs a local mpv instance for direct audio output
- Tracks registered devices with heartbeat and online status
- Routes playback commands to the active device
- Handles seamless device handoff with position preservation
- Generates per-device stream URLs with transcoding options
- Serves the embedded web UI

**Agents (melody-agent)** are lightweight remote playback endpoints:
- Run their own mpv instance on a different machine
- Register with the server and heartbeat every 10 seconds
- Expose a small HTTP API for the server to control playback
- Declare preferred audio format and bitrate for transcoding

**Clients** are remote controls that talk to the server's HTTP API:
- **melody-tui** -- Terminal UI with library browser, queue management, ratings, search
- **melodyc** -- Minimal CLI for playback control
- **melody-rofi** -- Rofi/dmenu client for album/track selection
- **melody-musiclist** -- Static music list exporter
- **Web UI** -- Embedded browser-based client with full playback
- **Android app** -- Material 3 app with streaming playback and device switching

## Features

- Browse artists, albums, and tracks from Navidrome
- Queue management (add, insert, replace, reorder, clear)
- Multi-device playback with seamless handoff (Spotify Connect-like)
- Per-device audio transcoding (format and bitrate)
- Per-device custom Navidrome URL (for mobile network access)
- ReplayGain support (off, track, album) for all device types
- Track and album ratings (1-10)
- Search across albums and tracks
- Random album and random tracks
- Last.fm scrobbling
- Keyboard-driven TUI with comprehensive hotkeys
- Material 3 Android app with background service for persistent device discovery

## Components

| Binary | Description |
|--------|-------------|
| `melodyd` | Server daemon with embedded web UI |
| `melody-agent` | Remote playback agent (mpv wrapper) |
| `melody-tui` | Terminal UI (Bubble Tea) |
| `melodyc` | CLI client |
| `melody-rofi` | Rofi/dmenu integration |
| `melody-musiclist` | Music list exporter |
| `android/` | Android app (Kotlin/Compose) |

## Build

### Go binaries

```sh
./build
```

Binaries are placed in `bin/`. Requires Go 1.24+.

### Android

```sh
cd android
./gradlew assembleDebug
```

The APK is at `android/app/build/outputs/apk/debug/app-debug.apk`.

### Arch Linux

```sh
makepkg -si
```

## Configuration

### Server

Config file: `$XDG_CONFIG_HOME/melody/melodyd.toml`

```toml
[server]
bind_to_address = ["0.0.0.0:6701", "/run/user/1000/melody/melody.sock"]

[navidrome]
url = "http://localhost:4533"
username = "admin"
password = "your-password"

[mpv]
socket = "/run/user/1000/melody/mpv.sock"
executable = "mpv"
replaygain = ""  # "track", "album", or "" for off

[random]
tracks = 20

[cache]
poll_interval = 300

[scrobble]
enabled = true
```

### Agent

Config file: `$XDG_CONFIG_HOME/melody/melody-agent.toml`

```toml
[agent]
name = "living-room"
bind = "0.0.0.0:6702"
master = "192.168.1.10:6701"
format = "opus"       # optional: "", "opus", "mp3", "aac", "flac"
max_bitrate = 128     # optional: 0 = original quality
```

### Android

Settings are configured in-app: server address, Navidrome URL override (for mobile networks), device name, audio format, bitrate, and ReplayGain mode.

## Systemd

```sh
cp melodyd/melodyd.service ~/.config/systemd/user/
systemctl --user enable --now melodyd
```

## Device Types

| Type | Transport | Audio | ReplayGain |
|------|-----------|-------|------------|
| Server (local) | mpv IPC | Direct | mpv native |
| Agent | HTTP | Remote mpv | mpv native |
| Browser/Web UI | SSE | Browser audio | Volume adjustment |
| Android | SSE | ExoPlayer | Volume adjustment |

## License

MIT
