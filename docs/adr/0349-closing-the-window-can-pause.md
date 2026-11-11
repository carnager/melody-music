# ADR-0349: Closing the window can pause what plays

## Status

Accepted, 2026-11-09. Qualifies ADR-0220's "Trackknife may be closed without
stopping playback": that stays the default.

## Context

- **The engine plays on with every client closed** (ADR-0220). That is the
  point when a server plays to the living room and the window on a laptop is
  only a remote.
- **It is the wrong default for some.** Someone who uses Melody as a player
  at the desk, its music on a server's speakers, closes the window meaning
  "stop the music", and finds it still playing, with no window left to pause
  it from. They have to open Melody again, or use the CLI, only to pause.

## Decision

- **Settings › Playback › Pause the music when Melody closes**
  (`playback/pause-on-close`), off by default.
- **On**, closing the window (and so quitting Melody) pauses what plays on
  every engine the window is connected to: this computer's, and a server's.
  Only an engine that is playing is told, with `playback.pause`, which keeps
  the queue and the place in the track, so Play in the next window carries
  on. A close that does not happen (a tagger with unsaved changes says no)
  pauses nothing.
- **The command is sent as the window closes.** Each engine connection
  finishes its commands before it closes, so the pause reaches the engine
  before Melody exits.
- **It pauses the engine, not just this listener**: anyone else listening to
  that engine (another client, a listening room) hears it pause too. The
  setting's tooltip says so.

## Consequences

- Off, nothing changes: closing the window never interrupts the music.
- On, an engine that should keep playing for others while this window
  closes cannot: this is a per-computer choice, for a computer that is the
  player.
- An engine the window cannot reach as it closes is not paused, and nothing
  waits for it.

## Verification

- `bench-main-window` › `closingPausesWhatPlaysWhenAsked`: with the setting
  off, music playing on the engine is still playing after the window
  closes; with it on, it is paused; Settings › Playback shows the setting.
