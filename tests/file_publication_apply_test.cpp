// SPDX-License-Identifier: GPL-3.0-only

#include "test_checks.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/output_path_preflight.hpp"
#include "trackknife/persistence/file_publication_journal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace core = trackknife::core;
namespace operations = trackknife::operations;
namespace persistence = trackknife::persistence;

using checks::require;

class TemporaryDirectory final {
  public:
    explicit TemporaryDirectory(
        const std::filesystem::path& root = std::filesystem::temp_directory_path())
        : path_{root / ("trackknife-file-apply-" + core::StableId::random().to_string())} {
        require(std::filesystem::create_directory(path_),
                "temporary file-Apply directory must be created");
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, const std::string_view bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    require(stream.good(), "file-Apply fixture must be written");
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

struct PathChange {
    std::filesystem::path source;
    std::filesystem::path target;
};

[[nodiscard]] operations::OutputPathPreflight
preflight_batch(const std::vector<PathChange>& changes,
                const std::filesystem::path& destination_root) {
    operations::OutputPathPlan plan{
        .layout = {},
        .destination =
            operations::DestinationProfile{
                .schema_version = 1U,
                .name = "File Apply test",
                .root_raw_path = destination_root.native(),
                .containment_policy = {"lexical-beneath-root", 1U},
            },
        .operations = {.rename_files = true, .move_files = true},
        .sources = {},
        .issues = {},
    };
    plan.sources.reserve(changes.size());
    for (std::size_t index = 0U; index < changes.size(); ++index) {
        const auto revision = core::observe_local_source_revision(changes[index].source.native());
        require(revision.has_value(), "batch source revision must be observable");
        plan.sources.push_back(operations::PlannedOutputPathSource{
            .source_raw_path = changes[index].source.native(),
            .source_revision = *revision,
            .target_raw_path = changes[index].target.native(),
            .raw_relative_directory = {},
            .sanitized_relative_directory = {},
            .raw_basename = changes[index].target.stem().native(),
            .sanitized_basename = changes[index].target.stem().native(),
            .item_indexes = {index},
            .sanitized = false,
            .no_change = changes[index].source == changes[index].target,
        });
    }
    auto checked = operations::preflight_output_paths(plan);
    if (checked && !checked->ready()) {
        for (const auto& issue : checked->issues) {
            std::cerr << issue.message << " [" << issue.source_raw_path << " -> "
                      << issue.target_raw_path << "]\n";
        }
    }
    require(checked.has_value() && checked->ready(),
            "batch fixture must pass reviewed filesystem preflight");
    return std::move(*checked);
}

[[nodiscard]] persistence::SqliteFilePublicationJournal
open_journal(const TemporaryDirectory& directory, const std::string_view name) {
    auto journal = persistence::SqliteFilePublicationJournal::open(directory.path() / name);
    if (!journal) {
        std::cerr << journal.error().message << '\n';
    }
    require(journal.has_value(), "file-Apply journal must open");
    return std::move(*journal);
}

void sharedDirectoriesCommitInOrderPreservingResults() {
    TemporaryDirectory directory;
    const auto first = directory.path() / "first.flac";
    const auto second = directory.path() / "second.flac";
    const auto unchanged = directory.path() / "unchanged.flac";
    const auto first_target = directory.path() / "organized" / "Artist" / "A" / "first.flac";
    const auto second_target = directory.path() / "organized" / "Artist" / "B" / "second.flac";
    write_file(first, "first publication bytes");
    write_file(second, "second publication bytes");
    write_file(unchanged, "unchanged bytes");
    const auto checked = preflight_batch(
        {{first, first_target}, {second, second_target}, {unchanged, unchanged}}, directory.path());
    require(checked.sources[0].missing_directory_raw_paths.front() ==
                checked.sources[1].missing_directory_raw_paths.front(),
            "fixture must share its first reviewed missing directory");

    auto journal = open_journal(directory, "shared.sqlite3");
    std::atomic_size_t dependent_commits{0U};
    std::vector<operations::FilePublicationApplyProgress> progress;
    const auto applied = operations::apply_file_publications(
        checked, journal,
        [&dependent_commits](
            const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            require(std::filesystem::exists(result.target_raw_path),
                    "dependent state must run after target publication");
            dependent_commits.fetch_add(1U, std::memory_order_relaxed);
            return {};
        },
        [&progress](const operations::FilePublicationApplyProgress& update) {
            progress.push_back(update);
        },
        {}, operations::FilePublicationApplyOptions{.maximum_parallelism = 2U});
    require(applied.has_value(), "ready file batch must return per-source results");
    require(applied->sources.size() == 3U && applied->committed_source_count() == 2U &&
                applied->unchanged_source_count() == 1U && applied->failed_source_count() == 0U &&
                applied->cancelled_source_count() == 0U,
            "two changed sources and one no-op must retain ordered terminal states");
    require(
        applied->sources[0].source_index == 0U &&
            applied->sources[0].state == operations::FilePublicationApplySourceState::committed &&
            applied->sources[1].source_index == 1U &&
            applied->sources[1].state == operations::FilePublicationApplySourceState::committed &&
            applied->sources[2].source_index == 2U &&
            applied->sources[2].state == operations::FilePublicationApplySourceState::unchanged,
        "completion order must not reorder reviewed source results");
    require(dependent_commits.load(std::memory_order_relaxed) == 2U,
            "no-change rows must not invoke dependent publication state");
    require(!std::filesystem::exists(first) && !std::filesystem::exists(second) &&
                std::filesystem::exists(unchanged) &&
                read_file(first_target) == "first publication bytes" &&
                read_file(second_target) == "second publication bytes",
            "shared planned directories must be established without false topology conflicts");
    require(progress.size() == 5U && progress.back().completed_sources == 3U,
            "progress must publish one no-op plus running/terminal changed-source events");
    require(std::ranges::is_sorted(progress, {},
                                   &operations::FilePublicationApplyProgress::completed_sources),
            "serialized progress completion counts must never move backwards");
}

void oneDependentFailureDoesNotRollbackUnrelatedSuccess() {
    TemporaryDirectory directory;
    const auto first = directory.path() / "first.flac";
    const auto second = directory.path() / "second.flac";
    const auto first_target = directory.path() / "first-renamed.flac";
    const auto second_target = directory.path() / "second-renamed.flac";
    write_file(first, "first partial bytes");
    write_file(second, "second partial bytes");
    const auto checked =
        preflight_batch({{first, first_target}, {second, second_target}}, directory.path());
    auto journal = open_journal(directory, "partial.sqlite3");
    const auto applied = operations::apply_file_publications(
        checked, journal,
        [&first](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            if (result.source_raw_path == first.native()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::io,
                    .message = "injected dependent relocation failure",
                    .context = {},
                });
            }
            return {};
        });
    require(applied.has_value() && applied->failed_source_count() == 1U &&
                applied->committed_source_count() == 1U && applied->sources[0].issue &&
                applied->sources[0].issue->message == "injected dependent relocation failure" &&
                applied->sources[1].state == operations::FilePublicationApplySourceState::committed,
            "a runtime source failure must remain an ordered partial result");
    require(std::filesystem::exists(first) && !std::filesystem::exists(first_target) &&
                !std::filesystem::exists(second) &&
                read_file(second_target) == "second partial bytes",
            "failed publication must roll back without reversing an unrelated commit");
}

