# Configuring melodyd

This example uses `/mnt/music` for the music folder and `192.168.1.10` for
the server address. Replace those two values with your own.

## Setup

Install `mpv` for playback on the server and `ffmpeg` for converting audio
when a client requests another format.

Run the setup command in a terminal, then start the server:

```sh
./bin/melodyd setup
./bin/melodyd
```

For installed binaries, use `melodyd setup` and `melodyd`. Setup writes the
configuration and exits; it does not start the daemon or install a service.
Run it before enabling the user service.

Started in a terminal without a configuration, it asks for your music
folder, ports, and an optional web password, then writes
`~/.config/melody/melodyd.toml` and starts. `./bin/melodyd setup` re-runs
the questions later with your current settings as defaults (hand-added
settings are kept; the previous file is saved as `melodyd.toml.bak`).
Comments are not preserved. The questions cover an existing music directory,
MPD port (`0` disables MPD), HTTP listening address, web password, and server
name. Enter keeps the current value, including an existing password; remove
`server.web_secret` by editing the file if you want to disable web login.
An invalid existing config is replaced only after the wizard asks you to confirm.
`melodyd help` lists commands; `melodyd version` prints the installed version.

You can also write the config yourself:

```toml
[server]
base_url = "http://192.168.1.10:6701"

[library]
music_dir = "/mnt/music"
```

`~` at the start of `music_dir` is expanded to your home directory.
If you set `XDG_CONFIG_HOME`, the config goes in its `melody` subdirectory instead.

It scans your music folder and watches for changes. Open
`http://192.168.1.10:6701/web/` in a browser, or connect an MPD client to
`192.168.1.10:6600`. The Android app uses `192.168.1.10:6701`.

Restart melodyd after editing the config.

## Playlists, Up Next, and Last.fm

With a supporting client, named playlists and scratch lists can be independent
playback contexts. Melody stashes the unnamed queue when a named list starts
and remembers the position left in each list. Ordinary MPD queue edits update
the active named list; inactive lists and the stashed queue remain separate.

**Up Next** is a daemon-owned temporary request queue. Requests play once in
order before normal playback resumes, and do not become permanent entries in
the underlying stored playlist. Pending requests and continuation state survive
restarts; submitting a request does not start stopped playback. Trackknife has
a panel for adding, reordering, removing, and clearing requests. See
[`melody_upnext`](protocol.md#up-next-request-queue-melody_upnext) for commands,
revision checks, and compatibility limits.

For scrobbling, open **Trackknife → Settings → Last.fm → Melody server**.
Enter your Last.fm application API key and shared secret, authorize in the
browser, finish authorization, and enable scrobbling. Melody accounts for its
primary output and continues scrobbling when clients close; a client should
not also scrobble that same playback. Local Trackknife playback has a separate
account. Love/Unlove actions do not require scrobbling to be enabled.

Account credentials and a bounded retry outbox are stored privately in
`lastfm-v1.json` beside `playqueue.json`. Trackknife's
[Last.fm guide](https://github.com/carnager/trackknife/blob/main/docs/lastfm.md)
explains eligibility, retries, and loved-track playlists. The
[protocol reference](protocol.md#lastfm-account-and-scrobbling-melody_lastfm)
documents the account and track commands for other clients.

## Album covers

Melody serves embedded **front covers** to MPD clients and its HTTP artwork API.
Artist photos, back covers, disc images, and unclassified pictures are not used
as album covers. FLAC, MP3, Ogg/Vorbis, and Opus pictures are selected by their
picture role, regardless of embedding order. M4A/MP4 uses its dedicated cover
art atom.

When no embedded front cover exists, album artwork falls back to conventional
`cover`, `folder`, `front`, or `album` PNG/JPEG files in the track's directory.
The MPD `readpicture` command remains embedded-only and returns no picture when
there is no front cover. No tags or images are rewritten. Clients may need to
reload cached artwork after updating and restarting melodyd.

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

If melodyd has no usable configuration yet, the service exits once with a
log message telling you to run `melodyd setup` in a terminal, instead of
restarting in a loop.

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
