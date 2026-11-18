# ADR-0340: Melody converts

## Status

Accepted, 2026-10-27 (merged from the branch `mpd-client`). Supersedes
ADR-0336. Amended 2026-11-18: melody-cli keeps converting in the engine.

## Context

ADR-0336 had the Convert window hand its conversions to the engine on this
computer, so that one path converted. That ties conversion to a local
engine, which the direction "MPD and a powerful client" (ADR-0339) does not
need: a server owns the library and playback, and Melody does the file work
on this computer -- with its CPU, which is the point of converting here
rather than on a home server or a Raspberry Pi. This computer's engine
still runs, for playback here; it no longer converts for the window.

## Decision

- **Melody converts**, in its own process, with the shared converter: the
  window's options, Cover settings' limit for lossy covers (ADR-0335), the
  folders made first. Files of an engine elsewhere are fetched to this
  computer first, as before (ADR-0237 stage 6).
- The `convert.files` job and its encodings are gone (protocol level 27);
  nothing asks an engine to convert for the window.
- `convert.apply` stays, an engine converting its own files for
  `melody-cli` (amended 2026-11-18, below).

## Amendment, 2026-11-18

`melody-cli convert` stays in the engine; it is not moved to the CLI's own
computer. Both hosts run the one converter (`convert::scan_conversion`), so
a file converts the same either way -- only where it runs differs, and for
the CLI the engine's machine is the right place:

- `--to FOLDER` and the paths are the engine's: the files are made where the
  music is, as a cron job on the server expects.
- ReplayGain on what was made is measured by the engine holding it
  (`replaygain.apply`, ADR-0357); made on the CLI's computer, it would need
  an engine there or a second ReplayGain path.
- melody-cli needs neither FFmpeg nor the converter.

The window converts in its own process because its files land on its own
computer, with its CPU; the CLI asks the engine because its files land on
the engine's.

## Verification

- `bench-main-window`: the Convert window's conversions -- a selection, a
  file fetched from another engine, a permanent gain -- carried out by
  Melody.
