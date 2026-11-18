# Documentation

## Using Melody

- [Installation](install.md) and [setup](setup.md).
- [Engines, agents and the phone](melody.md): how they connect, a headless
  `melodyd`, and `melody-cli`.
- [Local library](local-library.md#using-the-library): add folders, browse albums,
  search, and save searches.
- [Last.fm accounts and scrobbling](lastfm.md): independent local/server accounts and Love/Unlove.
- [Dynamic playlists](dynamic-playlists.md): rules and Last.fm sources, on either library.
- [Search syntax](query-language.md): field filters such as
  `bitspersample EQUAL 24` and `REPLAYGAIN_ALBUM_GAIN MISSING`.
- [Formatting syntax](formatting.md): fields, functions and examples; the full
  reference is [tkfmt.md](tkfmt.md).
- [Tagging and artwork](metadata-and-files.md#reviewing-fields-and-identifying-untagged-albums):
  review edits, match MusicBrainz tracks, and manage covers.

The more technical documents below describe how the features work and where
their limits are.

## Current state

Updated on 2026-11-18, through ADR-0359: database schema 58, protocol 1 at
level 36 ([protocol levels](protocol-levels.md)).

**The unified engine is in place.** `melodyd` owns the library and playback
and speaks protocol v1 over a socket or TCP. Melody, the Android app and
`melody-cli` are its clients, and audio comes out through outputs: the
engine's own speakers, agents on other machines and phones, and optional UPnP
renderers. File work -- tagging, ReplayGain, conversion, moves -- is the
engine's too, on its own machine. This supersedes the two-authority model in
older ADRs; [architecture.md](architecture.md) describes the system as it is.
[The migration plan](unified-engine.md) says how far along each phase is;
[melody.md](melody.md) says how to run it.

Every decision is in the [ADR index](adr/README.md), newest first, with its
status and the later decisions that changed it.

The application is built as `melody` from `src/bench` (ADR-0287). Older
development documents call it **Trackknife**, and before that **Trackbench**. Its library and playback belong
to its engines ([unified engine](unified-engine.md)); the MPD client it used
to contain was retired in [ADR-0224](adr/0224-retire-the-mpd-backend.md).

Milestones M5–M10 (tagging, MusicBrainz, ReplayGain, conversion, hardening)
are complete, and so is M11, the unified engine, as ADR-0259 has it. What 1.0
still needs is in the [roadmap](roadmap.md#toward-10). The [feature matrix](feature-matrix.md)
lists what works and its restrictions. The [roadmap](roadmap.md) lists the
remaining work.

Documents written for the older two-authority model -- an MPD side and a local
side in one window -- say so at the top, with what still holds.

## Working on the code

Start with [AGENTS.md](../AGENTS.md) for repository rules, then read:

1. [Product scope](product.md) and [compatibility](compatibility.md);
   [milestones](../MILESTONES.md) for the 1.0 criteria.
2. The relevant feature document from the list below.
3. [Architecture](architecture.md) and the related [design decisions](adr/README.md).
   A new ADR goes in `docs/adr/`; then `python3 scripts/adr_index.py` updates
   the index.

A development build and test run:

```sh
cmake --preset dev
cmake --build --preset dev
QT_QPA_PLATFORM=offscreen ctest --preset dev
cmake --build build/dev --target format-check
bash scripts/check_spdx.sh
```

Run these from the repository root. Sanitizer and static-analysis presets are
also available in [CMakePresets.json](../CMakePresets.json).

## Feature references

- [Up Next](up-next.md): temporary requests with automatic return to normal
  playback, held by the engine that plays; [design](adr/0196-up-next-request-queue.md).
- [Workspace](ui-workspace.md): tabs, views, controls, and performance requirements.
- [Local library](local-library.md): indexing, offline folders, refresh, and searches.
- [Metadata and files](metadata-and-files.md): tag drafts, artwork, rename/move, and recovery.
- [ReplayGain](replaygain.md): measurement, storage, and playback gain.
- [Playback and conversion](playback-library-conversion.md): working lists,
  conversion, and planned collection tools; its playback parts are partly
  historical.
- [Query language](query-language.md): the `tkq-1` grammar and evaluation rules.
- [Title formatting](title-formatting.md): the `tkfmt-1` language specification.
- [Plugins](plugins.md): context-menu actions run by programs of your own.
- [Listening together](together.md): a room friends listen in at once, through melody-together.
- [Tagging scripts](tagging-scripts.md): a tagging script as text, one
  `$`-statement per step.
- [Open decisions](open-decisions.md): questions that still need a decision.
- [Release checklist](release-checklist.md): packaging, legal, accessibility,
  stress, backup/restore, and end-to-end acceptance gates.
- [Roadmap](roadmap.md): remaining prioritized work and what 1.0 still needs,
  updated through ADR-0269.
- [Sources](sources.md): references used when designing and checking behavior.

[History](history/README.md) keeps retired and dated documents -- the MPD
client, the M3 and M10 validation notes; they, and older ADRs, record work at
that point in time. Use the feature matrix for current status;
an old test result or screenshot doesn't establish what the current app supports.

In specifications, **Melody decision** means behavior chosen for this app,
**Compatibility requirement** means behavior it must match elsewhere,
**Proposal** means a suggested direction, and **Unknown** means it still needs
research or a decision.
