# ADR-0009: Make the MPD client the primary product

- Status: retired by ADR-0224 (Melody no longer speaks MPD); was accepted
- Date: 2026-08-24
- Owners: Trackknife project
- Amended by: ADR-0020 removes the proposed mixed-session playback coordinator
  and makes local preparation/import the local-file product boundary.

## Context

The original plan treated local-library indexing, playback, and collection
tools as equal parts of the first useful release. That sequence delays the part
the project most urgently needs: a modern, fast, native Qt 6 client for MPD and
Melody. Cantata is no longer maintained, while current Linux MPD users still
need a polished mouse-and-keyboard client with serious queue management.

Melody already presents its library, queue, transport, and playback endpoints
through the MPD protocol. Its outputs are streaming playback endpoints rather
than only hardware devices, but this distinction is exposed as compatible MPD
output records plus optional extension fields and commands. Trackknife does not
need a Melody-specific client architecture to serve it well.

Local files remain important for playback, metadata work, ReplayGain, and
conversion, but they do not need to be imported into a second Trackknife-owned
library tree. An optional local music root can map an MPD URI to the same file
on disk when the client and server share the collection.

## Decision

Trackknife is, in priority order:

1. a polished Qt 6 MPD client, compatible with ordinary MPD first and aware of
   Melody capabilities when advertised;
2. a fast local-file metadata editor and universal parallel ReplayGain tool;
3. a multithreaded FFmpeg-based converter;
4. eventually, a Melody streaming playback endpoint.

The first implementation focus is MPD connection/session infrastructure,
server-library browsing and search, live queue control, stored playlists,
outputs, transport, and a tabbed queue/list workspace. Local playback through
FFmpeg and PipeWire follows without creating a second library index.

Each MPD connection profile may contain an optional local music root. A safe,
contained URI-to-path mapping allows local playback and file operations when a
server item is available on the same filesystem. Failure to map is ordinary:
the item remains fully usable through MPD.

MusicBrainz-aware metadata remains part of the domain model and default sorting
and grouping. Trackknife preserves MusicBrainz identifiers and sort-credit
fields received from MPD or local files and uses them when useful. No network
lookup service is required for the initial MPD client.

The existing `tkfmt-1`, metadata, job, operation-plan, FFmpeg, TagLib,
libebur128, SQLite, and PipeWire decisions remain supporting infrastructure.
They no longer determine the order in which the visible product is delivered.

## Alternatives considered

### Build a complete local library/player before the MPD client

This duplicates a service MPD or Melody already provides and postpones the
highest-value user experience.

### Make Trackknife Melody-specific immediately

Melody intentionally speaks MPD. Starting from the standard protocol produces
a useful client for both ecosystems and leaves a small capability-driven seam
for Melody additions.

### Import local files into a Trackknife library tree

That creates two competing library authorities and makes initial setup and
reconciliation more complex. Local files instead enter through working lists,
file/folder opening, drag-and-drop, and local tools.

## Consequences

- The MPD database is authoritative for the primary library tree.
- Trackknife can be useful without scanning or owning the user's collection.
- The optional music-root mapping is a convenience, never an availability
  requirement for remote MPD entries.
- Local-only files can participate in Trackknife working lists and Local-domain
  audition without appearing in the MPD library.
- MPD transport and local FFmpeg/PipeWire audition remain separate ownership
  domains; ADR-0020 rejects a combined queue and automatic handoff.
- Metadata editing, ReplayGain, and conversion operate on resolved local
  sources, including ad-hoc files and safely mapped MPD items.
- The milestones and first-release definition change substantially; local
  indexing, autoplaylists, and broad plugin work are no longer prerequisites
  for the first useful client.

## Validation

- Connect to both stock MPD and `../melody`, negotiate commands/tag types, and
  browse, search, control transport, edit the queue, and manage outputs.
- Keep the UI responsive while the server is slow, disconnected, updating its
  database, or being changed by another client.
- Resolve mapped MPD URIs only below the configured music root and show local
  availability without affecting remote playback.
- Open and audition local files without adding them to the server library or
  mutating MPD transport implicitly.

## Revisit when

- users demonstrably need a Trackknife-owned local index independent of MPD;
- MPD cannot expose metadata needed for acceptable primary browsing;
- Melody stops presenting client behavior through a compatible MPD boundary.
