// SPDX-License-Identifier: GPL-3.0-only

#include "test_checks.hpp"
#include "trackknife/persistence/file_publication_journal.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace core = trackknife::core;
namespace operations = trackknife::operations;
namespace persistence = trackknife::persistence;
using Kind = operations::OutputPathPublicationKind;
using Content = operations::FilePublicationContentKind;
using State = operations::FilePublicationJournalState;
using Record = operations::FilePublicationJournalRecord;
using Transition = operations::FilePublicationJournalTransition;

using checks::require;

class TemporaryDatabase final {
  public:
    TemporaryDatabase()
        : path_{std::filesystem::temp_directory_path() /
                ("trackknife-file-publication-" + core::StableId::random().to_string() +
                 ".sqlite3")} {
        remove();
    }
    TemporaryDatabase(const TemporaryDatabase&) = delete;
    TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;
    ~TemporaryDatabase() { remove(); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    void remove() const noexcept {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + "-wal", ignored);
        std::filesystem::remove(path_.string() + "-shm", ignored);
    }

    std::filesystem::path path_;
};

core::LocalSourceRevision revision(const std::uint64_t seed) {
    return {.device = seed,
            .inode = seed + 1U,
            .size = seed + 2U,
            .modification_time_seconds = -static_cast<std::int64_t>(seed + 3U),
            .modification_time_nanoseconds = static_cast<std::int64_t>(seed + 4U)};
}

Record planned_record(const Kind kind) {
    Record record{
        .id = core::StableId::random(),
        .state = State::planned,
        .publication = kind,
        .content = Content::preserve_source_bytes,
        .source_raw_path = std::string{"/incoming/source-\xff.flac", 23U},
        .target_raw_path = "/library/Artist/Album/track.flac",
        .prepared_raw_path = {},
        .expected_source_revision = revision(10U),
        .prepared_revision = std::nullopt,
        .target_revision = std::nullopt,
        .occurrence_indexes = {2U, 7U, 99U},
        .planned_missing_directory_raw_paths = {"/library/Artist", "/library/Artist/Album"},
        .reverses_journal_id = std::nullopt,
        .failure = std::nullopt,
    };
    if (kind == Kind::cross_filesystem_copy) {
        record.prepared_raw_path =
            operations::file_publication_prepared_path(record.target_raw_path, record.id).native();
    }
    return record;
}

void sameFilesystemDestinationArtifactUsesPreparedLifecycle() {
    TemporaryDatabase database;
    auto opened = persistence::SqliteFilePublicationJournal::open(database.path());
    require(opened.has_value(), "destination-artifact journal must open");
    auto journal = std::move(*opened);
    auto planned = planned_record(Kind::same_filesystem_rename);
    planned.content = Content::prepared_destination_artifact;
    planned.prepared_raw_path =
        operations::file_publication_prepared_path(planned.target_raw_path, planned.id).native();
    require(journal.create(planned).has_value(),
            "same-filesystem destination artifact must persist before preparation");
    const auto initially_loaded = journal.load(planned.id);
    require(initially_loaded && *initially_loaded == std::optional{planned},
            "destination-artifact content intent must survive restart storage");

    const auto artifact = revision(91U);
    require(journal
                .transition(planned.id, Transition{.expected_state = State::planned,
                                                   .state = State::target_prepared,
                                                   .prepared_revision = artifact,
                                                   .target_revision = std::nullopt,
                                                   .failure = std::nullopt})
                .has_value(),
            "same-filesystem changed content must enter the prepared state");
    require(journal
                .transition(planned.id, Transition{.expected_state = State::target_prepared,
                                                   .state = State::target_published,
                                                   .prepared_revision = artifact,
                                                   .target_revision = artifact,
                                                   .failure = std::nullopt})
                .has_value(),
            "same-filesystem changed content must publish the prepared inode");
    require(journal
                .transition(planned.id, Transition{.expected_state = State::target_published,
                                                   .state = State::dependent_state_committed,
                                                   .prepared_revision = artifact,
                                                   .target_revision = artifact,
                                                   .failure = std::nullopt})
                .has_value(),
            "destination artifact must commit dependent state before source removal");
    require(
        journal
            .transition(planned.id, Transition{.expected_state = State::dependent_state_committed,
                                               .state = State::source_removed,
                                               .prepared_revision = artifact,
                                               .target_revision = artifact,
                                               .failure = std::nullopt})
            .has_value(),
        "destination artifact must retain a distinct source-removed boundary");
    require(journal
                .transition(planned.id, Transition{.expected_state = State::source_removed,
                                                   .state = State::complete,
                                                   .prepared_revision = artifact,
                                                   .target_revision = artifact,
                                                   .failure = std::nullopt})
                .has_value(),
            "destination-artifact journal must complete from source-removed");
    const auto loaded = journal.load(planned.id);
    require(loaded && *loaded && (**loaded).state == State::complete &&
                (**loaded).content == Content::prepared_destination_artifact,
            "complete destination-artifact evidence must retain its content kind");
}