void failedMemberRetainsOwnershipOfCreatedBatchDirectories() {
    TemporaryDirectory directory;
    const auto first = directory.path() / "first.flac";
    const auto second = directory.path() / "second.flac";
    const auto target_parent = directory.path() / "organized" / "Artist" / "Album";
    const auto first_target = target_parent / "first.flac";
    const auto second_target = target_parent / "second.flac";
    write_file(first, "first rollback bytes");
    write_file(second, "second publication bytes");
    const auto checked =
        preflight_batch({{first, first_target}, {second, second_target}}, directory.path());
    require(checked.sources[0].missing_directory_raw_paths ==
                checked.sources[1].missing_directory_raw_paths,
            "rollback fixture must share its reviewed missing directories");

    auto journal = open_journal(directory, "rollback-directories.sqlite3");
    const auto applied = operations::apply_file_publications(
        checked, journal,
        [&first](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            if (result.source_raw_path == first.native()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "injected dependent-state conflict after directory creation",
                    .context = {},
                });
            }
            return {};
        },
        {}, {}, operations::FilePublicationApplyOptions{.maximum_parallelism = 1U});
    require(applied && applied->failed_source_count() == 1U &&
                applied->committed_source_count() == 1U && applied->sources[0].issue &&
                applied->sources[0].issue->message ==
                    "injected dependent-state conflict after directory creation" &&
                applied->sources[1].state == operations::FilePublicationApplySourceState::committed,
            "a rolled-back member must not make its executor-created directories external to the "
            "remaining batch");
    require(std::filesystem::exists(first) && !std::filesystem::exists(first_target) &&
                !std::filesystem::exists(second) &&
                read_file(second_target) == "second publication bytes",
            "the failed member must roll back while the next member uses the established parent");
}

