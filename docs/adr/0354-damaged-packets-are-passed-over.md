# ADR-0354: A damaged packet is passed over, not the track

## Status

Proposed, 2026-11-12. Qualifies the decoder's handling of codec errors; the
converter's verification of its own output (ADR-0133) is unchanged.

## Context

- **One bad packet failed the whole file.** The decoder stopped at the first
  packet the codec refused, "submitting encoded audio failed: Invalid data
  found when processing input". Real libraries hold such files: an MP3 with a
  few frames missing their header, an ALAC file in MP4 whose last packet is
  8 bytes. Both play everywhere else; FFmpeg's own `ffmpeg` and `ffplay` say
  "Header missing" or "invalid samples per frame: 0", pass the packet over
  and go on.
- **FFmpeg 9 made it common.** Built with FFmpeg 8 the engine decoded these
  files; with FFmpeg 9 and later the same packets come back as
  `AVERROR_INVALIDDATA`. In one library of 35,000 tracks, 78 of the first 90
  files a conversion took were refused this way (70 MP3, 8 ALAC), each a
  handful of damaged packets in an otherwise sound file. Playback goes
  through the same decoder.
- **Passing everything over would hide a file that is not audio.** A stream
  of nothing but refused packets, or a converted file that came out damaged,
  must still fail.

## Decision

1. **A packet the codec refuses as invalid data is passed over and counted.**
   At sending it and at receiving its frame alike. Its few milliseconds are
   missing from the decoded audio; everything else decodes as before. A file
   renamed over mid-read is still reopened first (it reads as damage too).
2. **Damage that is the file fails as before.** More than 32 refused packets
   in a row, or a file that ends having decoded no frame at all, fails with
   "decoding damaged audio". 32 is far more than any real damage seen: a
   handful at most, never more than a few together.
3. **The count is said.** `AudioDecoder::damaged_packets()`. A conversion's
   result carries `damaged_source_packets`, and the `convert.apply` job's
   file carries `damaged_packets` when there were any (protocol level 33).
4. **A converter's own output is held to none.** Its verification reads the
   new file back and refuses it if a single packet had to be passed over:
   what the engine writes is never damaged.
5. **A frame is converted as it comes.** Past a damaged packet the decoder
   reaches frames it never did before, among them, in real MP3 files, a mono
   frame in a stereo stream. The resampler was set up once for the stream,
   and read the mono frame's missing second plane: a null pointer, and the
   engine crashed. When a frame's channel layout, sample format or rate is
   not the resampler's, the resampler now takes the frame's; the output
   does not change, so a mono frame is mixed up to the stream's stereo.

## Verification

- `decoder-damaged-packets`, from the MP3 fixture: one frame's header
  removed decodes, one packet passed over, one frame short; every header
  removed fails or yields nothing; converted to FLAC, the damage is said and
  the output reads back with none. Without the decision the first and third
  fail with the error above. From the `mono-frame-in-stereo-mp3` fixture,
  a stereo stream with one mono frame decodes and converts; without
  decision 5 the test crashes (SIGSEGV), as the engine did converting
  "04 Obvious.mp3" on the server.
- `decoder-replaced-file`, `audio-convert`, `formats-probe`,
  `loudness-analyzer` and `local-playback` fail the same checks with and
  without it on macOS 27 (libvorbis absent, module files).
- The two real files above convert to Opus and FLAC, 5 and 1 packets passed
  over, matching what `ffmpeg` reports; the outputs decode cleanly.
