// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/persistence/workspace_backup.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

} // namespace

int main() {
    namespace core = trackknife::core;
    namespace persistence = trackknife::persistence;
    const auto root = std::filesystem::temp_directory_path() /
                      ("trackknife-workspace-backup-" + core::StableId::random().to_string());
    std::filesystem::create_directory(root);
    const auto source = root / "workspace.sqlite";
    const auto backup = root / "workspace-backup.sqlite";

    auto repository = persistence::ListRepository::open(source);
    require(repository.has_value(), "source workspace opens");
    require(repository
                ->save_search(persistence::SavedSearch{.id = core::StableId::random(),
                                                       .name = "Backup evidence",
                                                       .expression = "artist IS x"})
                .has_value(),
            "source state is committed");
    const auto searches = repository->load_saved_searches();
    require(searches && searches->size() == 1U, "and reads back");

    const auto created = persistence::create_workspace_database_backup(source, backup);
    require(created.has_value(), "consistent backup is created while source remains open");
    require(created->schema_version == 58U && created->size_bytes > 0U,
            "backup reports schema and size evidence");
    require(!persistence::create_workspace_database_backup(source, backup),
            "an existing backup is never replaced");

    auto restored = persistence::ListRepository::open(backup);
    require(restored.has_value(), "backup opens as a workspace");
    require(restored->load_saved_searches() == searches, "committed workspace state round trips");

    const auto corrupt = root / "corrupt.sqlite";
    {
        std::ofstream output{corrupt, std::ios::binary};
        output << "not sqlite";
    }
    require(!persistence::inspect_workspace_database_backup(corrupt),
            "corrupt input is rejected before restore");

    const auto live = root / "restored.sqlite";
    const auto rollback = root / "before-restore.sqlite";
    {
        auto prior = persistence::ListRepository::open(live);
        require(prior.has_value(), "prior live workspace opens");
        require(prior->load_saved_searches().has_value(), "prior live workspace is initialized");
    }
    const auto restored_info =
        persistence::restore_workspace_database_backup(backup, live, rollback);
    require(restored_info && std::filesystem::exists(rollback),
            "restore publishes the backup and retains the prior database");
    auto restored_live = persistence::ListRepository::open(live);
    require(restored_live && restored_live->load_saved_searches() == searches,
            "restored live database contains the backed-up workspace");
    require(!persistence::restore_workspace_database_backup(corrupt, live, root / "unused.sqlite"),
            "corrupt restore input cannot change the live workspace");

    std::filesystem::remove_all(root);
}
