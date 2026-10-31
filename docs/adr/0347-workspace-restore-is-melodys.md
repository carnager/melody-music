# ADR-0347: Workspace backup and restore are Melody's, part by part

## Status

Accepted, 2026-10-31. Amends ADR-0175 (restore) and ADR-0286 (tabs in the
backup).

## Context

Since ADR-0339 the window keeps its presets in its own database,
`presets.sqlite`, and File › Back up workspace database… copied that
database, the settings and the tabs' rows. Restore was never moved along: at
the next start it put the backup in place of **the engine's**
`lists.sqlite`. Both have the same schema, so the check passed, and a restore
would have replaced this computer's engine library, saved playlists, ratings
and listening history with Melody's presets. The engine's database is the
engine's (ADR-0220): the window must not replace it.

Restore was also all or nothing. A user who wants back their tagging scripts
without losing today's settings or open tabs had no way to say so.

## Decision

- **A workspace backup is three parts of Melody's own**, beside one another:
  the presets database (tagging scripts, naming layouts, encoder and view
  presets) at the chosen path, the settings in `<path>.settings.ini`, the
  open tabs' rows in `<path>.tabs`. Which tabs are open (`tabs/v1`) is a
  setting, but belongs to the tabs part. The menu entries are *Back up
  workspace…* and *Restore workspace…*.
- **Restore asks which parts**: a dialog lists the three, each the backup
  holds ticked, those it lacks greyed out; *Close Melody and restore* needs
  at least one. The choice is kept in the settings and applied at the next
  start, before anything opens what it replaces.
- **Each part goes back to where it came from**, and what it replaces is
  kept in Melody's data folder: `presets-before-restore-<time>.sqlite`,
  `settings-before-restore-<time>.ini`, `tabs-before-restore-<time>`.
  Settings restored without the tabs keep the tabs open now; the tabs
  restored without the settings change only `tabs/v1` and the rows. One part
  failing does not stop the others; the window says which were restored and
  which were not.
- **No engine's database is touched.** Backing up an engine is the engine's
  to offer, over the protocol, not the window's.
- A restore scheduled by the old code (`recovery/pending-workspace-restore`)
  is dropped, never applied.
- The backup saves the tab list before copying the settings, so the list and
  the rows in a backup are of the same moment.

## Verification

`bench-main-window` (`aRestoreTakesBackOnlyWhatIsTicked`): a backup offers
all three parts; with none ticked Restore is disabled; presets and settings
restored leave the open tabs as they are; the tabs restored alone leave the
settings; each replaced part is kept; nothing pending is applied twice; the
engine's `lists.sqlite` is unchanged and no rollback of it is made.
