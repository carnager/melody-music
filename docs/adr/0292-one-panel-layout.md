# ADR-0292: One panel layout; side panels as tabs

## Status

Accepted, 2026-10-17. Replaces ADR-0026's composed panel layouts.

## Context

- **ADR-0026 made the window's two panels composable** -- the sources and
  the lists, side by side, one above the other, or as tabs, swapped -- with
  a versioned layout tree, its validation, an editing mode and a reset.
- **Only side by side is any good.** Top and bottom leaves the lists a
  strip; tabbed hides the library the lists are filled from. Nothing else
  was ever composed into the tree: the panels that came since -- Up Next,
  Lyrics, a plugin's (ADR-0291) -- are docks beside it.
- **Those docks stack** one above another when several are open, each a
  sliver of the height.

## Decision

1. **One layout:** the sources left, the lists right, in a splitter whose
   divider is kept where it was left (`workspace/sources-width`, pixels; a
   quarter of the window at first). The editing mode, the arrangements,
   Swap and Reset panel layout go, and with them the layout tree, its
   serialization and its validation (`uicommon/panel_layout`,
   `PanelArrangement`). A kept `workspace/panel-layout-v1` is left as it is
   and no longer read.
2. **Side panels share their room as tabs.** A side panel opened while
   another on its side is open joins it as a tab, in front; closing a tab
   leaves the others as they were. The first opened slides in as before.
   The tabs sit above the panels.

## Consequences

- The window has no layout to break and no settings to describe one.
- Up Next, Lyrics and any number of plugin panels open together each get
  the whole side's height, one at a time.

## Verification

- `bench-main-window`: the splitter holds the sources and the lists, no
  layout actions remain, a divider moved is where it was in the next
  window; Up Next and Lyrics open together are two tabs, the last opened
  in front, and the one left keeps its width when the other closes.

## Amendment, 2026-11-09: the tabs keep their order and the one in front

- **Their order.** The side panels' tabs can be dragged (Qt's dock tab
  bars always could), but were tabbed again at every start in the order the
  panels were made -- Lyrics, then each plugin's -- so an order dragged to
  did not outlive the window. The order dragged to is kept
  (`panels/order`, the panels' object names; one not in it goes last) and
  the tabs are put in it whenever they are tabbed together: when the window
  opens, when Panels brings them back, when the plugins' are made again.
- **The one in front.** Which tab is in front is kept (`panels/front`)
  and brought back then, rather than whichever was tabbed last. Settings ›
  General › Panel in front chooses one to be in front whenever the window
  opens instead (`panels/start`): Lyrics or an enabled plugin's panel.
- `bench-main-window`: an order dragged to and the tab in front are as
  they were in the next window, after Panels hides and shows them, and
  after the plugins' panels are made again; the panel Settings chose is in
  front in the next window; Settings offers Lyrics and the enabled
  plugins' panels.
