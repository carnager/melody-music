# Melody

Melody plays your music library on your computer, phone, or remote speakers.
The server owns your library, playlists, queue, ratings, and playback state.
Control it from the terminal, web browser, Android app, or an MPD client such as
[Trackknife](https://github.com/carnager/trackknife).

Most of the code was written with AI. Design and testing are human-led.

## Getting started

Build with Go 1.25 or newer. Install `mpv` for playback on the server or a
remote computer, and `ffmpeg` on the server for transcoding.

```sh
./build
```

Binaries are written to `bin/`. Configure the server in a terminal, then start it:

```sh
./bin/melodyd setup
./bin/melodyd
```

The setup wizard asks for your music folder, MPD port, HTTP listening address,
optional web password, and server name. Press Enter to accept a suggested value.
It writes `~/.config/melody/melodyd.toml` (or
`$XDG_CONFIG_HOME/melody/melodyd.toml`) and exits without starting the server.

You can rerun `./bin/melodyd setup` to change settings. Existing values become
the defaults; additional settings are kept, and the previous file is backed up
as `melodyd.toml.bak`. Comments are not retained. Restart a running daemon after
changing its configuration. `./bin/melodyd help` lists the available commands.

On a first start without a usable config, `./bin/melodyd` launches setup
automatically when run in a terminal. For a systemd service, run setup first;
the service cannot answer interactive questions. If you prefer to edit the
config yourself, the minimum is:

```toml
[library]
music_dir = "/mnt/music"
```

Open <http://localhost:6701/web/> or run `./bin/melody-tui` in another terminal.
The library is scanned on startup.

For a server on another machine:

```sh
MPD_HOST=192.168.1.10 ./bin/melody-tui
```

With an installed binary, use `melodyd setup` instead of `./bin/melodyd setup`.
If your package includes the user service, start it after setup with
`systemctl --user enable --now melodyd`.

HTTP normally uses port `6701`; MPD uses `6600`. Keep the MPD port on a trusted
network or VPN: the web password does not authenticate TCP MPD connections.
See [Configuring melodyd](docs/melodyd.md) for remote access, mounted libraries,
playback settings, and service installation.

## Playback and library features

- Browse and search the indexed library, with album covers, lyrics, and track
  and album ratings.
- Play through the server, a remote agent, a browser, or a phone. Clients select
  outputs while Melody retains playback state.
- Open server-owned working lists and stored playlists as independent playback
  contexts with supporting clients. Switching lists preserves the displaced
  queue and remembers playback positions.
- Queue temporary **Up Next** requests, then return to normal playlist playback.
  The daemon handles progression even after the requesting client disconnects.
- Authorize Melody's own **Last.fm scrobbler**, submit Now Playing updates, and
  love or unlove tracks with a supporting client. Account state and pending
  scrobbles persist on the server.

Trackknife provides a native Linux interface for working lists, Up Next,
Last.fm setup, and library-matched dynamic playlists. These extensions require
updated client and daemon builds; ordinary MPD clients retain their normal
queue and transport commands. See [server features](docs/melodyd.md#playlists-up-next-and-lastfm)
and the [protocol reference](docs/protocol.md) for details.

## Clients and tools

- **[Trackknife](https://github.com/carnager/trackknife)** — native Qt MPD/Melody workspace, with local playback and file tools in separate tabs. [Connection guide](https://github.com/carnager/trackknife/blob/main/docs/melody.md).
- **melody-tui** — browse, search, queue music, view lyrics, and rate tracks and albums.
- **Web UI** — control playback or listen in your browser.
- **Android app** — control playback, stream to your phone, and download albums for offline listening.
- **melody-agent** — play through another computer's speakers using mpv. On macOS, use **melody-macos-agent**.
- **melody-cli** — search, rate, and control playback from the shell.
- **melody-rofi** — select albums and tracks with rofi or dmenu.
- **melody-musiclist** — export the library as an HTML page.
- **melody-lrcmatch** — match lyrics from a local lrclib dump and write `.lrc` files.
- **melody-watcher** — watch files on a NAS and tell melodyd to rescan changes.

[Client configuration](docs/clients.md) covers connection settings and remote
playback. Melody also supports MPD clients and scrobblers through port `6600`;
[Protocol extensions](docs/protocol.md) documents the Melody-specific commands
for client authors, and the [protocol roadmap](docs/protocol-roadmap.md) tracks
planned additions.

To build and install the Android app on a connected device:

```sh
cd android
./gradlew installDebug
```

Enter the server's HTTP address in the app settings, for example `192.168.1.10:6701`.

On Arch Linux, build the packages with `makepkg -si`.

## Documentation and development

- [Daemon setup and configuration](docs/melodyd.md)
- [Clients, agents, and remote playback](docs/clients.md)
- [MPD compatibility and Melody extensions](docs/protocol.md)
- [Protocol roadmap](docs/protocol-roadmap.md)

Run `go test ./...` to check the Go services and clients. Playback needs the
runtime tools described above; building the Android client also needs its
Android SDK and Gradle setup.

## Screenshots

### Terminal

![TUI queue](Screenshots/melody-tui-queue.png)
![TUI search](Screenshots/melody-tui-search.png)

### Web

![Web UI](Screenshots/melody-web-latest.png)

### Android

<p>
  <img src="Screenshots/android-queue.png" alt="Android queue" width="250" />
  <img src="Screenshots/android-search.png" alt="Android search" width="250" />
  <img src="Screenshots/android-devices.png" alt="Android devices" width="250" />
</p>

## License

Existing code is MIT. Files explicitly marked `GPL-3.0-only`, including the
Last.fm and Up Next implementations, retain that license. Builds of `melodyd`
that include these components must comply with GPL-3.0-only.
