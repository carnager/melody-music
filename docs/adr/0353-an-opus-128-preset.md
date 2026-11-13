# ADR-0353: An Opus 128 kbps preset

## Status

Proposed, 2026-11-12. Adds a preset to ADR-0133's converter.

## Context

- **A library carried on a laptop or a phone wants smaller files than
  Opus 192.** The converter's presets -- the files `convert.apply` writes
  and keeps, tags and cover carried and verified -- are WAV, FLAC, Opus 192,
  MP3 V0 and Vorbis q6. For a library of 35,000 tracks copied to a MacBook
  Air -- and next to an iPhone -- Opus 192 is a third larger than the
  listener asked for, and MP3 V0 larger still.
- **This is not the Opus of streaming.** Streams to phones and speakers are
  transcoded on the fly at any rate from 16 to 512 kbit/s (ADRs 0230,
  0231), for playback; they are not files kept with their tags. Only the
  converter's list lacked 128 kbit/s.
- **Opus at 128 kbit/s is transparent for most listeners** and is what
  portable players are commonly given. A preset is the converter's whole
  promise about a file: naming the rate in the id keeps an exported
  library's files the same from run to run.
- **Kept outside Melody, the preset was lost.** An external export plugin
  (melody-library-export) carried it as a local patch; an engine update
  without it left the plugin unable to convert, by design refusing to
  substitute another rate.

## Decision

1. **`opus-128` beside `opus-192`**: libopus at 128000 bit/s in an Ogg Opus
   file (`.opus`), at 48 kHz as Opus is. `opus-192` and every other preset
   are unchanged.
2. **Nothing else changes.** Conversion keeps the converter's tag, cover and
   verification paths (ADR-0133, ADR-0305) and its atomic publication;
   clients find the preset through `convert.presets`, as they find the others.
   No protocol change.

## Verification

- `audio-convert`: `builtinPresetsProbeAvailable` finds `opus-128` at
  128000 bit/s; `convertsToEveryPresetAtomically` encodes, probes and
  decodes a real file with every preset, `opus-128` among them, tags and
  cover carried, at 48 kHz.
- In use: the export plugin converted and delivered about 35,000 tracks of a
  real library with it, decoded and tag-checked.
