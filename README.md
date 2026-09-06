# Melody

Melody plays your music library on your computer, phone, or remote speakers.
The server keeps a shared queue, ratings, and playback state. Control it from
the terminal, web browser, or Android app.

Most of the code was written with AI. Design and testing are human-led.

## Getting started

Build with Go 1.25 or newer. Install `mpv` for playback on the server or a
remote computer, and `ffmpeg` on the server for transcoding.

```sh
./build
```

Binaries are written to `bin/`. Create `~/.config/melody/melodyd.toml`
(use `$XDG_CONFIG_HOME/melody/melodyd.toml` if set):

```toml
[library]
music_dir = "/mnt/music"
```

Start the server:

```sh
./bin/melodyd
```

Open <http://localhost:6701/web/> or run `./bin/melody-tui` in another terminal.
The library is scanned on startup.

For a server on another machine:

```sh
MPD_HOST=192.168.1.10 ./bin/melody-tui
```

See [Configuring melodyd](docs/melodyd.md) for network access, mounted libraries,
playback settings, and running it as a service.

## Clients and tools

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
playback. Melody also supports MPD clients and scrobblers through port `6600`.

To build and install the Android app on a connected device:

```sh
cd android
./gradlew installDebug
```

Enter the server's HTTP address in the app settings, for example `192.168.1.10:6701`.

On Arch Linux, build the packages with `makepkg -si`.

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

MIT
