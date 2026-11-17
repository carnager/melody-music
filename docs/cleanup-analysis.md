# Cleanup analysis, 2026-11-17

What in Melody's ~270k lines of C++ (75k of them tests) has no meaning any
more, and what could be simpler. Read-only survey on branch `cleanup`; every
item names where to look. Line counts are estimates.

At the file level the tree is clean: every source file is built, every header
included (`tests/qtry_narrowing.hpp` is force-included by
`tests/CMakeLists.txt:35` for Qt 6.8). What has lost its meaning is inside
files: compatibility with states no one is in any more, presentation API left
from the Qt Quick window, and helpers written several times.

## Tier 1 -- compatibility nobody needs (nothing is released)

| What | Where | Lines |
| --- | --- | --- |
| 57-step schema migration chain; squash into one v57 `CREATE` and refuse older | `persistence/src/list_repository.cpp:323-1665` | ~1,340 |
| SQL migration files read only by tests and a downgrade doc | `src/persistence/migrations/` (110 files), its tests, `docs/melody.md:270-285` | ~1,800 + tests |
| Backfills for old schemas | `local_library.cpp:1229-1325`, `:3244`; `list_repository.cpp:2343` | ~110 |
| Client-era list/profile/view-preset storage, reached only by one-time adoption into TabStore | `ListRepository` `load_all`/`replace_all`/`save_workspace` (2207-2784), `refresh_local_metadata` (2784-3179), profiles (3769-3875), view presets (3875-3956), `adopt_presets` (6548-6616), `Workspace::initialize` legacy load (`workspace.cpp:88-120`) | ~1,250 |
| MPD remnants in storage | `ListKind::mpd`, `ListSource::mpd`, `connection_profiles`, `list_repository.cpp:1705`, `workspace_lists.cpp:584` | ~80 |
| Retired-lists feature | `list.retired*` (`list_methods.cpp:787-840`), `load/drop_retired_*`, `workspace_connections.cpp:896-970`, `retired_engine_list*` tables | ~150 |
| One-time adopters | Trackknife state folder (`engine_state.cpp:84-160`, `daemon/main.cpp:456-480`), `dynamic.adopt` + `copied_rule`, `adoptDynamicPlaylists`, `adoptTrackknifeNames` + `trackknife_names_test` | ~300 |
| Older protocol levels / clients | engine: `file_work_wire_plan.cpp:305`, `metadata_services.cpp:227`, mDNS `"auth"`, `daemon/settings.cpp:98-110` renamed keys, `cli/main.cpp:1392`, `list_repository.cpp:1447` `remote` column; client: `engine_playback.cpp:953-962`, `workspace_lists.cpp:246-262`, `workspace_playback.cpp:314`, `workspace_connections.cpp:514`, `workspace_up_next.cpp:312`, `remote_engines.cpp:35,112`, `engine_folder_session.cpp:159`, `bench_lyrics.cpp:146` | ~150 |

**Must survive any squash** -- not rebuildable from the files:
`local_ratings`, `local_listening_*`, `engine_lists`/`engine_list_items`,
`saved_searches`, `engine_state`, `local_library_roots`/`_mounts`,
`local_library_tracks.added`, `operation_journal*`,
`file_publication_backups`, and all of `presets.sqlite`. Before squashing,
check both machines are at 57 (`SELECT version FROM schema_version`), and
that TabStore has adopted their lists and view presets.

Tier 1 total: **~5,000 lines**, mostly in one file.

## Tier 2 -- dead code (verified by hand)

