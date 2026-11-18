# ADR-0357: One ReplayGain round, in the engine

## Status

Accepted, 2026-11-18. Amends ADR-0237 ("ReplayGain from any client") and
ADR-0156.

## Context

ReplayGain was written twice. The window's ReplayGain dialog drove the
engine step by step -- tags read through `metadata.read`, loudness measured
through `loudness.scan`, the plan built in the window and written through
`metadata.apply` -- while `melody-cli` asked the engine for the whole round
in one job, `replaygain.apply {paths}`. Both used the same pipeline
(`loudness/replaygain.hpp`), so plain files got the same gains, but the
two differed where the inputs did:

- The job took bare paths, so it could not measure a CUE sheet's tracks
  (or a stream, subsong or range of a file) on their own, and offered only
  "album" and "track" grouping.
- The job counted gains already in a sidecar as the file's, so the same
  gains again wrote nothing; the window rewrote them.
- The window left a file it could not read out and measured the rest; the
  job stopped at it.
- The window observed a CUE sheet's revision on its own disk, which on a
  tab of another engine is not where the sheet is, so a CUE track there
  could not be written.

Converting stays the window's (ADR-0340): it makes new files on this
computer with its CPU. ReplayGain changes the files where they are, and the
engine holding them records the change.

## Decision

1. **`replaygain.apply` is the one round** (protocol level 35), and every
   client asks for it: `{items, grouping, expression?, true_peak, sidecar,
   preview}`. An item is a path, or `{path, cue?: {sheet, file, track},
   stream?, subsong?, start?, end?}`; the engine reads a CUE track from its
   sheet. Grouping is any of the window's: `release` (the default),
   `discs`, `album`, `track` or `expression`.
2. **The window's dialog only asks.** It sends the selected rows as items
   and shows how far the engine has got (reading, measuring, writing).
   `preview` answers the album groups, measuring and writing nothing.
3. **One behaviour.** The engine leaves out and names a track it cannot
   read, counts a fresh sidecar's gains as the file's, and writes what was
   measured whole or not at all. The result carries what was written, for
   the window's rows to show it.
4. **A CUE sheet's revision is observed where the files are**, as their
   tags are read (`capture_metadata_sources`), for the tag editor too.

## Consequences

- `melody-cli replaygain` reports files it could not read.
- The tag editor's own ReplayGain scan still measures through
  `loudness.scan` and stages the gains for review; it writes nothing itself.
- A round of up to 100,000 tracks is one request.
