# ADR-0010: Separate the live MPD queue from tabbed working lists

- Status: retired by ADR-0224 (Melody no longer speaks MPD); was accepted
- Date: 2026-08-24
- Owners: Trackknife project
- Amended by: ADR-0020 removes `mixed_session_queue`; the remaining queue/list
  kinds and ownership rules stay accepted.

## Context

Excellent queue and playlist tabs are useful for more than listening. They are
work surfaces for collecting files, comparing releases, reordering selections,
and running tag, ReplayGain, and conversion actions. At the same time, MPD has
one current server queue which other clients may edit concurrently. Local files
that the server cannot see cannot honestly be inserted into that queue.

Conflating these concepts would either lose MPD interoperability or make local
items appear remotely playable when they are not.

## Decision

The main workspace hosts multiple `TrackList` tabs with a shared fast track-view
engine. A tab has one explicit kind:

- `mpd_live_queue`: the connected server's authoritative current queue;
- `mpd_stored_playlist`: a server-owned named playlist;
- `scratch_list`: a locally persisted, initially unsaved working list;
- `saved_list`: a named Trackknife-owned working list.

ADR-0020 subsequently rejects a mixed playback-queue kind. Scratch and saved
lists may still contain both reference types as working memory.

The live queue is visually unmistakable and uses MPD song IDs for mutation,
because IDs remain safer than positions when other clients concurrently edit
the queue. Server playlist-version changes drive incremental refresh with a
full snapshot fallback.

Scratch and saved tabs preserve order and duplicates, survive application
restart, and can contain remote MPD references plus local raw paths. They are
first-class action targets: selections or a whole tab can be queued, tagged,
ReplayGain-scanned, converted, sorted with `tkfmt-1`, copied, or exported when
the source capabilities permit it. Closing a dirty scratch tab requires an
explicit save/discard decision.

Tabs support mouse and keyboard creation, switching, reordering, rename, pin,
duplicate, close, and cross-tab drag/copy. They do not each pretend to be a
simultaneously active playback queue. The current playback source and active
queue kind are always visible.

For the MPD foundation, use dynamically linked `libmpdclient` 2.22 or newer
behind a Qt-free adapter. Network and protocol work runs outside the UI thread.
Maintain separate bounded command and idle connections so waiting for MPD
events never blocks user commands. Capability discovery uses `commands` and
`tagtypes`; unsupported optional behavior is hidden or disabled rather than
guessed.

Melody output extensions are parsed without making them part of the standard
MPD model. Known optional fields include online/primary state and preferred
stream format/bitrate; the exclusive `switchoutput` command is used only when
advertised. Unknown response fields remain available for future capability
adapters.

## Alternatives considered

### Treat every tab as an MPD server queue

MPD exposes one current queue per partition, not arbitrary client-side queues,
and cannot represent inaccessible local paths. Stored playlists also have
different semantics from the current queue.

### Hide the MPD queue behind one Trackknife-owned queue

This breaks predictable interaction with other MPD clients and makes server
queue changes difficult to reconcile truthfully.

### Implement the full MPD wire protocol from scratch

Trackknife needs carefully tested product behavior, not ownership of commodity
protocol parsing. `libmpdclient` is maintained by the MPD project, has a stable
documented API and a GPLv3-compatible BSD license, and still permits generic
command/pair handling for advertised extensions.

### Use synchronous MPD calls on the UI thread

DNS, connection, and server response times are unbounded from a UI perspective.
Even a local daemon can stall while updating or under I/O pressure.

## Consequences

- “Queue tab” is a UI family, not one ambiguous persistence model.
- File-work tabs are available before local playback is complete.
- MPD playback and Local audition remain separate; working lists do not pretend
  to coordinate progression between them.
- Two connections and reconnect/backoff logic are required per active MPD
  session, but their concurrency is bounded and observable.
- Tests need a scriptable fake MPD server plus integration coverage against
  stock MPD and Melody.
- `libmpdclient` becomes a build/runtime dependency when the MPD adapter target
  is enabled; its notice is added to the dependency inventory.

## Validation

- Concurrent-client tests mutate the live queue between snapshot and command
  and verify ID-based operations or visible conflicts.
- Disconnect/reconnect tests prove that commands are not duplicated and stale
  responses cannot overwrite a newer session.
- Idle events refresh only the affected status, queue, database, output, or
  stored-playlist state.
- Tab persistence preserves kind, source identity, duplicate occurrences,
  order, selection-independent data, and dirty state.
- Melody tests retain its extra output fields while stock MPD output parsing is
  unchanged.

## Revisit when

- MPD partitions provide a better server-side model for a demonstrated tab use;
- `libmpdclient` prevents required protocol behavior or becomes unmaintained;
- a demonstrated future use requires another explicit playback authority.