void sameFilesystemStateMachineIsDurableAndOptimistic() {
    TemporaryDatabase database;
    auto opened = persistence::SqliteFilePublicationJournal::open(database.path());
    require(opened.has_value(), "file-publication journal must migrate a new database");
    auto journal = std::move(*opened);
    const auto planned = planned_record(Kind::same_filesystem_rename);
    require(journal.create(planned).has_value(), "same-filesystem publication must be journaled");
    const auto loaded = journal.load(planned.id);
    require(loaded && *loaded && **loaded == planned,
            "raw paths, source identity, directories, and occurrences must round trip");
    const auto initially_incomplete = journal.load_incomplete();
    require(initially_incomplete && *initially_incomplete == std::vector{planned},
            "planned publication must be available to startup recovery");

    const auto invalid =
        journal.transition(planned.id, Transition{.expected_state = State::planned,
                                                  .state = State::target_prepared,
                                                  .prepared_revision = revision(20U),
                                                  .target_revision = std::nullopt,
                                                  .failure = std::nullopt});
    require(!invalid && invalid.error().code == core::ErrorCode::invalid_argument,
            "same-filesystem rename must not invent a prepared-copy state");

    const auto target = revision(30U);
    require(journal
                .transition(planned.id, Transition{.expected_state = State::planned,
                                                   .state = State::target_published,
                                                   .prepared_revision = std::nullopt,
                                                   .target_revision = target,
                                                   .failure = std::nullopt})
                .has_value(),
            "atomic rename must advance directly to target-published");
    const auto stale = journal.transition(
        planned.id,
        Transition{.expected_state = State::planned,
                   .state = State::rolled_back,
                   .prepared_revision = std::nullopt,
                   .target_revision = std::nullopt,
                   .failure = core::Error{
                       .code = core::ErrorCode::conflict, .message = "stale", .context = {}}});
    require(!stale && stale.error().code == core::ErrorCode::conflict,
            "publication transitions must reject stale expected state");
    require(journal
                .transition(planned.id, Transition{.expected_state = State::target_published,
                                                   .state = State::dependent_state_committed,
                                                   .prepared_revision = std::nullopt,
                                                   .target_revision = target,
                                                   .failure = std::nullopt})
                .has_value(),
            "dependent state must follow physical publication");
    require(
        journal
            .transition(planned.id, Transition{.expected_state = State::dependent_state_committed,
                                               .state = State::complete,
                                               .prepared_revision = std::nullopt,
                                               .target_revision = target,
                                               .failure = std::nullopt})
            .has_value(),
        "same-filesystem publication completes after dependent state");
    require(journal.load_incomplete()->empty(),
            "completed publication must not replay during startup recovery");

    auto reopened = persistence::SqliteFilePublicationJournal::open(database.path());
    require(reopened.has_value(), "completed file-publication journal must reopen");
    auto durable = reopened->load(planned.id);
    require(durable && *durable && (**durable).state == State::complete &&
                (**durable).target_revision == target,
            "terminal same-filesystem identity must survive restart");
    auto recent = reopened->load_recent();
    require(recent && recent->size() == 1U && recent->front() == **durable,
            "terminal publication evidence must remain visible in recent history");
}

void crossFilesystemStateMachinePreservesEveryRecoveryBoundary() {
    TemporaryDatabase database;
    auto opened = persistence::SqliteFilePublicationJournal::open(database.path());
    require(opened.has_value(), "cross-filesystem journal must open");
    auto journal = std::move(*opened);
    const auto planned = planned_record(Kind::cross_filesystem_copy);
    require(journal.create(planned).has_value(), "cross-filesystem publication must be journaled");
    const auto prepared = revision(40U);
    const auto target = revision(50U);
    require(journal
                .transition(planned.id, Transition{.expected_state = State::planned,
                                                   .state = State::target_prepared,
                                                   .prepared_revision = prepared,
                                                   .target_revision = std::nullopt,
                                                   .failure = std::nullopt})
                .has_value(),
            "verified copy identity must be durable before target publication");
    require(journal
                .transition(planned.id, Transition{.expected_state = State::target_prepared,
                                                   .state = State::target_published,
                                                   .prepared_revision = prepared,
                                                   .target_revision = target,
                                                   .failure = std::nullopt})
                .has_value(),
            "published target must retain prepared and target identities");
    require(journal
                .transition(planned.id, Transition{.expected_state = State::target_published,
                                                   .state = State::dependent_state_committed,
                                                   .prepared_revision = prepared,
                                                   .target_revision = target,
                                                   .failure = std::nullopt})
                .has_value(),
            "dependent references must move before source deletion");
    require(
        journal
            .transition(planned.id, Transition{.expected_state = State::dependent_state_committed,
                                               .state = State::source_removed,
                                               .prepared_revision = prepared,
                                               .target_revision = target,
                                               .failure = std::nullopt})
            .has_value(),
        "cross-filesystem source deletion must have its own durable boundary");
    require(journal
                .transition(planned.id, Transition{.expected_state = State::source_removed,
                                                   .state = State::complete,
                                                   .prepared_revision = prepared,
                                                   .target_revision = target,
                                                   .failure = std::nullopt})
                .has_value(),
            "source-removed publication must complete without losing identities");
}