| Area | Items | Lines |
| --- | --- | --- |
| uicommon | `ListPersistenceService::saveWorkspace`, `saveProfiles`, `recordLocalListen` (+ `pending_listens_`, the never-emitted `listeningHistoryChanged` and its connect at `bench_dynamic_playlists.cpp:242`), `saveWorkspaceAndWait`, `refreshLocalMetadataAndWait`, `relocateLocalSourceAndWait`; `ui::trackRowLines`; `QueueTableView::dropInsertionRow` | ~230 |
| bench | `setTrackViewPresentation`, `LocalListModel::appendPaths`/`isMissing`, `MetadataFieldReviewBar::setFilesToggleVisible`/`filesToggleChecked`, `createMusicBrainzTrackMatchView`, `fileListConstructed` signal, `database_path_` alias, stale `playback_row_` comment | ~60 |
| workspace | `Workspace::dropOnNewList`, `dropOnEngineList`, `enqueueRows`, `setColumnVisible`; `DynamicPlaylistSession::invalidateAuthority` + `authority_valid_` (always true) and the `authorityValid()` branches in bench; LibraryBrowser `addToList`/`addToNewList`/`newListName`/`engineText`/`setNewestFirst`/`roots()`; 33 Q_PROPERTY/Q_INVOKABLE in LibraryBrowser and 10 in ListEditJob (no metaobject readers -- keep the signals bench re-emits); Tagger `canRetry` (never defined)/`itemCount`, Artwork `pickerOpen`/`frontImage`/`frontMixed`, Identify `moveFile`/`matchedRow`/`fileCount`/`pairedCount`, Convert `gainWarningRich`/`canAddPreset`, Profiles `destinationNames`/`placeKey`, ListEdit `shuffleAlbums`, EngineFolder `showsHidden` | ~310 |
| audio | Source relocation in `LocalAuditionService` -- the engine's `Player::relocate` does it now (`local_audition.cpp:242-295, 889-935, 1715-1760`, structs in `.hpp:115-135`, ~18 copies of the relocation boilerplate); `RequestQueue::abandon`; `should_write_resume` | ~330 + tests |
| musicbrainz | `MusicBrainzClient` (the engine paces and caches itself, `metadata_services.cpp:229-303`); `album_lookup` (`judge_album`) -- test-only | ~420 + tests |
| metadata / operations | `export_metadata_rule_script` (import-only by design), `project_musicbrainz`, `preparation_plan_issue_kind_name`, `common_source_directory_raw_path` | ~150 |
| engine / output | `Player::adopt_queue_list`, `decode_publication_commit`, `decode_saved_layout`, `AgentAudition::on_changed` | ~60 |

Tier 2 total: **~1,550 lines**, plus the tests of what goes.

Not dead, though a scan says so: Qt overrides (`renderItem`, `tabSizeHint`,
`createWidget`, `supportedDropActions`), `openLinkUrl` (called by name from
`main.cpp:229`), `expire_consume`, `set_tick`, `acceptsArity`,
`optional_flag`, `forgetUndo`, statics called as `Class::x()`.

## Tier 3 -- written more than once

- **ReplayGain:** the desktop drives `loudness.scan` then `metadata.apply`
  step by step (`bench/replaygain_scan.cpp:38`); the CLI uses the engine's
  `replaygain.apply` (`job_methods.cpp:568-650`). One path, with a preview
  mode for the desktop.
- **Client-side file access still possible:** `FileWorkTools{}` defaults to
  this process (`file_work_tools.hpp:38-50`, defaults in
  `replaygain_dialog.hpp:29`, `replaygain_job.hpp:33`,
  `metadata_properties_dialog.hpp:83`); product code always passes the
  engine's. Make the engine's mandatory.
- **Local tabs probe and read tags in the client** (`workspace_ingest.cpp:525-583`,
  `800+`, `playlist_transfer.cpp:144`) while remote tabs ask the engine --
  the local/remote branch AGENTS.md is removing; ~25 `isLocal()` sites.
- **Encoder probing in the client** (`convert_job.cpp:203`) beside the
  engine's (`convert_methods.cpp:97`), which is the right one for a remote engine.
- **Two cover loaders with their own caches:** Workspace
  (`workspace_ingest.cpp:645-785`) and `LibraryBrowser::wantCovers`
  (`library_browser.cpp:1738`).
