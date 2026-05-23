# Changelog

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
