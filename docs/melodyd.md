# Configuring melodyd

The config file is `~/.config/melody/melodyd.toml`, or
`$XDG_CONFIG_HOME/melody/melodyd.toml` when set. melodyd creates a default file
on first start, then exits until `library.music_dir` is filled in.
Restart melodyd after changing the config. Use absolute paths; `~` and
environment variables are not expanded inside TOML strings.

## Basic setup

Only the music directory is required:

```toml
[library]
music_dir = "/srv/music"
```

The server scans this directory on startup and watches for local file changes.
The web UI is at `http://localhost:6701/web/`; MPD clients connect to port `6600`.
Both TCP listeners accept connections on all IPv4 interfaces by default.

Install `mpv` for playback on the server and `ffmpeg` for transcoding.
If mpv cannot start, melodyd logs the error and continues serving remote clients.

## Network access

For a server on your home network:

```toml
[server]
name = "Music server"
bind_to_address = ["0.0.0.0:6701"]
base_url = "http://192.168.1.10:6701"

[mpd]
port = 6600
```

Replace the address with your server's address. `base_url` is used for stream
URLs sent to playback devices, so those devices must be able to reach it.
Leave off `/web/` and `/api/v1`. If empty, melodyd derives an HTTP address
from its first TCP listener, which may be wrong for VPN or reverse proxy clients.

`bind_to_address` controls HTTP and WebSocket listeners. It accepts TCP
addresses and absolute Unix socket paths. The default adds a Unix socket at
`$XDG_RUNTIME_DIR/melody/melodyd.sock`; without `XDG_RUNTIME_DIR`, it uses
`<temp-dir>/melody-<uid>/melody/melodyd.sock`. An empty list restores the defaults.

`MELODYD_BIND_TO_ADDRESS` overrides the list with comma-separated addresses:

```sh
MELODYD_BIND_TO_ADDRESS=127.0.0.1:6701 ./bin/melodyd
```

The MPD TCP listener is separate and always binds to `0.0.0.0`.
Set `mpd.port = 0` to disable it. The web UI and local playback still work;
the TUI, CLI, and remote Go agents need the MPD TCP port.

### Authentication and HTTPS

`server.web_secret` sets the web login password. Empty means no login is required.
It protects the HTTP streams, cover art, and `/mpd` WebSocket endpoint using a
session cookie. It does not protect the MPD TCP port, which has no authentication.
Keep that port on a trusted network or VPN.

For HTTPS, use a reverse proxy with WebSocket support and set `base_url` to
the public HTTPS URL. melodyd itself serves plain HTTP.

The Android app and Go agents do not log in to HTTP or send session cookies.
Setting `web_secret` prevents Android connections and Go agent HTTP streaming.
Go agents can still play files through a configured local `agent.music_dir`.

## Mounted libraries

For NFS or other mounted storage, list every mount used by the library:

```toml
[library]
music_dir = "/mnt/music/flac"
required_mounts = ["/mnt/music"]
```

Use the actual mount points, not a subdirectory inside a mount. This protects
the first scan if storage is already offline. Mount checks require Linux;
leave `required_mounts` empty on other systems.

Without an explicit list, successful Linux scans remember the mount containing
the music directory and mounts inside it. If one disappears, melodyd logs an
error and preserves the indexed tracks. Directory access and traversal errors
also prevent removal of missing tracks from the database.

After storage returns, rescan with:

```sh
melody-cli raw update
```

An explicit `required_mounts` list overrides the remembered layout. Update it
when moving the library to different storage. When upgrading an older database
with no remembered mounts, an empty music root is preserved until storage
returns or `required_mounts` confirms the layout. Missing storage still means
its audio files cannot be played.

Changes made on a file server may not reach melodyd's local file watcher.
Run `melody-watcher` on the file server, using its local path to the same library:

```sh
melody-watcher -music /srv/music/flac -host 192.168.1.10 -port 6600
```

It sends updates after three seconds without another change. Set `-debounce`
to change that delay. The flags also read `MUSIC_DIR`, `MELODYD_HOST`,
`MELODYD_PORT`, and `DEBOUNCE`; flags take precedence.
An example service is in [contrib](../melodyd/contrib/melody-watcher.service).

## Playback

These settings apply to the server's local output. Remote agents have their
own [player settings](clients.md#melody-agent).

```toml
[player]
replaygain = "album"
volume = 100
mpv_path = "mpv"
mpv_socket = ""
```

- `replaygain`: `off`, `track`, or `album`. The default, `""`, leaves mpv's setting alone.
- `volume`: startup volume; defaults to `100`. A configured `0` also becomes `100`.
- `mpv_path`: executable name or absolute path. Defaults to `mpv`.
- `mpv_socket`: IPC socket for the mpv process melodyd starts. Empty creates a runtime path automatically.

`server.name` names the local output. Empty uses the hostname.

## Transcoding

Clients choose the stream format and bitrate. The server supports MP3, Opus,
Ogg Vorbis, AAC, and FLAC through `ffmpeg`; original files are served directly.

```toml
[transcode]
cache_max_mb = 5120
```

The cache defaults to 5120 MiB, even though this section is absent from the
generated config. Set `0` for unlimited storage. When the cache exceeds the
limit, melodyd removes the least recently accessed files until it falls below
90% of the limit. It checks on startup and after caching a completed transcode.

## Lyrics

Lyrics are read from `.lrc` sidecar files and embedded tags. Missing lyrics are
looked up on NetEase, then lrclib. Saving fetched lyrics is optional:

```toml
[library]
music_dir = "/srv/music"
save_lrc = true
embed_lyrics = false
```

Add these keys to your existing `[library]` section. Both default to `false`.
`save_lrc` writes a `.lrc` file next to the track. `embed_lyrics` writes FLAC
tags using `metaflac`; other formats are not supported for embedding.
Both need write access to the music files or their directory.

For bulk matching from a local lrclib dump, see [melody-lrcmatch](clients.md#melody-lrcmatch).

## Other settings

`[random] tracks = 20` is present in the generated config but currently has
no effect on playback or queue generation.

## Running as a service

The Arch package installs a user service. Once configured:

```sh
systemctl --user enable --now melodyd
```

For a source build, copy the service first:

```sh
mkdir -p ~/.config/systemd/user
cp melodyd/melodyd.service ~/.config/systemd/user/
```

Edit `~/.config/systemd/user/melodyd.service` and replace
`ExecStart=/usr/bin/melodyd` with the absolute path to your built binary. Then:

```sh
systemctl --user daemon-reload
systemctl --user enable --now melodyd
```

Apply later config changes with `systemctl --user restart melodyd`.
Read logs with `journalctl --user -u melodyd -f`.
To keep the user service running after logout, enable lingering with
`loginctl enable-linger "$USER"`.

## Data files

State lives in `$XDG_DATA_HOME/melody`, or `~/.local/share/melody` by default:

| File | Contents |
| --- | --- |
| `melody.db` | Library index, ratings, and remembered mounts |
| `playqueue.json` | Saved queue |
| `playstate.json` | Playback position and modes |
| `active_device` | Enabled outputs and primary output |
| `transcode_cache/` | Cached audio conversions |

Stop melodyd before copying its state for a backup. Keep the config and music
files too; they are stored separately.