- **UI helpers:** 12 colour-mix helpers (`TrackknifeStyle::mix` should be the
  one), 5 two-line "title + muted detail" delegates, the QueueTableView setup
  copied 4 times, `smaller(QFont)` twice. ~200 lines.
- **Tests:** `check()` copied into 43 files, `require()` into 32,
  `write_file`/`write_silent_wave`/`write_bytes` 3-4 times each;
  `engine_lastfm_test.cpp:45` has an old copy of `recording_audition.hpp`.

## Tier 4 -- structure (no lines saved, easier to work in)

- `Workspace` is one class of 1,069 header lines and 8,176 in 15 files, all
  public; 61 members are used only by itself. `BenchMainWindow` re-exposes it
  through 54 reference aliases and 30 one-line forwarders (~120 lines).
- `src/workspace` exists apart from `src/bench` for a second window that no
  longer exists; it includes `bench/*` in 54 files and builds 55 of its
  sources. Merging the two, or making the line real, is a decision.
- Small one-widget sessions (QuickPick, EngineFolder, OpenList, LibraryWatch,
  UndoLocation, PlaylistFolder) could live in their widgets.
- Long functions: `SettingsDialog` constructor 945 lines, `buildWorkspace`
  496, `LocalAuditionService::Impl` 1,163, titleformat `Evaluator` 929;
  `metadata_commit.cpp` 4,099 and `file_publication.cpp` 3,329 lines, the
  latter with parallel same-/cross-filesystem commit and recovery functions.
- ~~`bench_main_window_test` is one class of 262 cases split by size, not by
  feature, over 8 files (21k lines).~~ Done 2026-11-17: a program for each
  feature, `tests/window_*_test.cpp`, sharing `window_test.hpp`.

## Tier 5 -- the direction's own work (AGENTS.md)

Not cleanup, but the largest simplification there is: the UI still holds
storage. `ListPersistenceService` opens `presets.sqlite` through the full
engine schema (~450 lines after Tier 2); `search_session.cpp:236` opens the
engine's database for saved searches through `CatalogueSource::database()`;
`engine_list_sync.cpp` (1,064 lines) diffs lists in the client. Each moves
behind the engine as its own phase.

## Documentation

- `docs/README.md`'s ADR index stops at 0294 of 0353; generate it.
- ADR-0220 still says Proposed while AGENTS.md calls it accepted.
- Superseded ADRs are not marked: 5 say so, ~18 newer ones supersede ADRs
  that still read Accepted. Two status formats are in use. Proposal: one
  `Status:` line with `Superseded-by:`, retired ADRs to `docs/adr/retired/`
  under their numbers, index generated -- no history deleted.
- Stale: `CODEX_TASKS.md` (a Trackbench task list from September),
  `MILESTONES.md`, `docs/architecture.md`, `docs/mpd-client.md`,
  `m3-`/`m10-validation.md`; MPD and authority passages in
  `unified-engine.md`, `ui-workspace.md`, `open-decisions.md`,
  `compatibility.md`; AGENTS.md's reading order and its "Go daemon" line.
- Stale comments: `workspace/CMakeLists.txt:6` (two windows),
  `library_tree_model.hpp:77` (QML's tree adapter -- the one-column limit can
  go), `daemon/CMakeLists.txt:35`, `daemon/main.cpp:5-7`,
  `protocol_message_test.cpp:4` (Go implementation).

## Suggested order

1. Tier 2 dead code and stale comments -- mechanical, behaviour unchanged.
2. Tier 1 on the engine: squash migrations, drop adopters, retired lists,
   old-level fallbacks -- after checking both machines' schema and TabStore.
3. Tier 3 helpers and test helpers.
4. Docs: index, statuses, archive.
5. Tier 3 duplicated paths and Tier 4/5 -- each a decision or an ADR of its own.

Tiers 1 and 2 together: roughly **6,500 lines** of product code, and a
similar amount of tests and SQL with them.
