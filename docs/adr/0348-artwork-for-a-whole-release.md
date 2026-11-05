# ADR-0348: The tag editor's covers are for a whole release

## Status

Accepted, 2026-11-05. Amends ADR-0077 (the 64-source inventory limit),
ADR-0085 (one inventory row per picture per file) and ADR-0137 (a file whose
covers cannot be written makes the whole selection view-only).

## Context

The tag editor's Artwork section refused to read more than 64 files: above
that it said "Artwork inventory is limited to 64 physical sources" and Add,
Fetch cover and the compact cover were off. A CD with 99 tracks -- some
discs end in many short silent ones -- could not be given a cover in one go.
Nothing else in the cover path is bounded that tightly: the write plan takes
100,000 changes and Apply writes file by file on a bounded worker.

Two more things made a release's covers awkward:

- Every file's picture was its own row, so an album's cover was shown once
  per track, each decoded again for its thumbnail.
- One file whose covers Melody cannot write (WAV, AIFF, APE, WavPack, ...)
  turned Add off for every file selected with it.

## Decision

- **No file-count limit.** The inventory reads every selected file, in the
  background and cancellable as before. Inventory items hold fingerprints
  and thumbnails, not image bytes, so its size grows with files only slowly.
- **One row per distinct image.** Rows group pictures with the same bytes,
  role, MIME type and description -- and, for an image beside the files, the
  same path. The File column names the file, or says "*N* files" with the
  paths in its tooltip. A thumbnail is decoded once per distinct image.
  Replace and Remove on a row act on that picture in every file that holds
  it; the row is struck through once all of them go.
- **Files that cannot hold a cover are skipped.** Adding, fetching, pasting
  or copying a cover stages it for the files whose covers can be written and
  leaves the others out. They keep their "View-only" row in Problems, which
  says that added covers skip them, and do not make the front cover read as
  mixed. Only if no file can take a cover is adding off.
- **A file changed since the editor opened still stops it**, as it stops the
  tags' Apply: that is a conflict, not a format the user chose.

## Consequences

- A release of any length takes its cover in one step; a box set shows its
  few covers as a few rows.
- The pending list still has one row per file changed; it is capped in
  height and scrolls.
