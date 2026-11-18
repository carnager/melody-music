# ADR-0129: MPD stored-playlist browse, open, and edit in the workspace

- Status: retired by ADR-0224 (Melody no longer speaks MPD); was accepted
- Date: 2026-09-08
- Owners: Trackknife project
- Extends: ADR-0058 unified workspace, ADR-0071 retirement scope,
  ADR-0120/0122 MPD library panel, ADR-0125 find bar

## Context

The protocol layer (`mpd::Client`/`mpd::Session`) and the
`MpdProbeController` have carried a complete stored-playlist API since the
retired shell, but nothing in the Trackbench widget layer calls it: the
feature matrix records "Backend only", and `docs/ui-workspace.md` already
specifies the intended behavior — opening a server playlist creates or
refreshes a separately closable tab keyed by its server name, with compact
add/remove/reorder controls and queue-load/clear/rename/delete in its list
menu, gated on advertised commands, where accepted edits reload
authoritative server contents. `IdleEvent::stored_playlist` was silently
dropped by the controller, so other clients' playlist changes never reached
the UI.

## Decision

- **Sidebar browse.** The MPD library panel gains a bounded "Playlists"
  list below the browse/search stack (`bench-mpd-playlists`), populated
  from `listplaylists` on connect, after playlist mutations, and on
  `stored_playlist` idle events (debounced). It hides when empty or
  disconnected. Activation opens the playlist tab; its context menu offers
  Open, Load into queue, Rename…, Clear…, Delete…, and Save queue as
  playlist…, each enabled only while the server advertises the matching
  command.
- **Playlist tabs.** Opening a server playlist creates or refreshes one
  closable tab keyed by its server name, backed by a Bench-owned
  `MpdQueueModel` filled from the controller's authoritative
  `listplaylistinfo` re-read. Tabs are MPD-authority surfaces: the active
  playlist tab binds transport, sidebar, and status controls to MPD exactly
  like the live queue. They are session-only and are not persisted across
  restart. Rename retargets the open tab; deletion closes it.
- **Edits are server round trips.** Row removal issues the batched
  `playlistdelete`; a single-row drag issues `playlistmove`; drops from the
  server library, MPD search, the live queue, or another playlist tab issue
  a batched `playlistadd` (with a `TO` position when the drop targets a
  row). No edit is applied optimistically — the visible rows change only
  when the post-mutation `listplaylistinfo` re-read arrives. Multi-row
  reorder is rejected with a status message in this slice.
- **Queue interchange.** Enter/double-click and the append/insert-next
  context actions add the selected playlist rows to the live queue; Load
  into queue uses `load`. The live queue context menu gains "Save queue as
  playlist…" (`save`) and drops from a playlist tab into the queue append
  at the drop position.
- **Session batch add.** `mpd::Client`/`mpd::Session` gain a batched
  `playlistadd` (command list, 1–4096 URIs, optional incrementing `TO`
  positions) mirroring the existing batched `playlistdelete`, so one user
  gesture is one serialized server transaction.
- **Idle propagation.** The controller now forwards
  `IdleEvent::stored_playlist` as a `storedPlaylistsChanged` signal; the
  workspace refreshes the sidebar list and re-reads every open playlist
  tab. The controller's inverted `browser_showing_playlist_` flag is fixed
  to report an open playlist truthfully.
- **Destructive gates.** Delete and Clear ask one confirmation naming the
  playlist — they mutate the server with no undo; every other action
  follows the direct-apply contract (ADR-0083). Failures surface through
  the existing notification path and leave the last authoritative rows
  visible.
- **Find.** The ADR-0125 find bar treats playlist tabs like the MPD queue
  (cached text and exact URIs, no server commands).

## Consequences

- The complete browse/open/edit/save workflow specified in
  `docs/ui-workspace.md` is reachable in the primary workspace, and playlist
  changes from other clients appear without manual refreshes.
- Playlist tab presentation reuses the shared MPD track-view layout;
  per-playlist layout persistence, multi-row reorder, restoring open
  playlist tabs across restart, and committed search-result tabs remain
  follow-up work.
- Tests: the fake-server client/session suites cover the batched
  `playlistadd`; the Bench widget suite proves sidebar population, tab
  keying/refresh/close, rename/delete propagation, capability-gated menus,
  and that playlist edits submit server commands instead of mutating the
  model.
