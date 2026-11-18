// SPDX-License-Identifier: GPL-3.0-only

#include "bench/remote_mount.hpp"

#include <filesystem>
#include <system_error>

namespace trackknife::bench {
namespace {

// `path` with `from` at its start replaced by `to`; `path` itself when
// either is empty (the same paths on both machines).
[[nodiscard]] std::optional<std::string> rebase(const std::string& path, const std::string& from,
                                                const std::string& to) {
    if (from.empty() || to.empty()) {
        return path;
    }
    if (!path_within(path, from)) {
        return std::nullopt;
    }
    return to + path.substr(from.size());
}

} // namespace

bool path_within(const std::string_view path, const std::string_view folder) {
    if (folder.empty() || !path.starts_with(folder)) {
        return false;
    }
    return path.size() == folder.size() || folder.back() == '/' || path[folder.size()] == '/';
}

std::optional<std::string> RemoteMount::to_local(const std::string& remote_path) const {
    auto local = rebase(remote_path, remote_folder, local_folder);
    std::error_code error;
    if (!local || !std::filesystem::is_regular_file(*local, error)) {
        return std::nullopt;
    }
    return local;
}

std::optional<std::string>
RemoteMount::to_remote(const std::string& local_path,
                       const std::vector<std::string>& remote_roots) const {
    auto remote = rebase(local_path, local_folder, remote_folder);
    if (!remote) {
        return std::nullopt;
    }
    for (const auto& root : remote_roots) {
        if (path_within(*remote, root)) {
            return remote;
        }
    }
    return std::nullopt;
}

std::optional<std::string> RemoteMount::remote_path_of(const std::string& local_path) const {
    return rebase(local_path, local_folder, remote_folder);
}

std::optional<std::string> RemoteMount::local_path_of(const std::string& remote_path) const {
    return rebase(remote_path, remote_folder, local_folder);
}

} // namespace trackknife::bench
