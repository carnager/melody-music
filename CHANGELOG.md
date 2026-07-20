# Changelog

## 1.3.0 (2026-07-20)

### Features

- Add synchronized multi-output playback with additive output toggles, exclusive handoff, and a designated primary output for queue advancement.
- Add targeted library subtree updates and `melody-watcher` for forwarding filesystem changes from a remote file server to melodyd.
- Reintroduce `melody-rofi`, a fast rofi/dmenu launcher for browsing albums or tracks and adding, inserting, or replacing the queue.
- Add `melody-macos-agent`, a macOS-only playback target with its own generated config file.
- Android: volume control slider in Now Playing.
- Android: queue shuffle, save queue as playlist, delete playlists (with confirmation), and set/clear track priorities from the queue.
- Android: tag-qualified search (`artist:`, `album:`, `title:`, `date:`) matching the TUI syntax.
- Android: trigger a server library update from Settings.
- Android: media-style notification with lockscreen controls; lockscreen/headset transport now goes through the server so queue state stays in sync.
- Android: audio focus handling (pauses for calls/other apps), pause on headphone unplug, and network wake mode for screen-off streaming.
- Android: Wi-Fi-only download preference (on by default); downloads pause on metered networks and resume automatically.
- Android: failed action feedback via snackbars instead of silent failures.
- Android: setup screen verifies the server is reachable before saving.
- Android: play/shuffle buttons on the album screen; tapping a track in the library or a playlist now plays it in context (long-press or ⋮ for the menu).
- Android: back from any tab returns to the Library tab instead of closing the app; predictive back gestures enabled.
- Android: clear-queue asks for confirmation; random-album uses a dice icon; track rows show durations; library shows a proper loading/error state with retry; app version in Settings.
- Android: Material You dynamic color and light/dark/system theme selection (Settings → Appearance), with the Melody palette as non-dynamic fallback.
- Android: star ratings cycle full → half → clear on repeated taps (one shared, accessible widget) instead of invisible left/right tap zones.
- Android: library sort/filter toggles are labeled chips ("Latest", "Downloaded"); queue shows a priority-dot legend; animated tab transitions (including the back gesture to Library).
- melody-tui: queue management — `Shift+↑`/`Shift+↓` (or `J`/`K`) move the track or selection, `g` jumps to the current song, `Z` shuffles, `S` saves the queue as a playlist, `!` sets or clears track priorities, `←`/`→` seek.
- melody-tui: `:` command palette with completion (play/pause/seek/volume/save/shuffle/replaygain/modes/outputs/…).
- melody-tui: `F5`-`F8` transport keys (prev / play-pause / stop / next); Space no longer toggles pause — it quick-adds in the library and toggles selection in the queue.
- melody-tui: `Enter` replaces the queue and plays library items; the action menu moved to `m`.
- melody-tui: `1`/`2`/`3` switch the library root (Artists / Playlists / Latest); `d` deletes a playlist (with confirmation); `q` steps back instead of quitting while drilled in.
- melody-tui: transient status toasts for actions and MPD errors, a disconnected indicator, and a volume display in the player bar.
- melody-tui: `p` now also adds whole albums and artists to playlists.
- melody-tui: mouse support — wheel scrolls lists and lyrics, click focuses panels and selects rows, double-click plays a queue track.
- melody-tui: tag-qualified search (`artist:`, `album:`, `title:`, `date:`).
- melody-tui: configurable UI colors and `album_art` mode in `melody-tui.toml`; album art auto-detects kitty/ghostty terminals.
- melody-tui: transport keys (`F5`-`F8`, `<`, `>`, `s`, volume) keep working inside the help, track-info, and now-playing screens; `←`/`→` seek in now playing.

### Bug Fixes

