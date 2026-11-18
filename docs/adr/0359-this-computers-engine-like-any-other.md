# ADR-0359: This computer's engine, like any other

## Status

Accepted, 2026-11-18.

## Context

Going through the window's `isLocal()` branches after ADRs 0355–0358 left
three where this computer's engine was treated differently from any other
for no reason of its own:

- Choosing albums in this computer's library resolved them to paths and
  opened the files again (`files.open`); every other engine's library handed
  over its rows.
- What an engine plays when no tab holds it -- a queue another client
  replaced -- went into a new list for this computer each time, and for an
  engine elsewhere replaced the contents of its first tab, which could be a
  list of the user's.
- Every engine elsewhere keeps its own move destinations; this computer's
  were kept by the window in Melody's presets.

## Decision

1. **A library's tracks reach a list the same way for every engine**: its
   catalogue gives their rows, into the list on screen when it is of that
   engine, else the engine's own.
2. **What an engine plays when no tab holds it goes into that engine's
   playing list**, "Playing on …": made the first time, followed from then
   on, never taking the place of another list.
3. **This computer's engine keeps its move destinations**, as every engine
   does (`destinations.list/save/remove`). Those kept in `presets.sqlite`
   before are moved into it once, automatically, when it is first reached;
   one it already has by name is not saved twice. The Convert window offers
   the same destinations, from this computer's engine.

## Consequences

- Naming layouts, conversion profiles and tagging scripts stay Melody's
  presets (ADR-0339); only destinations, which name folders on one machine,
  are an engine's.
- The remaining `isLocal()` uses are about this computer itself: its name,
  its file dialog, its mounts of other engines' music, and the engines
  configured in Settings.
