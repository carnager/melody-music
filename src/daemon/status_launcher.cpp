// SPDX-License-Identifier: GPL-3.0-only

#include "status_launcher.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <iostream>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char** environ;
#endif

namespace trackknife::daemon {
void launch_status_icon(const char* executable, const std::filesystem::path& socket,
                        const std::string& service) {
    std::string program = "melody-status";
    std::error_code error;
    const auto here = std::filesystem::absolute(executable, error).parent_path();
    for (const auto& candidate : {here / "melody-status", here / "../status/melody-status"}) {
        if (::access(candidate.c_str(), X_OK) == 0) {
            program = candidate.string();
            break;
        }
    }
    std::vector<std::string> arguments{program, "--socket", socket.string()};
    if (!service.empty()) {
        arguments.insert(arguments.end(), {"--service", service});
    }
    std::vector<char*> words;
    for (auto& argument : arguments) {
        words.push_back(argument.data());
    }
    words.push_back(nullptr);
#ifdef __APPLE__
    auto environment = *_NSGetEnviron();
#else
    auto environment = environ;
#endif
    pid_t child{};
    const auto result =
        ::posix_spawnp(&child, program.c_str(), nullptr, nullptr, words.data(), environment);
    if (result != 0) {
        std::cerr << "melodyd: cannot start melody-status: "
                  << std::generic_category().message(result) << '\n';
        return;
    }
    // Reap an icon closed during this engine's lifetime.
    std::thread([child] {
        int status{};
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
}
} // namespace trackknife::daemon
