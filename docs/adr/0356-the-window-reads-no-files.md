# ADR-0356: The window reads no files

## Status

Accepted, 2026-11-18. Completes ADR-0355.

## Context

After ADR-0355 a tab's rows were filled by its engine, but the window still
read files in three places: opening files and folders (walking folders,
resolving CUE sheets and reading the tags of the audio they name), Search
probing a tab's rows for the technicals a query needed, and a playlist
import reading every entry's tags. AGENTS.md: the UI asks, the core answers.

## Decision

1. **Opening is this computer's engine's.** What a window opens -- from a
   file manager, the command line, Open -- is this computer's, so its
   engine walks and reads it: `files.open` (protocol level 34) answers list
   items. A CUE sheet's tracks come with their logical reference, range,
   words and ReplayGain, the sheet's words before the audio file's tags; the
   files a sheet names are not rows again; any other file is a bare item
   the tab's engine fills (ADR-0355). For a list of an engine elsewhere,
   each file is translated to that engine's path where its music is mounted
   here, as before; the rest go into a list of this computer's.
2. **Asked once the engine is reached.** Opened before this computer's
   engine answers -- `melody file.flac` at a start -- a request waits,
   saying so, and runs when it does; requests run one after another rather
   than one being refused while another runs.
3. **Search does not probe.** Every row has its technicals from its engine,
   so a technical query reads them as they are.
4. **An imported playlist is not read.** Its rows keep the playlist's title
   and duration and are filled by the tab's engine.
5. **Covers come from the tab's engine**, this computer's too
   (`catalogue.artwork`), for any file, as its tags are: the rule that an
   engine gives covers only of indexed or held files is dropped. It never
   limited what a peer could read -- `metadata.read` reads any path -- and
   every peer is the engine's own user or authenticated (ADR-0223).

## Consequences

- No code in `src/bench`, `src/workspace` or `src/uicommon` calls the
  probe, the tag reader, the cover reader, folder discovery or the CUE
  resolver (`ui::loadLocalArtwork` is gone).
- A CUE row has no revision of its audio file -- as an engine list's CUE
  item never had -- so it is not followed through a move of that file; nor
  does it carry the sheet's track number, songwriter or ISRC.
- Parsing an M3U file chosen to import is still the window's; the playlist
  folder (ADR-0296) is the engine's way of reading playlists.
- `FileWorkTools` still defaults to this process's file access where an
  engine has not said it does file work; with every engine doing it, that
  default is the next thing to go.
