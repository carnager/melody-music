// SPDX-License-Identifier: GPL-3.0-only

// ADR-0287: an engine's state moves from Trackknife's folder to melody's,
// once, and only what is the engine's.

#include "trackknife/core/engine_state.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace core = trackknife::core;
namespace fs = std::filesystem;

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

} // namespace

int main() {
    const auto root =
        fs::temp_directory_path() / ("melody-engine-state-" + std::to_string(::getpid()));
    fs::remove_all(root);
    ::setenv("XDG_DATA_HOME", root.c_str(), 1);
    ::unsetenv("MELODY_STATE_DIR");
    const auto fresh = core::default_engine_state_directory();
    CHECK(fresh == root / "melody" / "engine");

    // The engine's folder in the data home.
    CHECK(core::engine_state_directory() == fresh);

    // A folder named outright is the one used, whatever else there is.
    ::setenv("MELODY_STATE_DIR", (root / "named").c_str(), 1);
    CHECK(core::default_engine_state_directory() == root / "named");
    CHECK(core::engine_state_directory() == root / "named");

    fs::remove_all(root);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
