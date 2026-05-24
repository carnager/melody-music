# Changelog

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
