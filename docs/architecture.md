# Architecture

How Melody is built today (2026-11-18, through ADR-0358). The direction it
follows is [ADR-0220](adr/0220-unified-engine-and-remote-agents.md); how the
code got here is in [the unified engine plan](unified-engine.md) and the
[ADR index](adr/README.md). The two-authority design this document used to
describe -- an MPD side and a local side in one window -- is gone
([ADR-0224](adr/0224-retire-the-mpd-backend.md)); its version of this
document is in the repository history.

## System shape

One engine process, `melodyd`, owns a library and everything done to it: the
catalogue, lists, ratings and history, playback, and every read and write of
its files. Everything else is a client of one or more engines over protocol v1
-- a Unix socket on the same machine, TCP with a password from elsewhere
([ADR-0223](adr/0223-tcp-transport-and-authentication.md)).

```text
 melody (window)   melody-cli   melody-pick   Android app   melody-watch
        \               |            |            |             /
         \--------------+---- protocol v1 --------+------------/
                                     |
                               melodyd (engine)
             catalogue · lists · playback · file work · journals
                  |                    |                    |
           its music files      lists.sqlite        outputs: its own speakers,
                                                     agents (melody-agent,
                                                     phones), UPnP renderers
```

- **melodyd** (`src/daemon`, `src/engine`): the engine. Its state is one
  SQLite database, `lists.sqlite`, in its state directory
  (`~/.local/share/melody/engine` by default). It plays on its own speakers
  and on outputs elsewhere, and keeps playing when every client is closed.
- **melody** (`src/bench`, `src/workspace`, `src/uicommon`): the desktop
  window, Qt Widgets. It starts this computer's engine when none is running
  ([ADR-0226](adr/0226-trackknife-runs-its-own-engine.md)) and holds a
  connection to every engine configured
  ([ADR-0234](adr/0234-engines-are-connections.md)).
- **melody-cli** (`src/cli`) and **melody-pick** (`src/bench/pick_main.cpp`,
  [ADR-0346](adr/0346-melody-pick.md)): command-line and key-binding
  clients.
- **melody-agent** (`src/agent`): an output on another machine
  ([ADR-0228](adr/0228-output-agents-on-protocol-v1.md)); engines find
  agents and each other on the network
  ([ADR-0229](adr/0229-engines-find-each-other.md)).
- **melody-watch** (`src/watch`): tells an engine which files changed on a
  NAS it reads over a mount ([ADR-0232](adr/0232-melody-watch.md)).
- **The Android app** (`android/`, Kotlin): a client, with Opus streams and
  offline albums ([ADR-0230](adr/0230-opus-streams-and-download-tickets.md),
  [ADR-0231](adr/0231-android-client.md)).

## The rules

- **The UI asks; the core answers.** Nothing in `src/bench`, `src/workspace`
  or `src/uicommon` opens the engine's database, reads tags, decodes audio,
  walks folders or reads covers. The window reads no files
  ([ADR-0356](adr/0356-the-window-reads-no-files.md)): it asks the engine the
  files belong to. Converting is the one exception: the window converts in
  its own process, because what it makes lands on its own computer
  ([ADR-0340](adr/0340-melody-converts.md)).
- **One implementation, not two.** There is no local-versus-remote branch: a
  tab names the engine its list came from
  ([ADR-0221](adr/0221-entry-and-track-identity.md),
  [ADR-0233](adr/0233-lists-live-in-the-engine.md)), and every tab is filled,
  given covers and worked on by that engine in the same way
  ([ADR-0355](adr/0355-every-tab-fills-its-rows-from-its-engine.md)). This
  computer's engine is one engine among others, reached the same way.
- **Work happens where the files are.** Tagging, artwork, ReplayGain, moves
  and undo are done by the engine holding the files, on its machine
  ([ADR-0237](adr/0237-file-work-in-the-engine.md)). A client previews; the
  engine revalidates, writes, journals and follows the change in its library
  and lists.
- **Everyone runs the latest.** A client uses an engine at its own protocol
  level or newer and refuses an older one
  ([ADR-0260](adr/0260-protocol-version-and-compatibility.md),
  [protocol levels](protocol-levels.md)). The database has one schema and
  one automatic upgrade step from the version before
  ([ADR-0354](adr/0354-one-schema-no-migrations.md)); a user never runs a
  command after an update.

## The engine

`src/engine` is the engine's library; `src/daemon` is its `main`, settings and
configuration methods.

- **Catalogue** (`catalogue.hpp`): the library index over the engine's music
  folders -- scanning, queries, album and artist views, covers and lyrics, and
  `read_tracks` for any file a list holds. `LocalCatalogue` is the engine's
  own; `RemoteCatalogue` is the same interface over the protocol, which the
  window uses for every engine, this computer's included.
- **Workspace** (`workspace.hpp`): the lists -- each a set of entries in an
  order -- their edits ([ADR-0256](adr/0256-lists-travel-as-edits.md)),
  playlists as files in the playlist folder
  ([ADR-0296](adr/0296-playlists-are-files.md)), favourites, dynamic
  playlists.
- **Player and outputs** (`player.hpp`, `outputs.hpp`): one playback state
  machine, numbered states
  ([ADR-0236](adr/0236-playback-states-are-numbered.md)), Up Next
  ([ADR-0196](adr/0196-up-next-request-queue.md)), and outputs: the
  engine's own speakers through PipeWire, agents, UPnP renderers, streams.
