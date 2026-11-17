// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <cstddef>
#include <filesystem>

namespace trackknife::core {

// ADR-0287: where an engine keeps its state -- its database, id, keys,
// staging and undo copies -- when no folder is named: melodyd's default,
// and the folder the window opens the same database in.
//
// `MELODY_STATE_DIR` when set; else `$XDG_DATA_HOME/melody/engine`, on
// macOS `~/Library/Application Support/melody/engine`, else
// `~/.local/share/melody/engine`.
[[nodiscard]] std::filesystem::path default_engine_state_directory();

// ADR-0339: Melody's own data -- its presets: naming layouts, destinations,
// encoder presets, tagging scripts -- which the window and melody-cli share
// on this computer and no engine keeps. `MELODY_DATA_DIR` when set; else
// Qt's AppDataLocation for Melody: `$XDG_DATA_HOME/melody/melody`, on macOS
// `~/Library/Application Support/melody/melody`, else
// `~/.local/share/melody/melody`.
[[nodiscard]] std::filesystem::path melody_data_directory();

// The presets' database in it.
[[nodiscard]] std::filesystem::path presets_database_path();

// The folder in use: the default.
[[nodiscard]] std::filesystem::path engine_state_directory();

} // namespace trackknife::core
