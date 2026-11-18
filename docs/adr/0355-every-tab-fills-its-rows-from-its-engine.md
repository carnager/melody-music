# ADR-0355: Every tab fills its rows from its engine

## Status

Accepted, 2026-11-18.

## Context

A tab's rows start as paths -- dragged in, opened, taken over from a queue,
followed to where a file moved -- and are filled in afterwards. That was
done two ways. A tab of an engine elsewhere asked that engine's index
(`catalogue.cached_tracks`); a path the index did not know kept its file
name. A tab of this computer's engine read and probed its files in the
window's own process, with a probe queue, and split a file into its
container chapters or codec subsongs, and projected a ReplayGain sidecar
onto it -- none of which the library or a remote tab did.

So the same file showed differently by which engine its tab was of, and
the window read files itself, which AGENTS.md rules out ("the UI asks; the
core answers").

## Decision

1. **One path for every tab.** Its rows are filled by the engine the tab is
   of, local or elsewhere, through `catalogue.read_tracks` (protocol level
   33): an indexed track from the index, any other file read from its file
   as a scan reads it (`prepare_file`), and not indexed. A file that cannot
   be read is left out; its row keeps what it had and is not asked again.
2. **The revision travels.** Tracks from `cached_tracks` and `read_tracks`
   carry the file's revision, so every row can follow its file through the
   move journal, a remote tab's included.
3. **A file's tags win, but a missing tag does not erase.** Where the file
   has no title, artist, album, album artist, date or track number, the row
   keeps the one it had -- a queue entry another client gave its tags, say.
4. **No splitting into chapters or subsongs, no sidecar projection, in the
   window.** A file is one row, as the library has it. The engine plays and
   applies ReplayGain itself.
5. **FFmpeg's spellings are aliases** (`track`, `disc`, `album_artist`,
   `encoder`): `metadata::stream_tag_alias`, used by the scanner, so they
   no longer come through as extra fields -- in tabs or in the library
   (after a rescan).

## Consequences

- The window's probe queue, its row builders and its sidecar projection
  are gone (about 400 lines); so are the tests of chapter and subsong rows.
- CUE sheets still give one row per track: they are resolved when a folder
  or file is opened, which is still the window's (discovery) -- the next
  step towards the window reading no files at all, with the reads in
  `search_session.cpp` and `playlist_transfer.cpp`.
