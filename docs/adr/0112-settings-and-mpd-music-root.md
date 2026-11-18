# ADR-0112: Settings dialog and the MPD music-root bridge

- Status: retired by ADR-0224 (Melody no longer speaks MPD); was accepted
- Date: 2026-09-03
- Owners: Trackknife project
- Extends: ADR-0058 explicit MPD authority, ADR-0083 trust over
  confirmation; delivers the first piece of the M9 music-root work item

## Context

Two settings needed a home: which queue context activates on start, and
the MPD music folder as this machine sees it. The music folder is the
bridge that lets MPD-mode selections become local files — the
prerequisite for tagging, converting, and ReplayGain-scanning tracks
that were picked in the MPD queue or server library.

## Decision

- Edit → Settings… (Ctrl+,) opens a small dialog with exactly those two
  values, persisted through QSettings on Save: the startup context
  (Local queue / MPD queue, applied when the workspace restores) and the
  MPD music folder.
- "Load as local files" appears in the MPD queue context menu (selection)
  and the server-library context menu (any node, using the same
  lazy-fetch machinery as the queue actions). It resolves each URI below
  the configured music folder, opens the files that exist as an ordinary
  local scratch tab named "Local files", and reports misses in the
  status bar without failing the rest. An unset music folder points the
  user at Settings instead of guessing.
- The loaded tab is a plain local list: Properties, Convert, ReplayGain,
  and publication behave exactly as for any local selection, and any MPD
  database update afterwards stays an explicit MPD action (ADR-0058).

## Consequences

- The M9 work item shrinks to per-connection roots and deeper
  integration (implicit resolution for Properties directly on MPD rows);
  the explicit load-as-tab flow already covers the practical need.
- Tests pin the startup-context switch, dialog persistence, URI
  resolution with partial misses into a fresh tab, and the presence of
  the queue context action.