- **File work** (`file_work_methods.hpp`, `job_methods.hpp`): reads
  (`metadata.read`, `files.open`, `playlists.read`, `media.probe`), loudness
  scans, and the journaled writes -- `metadata.apply`, `artwork.apply`,
  `preparation.apply` (moves and renames), `operations.undo` -- plus the
  whole ReplayGain round, `replaygain.apply`
  ([ADR-0357](adr/0357-one-replaygain-round-in-the-engine.md)).
- **Jobs** (`job_registry.hpp`): long work runs as a job a client submits,
  watches (`job.progress`) and may cancel.
- **Server** (`server.hpp`): protocol v1 -- newline-delimited JSON requests,
  answers and events -- on a socket and on TCP, and the HTTP stream port.
- **Events**: every change to a library or list is told to every client
  connected, which follows it in place
  ([ADR-0268](adr/0268-the-library-follows-its-files.md)).

## The window

`src/bench` is the window and its dialogs; `src/workspace` holds what they draw
over, Qt-based but without widgets; `src/uicommon` holds shared presentation.

- **Workspace** (`src/workspace/workspace.hpp`): the tabs and their rows, a
  connection per engine (`EngineLink`: its catalogue, its playback, its file
  work), Up Next, the desktop media session. A tab's rows come from its
  engine (`catalogue.read_tracks`); files opened from outside become rows
  through this computer's engine (`files.open`); covers come through one
  `CoverLoader`.
- **File tools**: the tag editor (`tagger_session`), artwork, ReplayGain,
  rename and move, and conversion are sessions in `src/workspace` drawn by
  dialogs in `src/bench`. Their reads and writes go through the engine's
  `RemoteFileWork` (`src/engine/include/trackknife/engine/remote_file_work.hpp`).
- **Library panel** (`library_browser`): the library of the engine chosen as
  the library ([ADR-0286](adr/0286-one-library.md)), through its catalogue.
- **Presets** -- naming layouts, conversion profiles, tagging scripts -- are
  Melody's, in `presets.sqlite` on this computer
  ([ADR-0339](adr/0339-presets-are-melodys.md)); an engine is handed the
  ones it needs.

## Shared libraries

Each is Qt-free C++23 with its own tests, used by the engine and, where
noted, by clients.

| Module | What it holds |
|---|---|
| `core` | Errors and results, raw paths and source revisions, cancellation, Unicode, environment and state paths. |
| `protocol` | Protocol v1 messages, the client, raw-path encoding, version levels. |
| `discovery` | Finding engines and agents on the network. |
| `formats` | FFmpeg probing and decoding, CUE sheets, chapters, tracker subsongs. |
| `audio` | Playback: decoding ahead, gapless, ReplayGain gain, PipeWire output, the real-time ring. |
| `output` | Outputs beyond the engine's own speakers: agents, UPnP, audition. |
| `metadata` | Tag reading and writing through TagLib and native writers, documents, staged selections, write plans, artwork, sidecars, transformations. |
| `loudness` | EBU R128 measurement, album grouping, the ReplayGain pipeline. |
| `convert` | Encoder presets and the verified single-file conversion. |
| `operations` | Plan, preview, journal, commit, recover and undo for every file mutation. |
| `persistence` | SQLite: the library index, lists, history, journals; the one schema. |
| `lists` | List ordering and duplicates, M3U8 reading and writing. |
| `query` | The `tkq-1` search language and its evaluation. |
| `titleformat` | The `tkfmt-1` formatting language ([title-formatting.md](title-formatting.md), [ADR-0008](adr/0008-trackknife-format-expression-language.md)). |
| `musicbrainz` | MusicBrainz lookups and matching, AcoustID. |
| `watch` | Filesystem watching, for the engine and melody-watch. |

## Mutation

Every change to a file goes the same way:

```text
draft -> plan -> preview -> (engine) revalidate -> journal -> write
      -> verify -> follow in library and lists -> complete
                                     \-> partial / needs reconciliation
```

Plans are immutable once previewed. The engine refuses a plan whose files
changed since it was made, writes each file whole beside the old one and
swaps it in, keeps what it replaced for undo while a tagger is open
([ADR-0263](adr/0263-undo-what-was-written.md)), and recovers an interrupted
write when it next starts. Destructive work is never done without a complete
preview, cancellation, and a journal.

## Concurrency

- The UI thread receives only small results; it never waits on I/O, the
  network or a database.
- Work runs on bounded pools; no thread per track. FFmpeg's own codec threads
  count against them.
- The audio real-time callback does no blocking I/O, allocation, SQL or UI
  work, and shares no lock with anything that does.
- Every long operation reports progress and can be cancelled across each
  boundary it crosses, including the protocol (`job.cancel`).
- One write at a time holds a file; a stale result -- of a list or query that
  changed meanwhile -- is dropped, not shown.

## Where to read more

- [unified-engine.md](unified-engine.md): the phases of the move to one
  engine and where each stands.
- [melody.md](melody.md): running engines, agents, the phone and the CLI.
- [protocol-levels.md](protocol-levels.md): what each protocol level added.
- [local-library.md](local-library.md), [metadata-and-files.md](metadata-and-files.md),
  [replaygain.md](replaygain.md), [up-next.md](up-next.md),
  [ui-workspace.md](ui-workspace.md): the features in depth.
