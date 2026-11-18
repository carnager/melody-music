# ADR-0102: Go to Artist/Album in the MPD library

- Status: retired by ADR-0224 (Melody no longer speaks MPD); was accepted
- Date: 2026-09-02
- Owners: Trackknife project

## Context

The MPD queue and the library tree lived side by side with no bridge:
finding the playing track's album meant re-typing a search or expanding
the tree by hand — navigation Cantata offered and Trackbench lacked.

## Decision

- The queue's context menu gains "Go to artist" and "Go to album"
  (`action-mpd-go-to-artist` / `action-mpd-go-to-album`), enabled from
  the current row's grouping-artist role and album column.
- Navigation is an asynchronous walk of the lazy library tree: an
  unloaded tree reloads its roots first (rows-inserted or model-reset,
  whichever the model emits), the artist row is matched by its root
  grouping value, the artist branch is fetched when needed, and the
  album row is selected and centered.
- Album-level grouping values are definition-specific composites (a
  release id, or album|date), so the album match is definition-aware:
  the plain album name matches the grouping value, its album|-prefix,
  or the displayed label. A miss selects the artist and reports in the
  status bar instead of failing silently; an artist the tree does not
  know reports without navigating.

## Consequences

- Two clicks jump from any queue entry to its place in the library —
  including the playing track via its queue row.
- The offscreen test drives the full asynchronous chain against the
  scripted model: unloaded tree → root load → branch fetch → album
  selection, plus direct artist selection and the unknown-artist
  status-bar report.