- melodyd: resume playback at the last reported position when an active agent re-registers, instead of restarting the current track from the beginning.
- melodyd/melody-agent: agents send a per-process instance ID on registration; melodyd logs a warning when two different processes fight over the same agent name (previously this caused a silent 5-second replace loop that restarted the track endlessly). The Android app sends the instance ID too.
- melodyd: abort the HTTP response when ffmpeg dies mid-transcode so download clients detect truncation instead of saving a partial file as complete.
- Android: a transient status-poll failure no longer blanks the Now Playing screen and kills the 1-second progress updates mid-song.
- Android: seek thumb no longer snaps back to the pre-seek position after release.
- Android: queue drag-reorder updates the list optimistically and uses stable row keys — the dragged row no longer jumps to the wrong item.
- Android: the mini player clears instead of showing the last track as "playing" forever after the queue empties.
- Android: idle notifications arriving during an in-flight refresh are coalesced instead of dropped; concurrent refreshes are serialized.
- Android: lyrics can no longer attach to the wrong track after fast skips, and clear when the queue empties.
- Android: agent reconnect races fixed (session generation guard); reconnect cleanup now actually runs (NonCancellable); one shared OkHttp client instead of leaking one per reconnect attempt.
- Android: track-end advance is no longer suppressed when a preload never arrived; ended playback reports "stop" instead of a stale "play".
- Android: MpdClient no longer wedges permanently after a non-MPD greeting (captive portal); non-idempotent commands are no longer retried after response timeouts (duplicate queue adds/moves).
- Android: album list no longer restores another artist's scroll position; "Latest" mode shows a proper title.
- Android: back button exits search selection mode; the keyboard no longer covers search results; re-tapping the Playlists tab no longer closes the open playlist.
- Android: changing format/bitrate chips in Settings re-registers the agent once on close instead of interrupting playback per tap.
- Android: dismissing the "Play on phone?" prompt cancels the action instead of silently starting playback on the remote device.
- Android: removing an album mid-download actually cancels it (it no longer silently re-downloads and resurrects); downloads survive the UI closing (app-scoped queue persisted across restarts) and retry with exponential backoff instead of hammering every 5 seconds forever.
- Android: downloaded-album state reconciles with on-disk files at startup; orphaned audio files are garbage-collected; quality changes trigger re-downloads instead of silently keeping the old encoding.
- Android: on unknown Wi-Fi (SSID unreadable), the app probes the home server instead of blindly assuming it's on the home network.
- Android: cloud backup excludes the offline music cache and its prefs (restores no longer claim albums are downloaded when files weren't restored).
- melody-tui: fix library filter with no matches acting on the first item of the unfiltered list.
- melody-tui: fix `q` quitting the whole app from the playlist picker, go-to, and device popups (including while typing a playlist name).
- melody-tui: refresh the artist list after library updates and playlist views after stored-playlist changes; watch the `output` idle subsystem.
- melody-tui: rewrite the stale help screen (removed vim keys were still documented; `J`/`K` queue move existed only in the help text).
- melody-tui: fetch album ratings in one command-list round-trip instead of one request per album.
- melody-tui: lists scroll at the edges with a small margin instead of recentering the cursor on every keypress.

## 1.2.0 (2026-06-04)

### Breaking Changes

- Go playback targets now require `mpv`; `melodyd`'s local agent and `melody-agent` use an mpv JSON IPC subprocess instead of the built-in Go decoder stack.

### Features

- Add mpv-backed two-slot playback for Go agents with gapless preloading, ReplayGain via mpv, seek, volume, and clear-preload support.
- Add configurable mpv path/socket settings to generated `melodyd` and `melody-agent` configs.
- Add complete generated config files and README documentation for all server, agent, player, Android, TUI, CLI, and lyrics matcher options.
- Add Android support for clearing stale preloaded tracks.

### Bug Fixes

- Fix MPD idle handling after playlist changes so clients can safely send command lists immediately after an idle notification.
- Fix MPD `setvol` and relative `volume` commands not updating active playback targets.
- Fix Android server recovery when WiFi SSID reporting is temporarily unavailable.
- Fix Android playback ignoring selected remote stream format/bitrate settings.
- Fix Android device discovery recovery after WiFi/mobile server switches.
- Fix device handoff preserving playback position with mpv-backed agents.
- Fix mpv subprocess cleanup when an agent exits.
- Fix remote stream transcoding to reject unsupported formats instead of passing arbitrary values to ffmpeg.
- Show the TUI volume hotkeys in the help screen.

### Improvements

- Remove unused/fake API and Android device secret settings.
- Warn at startup when `server.web_secret` is empty or `server.base_url` is missing/invalid.
- Expose agent stream format and bitrate in MPD `outputs`.

## 1.1.1 (2026-05-30)

### Improvements

- Add TUI +/- hotkeys for MPD software volume changes.
- Add Android release signing configuration.

## 1.1 (2026-05-30)

### Features

- Add Opus audio decoding support in the built-in player
- Add MPD `plchangesposid` support for clients that use positional queue diffs

### Bug Fixes

- Fix MPD and TUI clients sometimes showing stale queue data after melodyd restarts
- Fix queue cache invalidation on melody-tui reconnect
- Fix Android queue refresh when the server reports an empty queue
- Fix playlist idle notification when skipping away from an auto-consumed priority track
- Fix last-track replay loop in the built-in player

### Improvements

- Persist MPD queue version with the saved queue to reduce client cache ambiguity across restarts

## 1.0.0 (2026-05-29)

### Breaking Changes

- Replace mpv-based playback with autonomous agent architecture — agents now own their audio pipeline (built-in Go audio player for server, ExoPlayer for Android)
- Remove melody-rofi (unused)
- melodyd and melody-agent no longer depend on mpv

### Features

- Autonomous agent protocol (v2): agents manage their own playback queue, gapless preloading, and report state back to the server
- Built-in local audio player for melodyd using beep/oto (FLAC, MP3, OGG Vorbis)
- Local agent connects to server via in-memory pipe — no external process needed
- ReplayGain support (track and album mode) in built-in player
- Redesigned melody-musiclist HTML output with modern styling, dark mode, and sortable table

### Bug Fixes

- Fix Android queue flickering by rejecting truncated playlistinfo responses
- Fix Android device list not updating on agent connect/disconnect (refresh on output idle events)
- Reduce idle notification spam: agent_state heartbeats only notify on meaningful changes (state, track, volume), not every 2-second elapsed update
- Fix Android seek not working on device switch (atomic setMediaItems with start position)
- Fix Android progress bar stuck at 00:00 due to locale-dependent number formatting
- Fix Android play button not working when agent is stopped (use play instead of pause 0)
- Fix Android track transition loop caused by preload cleanup removing the current track

### Improvements

- melody-agent rewritten as autonomous player with 2-track window and gapless transitions
- Android PlaybackService rewritten for v2 protocol with dedicated WebSocket for queue sync
- Server agent handling: debounced state notifications, proper device switching handoff

## 0.14.0 (2026-05-27)

### Features

- Android: lyrics button and screen in now playing view
- melody-lrcmatch: add NetEase Cloud Music as a lyrics source (`-netease` flag)
- Server: async lrclib.net lyrics fetching with caching — no longer blocks MPD commands

### Bug Fixes

- Server: fix MPD idle notifications being lost between commands (register clients for entire connection lifetime)
- Server: fix protocol desync with ncmpcpp when idle returns immediately from pending events
- Server: ignore mpv end-file eof events when a remote device is active (prevents double-advancing)
- Server: guard trackended command against stray signals from non-active devices
- Android: send trackended when ExoPlayer finishes a track (fixes playback stalling)
- melody-tui: fix nil pointer crash during reconnect when global client is nil

### Improvements

- Android: refresh UI immediately after play/pause/next/prev instead of waiting for idle round-trip
- melody-tui: remove vim keybindings (j/k/g/G/h/l) — use arrow keys, home/end, pgup/pgdown
- Server: add disc number to Track metadata, fix multi-disc album sorting
- Android: store all track tags in offline metadata, sort cached albums by date

## 0.13.0 (2026-05-25)

### Features

- Lyrics support: `readlyrics` command reads from .lrc sidecar files or embedded tags, falls back to lrclib.net if not found locally (and saves as .lrc for next time)
- Synced lyrics display in TUI with auto-scroll following playback position
- `melody-cli lyrics` command to show lyrics for the current track
- `melody-lrcmatch` tool for offline bulk-matching your library against a local lrclib database dump to write .lrc sidecar files

### Bug Fixes

- Fix Android app losing connection on idle (shared OkHttpClient with 10s readTimeout was killing the idle WebSocket)
- Split into separate command/idle WebSocket clients with appropriate timeouts
- Add 30s WebSocket ping interval to survive NAT/mobile carrier proxy timeouts
- Idle connection failure now proactively tears down command connection for faster recovery

### Improvements

- Server: TCP keep-alive (30s) on MPD listener to detect silently-dropped clients
- Server: 5-minute read deadline on idle connections prevents goroutine leaks from zombie WebSocket clients
- Android: force reconnect and full refresh on app resume from background
- Android: improved home WiFi detection when no SSID is configured

## 0.12.0 (2026-05-24)

### Features

- Track priorities: add tracks with Low/Medium/High priority to play them next regardless of playback mode
- Prioritized tracks are auto-consumed (removed from queue) after playing
- Playback resumes from the original queue position after all priority tracks finish
- Priority works in all modes: sequential, random, repeat, single
- Multiple prioritized tracks play in priority order (highest first), ties broken by queue position
- Colored priority indicators in queue: bright orange (high), orange (medium), light orange (low)
- TUI: "Add with priority" option in action menu with Low/Medium/High popup
- Web UI: priority options in right-click context menu for tracks
- Android: priority field parsed and displayed, API methods for priority commands
- MPD commands: `prio`, `prioid`, `addidprio` for setting priorities
- Queue priorities persist across server restarts

## 0.11.1 (2026-05-24)

### Bug Fixes

- Fix end-of-queue not stopping playback (kept playing last track)
- Fix playlistadd failing due to relative vs absolute path mismatch
- Fix computed album rating showing with only 1 rated track (now requires 70% threshold)

### Improvements

- Search tracks display as columnar layout (artist, title, album, rating, duration)
- Search results sorted by album then track number
- Playlist picker redesigned as selectable list with "New Playlist..." entry
- Empty stars on cover art rendered as gray outlines
- pgup/pgdown/home/end work in all scrollable views
- Persist ReplayGain mode across server restarts
- TUI cover art shows computed album rating stars (with threshold)

## 0.11.0 (2026-05-24)

### Features

- 2-track target window: server loads only current + preloaded next track into targets for gapless playback without race conditions
- Event-driven track advancement via mpv end-file events (replaces polling-based detection)
- Desktop agent sends trackended to server via mpv end-file event detection
- Persist playback modes (repeat, random, single, consume) across server restarts
- Album rating stars burned into cover art image in TUI mini player
- Redesigned TUI mini player: 4-line layout with separate track/album lines, RG and mode flags, larger cover art
- Library panel hotkey hints removed for cleaner look

### Bug Fixes

- Fix random mode not working after server restart (modes not persisted)
- Fix songs skipping too early on natural track end (syncTarget was restarting the already-playing track)
- Fix external mpc next having no effect in random mode

## 0.9.0 (2026-05-24)

### Features

- Library sort by latest (mtime): all clients (web UI, Android, TUI)
- Server-side cache for latest-albums query with pre-formatted MPD response
- "Play on phone?" prompt when on mobile data and phone agent isn't active
- Web UI library view with latest sort toggle, cover art, and context menus
- Server accepts both numeric index and string device ID for enableoutput
- ExoPlayer error logging for silent playback failures

### Bug Fixes

- Fix mtime sort using DB insertion time instead of actual filesystem mtime
- Fix TUI latest-sort performance (remove N+1 album rating RPCs)
- Fix agent reconnect not reloading play queue (reloadQueueIntoAgent)
- Fix seekbar reset to zero on device handoff with transcoded streams
- Fix offline checkmark always showing when transcoding enabled
- Fix codec info text readability in Android mini player
- Fix WebSocket ping timeout (3s→30s) causing agent disconnects during queue reload
- Capture ffmpeg stderr for transcoding error diagnostics

## 0.8.0 (2026-05-23)

### Features

- Save and restore playback state across server restarts (queue position, time, play/pause)
- Per-client resume-on-connect setting (Android app settings toggle, agent config option)
- Offline library filter in Android app (FilterList icon on artist list)
- Graceful server shutdown with signal handling (saves state, closes agent connections)
- TUI redraws library browser on server reconnect
- Network-aware server selection: use external address when not on WiFi

### Bug Fixes

- Fix offline files starting from 00:00 on device handoff (always pass real timePos)
- Fix offline filter showing all artists (polling was overwriting filtered list)
- Fix offline filter hiding toggle button when no albums downloaded
- Fix Android using local server address when not on any WiFi
- Desktop agent stops playback immediately when server disconnects

## 0.7.0 (2026-05-23)

### Features

- Replace libmpv with ExoPlayer for Android audio playback
- Transcoded stream seeking via Subsonic-style start= offset (seek-by-reload)
- 1-second playback polling for smooth seekbar updates during active playback
- Agent stops playback immediately when server connection drops

### Bug Fixes

- Fix transcoded seek using locale-dependent decimal separator (comma instead of dot)
- Use database duration for agent targets to avoid incorrect stream-reported durations
- Fix device handoff with transcoding double-reloading the stream

## 0.6.1 (2026-05-23)

### Bug Fixes

- Fix idle notification race condition that prevented TUI and status bar widgets from updating when tracks were changed from other clients (e.g. Android app)
- Add mpv track change polling so idle clients are notified on natural track advances
- Fix progress bar showing 00:00 duration during track transitions (server-side DB fallback + TUI currentsong fallback)

## 0.6.0 (2026-05-23)

### Features

- Kitty graphics protocol album art display in TUI (Unicode placeholder mode)
- fzf-style library filtering in TUI
- Scroll letter indicator for library and search lists

### Bug Fixes

- Fix TUI progress bar not advancing during playback
- Skip FTS rebuild on startup for faster launch
- MPD protocol compatibility fixes (readpicture, plchanges, window parameter)

## 0.5.0

### Features

- Android app with MPD over WebSocket
- Search screen redesign with rating filter and multi-select batch actions
- Rating system (track and album ratings)
- FTS5 full-text search
- Queue diffing and MPD idle support
- Performance optimizations
- melody-cli tool
