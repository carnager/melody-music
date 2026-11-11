# ADR-0352: A scan names the files it could not read

## Status

Accepted, 2026-11-11.

## Context

A scan that cannot read some files ends with "Scan incomplete. 16 files
could not be read; previous entries were kept." and nothing more. The scan
knew which files, and why -- a probe that found no audio stream, tags that
would not parse, a file that changed while it was read -- and threw that
away, keeping only the count. The user is left to guess which sixteen of
their thousands of files to look at.

## Decision

- **The scan keeps which files, and why.** `LibraryScanResult` carries
  `unreadable`: each file's raw path and a reason in words, in the order the
  walk met them. The reason is the probe's or reader's own error message,
  "no audio stream was found", or "the file changed while it was being
  read".
- **Bounded.** At most 1000 files are named
  (`LibraryScanResult::unreadable_named`); every one is still counted in the
  progress's `failed`, so the window can say how many more there were.
- **Over the protocol** (level 32): the `catalogue.scan` job's outcome
  carries `unreadable: [{path, reason}]`, paths encoded as everywhere else.
  An older engine sends none; the window then shows the count only, as
  before. An entry the client cannot decode is passed over rather than
  failing a scan that happened.
- **Shown on request.** Under the library's status line, "Show the files"
  opens a list of each file and why, which can be copied. It goes when the
  next scan starts. Nothing more is kept: the list belongs to that scan, not
  to the library.

## Consequences

- A file named here keeps its previous library entry (ADR-0313); the list is
  what to fix, then Refresh again.
- A refresh of named files (`catalogue.scan {paths}` naming a file) still
  counts and names nothing it could not read, as before.
