# Configuring melodyd

This example uses `/mnt/music` for the music folder and `192.168.1.10` for
the server address. Replace those two values with your own.

## Setup

Install `mpv` for playback on the server and `ffmpeg` for converting audio
when a client requests another format.

Create `~/.config/melody/melodyd.toml`:

```toml
[server]
base_url = "http://192.168.1.10:6701"

[library]
music_dir = "/mnt/music"
```

Use the full path to your music folder; `~` does not work inside the config.
If you set `XDG_CONFIG_HOME`, the config goes in its `melody` subdirectory instead.

Start melodyd from your build directory:

```sh
./bin/melodyd
```

It scans your music folder and watches for changes. Open
`http://192.168.1.10:6701/web/` in a browser, or connect an MPD client to
`192.168.1.10:6600`. The Android app uses `192.168.1.10:6701`.

Restart melodyd after editing the config.

## Music on a network share

If `/mnt/music` is an NFS or other mount on Linux, add this line under
`[library]` in the config above:

```toml
required_mounts = ["/mnt/music"]
```

This tells melodyd to preserve the library index when that mount is missing.
Use the actual mount point, even if your music folder is further inside it.
List every mount the library depends on. Leave this setting out on other systems.

Once the share is back, rescan with:

```sh
./bin/melody-cli raw update
```

Changes made directly on a NAS may not be detected automatically. Run
`melody-watcher` on the NAS to send updates to melodyd; `melody-watcher -help`
lists its options.

## Start automatically

The Arch package includes a systemd user service:

```sh
systemctl --user enable --now melodyd
```

For a source build, copy the service first:

```sh
mkdir -p ~/.config/systemd/user
cp melodyd/melodyd.service ~/.config/systemd/user/
```

In the copied file, change `ExecStart` to the full path to `bin/melodyd` in
your build directory. Then run:

```sh
systemctl --user daemon-reload
systemctl --user enable --now melodyd
```

After changing the config, run `systemctl --user restart melodyd`.
View logs with `journalctl --user -u melodyd -f`.

## Optional settings

Add each setting under the section shown. If that section already exists,
add the line there. Omitted settings use their defaults.

| Section | Setting | What it does |
| --- | --- | --- |
| `[server]` | `name = "Music server"` | Names the server's playback output. Defaults to the hostname. |
| `[server]` | `bind_to_address = ["0.0.0.0:6701"]` | Chooses where HTTP listens. The default listens on all IPv4 interfaces and also creates a local Unix socket. |
| `[player]` | `replaygain = "album"` | Sets volume normalization for server playback: `off`, `track`, or `album`. By default, mpv's setting is used. |
| `[player]` | `volume = 100` | Sets startup volume. Defaults to `100`; `0` also becomes `100`. |
| `[player]` | `mpv_path = "mpv"` | Chooses the mpv executable. Defaults to `mpv`. |
| `[library]` | `save_lrc = true` | Saves fetched lyrics next to each track as a `.lrc` file. Defaults to `false`. |
| `[library]` | `embed_lyrics = true` | Writes fetched lyrics into FLAC tags. Requires `metaflac` and write access. Defaults to `false`. |
| `[transcode]` | `cache_max_mb = 5120` | Limits cached audio conversions to about 5 GiB. This is the default; `0` means unlimited. |
| `[mpd]` | `port = 6600` | Sets the MPD TCP port. Defaults to `6600`; `0` disables it. |

Remote playback agents have their own [configuration](clients.md#melody-agent).

### Authentication and HTTPS

By default, anyone who can reach melodyd can use it. Keep it on a trusted
network or VPN. For HTTPS, use a reverse proxy with WebSocket support and
change `base_url` to the HTTPS address.

Setting `web_secret` under `[server]` adds a web login. Android does not
currently support this login, and Go agents cannot stream with it enabled.
The MPD TCP port remains unauthenticated.

## Backups

The library index, ratings, queue, and playback state are stored in
`~/.local/share/melody` (or `$XDG_DATA_HOME/melody`). Stop melodyd before
backing up that directory. Keep the config and music files too.