void freshAdmissionRejectsChangedSourcesAndExternalDirectories() {
    TemporaryDirectory directory;
    const auto changed = directory.path() / "changed.flac";
    const auto stable = directory.path() / "stable.flac";
    const auto changed_target = directory.path() / "changed-target.flac";
    const auto stable_target = directory.path() / "stable-target.flac";
    write_file(changed, "reviewed bytes");
    write_file(stable, "stable bytes");
    const auto checked =
        preflight_batch({{changed, changed_target}, {stable, stable_target}}, directory.path());
    write_file(changed, "changed after reviewed preflight");
    auto journal = open_journal(directory, "fresh.sqlite3");
    std::atomic_size_t callbacks{0U};
    const auto applied = operations::apply_file_publications(
        checked, journal,
        [&callbacks](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            callbacks.fetch_add(1U, std::memory_order_relaxed);
            return {};
        });
    require(applied.has_value() && applied->failed_source_count() == 1U &&
                applied->committed_source_count() == 1U && applied->sources[0].issue &&
                callbacks.load(std::memory_order_relaxed) == 1U,
            "fresh per-source admission must reject a changed source without blocking others");
    require(read_file(changed) == "changed after reviewed preflight" &&
                !std::filesystem::exists(changed_target) && !std::filesystem::exists(stable) &&
                read_file(stable_target) == "stable bytes",
            "fresh-admission failure must not mutate the changed source");

    TemporaryDirectory topology_directory;
    const auto source = topology_directory.path() / "source.flac";
    const auto target = topology_directory.path() / "new" / "Album" / "target.flac";
    write_file(source, "external topology bytes");
    const auto topology_checked = preflight_batch({{source, target}}, topology_directory.path());
    std::filesystem::create_directories(target.parent_path());
    auto topology_journal = open_journal(topology_directory, "topology.sqlite3");
    const auto topology = operations::apply_file_publications(
        topology_checked, topology_journal,
        [](const operations::FilePublicationCommitResult&) -> core::Result<void> { return {}; });
    require(topology.has_value() && topology->failed_source_count() == 1U &&
                topology->sources.front().issue && std::filesystem::exists(source) &&
                !std::filesystem::exists(target),
            "an externally appeared reviewed directory must not gain in-batch trust");
}

void cancellationStopsAdmissionAndRollsBackInFlightSources() {
    using namespace std::chrono_literals;
    TemporaryDirectory directory;
    std::vector<PathChange> changes;
    for (std::size_t index = 0U; index < 4U; ++index) {
        const auto source = directory.path() / ("cancel-" + std::to_string(index) + ".flac");
        const auto target =
            directory.path() / ("cancelled-target-" + std::to_string(index) + ".flac");
        write_file(source, "cancellation bytes " + std::to_string(index));
        changes.push_back({.source = source, .target = target});
    }
    const auto checked = preflight_batch(changes, directory.path());
    auto journal = open_journal(directory, "cancel.sqlite3");
    core::CancellationSource cancellation;
    std::atomic_size_t admitted{0U};
    auto future = std::async(std::launch::async, [&] {
        return operations::apply_file_publications(
            checked, journal,
            [&admitted, &cancellation](
                const operations::FilePublicationCommitResult& result) -> core::Result<void> {
                admitted.fetch_add(1U, std::memory_order_relaxed);
                while (!cancellation.token().is_cancellation_requested()) {
                    std::this_thread::sleep_for(1ms);
                }
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::cancelled,
                    .message = "cancelled " + result.source_raw_path,
                    .context = {},
                });
            },
            {}, cancellation.token(),
            operations::FilePublicationApplyOptions{.maximum_parallelism = 2U});
    });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (admitted.load(std::memory_order_relaxed) < 2U &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(admitted.load(std::memory_order_relaxed) == 2U,
            "the configured worker bound must admit exactly two blocked sources");
    cancellation.request_cancellation();
    const auto applied = future.get();
    require(applied.has_value() && applied->cancellation_requested &&
                applied->cancelled_source_count() == 4U &&
                applied->committed_source_count() == 0U &&
                admitted.load(std::memory_order_relaxed) == 2U,
            "cancellation must stop admission and classify every remaining source");
    for (const auto& change : changes) {
        require(std::filesystem::exists(change.source) && !std::filesystem::exists(change.target),
                "in-flight cancellation must reach verified source rollback");
    }
}