void failureEvidenceIsValidatedAndReconciliationRemainsVisible() {
    TemporaryDatabase database;
    auto opened = persistence::SqliteFilePublicationJournal::open(database.path());
    require(opened.has_value(), "failure journal must open");
    auto journal = std::move(*opened);
    auto invalid = planned_record(Kind::cross_filesystem_copy);
    invalid.occurrence_indexes = {7U, 7U};
    require(!journal.create(invalid), "duplicate occurrence evidence must fail atomically");

    const auto planned = planned_record(Kind::cross_filesystem_copy);
    require(journal.create(planned).has_value(), "reconciliation fixture must persist");
    const auto prepared = revision(60U);
    require(journal
                .transition(planned.id, Transition{.expected_state = State::planned,
                                                   .state = State::target_prepared,
                                                   .prepared_revision = prepared,
                                                   .target_revision = std::nullopt,
                                                   .failure = std::nullopt})
                .has_value(),
            "reconciliation fixture must reach prepared state");
    const auto evidence_free =
        journal.transition(planned.id, Transition{.expected_state = State::target_prepared,
                                                  .state = State::needs_reconciliation,
                                                  .prepared_revision = prepared,
                                                  .target_revision = std::nullopt,
                                                  .failure = std::nullopt});
    require(!evidence_free && evidence_free.error().code == core::ErrorCode::invalid_argument,
            "reconciliation state must retain explicit failure evidence");
    const core::Error failure{
        .code = core::ErrorCode::io, .message = "ambiguous target publication", .context = {}};
    require(journal
                .transition(planned.id, Transition{.expected_state = State::target_prepared,
                                                   .state = State::needs_reconciliation,
                                                   .prepared_revision = prepared,
                                                   .target_revision = std::nullopt,
                                                   .failure = failure})
                .has_value(),
            "ambiguous prepared evidence must become durable reconciliation state");
    auto incomplete = journal.load_incomplete();
    require(incomplete && incomplete->size() == 1U &&
                incomplete->front().state == State::needs_reconciliation &&
                incomplete->front().failure == failure,
            "reconciliation evidence must remain visible to startup recovery UI");
}

void reversalRelationsRoundTripAndRequireAnExistingOriginal() {
    TemporaryDatabase database;
    auto opened = persistence::SqliteFilePublicationJournal::open(database.path());
    require(opened.has_value(), "reversal journal must open");
    auto journal = std::move(*opened);
    const auto original = planned_record(Kind::same_filesystem_rename);
    require(journal.create(original).has_value(), "reversal original must persist");
    const auto published = revision(80U);
    require(
        journal.transition(original.id, Transition{.expected_state = State::planned,
                                                   .state = State::target_published,
                                                   .prepared_revision = std::nullopt,
                                                   .target_revision = published,
                                                   .failure = std::nullopt}) &&
            journal.transition(original.id, Transition{.expected_state = State::target_published,
                                                       .state = State::dependent_state_committed,
                                                       .prepared_revision = std::nullopt,
                                                       .target_revision = published,
                                                       .failure = std::nullopt}) &&
            journal.transition(original.id,
                               Transition{.expected_state = State::dependent_state_committed,
                                          .state = State::complete,
                                          .prepared_revision = std::nullopt,
                                          .target_revision = published,
                                          .failure = std::nullopt}),
        "reversal original must reach complete");

    auto reversal = planned_record(Kind::same_filesystem_rename);
    reversal.source_raw_path = original.target_raw_path;
    reversal.target_raw_path = original.source_raw_path;
    reversal.expected_source_revision = published;
    reversal.planned_missing_directory_raw_paths.clear();
    reversal.reverses_journal_id = original.id;
    require(journal.create(reversal).has_value(), "reverse operation must persist its relation");
    auto related = journal.load_reversals(original.id);
    require(related && *related == std::vector{reversal},
            "reverse lookup must retain complete raw and revision evidence");
    auto recent = journal.load_recent();
    require(recent && recent->size() == 2U && recent->front() == reversal &&
                recent->back().id == original.id && recent->back().state == State::complete,
            "recent publication history must retain newest-first reversal relationships");

    auto missing_parent = reversal;
    missing_parent.id = core::StableId::random();
    missing_parent.reverses_journal_id = core::StableId::random();
    const auto rejected = journal.create(missing_parent);
    require(!rejected && rejected.error().code == core::ErrorCode::invalid_argument &&
                journal.load_reversals(*missing_parent.reverses_journal_id)->empty(),
            "a reversal cannot reference an absent original operation");
}

} // namespace

int main() {
    sameFilesystemStateMachineIsDurableAndOptimistic();
    sameFilesystemDestinationArtifactUsesPreparedLifecycle();
    crossFilesystemStateMachinePreservesEveryRecoveryBoundary();
    failureEvidenceIsValidatedAndReconciliationRemainsVisible();
    reversalRelationsRoundTripAndRequireAnExistingOriginal();
    return EXIT_SUCCESS;
}
