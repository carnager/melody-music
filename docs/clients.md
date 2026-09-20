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

`MPD_HOST` and `MPD_PORT` override the config. Run `melody-cli help` (or
`--help`) for the command reference without connecting to a server.
Arguments are quoted for MPD automatically, preserving shell-quoted names,
paths, and filter expressions. `melody-cli raw <command>` sends literal MPD
protocol text instead.

Useful commands:

```sh
melody-cli current
melody-cli lyrics
melody-cli rate <songid> <rating>
melody-cli albumrate <artist> <album> <date> <rating>
melody-cli raw status
```

### Last.fm

Authorize the daemon, then explicitly enable scrobbling:

```sh
melody-cli lastfm begin <api-key> <shared-secret>
# Open the URL in the returned JSON and authorize it in your browser.
melody-cli lastfm finish
melody-cli lastfm enable 1
melody-cli lastfm
melody-cli lastfm love
melody-cli lastfm info "Cocteau Twins" "Heaven or Las Vegas"
melody-cli lastfm unlove
melody-cli lastfm enable 0
```

`lastfm` defaults to `status`. `info`, `love`, and `unlove` use the current
song's artist/title when omitted. Responses retain the daemon's `lastfm: JSON`
format, including the authorization URL, connection state, pending scrobbles,
and service messages. `lastfm disconnect` removes the account and clears pending
scrobbles. Last.fm runs on melodyd for all playback outputs; credentials belong
to the daemon. Use a trusted MPD connection.

### Search and playback lists

Tag/value searches and filter expressions support sorting, windows, generic
tags, and album results:

```sh
melody-cli find Genre "Shoegaze"
melody-cli find "(samplerate >= 96000)" sort -Added window 0:50
melody-cli search "((Genre == 'Jazz') OR (Genre == 'Blues'))"
melody-cli searchalbums "(albumrating >= 8)" sort -rating
melody-cli list Genre group AlbumArtist
melody-cli context play "Road trip"
melody-cli context queueinfo
melody-cli context queue
melody-cli context queueadd -1 "Artist/Album/Track.flac"
melody-cli scratch "Working list" 1
melody-cli playlistappend "Road trip" "first.flac" "second.flac"
```

`context` reports the active playback context. Its commands edit or restore the
saved queue while a playlist plays. Positions are zero-based; `queueadd -1`
appends. `scratch` lists working playlists; pass an existing playlist name and
`1` to mark it as a working list or `0` to promote it to a regular playlist.
Standard MPD playlist commands such as `save`, `rename`, `playlistadd`,
`playlistdelete`, `playlistmove`, and `playlistclear` are also available.

### Up Next

```sh
melody-cli upnext
melody-cli upnext append "Artist/Album/Track.flac"
melody-cli upnext prepend "Artist/Album/Another track.flac"
melody-cli upnext move <request-id> 0
melody-cli upnext remove <request-id>
melody-cli upnext undo
melody-cli upnext return
```

The listing includes pending tracks, their `Id` values, and the queue revision.
Edits fetch the revision automatically and submit one guarded mutation; if the
queue changes concurrently, the error is returned without retrying. `insert`
takes a zero-based position followed by URIs; `play` takes a pending request ID.
`clear` removes pending requests; `return` also leaves the active request and
resumes normal playback. Use the full `melody_upnext` protocol command when a
script needs to supply its own revision.

These extensions require a daemon that advertises them in `commands`; older
servers return their normal MPD error. See [the protocol reference](protocol.md)
for server behavior and response fields.

## melody-musiclist

`melody-musiclist` reads `~/.config/melody/melody-musiclist.toml` and creates
it on first start. Set both `[upload].host` (an SSH host or alias) and
`[upload].path` (the remote directory for `index.html`) before running it.
The generated configuration leaves these blank; missing values produce an
error before any server connection or upload. Existing configured destinations
are preserved.

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
