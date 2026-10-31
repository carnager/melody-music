// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

namespace trackknife::bench {

// Before anything opens the settings or the workspace database, as either
// window starts.
//
// ADR-0287: the application is Melody. Its settings, its data and its cache
// move from Trackknife's names, once.
void adoptTrackknifeNames();
// A window in Qt's test mode -- every test, a --screenshot run -- keeps its
// engine's state in its own test folder when none is named: the engine's
// folder is found from the environment, not from Qt, and a test that forgot
// to name one once moved a user's library out from under their engine.
// Melody's presets (ADR-0339) are kept there too.
void isolateTestEngineState();
// File › Back up workspace… writes three parts beside one another: Melody's
// own database (presets.sqlite -- presets, layouts, view presets) at the
// chosen path, its settings in `path`.settings.ini and the open tabs' rows
// in `path`.tabs. Never the engine's database: that is the engine's.
struct WorkspaceRestore {
    // The backup's database path; the other parts are found beside it.
    QString from;
    bool presets{false};
    // Every setting but the open tabs.
    bool settings{false};
    // Which tabs are open, and their rows.
    bool tabs{false};
};
// What a backup at `from` holds: each part there, true.
[[nodiscard]] WorkspaceRestore workspaceBackupParts(const QString& from);
// Kept for the next start: the running window still holds what it replaces.
void scheduleWorkspaceRestore(const WorkspaceRestore& restore);
// A restore chosen last time is applied now, what it replaces kept beside
// it. What happened, to tell the user; empty when nothing was pending.
[[nodiscard]] QString applyPendingWorkspaceRestore();

} // namespace trackknife::bench
