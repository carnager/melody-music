// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <string>

namespace trackknife::daemon {
// A separate GUI process: no Qt dependency in the engine, and the icon survives its restart.
void launch_status_icon(const char* executable, const std::filesystem::path& socket,
                        const std::string& service);
} // namespace trackknife::daemon
