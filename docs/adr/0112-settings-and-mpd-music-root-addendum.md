# ADR-0112 addendum: scoped MPD database updates from the library

- Status: retired by ADR-0224 (Melody no longer speaks MPD); was accepted
- Date: 2026-09-03
- Extends: ADR-0112 settings and music-root bridge, ADR-0058 explicit MPD
  authority

## Decision

"Update this folder in MPD" joins the server-library context menu. The
library is tag-organized while MPD's `update` is path-scoped, so the
action derives its path honestly from the node's own files: the common
directory prefix of the node's track URIs (through the same lazy-fetch
machinery as the other node actions). Nodes whose files share no common
folder fall back to a whole-database update, and the status bar states
exactly which path was requested either way. The command travels the
full stack as a first-class session mutation
(`Client::update_database` → `SessionCommandKind::database_update` →
`MpdProbeController::updateDatabase`), and the library reloads through
the existing database idle event when MPD finishes.

This completes the explicit-update half of ADR-0058 for the tagging
workflow: load MPD tracks as local files, retag them, then ask MPD to
rescan exactly the folder you touched.
