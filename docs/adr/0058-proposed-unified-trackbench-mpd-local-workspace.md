# ADR-0058: Unified Trackbench MPD and local workspace

- Status: retired: its MPD side by ADR-0224, its two-sided workspace by ADR-0220; was accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Supersedes: the process-boundary parts of ADR-0025

## Context

ADR-0025 split the MPD client and local-file workstation into separate
applications to remove an ambiguous mixed-domain transport and keep MPD state
away from destructive local-file operations. Experience with the resulting
workspaces suggests that this separation also costs useful continuity: users
must switch applications even though queue/list presentation, layout editing,
and much of the surrounding workspace language are deliberately shared.

The accepted direction is not a return to a mixed queue or an implicit playback
handoff. MPD and local files retain different authorities, capabilities, and
failure domains. The question is whether tabs and an explicit active context can
make those boundaries clear inside one Trackbench process without recreating
the permanent domain controls rejected by ADR-0025.

## Decision

- Trackbench becomes the single workspace for both authorities. It has an
  **MPD Queue** tab and a **Local Queue** tab; MPD occurrences and local-file
  rows never coexist in one queue document.
- The active authority follows the selected primary tab:
  - With **MPD Queue** active, transport controls operate on MPD, the output
    selector presents MPD outputs, and the left **Folders** surface becomes the
    MPD server **Library**.
  - With **Local Queue** active, transport controls operate on Trackbench's
    local playback engine, the output selector presents PipeWire devices, and
    the left surface presents local folders.
- Local-only commands are unavailable in MPD context. In particular, tagging,
  ReplayGain analysis, rename/move, conversion, and other filesystem mutations
  cannot target MPD queue occurrences merely because they are visible in
  Trackbench.
- The MPD queue uses the **exact complete declarative track-view system used by
  local files**: the same versioned layout document and validation, layout
  editor and presets, grouping, album headers and artwork, column order,
  visibility and widths, compact modes, and `tkfmt-1` column/group expressions.
  It is not a visually similar second implementation and has no MPD-specific
  capability subset. MPD and local tabs may bind different saved layout
  selections, but both selections are interpreted and rendered by the same
  code path. Acceptance requires migrating the MPD queue away from its legacy
  Qt-header-only persistence.
- Playback ownership remains explicit. Tab selection changes which already
  distinct controller the shared chrome addresses; it does not copy queues,
  transfer playback, or infer local paths from MPD URIs.
- MPD-only chrome follows that same context boundary. Its right-aligned search
  field occupies the Track Lists tab strip and opens a transient result surface
  without replacing the active queue; Repeat, Random, Single, Consume, and
  advertised ReplayGain controls occupy the status bar only while MPD is the
  active authority. Search results use fixed one-line rows, bounded square
  covers, and visible keyboard-action focus, and the surface closes when focus
  leaves it. Server-library append/add-next/replace actions and exact-position
  drops target the live MPD queue. The live-queue context menu exposes the
  applicable selection commands and advertised priority levels.
- A persistent local library remains deferred. The local sidebar continues to
  expose filesystem sources and saved working lists until a separate product
  decision establishes that an index is necessary.

The merge starts with the permanent MPD Queue inside Trackbench and retains the
standalone `trackknife` executable as a compatibility shell while equivalent
surfaces move into the unified workspace. Removing that executable is a later
packaging decision, not a prerequisite for sharing the runtime workspace now.

## Alternatives considered

### Keep Trackknife and Trackbench as separate applications

This was the ADR-0025 design. It gives each playback authority an unambiguous
process and failure boundary, at the cost of
switching applications and maintaining two top-level workspace shells.

### Add MPD commands to Trackbench without an MPD queue tab

Rejected. Hidden or menu-only MPD control would make the
active playback authority harder to see and would not provide the integrated
server library and queue workflow being evaluated.

### Share only the visual styling of the two queue views

Rejected. Similar-looking parallel implementations would drift. The decision
requires one layout schema, editor, renderer, persistence contract, and test
corpus for MPD and local track views.

## Consequences

- ADR-0025's two-process product boundary and the rule that Trackbench speaks
  no MPD protocol are superseded. Its separation of MPD and local queue
  authorities, and all local-operation safety rules, remain.
- The active tab becomes a consequential context switch. Focus restoration,
  keyboard shortcuts, transport state, sidebar selection, async result
  delivery, and output controls need stale-context tests.
- The standalone Trackknife executable, package/data migration, connection
  profile ownership, command-line entry points, and compatibility-launcher
  story require an explicit migration plan before removal.
- The shared track-view engine becomes a hard prerequisite for the prototype;
  MPD layout parity cannot be deferred as later polish.

## Validation

- Trackbench switches between MPD Queue and Local Queue without a
  domain chip and always makes the active authority apparent from the queue,
  sidebar, transport state, and output selector together.
- The same saved track-view layout can be applied to both queues with identical
  supported controls and rendering semantics; automated tests prove they use
  the same layout serializer, validator, and projection path.
- MPD context exposes no local tagging or filesystem mutation commands, even
  for MPD entries whose URI resembles a local path.
- Switching tabs while MPD, local playback, library loading, artwork loading,
  or an operation preview is active cannot route a late result to the wrong
  authority.
- Search remains inside a resized Track Lists panel; album covers retain their
  aspect ratio, keyboard action focus is visible, and leaving the surface
  dismisses it. The library disclosure arrow toggles once, and library-to-queue
  drops use the same insertion row that the queue paints.
- Existing live MPD compatibility and local playback/tagging safety suites stay
  green in the combined process.

## Open questions

- How long the compatibility `trackknife` executable remains and whether it
  eventually becomes a launcher for Trackbench's MPD Queue.
- Whether future committed search and stored-playlist tabs also select MPD
  context, or only the permanent MPD Queue tab and its transient search do.
- How connection-offline startup and restoration choose the initially active
  primary tab without surprising local-only users.
- Whether packaging can offer an optional MPD feature while preserving one
  coherent persisted workspace schema.
