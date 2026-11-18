# ADR-0020: Separate MPD control from local preparation

- Status: retired by ADR-0224 (Melody no longer speaks MPD); was accepted
- Date: 2026-08-27
- Owners: Trackknife project
- Supersedes: the mixed-session playback portions of ADR-0009 and ADR-0010

## Context

ADR-0009 and ADR-0010 reserved a future Trackknife-owned mixed session queue
that would hand playback between MPD and a local FFmpeg/PipeWire engine. That
model does not fit the actual ownership boundary well. MPD remains a shared
server controlled by multiple clients, while Melody may expose multiple
server-managed playback agents. A client-local queue cannot be merged honestly
with that server state, and automatic backend handoff would make transport and
queue ownership difficult to explain.

The primary local-file job is also clearer now: take new music, identify and
edit its metadata, manage artwork, analyze ReplayGain, organize or transcode it,
and import the prepared result into an MPD-owned library. Local playback is
useful for auditioning those files, but it is not a reason to invent a combined
MPD/local playback order.

File mutations require a real local file. A remote MPD reference may become
eligible only after its configured music-root mapping has resolved and been
revalidated. Future network metadata providers and a possible Melody remote
import extension could improve this workflow, but neither capability exists as
a dependency today.

## Decision

- The default Library pane has two explicit domains: **MPD** and **Local**.
  The MPD domain contains the server library and stored-playlist entry points.
  The Local domain grows from direct folder navigation into the preparation
  surface for local files. Existing live-queue, server-playlist, search, scratch,
  and named-list document tabs remain in the main track workspace.
- MPD owns its library membership, live queue, stored playlists, transport, and
  advertised outputs or Melody agents. Trackknife continues to reconcile that
  state as one of potentially several connected clients.
- Trackknife will not add a `mixed_session_queue` or automatically hand
  progression between MPD and local playback. Local FFmpeg/PipeWire playback is
  scoped auditioning of local preparation material with visibly local controls;
  it never rewrites the MPD queue or presents one combined transport timeline.
- Scratch and named Trackknife lists may still retain both remote references and
  local raw paths as working memory. Playback and mutation capabilities are
  resolved per item. Invoking a file operation on a safely mapped MPD item opens
  or resolves its local counterpart in the Local domain; remote-only items
  remain inspectable but cannot be mutated.
- Tagging, artwork writes, ReplayGain analysis/writes, rename/move/copy, and
  conversion are Local-domain operations. They use the shared immutable
  plan/preview/revalidate/journal/verify lifecycle.
- Local import first targets a configured, accessible MPD `music_root`. An
  import plan chooses copy, move, or converted output; generates contained
  relative destinations; previews conflicts; stages and verifies results;
  publishes them; requests an explicit MPD database update; and may then add
  returned or derived MPD URIs to a stored playlist or live queue. Deleting an
  original for move semantics occurs only after verified publication.
- Metadata lookup is represented by a provider boundary that returns proposed
  values, identifiers, artwork references, provenance, and confidence to the
  normal metadata preview. A MusicBrainz provider is a likely first user, but
  online lookup and a public plugin ABI remain deferred until built-in metadata
  workflows prove the boundary. Providers do not write files directly.
- A possible future Melody upload/import extension is an optional import
  destination, not a current protocol claim or milestone dependency. The
  initial workflow must be complete using an accessible `music_root`. If Melody
  later advertises a transactional upload capability, it can reuse the same
  import plan through a capability-gated destination adapter.

## Alternatives considered

### Keep the mixed MPD/local session queue

This makes one client appear to own progression across a shared server queue
and a private local queue. Multiple MPD clients and Melody playback agents make
the handoff semantics misleading and unnecessarily fragile.

### Put all local preparation in external applications

This would force users to move between a tagger, loudness scanner, converter,
file manager, and MPD client. Trackknife's product purpose is to provide those
preparation actions coherently with one preview and safety model.

### Require a new Melody upload protocol for import

No such extension currently exists, and stock MPD must remain useful. Making it
a prerequisite would block the local workflow on an optional future server
feature.

## Consequences

- M4 becomes local audition plus the source/capability foundation for a Local
  preparation workspace; it no longer contains a mixed queue or backend-neutral
  progression coordinator.
- M5, M6, and M7 form one preparation pipeline: metadata and files, ReplayGain,
  then conversion and verified MPD import.
- The existing FFmpeg decode, bounded playback core, and PipeWire adapter remain
  valuable for auditioning, analysis, conversion, and future endpoint work.
- The UI must keep MPD and Local ownership obvious even when the same physical
  file is reachable through a safe music-root mapping.
- A future remote import transport changes only the destination adapter, not the
  preparation, preview, verification, or post-import workflow.

## Validation

- MPD queue and transport behavior remains correct when another client changes
  server state and when Melody exposes multiple outputs or agents.
- Local audition never inserts an item into MPD or changes server transport as
  an implicit side effect.
- File actions reject remote-only items and revalidate every mapped local path.
- A filesystem import fixture proves contained destination planning, conflict
  preview, staging, verification, publication, MPD-update request, and safe
  move semantics under injected failures.
- Metadata-provider proposals cannot bypass the staged metadata preview or
  source-revision checks.

## Revisit when

- an implemented and documented Melody import capability can be validated;
- users demonstrate a need for a separate persistent local playback queue rather
  than scoped auditioning;
- a real metadata provider proves a stable public plugin boundary.
