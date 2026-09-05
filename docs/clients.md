# Clients and tools

Config files live in `$XDG_CONFIG_HOME/melody`, or `~/.config/melody` by default.
Restart the client after editing its config.

## melody-agent

`melody-agent` reads `~/.config/melody/melody-agent.toml` and creates it on
first start.

```toml
[agent]
name = "living-room"
master = "192.168.1.10:6600"
music_dir = ""
format = ""
max_bitrate = 0

[player]
replaygain = "track"
volume = 100
mpv_path = "mpv"
mpv_socket = ""
```

- `music_dir` is the local library root. Its relative folder layout must match the server.
- Empty `music_dir` streams audio from the daemon.
- `format` selects the stream format (`mp3`, `opus`, `ogg`, `aac`, or `flac`). Empty uses the original file. `max_bitrate` is in kbit/s; `0` leaves it unrestricted.
- Go agents require `mpv`.

Start `melody-agent`, then select its output in the TUI (`D`), web UI, or
Android app. From a source build, run `./bin/melody-agent`.

Agent streaming currently uses HTTP port `6701` on the `master` host. It does
not use `server.base_url` or apply the advertised format and bitrate preferences
to its stream requests. See also [authentication limitations](melodyd.md#authentication-and-https).

## melody-macos-agent

`melody-macos-agent` is built only for macOS. It reads
`~/.config/melody/melody-macos-agent.toml` and creates it on first start. The
config format is the same as `melody-agent`; the generated default name is the
hostname with `-macos` appended.

Requires `mpv`. Playback uses the current macOS audio output.

## melody-tui

`melody-tui` reads `~/.config/melody/melody-tui.toml` and creates it on first
start.

```toml
mpd_host = "localhost"
mpd_port = 6600
```

`MPD_HOST` and `MPD_PORT` override the config. `MPD_HOST` may include a port,
for example `MPD_HOST=192.168.1.10:6600`.

## melody-cli

`melody-cli` reads `~/.config/melody/melody-cli.toml` and creates it on first
start.

```toml
mpd_host = "localhost"
mpd_port = 6600
```

`MPD_HOST` and `MPD_PORT` override the config. `melody-cli raw <command>`
sends an arbitrary MPD command.

Useful commands:

```sh
melody-cli current
melody-cli lyrics
melody-cli rate <songid> <rating>
melody-cli albumrate <artist> <album> <date> <rating>
melody-cli raw status
```

## melody-musiclist

`melody-musiclist` reads `~/.config/melody/melody-musiclist.toml` and creates
it on first start.

```toml
[mpd]
host = "localhost"
port = 6600

[upload]
host = "your-web-server"
path = "/srv/http/list"

[output]
temp_file = "/tmp/musiclist.html"
```

## melody-lrcmatch

`melody-lrcmatch` reads `~/.config/melody/melody-lrcmatch.toml` and creates
it on first start. Command-line flags override the config.

```toml
melody_db = ""
lrclib_db = ""
netease = false
dry_run = false
verbose = false
```

The same options are available as flags:

```sh
melody-lrcmatch -db ~/.local/share/melody/melody.db \
  -lrclib ~/lrclib.sqlite3 \
  -netease \
  -dry-run \
  -verbose
```

- `-db`: path to Melody's SQLite database.
- `-lrclib`: path to an lrclib SQLite dump.
- `-netease`: fetch missing lyrics from NetEase Cloud Music.
- `-dry-run`: print matches without writing files.
- `-verbose`: show detailed match/mismatch information for NetEase queries.

## Android App

The Android app stores settings in app preferences:

- Local server address, e.g. `192.168.1.10:6701`.
- External server address, e.g. `https://music.example.com`.
- Home WiFi SSID for automatic local/external server switching.
- Device name.
- Audio format: original, Opus, MP3, AAC, or FLAC.
- Audio bitrate: max, 64k, 128k, 192k, 256k, or 320k.
- ReplayGain mode: off, track, or album.
- Resume on connect.

The app currently requires an empty `server.web_secret` on melodyd.

For Android build and signing instructions, see [RELEASE.md](../RELEASE.md).
