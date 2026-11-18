# ADR-0358: The engine reads an imported playlist

## Status

Accepted, 2026-11-18. Follows ADR-0356.

## Context

After ADR-0356 the window read no audio files, but importing an M3U8
playlist still read the playlist file in the window (`lists::read_m3u8`).
AGENTS.md: the UI asks, the core answers.

## Decision

- **A playlist file chosen to import is read by this computer's engine**:
  `playlists.read {path}` (protocol level 36) answers `{entries: [{path,
  title, duration_ms?}]}`, parsed by the same reader as before -- strict
  UTF-8, relative entries anchored at the playlist, missing files and
  repeats kept, any error rejecting the whole import.
- The window makes the rows from the entries as before (ADR-0355: filled by
  the tab's engine), named after the playlist file.
- Before this computer's engine is reached, an import is refused with a
  message saying so, as the other file tools are.

## Consequences

- Exporting a list as M3U8 still writes the file from the window: it makes
  a new file, as converting does (ADR-0340), rather than reading one.
