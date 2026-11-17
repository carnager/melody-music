// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/engine_state.hpp"

#include "trackknife/core/environment.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace trackknife::core {
namespace {

// The user's data folder, as melodyd has always resolved it.
[[nodiscard]] std::filesystem::path data_home() {
    if (const auto* data = environment_variable("XDG_DATA_HOME"); data != nullptr && *data) {
        return data;
    }
    if (const auto* home = environment_variable("HOME"); home != nullptr && *home) {
#ifdef __APPLE__
        return std::filesystem::path{home} / "Library" / "Application Support";
#else
        return std::filesystem::path{home} / ".local" / "share";
#endif
    }
    return std::filesystem::current_path();
}

} // namespace

std::filesystem::path default_engine_state_directory() {
    if (const auto* explicit_home = environment_variable("MELODY_STATE_DIR");
        explicit_home != nullptr && *explicit_home) {
        return explicit_home;
    }
    return data_home() / "melody" / "engine";
}

std::filesystem::path melody_data_directory() {
    if (const auto* chosen = environment_variable("MELODY_DATA_DIR");
        chosen != nullptr && *chosen) {
        return chosen;
    }
    return data_home() / "melody" / "melody";
}

std::filesystem::path presets_database_path() { return melody_data_directory() / "presets.sqlite"; }

std::filesystem::path engine_state_directory() { return default_engine_state_directory(); }

} // namespace trackknife::core