void dispatchesCrossFilesystemPublicationWhenAvailable() {
    const std::filesystem::path shared_memory{"/dev/shm"};
    std::error_code error;
    if (!std::filesystem::is_directory(shared_memory, error) || error) {
        return;
    }
    TemporaryDirectory source_directory;
    TemporaryDirectory target_directory{shared_memory};
    const auto source = source_directory.path() / "cross-source.flac";
    const auto target = target_directory.path() / "cross-target.flac";
    const std::string bytes((1U * 1024U * 1024U) + 17U, 'x');
    write_file(source, bytes);
    struct stat target_status{};
    require(::stat(target_directory.path().c_str(), &target_status) == 0,
            "cross-filesystem target must be observable");
    const auto source_revision = core::observe_local_source_revision(source.native());
    require(source_revision.has_value(), "cross-filesystem source revision must be observable");
    if (source_revision->device == static_cast<std::uint64_t>(target_status.st_dev)) {
        return;
    }
    const auto checked = preflight_batch({{source, target}}, target_directory.path());
    require(checked.sources.front().publication ==
                operations::OutputPathPublicationKind::cross_filesystem_copy,
            "batch preflight must retain its cross-filesystem classification");
    auto journal = open_journal(source_directory, "cross.sqlite3");
    const auto applied = operations::apply_file_publications(
        checked, journal,
        [](const operations::FilePublicationCommitResult&) -> core::Result<void> { return {}; });
    require(applied.has_value() && applied->committed_source_count() == 1U &&
                applied->sources.front().commit && !std::filesystem::exists(source) &&
                read_file(target) == bytes,
            "batch dispatch must execute verified cross-filesystem publication");
}

void rejectsInvalidBatchContracts() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "target.flac";
    write_file(source, "validation bytes");
    const auto checked = preflight_batch({{source, target}}, directory.path());
    auto journal = open_journal(directory, "invalid.sqlite3");
    const auto missing_callback = operations::apply_file_publications(
        checked, journal, operations::FilePublicationDependentStateCommitter{});
    require(!missing_callback && missing_callback.error().code == core::ErrorCode::invalid_argument,
            "file Apply must require dependent-state commit");
    const auto invalid_workers = operations::apply_file_publications(
        checked, journal,
        [](const operations::FilePublicationCommitResult&) -> core::Result<void> { return {}; }, {},
        {}, operations::FilePublicationApplyOptions{.maximum_parallelism = 0U});
    require(!invalid_workers && invalid_workers.error().code == core::ErrorCode::invalid_argument &&
                std::filesystem::exists(source) && !std::filesystem::exists(target),
            "invalid batch options must not mutate files or create journals");
}

} // namespace

int main() {
    rejectsInvalidBatchContracts();
    sharedDirectoriesCommitInOrderPreservingResults();
    oneDependentFailureDoesNotRollbackUnrelatedSuccess();
    failedMemberRetainsOwnershipOfCreatedBatchDirectories();
    freshAdmissionRejectsChangedSourcesAndExternalDirectories();
    cancellationStopsAdmissionAndRollsBackInFlightSources();
    dispatchesCrossFilesystemPublicationWhenAvailable();
    return EXIT_SUCCESS;
}
