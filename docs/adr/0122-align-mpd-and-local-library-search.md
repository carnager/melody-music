# ADR-0122: Align MPD and local library search

- Status: retired by ADR-0224 (Melody no longer speaks MPD); was accepted
- Date: 2026-09-06
- Owners: Trackknife project

## Decision

**Trackknife decision:** the user requests matching appearance and behavior,
not just the placement introduced in ADR-0120. MPD search uses the same
`ServerLibraryTreeView` and delegate as local search: Albums and Tracks roots,
expandable releases, numbered tracks, artist subtitles, cover thumbnails,
extended selection, hover/focus queue actions, and a matching context menu.
The search field, tools, and content share local library margins and spacing.

Both fields use a 200 ms debounce and accept any nonempty query. Return in the
field has normal text-field behavior; tree Enter/click expands branches, while
track activation appends the selection. Left/Right navigate the tree rather
than a separate row-action column. Clearing the field restores browsing.
Ctrl+L focuses the active authority's library search without switching queue tabs.
This supersedes the flat result presentation and special search keyboard
bindings retained by ADR-0120. MPD pagination becomes a Show more… tree item.

The existing bounded MPD search and serial cover loader remain. A tree model
projects their results; complete release expansion uses exact asynchronous
album lookups through the controller. Expansion and selection share pending
lookups. Multi-selection resolves in tree order, removes overlapping URIs,
and submits one queue action only after all selected entries resolve. Query
changes invalidate pending selections; late replies cannot repopulate them.
The request queue is bounded to 64 albums, selected entries to 1,000, and an
MPD queue action to 4,096 tracks. Dragging search results into the MPD queue
uses the same complete selection resolution and the requested insertion row.

Local files and MPD URIs retain separate authority. Local filesystem actions
remain local. Search never scans local folders. No dependency or schema change.

## Verification

Workspace tests check the shared tree/delegate, layout and interaction settings,
numbered titles, square cover rendering, branch keyboard navigation, multiple
selection, one-character projection, pagination, authority retention, query
clearing, and stale response rejection. Model tests resolve complete albums
from partial search matches, coalesce expansion requests, deduplicate selections,
and reject late expansion replies after a query change.
