// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/list_edits.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include "presets.hpp"
#include <sqlite3.h>

#include <iterator>

#include <fstream>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

void saved_searches_are_persistent_and_conflict_checked() {
    namespace persistence = trackknife::persistence;
    namespace core = trackknife::core;
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackbench-searches-" + core::StableId::random().to_string());
    std::filesystem::create_directory(directory);
    const auto path = directory / "state.sqlite3";
    {
        auto repository = persistence::ListRepository::open(path);
        require(repository.has_value(), "saved-search database opens");
        persistence::SavedSearch search{.id = core::StableId::random(),
                                        .name = "FLAC ohne ReplayGain",
                                        .expression =
                                            "codec IS flac AND NOT replaygaintrackgain PRESENT",
                                        .dialect = "tkq-1",
                                        .scope = persistence::SavedSearchScope::library};
        require(repository->save_search(search).has_value(), "valid query is saved");
        auto loaded = repository->load_saved_searches();
        require(loaded && loaded->size() == 1 && loaded->front().revision == 1,
                "saved search gets a revision");
        search.revision = 1;
        require(loaded->front() == search, "query, name, dialect, scope, and identity round trip");
        auto other = persistence::ListRepository::open(path);
        require(other && other->load_saved_searches() == loaded, "definitions survive reopening");
        auto duplicate = search;
        duplicate.id = core::StableId::random();
        duplicate.revision = 0;
        require(!other->save_search(duplicate), "duplicate names cannot overwrite definitions");
        auto updated = search;
        updated.name = "Songs in current tab";
        updated.expression = "Jazz";
        updated.dialect = "words-1";
        updated.scope = persistence::SavedSearchScope::current_tab;
        require(other->save_search(updated).has_value(), "query and scope can be updated");
        require(!repository->save_search(search), "stale writer cannot overwrite another update");
        require(!repository->remove_search(search), "stale deletion cannot erase another update");
        auto invalid = duplicate;
        invalid.name = "Invalid";
        invalid.expression = "AND (";
        require(!repository->save_search(invalid), "invalid structured queries are rejected");
        invalid.expression = "genre IS jazz";
        invalid.dialect = "tkq-2";
        require(!repository->save_search(invalid), "unknown dialects are rejected");
        invalid.dialect = "tkq-1";
        invalid.name = std::string(257, 'n');
        require(!repository->save_search(invalid), "oversized names are rejected");
        loaded = other->load_saved_searches();
        require(loaded && loaded->size() == 1 && loaded->front().revision == 2 &&
                    loaded->front().name == updated.name,
                "failed writes preserve the winning update");
        require(repository->remove_search(loaded->front()).has_value(),
                "current revision can be deleted");
        require(other->load_saved_searches()->empty(), "deletion persists across connections");
    }
    std::filesystem::remove_all(directory);
}

void local_listening_history_is_monotonic_and_persistent() {
    namespace persistence = trackknife::persistence;
    namespace core = trackknife::core;
    const auto path = std::filesystem::temp_directory_path() /
                      ("trackbench-listens-" + core::StableId::random().to_string() + ".sqlite3");
    const std::string hash(64U, 'a');
    {
        auto repository = persistence::ListRepository::open(path);
        require(repository.has_value(), "listening-history database opens");
        const auto absent = repository->load_local_listening_history(hash);
        require(absent && !*absent, "unknown track has no history");
        require(repository->save_local_resume(hash, 42'000, 1'000).has_value(),
                "resume position is stored");
        require(repository->save_local_resume(hash, 12'000, 999).has_value(),
                "stale resume observation is harmless");
        auto loaded = repository->load_local_listening_history(hash);
        require(loaded && *loaded && (*loaded)->resume_position_ms == 42'000,
                "older observation cannot rewind the stored resume point");
        require(repository->record_local_play(hash, 2'000).has_value(),
                "completed play is recorded");
        require(repository->record_local_play(hash, 3'000).has_value(), "play count accumulates");
        loaded = repository->load_local_listening_history(hash);
        require(loaded && *loaded && (*loaded)->play_count == 2U &&
                    (*loaded)->last_played_ms == 3'000 && (*loaded)->resume_position_ms == 0,
                "completion advances history and clears resume");
        require(!repository->save_local_resume({}, 1, 1), "empty identities are rejected");
        require(!repository->record_local_play(std::string(64U, 'z'), 1),
                "non-hash identities are rejected");
        require(!repository->save_local_resume(hash, -1, 4'000),
                "negative resume positions are rejected");
        require(!repository->record_local_play(hash, 0),
                "nonpositive play timestamps are rejected");
        require(repository->save_local_resume(hash, 20'000, 5'000).has_value(),
                "a later listening session can save progress");
        require(repository->record_local_play(hash, 4'000).has_value(),
                "a delayed earlier listen still contributes to the count");
        require(repository->save_local_resume(hash, 1'000, 5'000).has_value(),
                "duplicate timestamps cannot overwrite a stored observation");
        loaded = repository->load_local_listening_history(hash);
        require(loaded && *loaded && (*loaded)->play_count == 3U &&
                    (*loaded)->resume_position_ms == 20'000 && (*loaded)->updated_at_ms == 5'000,
                "delayed completion preserves newer resume state");
    }
    auto reopened = persistence::ListRepository::open(path);
    require(reopened.has_value(), "listening-history database reopens");
    const auto loaded = reopened->load_local_listening_history(hash);
    require(loaded && *loaded && (*loaded)->play_count == 3U,
            "listening history survives reopening");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + "-wal", ignored);
    std::filesystem::remove(path.string() + "-shm", ignored);
}

void local_listening_occurrences_are_idempotent_and_source_qualified() {
    namespace persistence = trackknife::persistence;
    namespace core = trackknife::core;
    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackbench-occurrences-" + core::StableId::random().to_string() + ".sqlite3");
    {
        auto repository = persistence::ListRepository::open(path);
        require(repository.has_value(), "occurrence database opens");
        persistence::ListItem source;
        source.source = persistence::ListSource::local;
        source.source_reference = std::string{"/music/raw-"} + char(0xff) + ".flac";
        source.source_revision = core::LocalSourceRevision{.device = 1, .inode = 2, .size = 3};
        const auto unseen = repository->lookup_local_listening_history(source);
        require(unseen && !*unseen, "unseen source has no listening record");
        sqlite3* read_only = nullptr;
        require(sqlite3_open_v2(path.c_str(), &read_only, SQLITE_OPEN_READONLY, nullptr) ==
                    SQLITE_OK,
                "read-only validation connection opens");
        int identities = -1;
        require(sqlite3_exec(
                    read_only, "SELECT count(*) FROM local_listening_sources",
                    [](void* output, int, char** values, char**) {
                        *static_cast<int*>(output) = std::stoi(values[0]);
                        return 0;
                    },
                    &identities, nullptr) == SQLITE_OK &&
                    identities == 0,
                "display lookup never creates listening identities");
        sqlite3_close(read_only);
        const auto key = repository->local_listening_key(source);
        require(key.has_value(), "raw-byte source has a stable key without tags");
        require(repository->save_local_resume(*key, 1234, 900).has_value(),
                "resume fixture stored");
        const auto event = core::StableId::random();
        require(repository->record_local_listen(source, event, 1000).has_value(),
                "first event counts");
        require(repository->record_local_listen(source, event, 1000).has_value(),
                "replay succeeds");
        require(!repository->record_local_listen(source, event, 2000),
                "different replay is rejected");
        auto other = persistence::ListRepository::open(path);
        require(other && other->record_local_listen(source, event, 1000),
                "restart replay succeeds");
        auto history = other->load_local_listening_history(*key);
        require(history && *history && (*history)->play_count == 1, "retries cannot double count");
        require(repository->lookup_local_listening_history(source) == history,
                "read-only source lookup returns the stored statistics");
        require((*history)->resume_position_ms == 1234,
                "qualification does not imply completion or erase resume state");
        require(other->record_local_listen(source, core::StableId::random(), 2000).has_value(),
                "a separate occurrence counts again");
        history = repository->load_local_listening_history(*key);
        require(history && *history && (*history)->play_count == 2 &&
                    (*history)->last_played_ms == 2000,
                "independent connections see durable counts");
        auto logical = source;
        logical.segment = persistence::ListItemSegment{0, 44100};
        const auto logical_key = repository->local_listening_key(logical);
        require(logical_key && logical_key != key, "logical range differs from whole file");
        logical.segment = persistence::ListItemSegment{44100, 88200};
        require(repository->local_listening_key(logical) != logical_key,
                "CUE ranges stay separate");
        logical = source;
        logical.source_selection = persistence::ListItemSourceSelection{1, 2};
        require(repository->local_listening_key(logical) != key,
                "decoder selections stay separate");
        auto replacement = source;
        ++replacement.source_revision->inode;
        require(repository->local_listening_key(replacement) != key,
                "reused paths do not inherit counts");
        replacement.source = persistence::ListSource::mpd;
        require(!repository->record_local_listen(replacement, core::StableId::random(), 3000),
                "server sources cannot write local history");
        replacement = source;
        replacement.source_revision.reset();
        require(!repository->local_listening_key(replacement), "revisionless sources are rejected");
        require(!repository->record_local_listen(source, {}, 3000), "nil occurrence rejected");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");
}

template <typename Action>
const Action& require_action(const trackknife::metadata::MetadataTransformationChain& chain,
                             const std::size_t index, const std::string_view message) {
    require(index < chain.actions.size(), message);
    const auto* action = std::get_if<Action>(&chain.actions[index]);
    require(action != nullptr, message);
    return *action;
}

void metadata_transformation_chains_round_trip_transactionally() {
    namespace core = trackknife::core;
    namespace metadata = trackknife::metadata;
    namespace persistence = trackknife::persistence;
    const auto database_path =
        std::filesystem::temp_directory_path() /
        ("trackknife-transformation-chains-" + core::StableId::random().to_string() + ".sqlite3");
    const auto cleanup = [&database_path] {
        std::error_code ignored;
        std::filesystem::remove(database_path, ignored);
        std::filesystem::remove(database_path.string() + "-wal", ignored);
        std::filesystem::remove(database_path.string() + "-shm", ignored);
    };
    cleanup();

    const auto chain_id = core::StableId::random();
    const persistence::SavedMetadataTransformationChain expected{
        .id = chain_id,
        .chain =
            metadata::MetadataTransformationChain{
                .schema_version = 1U,
                .name = "Exact cleanup",
                .actions =
                    {
                        metadata::MetadataSetValuesAction{.target_field = "Artist",
                                                          .values = {"one", "", "one"}},
                        metadata::MetadataAddValuesAction{.target_field = "Artist",
                                                          .values = {"tail", ""}},
                        metadata::MetadataRemoveFieldAction{
                            .target_field = "Comment:",
                            .match_mode = metadata::MetadataFieldMatchMode::exact_native},
                        metadata::MetadataTransformValuesAction{
                            .target_field = "Title",
                            .transform = metadata::MetadataValueTransformKind::trim_ascii},
                        metadata::MetadataTransformValuesAction{
                            .target_field = "Album",
                            .transform = metadata::MetadataValueTransformKind::lowercase},
                        metadata::MetadataTransformValuesAction{
                            .target_field = "Genre",
                            .transform = metadata::MetadataValueTransformKind::uppercase},
                        metadata::MetadataTransformValuesAction{
                            .target_field = "Subtitle",
                            .transform = metadata::MetadataValueTransformKind::capitalize_first},
                        metadata::MetadataCopyFieldAction{.target_field = "Credits",
                                                          .source_field = "Artist"},
                        metadata::MetadataSplitValuesAction{.target_field = "Tags",
                                                            .separator = "::"},
                        metadata::MetadataJoinValuesAction{.target_field = "Tags", .separator = ""},
                        metadata::MetadataFormatValueAction{
                            .target_field = "Sort title", .dialect = {}, .source = "%artist%"},
                        metadata::MetadataRemoveMatchingValuesAction{.target_field = "Artist",
                                                                     .match = "Unknown"},
                        metadata::MetadataReplaceMatchingValuesAction{
                            .target_field = "Genre",
                            .match = "Rock",
                            .replacement_values = {"Alternative", "Indie"}},
                        metadata::MetadataNumberSelectedItemsAction{
                            .target_field = "Track Number", .start = 7U, .padding = 2U},
                        metadata::MetadataKeepFirstCharactersAction{.target_field = "Date",
                                                                    .character_count = 4U},
                        metadata::MetadataRemoveFieldIfAction{
                            .target_field = "Disc Number",
                            .dialect = {},
                            .condition = "$not(%totaldiscs%)",
                            .match_mode = metadata::MetadataFieldMatchMode::exact_native,
                        },
                        metadata::MetadataCaptureValuesAction{
                            .dialect = {},
                            .source_kind = metadata::MetadataCaptureSourceKind::filename,
                            .source = {},
                            .pattern = "%tracknumber%. %title%",
                        },
                        metadata::MetadataCaptureValuesAction{
                            .dialect = {},
                            .source_kind = metadata::MetadataCaptureSourceKind::full_path,
                            .source = {},
                            .pattern = "%directory%/%title%.flac",
                        },
                        metadata::MetadataCaptureValuesAction{
                            .dialect = {},
                            .source_kind = metadata::MetadataCaptureSourceKind::formatted,
                            .source = "%artist% — %title%",
                            .pattern = "%displayartist% — %displaytitle%",
                        },
                        metadata::MetadataCaptureValuesAction{
                            .dialect = {},
                            .source_kind = metadata::MetadataCaptureSourceKind::field,
                            .source = "Comment",
                            .pattern = "%note%",
                        },
                        metadata::MetadataNumberGroupedItemsAction{
                            .target_field = "Track Number",
                            .dialect = {},
                            .group_expression = "%album%|%discnumber%",
                            .start = 1U,
                            .padding = 2U,
                        },
                        metadata::MetadataBlocklistFieldsAction{.fields = {"COMMENT", "ENCODER"}},
                        metadata::MetadataAllowlistFieldsAction{
                            .fields = {"TITLE", "ARTIST", "ALBUM"}},
                        metadata::MetadataConvertRatingAction{
                            .target_field = "FMPS_RATING",
                            .source_field = "RATING",
                            .scale = metadata::PlainRatingScale::hundred},
                    },
            },
        .automatic = true,
    };

    {
        auto opened = persistence::ListRepository::open(database_path);
        require(opened.has_value(), "transformation repository must open");
        auto repository = std::move(*opened);
        const auto stored_chain = repository.upsert_metadata_transformation_chain(expected);
        require(stored_chain.has_value(),
                stored_chain ? "a validated transformation chain must persist atomically"
                             : stored_chain.error().message);
        const auto loaded = repository.load_metadata_transformation_chains();
        require(loaded.has_value() && loaded->size() == 1U && loaded->front().id == chain_id &&
                    loaded->front().chain.schema_version == 1U &&
                    loaded->front().chain.name == expected.chain.name &&
                    loaded->front().automatic &&
                    loaded->front().chain.actions.size() == expected.chain.actions.size(),
                "saved transformation identity, schema, automatic policy, name, and action count "
                "must round trip");
        const auto& chain = loaded->front().chain;
        require(
            require_action<metadata::MetadataSetValuesAction>(chain, 0U, "set action") ==
                    require_action<metadata::MetadataSetValuesAction>(expected.chain, 0U,
                                                                      "expected set action") &&
                require_action<metadata::MetadataAddValuesAction>(chain, 1U, "add action") ==
                    require_action<metadata::MetadataAddValuesAction>(expected.chain, 1U,
                                                                      "expected add action") &&
                require_action<metadata::MetadataRemoveFieldAction>(chain, 2U, "remove action") ==
                    require_action<metadata::MetadataRemoveFieldAction>(expected.chain, 2U,
                                                                        "expected remove action") &&
                require_action<metadata::MetadataTransformValuesAction>(chain, 3U, "trim action") ==
                    require_action<metadata::MetadataTransformValuesAction>(
                        expected.chain, 3U, "expected trim action") &&
                require_action<metadata::MetadataTransformValuesAction>(chain, 4U,
                                                                        "lower action") ==
                    require_action<metadata::MetadataTransformValuesAction>(
                        expected.chain, 4U, "expected lower action") &&
                require_action<metadata::MetadataTransformValuesAction>(chain, 5U,
                                                                        "upper action") ==
                    require_action<metadata::MetadataTransformValuesAction>(
                        expected.chain, 5U, "expected upper action") &&
                require_action<metadata::MetadataTransformValuesAction>(chain, 6U,
                                                                        "capitalize action") ==
                    require_action<metadata::MetadataTransformValuesAction>(
                        expected.chain, 6U, "expected capitalize action") &&
                require_action<metadata::MetadataCopyFieldAction>(chain, 7U, "copy action") ==
                    require_action<metadata::MetadataCopyFieldAction>(expected.chain, 7U,
                                                                      "expected copy action") &&
                require_action<metadata::MetadataSplitValuesAction>(chain, 8U, "split action") ==
                    require_action<metadata::MetadataSplitValuesAction>(expected.chain, 8U,
                                                                        "expected split action") &&
                require_action<metadata::MetadataJoinValuesAction>(chain, 9U, "join action") ==
                    require_action<metadata::MetadataJoinValuesAction>(expected.chain, 9U,
                                                                       "expected join action") &&
                require_action<metadata::MetadataFormatValueAction>(chain, 10U, "format action") ==
                    require_action<metadata::MetadataFormatValueAction>(expected.chain, 10U,
                                                                        "expected format action") &&
                require_action<metadata::MetadataRemoveMatchingValuesAction>(
                    chain, 11U, "remove matching action") ==
                    require_action<metadata::MetadataRemoveMatchingValuesAction>(
                        expected.chain, 11U, "expected remove matching action") &&
                require_action<metadata::MetadataReplaceMatchingValuesAction>(
                    chain, 12U, "replace matching action") ==
                    require_action<metadata::MetadataReplaceMatchingValuesAction>(
                        expected.chain, 12U, "expected replace matching action") &&
                require_action<metadata::MetadataNumberSelectedItemsAction>(chain, 13U,
                                                                            "number action") ==
                    require_action<metadata::MetadataNumberSelectedItemsAction>(
                        expected.chain, 13U, "expected number action") &&
                require_action<metadata::MetadataKeepFirstCharactersAction>(chain, 14U,
                                                                            "keep-first action") ==
                    require_action<metadata::MetadataKeepFirstCharactersAction>(
                        expected.chain, 14U, "expected keep-first action") &&
                require_action<metadata::MetadataRemoveFieldIfAction>(
                    chain, 15U, "conditional-remove action") ==
                    require_action<metadata::MetadataRemoveFieldIfAction>(
                        expected.chain, 15U, "expected conditional-remove action") &&
                require_action<metadata::MetadataCaptureValuesAction>(chain, 16U,
                                                                      "capture action") ==
                    require_action<metadata::MetadataCaptureValuesAction>(
                        expected.chain, 16U, "expected capture action") &&
                require_action<metadata::MetadataCaptureValuesAction>(chain, 17U,
                                                                      "full-path capture action") ==
                    require_action<metadata::MetadataCaptureValuesAction>(
                        expected.chain, 17U, "expected full-path capture action") &&
                require_action<metadata::MetadataCaptureValuesAction>(chain, 18U,
                                                                      "formatted capture action") ==
                    require_action<metadata::MetadataCaptureValuesAction>(
                        expected.chain, 18U, "expected formatted capture action") &&
                require_action<metadata::MetadataCaptureValuesAction>(chain, 19U,
                                                                      "field capture action") ==
                    require_action<metadata::MetadataCaptureValuesAction>(
                        expected.chain, 19U, "expected field capture action") &&
                require_action<metadata::MetadataNumberGroupedItemsAction>(
                    chain, 20U, "grouped numbering action") ==
                    require_action<metadata::MetadataNumberGroupedItemsAction>(
                        expected.chain, 20U, "expected grouped numbering action") &&
                require_action<metadata::MetadataBlocklistFieldsAction>(chain, 21U,
                                                                        "blocklist action") ==
                    require_action<metadata::MetadataBlocklistFieldsAction>(
                        expected.chain, 21U, "expected blocklist action") &&
                require_action<metadata::MetadataAllowlistFieldsAction>(chain, 22U,
                                                                        "allowlist action") ==
                    require_action<metadata::MetadataAllowlistFieldsAction>(
                        expected.chain, 22U, "expected allowlist action") &&
                require_action<metadata::MetadataConvertRatingAction>(chain, 23U,
                                                                      "rating conversion") ==
                    require_action<metadata::MetadataConvertRatingAction>(
                        expected.chain, 23U, "expected rating conversion"),
            "explicit action kinds and exact ordered payloads must round trip");

        auto conflicting = expected;
        conflicting.id = core::StableId::random();
        const auto conflict = repository.upsert_metadata_transformation_chain(conflicting);
        require(!conflict && conflict.error().code == core::ErrorCode::conflict &&
                    repository.load_metadata_transformation_chains()->size() == 1U,
                "an exact duplicate saved name must preserve the preceding transaction");
    }
    {
        auto reopened = persistence::ListRepository::open(database_path);
        require(reopened.has_value(), "transformation repository must reopen");
        auto loaded = reopened->load_metadata_transformation_chains();
        require(loaded.has_value() && loaded->size() == 1U &&
                    loaded->front().chain.actions.size() == expected.chain.actions.size() &&
                    require_action<metadata::MetadataKeepFirstCharactersAction>(
                        loaded->front().chain, 14U, "reopened keep-first action") ==
                        require_action<metadata::MetadataKeepFirstCharactersAction>(
                            expected.chain, 14U, "expected reopened keep-first action") &&
                    require_action<metadata::MetadataRemoveFieldIfAction>(
                        loaded->front().chain, 15U, "reopened conditional-remove action") ==
                        require_action<metadata::MetadataRemoveFieldIfAction>(
                            expected.chain, 15U, "expected reopened conditional-remove action") &&
                    require_action<metadata::MetadataCaptureValuesAction>(
                        loaded->front().chain, 19U, "reopened capture action") ==
                        require_action<metadata::MetadataCaptureValuesAction>(
                            expected.chain, 19U, "expected reopened capture action"),
                "typed numeric and conditional transformations must survive restart");
        auto updated = expected;
        updated.chain.name = "Exact cleanup v2";
        updated.chain.actions = {
            metadata::MetadataJoinValuesAction{.target_field = "Artist", .separator = "; "}};
        require(reopened->upsert_metadata_transformation_chain(updated).has_value(),
                "upserting one stable identity must replace its chain atomically");
    }
    {
        auto reopened = persistence::ListRepository::open(database_path);
        require(reopened.has_value(), "updated transformation repository must reopen");
        auto loaded = reopened->load_metadata_transformation_chains();
        require(loaded.has_value() && loaded->size() == 1U &&
                    loaded->front().chain.name == "Exact cleanup v2" && loaded->front().automatic &&
                    require_action<metadata::MetadataJoinValuesAction>(loaded->front().chain, 0U,
                                                                       "reopened join action")
                            .separator == "; ",
                "updated saved transformations must survive restart");
        require(reopened->remove_metadata_transformation_chain(chain_id).has_value() &&
                    reopened->load_metadata_transformation_chains()->empty(),
                "deleting a saved chain must cascade through its ordered payload");
        const auto missing = reopened->remove_metadata_transformation_chain(chain_id);
        require(!missing && missing.error().code == core::ErrorCode::not_found,
                "deleting an absent saved chain must report not-found");
    }
    cleanup();
}

void output_layout_and_destination_profiles_round_trip_transactionally() {
    namespace core = trackknife::core;
    namespace operations = trackknife::operations;
    namespace persistence = trackknife::persistence;
    const auto database_path =
        std::filesystem::temp_directory_path() /
        ("trackknife-output-profiles-" + core::StableId::random().to_string() + ".sqlite3");
    const auto cleanup = [&database_path] {
        std::error_code ignored;
        std::filesystem::remove(database_path, ignored);
        std::filesystem::remove(database_path.string() + "-wal", ignored);
        std::filesystem::remove(database_path.string() + "-shm", ignored);
    };
    cleanup();

    const auto layout_id = core::StableId::random();
    const auto destination_id = core::StableId::random();
    const persistence::SavedOutputLayoutProfile expected_layout{
        .id = layout_id,
        .profile =
            operations::OutputLayoutProfile{
                .schema_version = 1U,
                .name = "Album folders",
                .dialect = {},
                .relative_directory_expression = "%album artist%/%album%",
                .basename_expression = "$num(%tracknumber%,2) - %title%",
                .sanitization_policy = {.name = "linux", .version = 1U},
            },
    };
    const std::string raw_destination{"/srv/music/library-\xff", 20U};
    const persistence::SavedDestinationProfile expected_destination{
        .id = destination_id,
        .profile =
            operations::DestinationProfile{
                .schema_version = 1U,
                .name = "Main library",
                .root_raw_path = raw_destination,
                .containment_policy = {.name = "lexical-beneath-root", .version = 1U},
            },
    };

    {
        auto opened = persistence::ListRepository::open(database_path);
        require(opened.has_value(), "output-profile repository must open");
        auto repository = std::move(*opened);
        require(repository.schema_version() == 58U, "the state schema is at its version");
        require(repository.upsert_output_layout_profile(expected_layout).has_value() &&
                    repository.upsert_destination_profile(expected_destination).has_value(),
                "validated output and destination profiles must persist atomically");
        require(repository.load_output_layout_profiles() == std::vector{expected_layout} &&
                    repository.load_destination_profiles() == std::vector{expected_destination},
                "versioned expressions, policies, and raw destination bytes must round trip");

        auto conflicting_layout = expected_layout;
        conflicting_layout.id = core::StableId::random();
        const auto layout_conflict = repository.upsert_output_layout_profile(conflicting_layout);
        require(!layout_conflict && layout_conflict.error().code == core::ErrorCode::conflict &&
                    repository.load_output_layout_profiles() == std::vector{expected_layout},
                "duplicate output-layout names must preserve the preceding transaction");

        auto conflicting_destination = expected_destination;
        conflicting_destination.id = core::StableId::random();
        const auto destination_conflict =
            repository.upsert_destination_profile(conflicting_destination);
        require(!destination_conflict &&
                    destination_conflict.error().code == core::ErrorCode::conflict &&
                    repository.load_destination_profiles() == std::vector{expected_destination},
                "duplicate destination names must preserve the preceding transaction");

        auto invalid_destination = expected_destination;
        invalid_destination.profile.root_raw_path = "relative/library";
        const auto rejected = repository.upsert_destination_profile(invalid_destination);
        require(!rejected && rejected.error().code == core::ErrorCode::invalid_argument &&
                    repository.load_destination_profiles() == std::vector{expected_destination},
                "invalid destination roots must be rejected without changing persisted state");

        auto updated_layout = expected_layout;
        updated_layout.profile.name = "Disc folders";
        updated_layout.profile.relative_directory_expression =
            "%album artist%/%album%/Disc %discnumber%";
        updated_layout.profile.basename_expression = "%tracknumber%. %title%";
        auto updated_destination = expected_destination;
        updated_destination.profile.name = "Archive";
        updated_destination.profile.root_raw_path = "/mnt/archive";
        require(repository.upsert_output_layout_profile(updated_layout).has_value() &&
                    repository.upsert_destination_profile(updated_destination).has_value(),
                "upserting stable profile identities must replace their payloads");
    }
    {
        auto reopened = persistence::ListRepository::open(database_path);
        require(reopened.has_value(), "output-profile repository must reopen");
        auto layouts = reopened->load_output_layout_profiles();
        auto destinations = reopened->load_destination_profiles();
        require(layouts.has_value() && layouts->size() == 1U && layouts->front().id == layout_id &&
                    layouts->front().profile.name == "Disc folders" &&
                    layouts->front().profile.relative_directory_expression ==
                        "%album artist%/%album%/Disc %discnumber%" &&
                    destinations.has_value() && destinations->size() == 1U &&
                    destinations->front().id == destination_id &&
                    destinations->front().profile.name == "Archive" &&
                    destinations->front().profile.root_raw_path == "/mnt/archive",
                "updated output and destination profiles must survive restart");
        require(reopened->remove_output_layout_profile(layout_id).has_value() &&
                    reopened->remove_destination_profile(destination_id).has_value() &&
                    reopened->load_output_layout_profiles()->empty() &&
                    reopened->load_destination_profiles()->empty(),
                "profile deletion must remove exactly the selected stable identities");
        const auto missing_layout = reopened->remove_output_layout_profile(layout_id);
        const auto missing_destination = reopened->remove_destination_profile(destination_id);
        require(!missing_layout && missing_layout.error().code == core::ErrorCode::not_found &&
                    !missing_destination &&
                    missing_destination.error().code == core::ErrorCode::not_found,
                "deleting absent output profiles must report not-found");
    }
    cleanup();
}

void encoder_presets_round_trip_transactionally() {
    namespace core = trackknife::core;
    namespace convert = trackknife::convert;
    namespace persistence = trackknife::persistence;
    const auto database_path =
        std::filesystem::temp_directory_path() /
        ("trackknife-encoder-presets-" + core::StableId::random().to_string() + ".sqlite3");
    const auto cleanup = [&database_path] {
        std::error_code ignored;
        std::filesystem::remove(database_path, ignored);
        std::filesystem::remove(database_path.string() + "-wal", ignored);
        std::filesystem::remove(database_path.string() + "-shm", ignored);
    };
    cleanup();

    const auto opus_id = core::StableId::random();
    const auto flac_id = core::StableId::random();
    const persistence::SavedEncoderPreset custom_opus{
        .id = opus_id,
        .preset =
            convert::EncoderPreset{
                .id = opus_id.to_string(),
                .version = 1,
                .display_name = "Opus for the car",
                .codec_name = "libopus",
                .container_name = "opus",
                .file_extension = "opus",
                .lossless = false,
                .bit_rate = 96'000,
                .vbr_quality = std::nullopt,
                .sample_format_hint = {},
            },
    };
    const persistence::SavedEncoderPreset custom_flac{
        .id = flac_id,
        .preset =
            convert::EncoderPreset{
                .id = flac_id.to_string(),
                .version = 1,
                .display_name = "Archive FLAC",
                .codec_name = "flac",
                .container_name = "flac",
                .file_extension = "flac",
                .lossless = true,
                .bit_rate = std::nullopt,
                .vbr_quality = std::nullopt,
                .sample_format_hint = "s32",
            },
    };
    {
        auto opened = persistence::ListRepository::open(database_path);
        require(opened.has_value(), "encoder-preset repository must open");
        auto repository = std::move(*opened);
        require(repository.load_encoder_presets().value_or(
                    std::vector<persistence::SavedEncoderPreset>{{}}) ==
                    std::vector<persistence::SavedEncoderPreset>{},
                "a fresh repository has no encoder presets");
        require(repository.upsert_encoder_preset(custom_opus).has_value(),
                "custom opus preset must save");
        require(repository.upsert_encoder_preset(custom_flac).has_value(),
                "custom flac preset must save");

        auto name_clash = custom_flac;
        name_clash.id = core::StableId::random();
        name_clash.preset.id = name_clash.id.to_string();
        const auto clashed = repository.upsert_encoder_preset(name_clash);
        require(!clashed.has_value() &&
                    clashed.error().code == trackknife::core::ErrorCode::conflict,
                "duplicate encoder preset names must conflict");

        auto both_rates = custom_opus;
        both_rates.preset.vbr_quality = 2;
        require(!repository.upsert_encoder_preset(both_rates).has_value(),
                "bit rate and VBR quality together must be rejected");
    }
    {
        auto reopened = persistence::ListRepository::open(database_path);
        require(reopened.has_value(), "encoder-preset repository must reopen");
        const auto loaded = reopened->load_encoder_presets();
        require(loaded.has_value() && loaded->size() == 2U,
                "both encoder presets survive a restart");
        require(loaded && loaded->front() == custom_flac && loaded->back() == custom_opus,
                "encoder presets reload exactly, ordered by name");
        require(reopened->remove_encoder_preset(opus_id).has_value(),
                "encoder preset removal must succeed");
        const auto missing = reopened->remove_encoder_preset(opus_id);
        require(!missing.has_value() &&
                    missing.error().code == trackknife::core::ErrorCode::not_found,
                "removing a removed encoder preset reports not_found");
        const auto remaining = reopened->load_encoder_presets();
        require(remaining.has_value() && remaining->size() == 1U &&
                    remaining->front() == custom_flac,
                "the remaining encoder preset is intact");
    }
    cleanup();
}

// ADR-0286: every move is journaled, so a client that saw a file at its old
// path and revision -- a tab kept while its window was closed -- finds where
// it went, through each move since; and a replay of a move is a no-op.
void a_move_is_journaled_and_followed_through_later_moves() {
    namespace core = trackknife::core;
    namespace persistence = trackknife::persistence;
    const auto database_path =
        std::filesystem::temp_directory_path() /
        ("trackknife-source-relocation-" + core::StableId::random().to_string() + ".sqlite3");
    const auto cleanup = [&database_path] {
        std::error_code ignored;
        std::filesystem::remove(database_path, ignored);
        std::filesystem::remove(database_path.string() + "-wal", ignored);
        std::filesystem::remove(database_path.string() + "-shm", ignored);
    };
    cleanup();

    const std::string source = std::string{"/music/source-"} + '\xff' + ".flac";
    const std::string middle = std::string{"/music/middle-"} + '\xfe' + ".flac";
    const std::string target = std::string{"/archive/target-"} + '\xfd' + ".flac";
    const core::LocalSourceRevision observed{.device = 10,
                                             .inode = 20,
                                             .size = 30,
                                             .modification_time_seconds = 40,
                                             .modification_time_nanoseconds = 50};
    const core::LocalSourceRevision copied{.device = 11,
                                           .inode = 21,
                                           .size = 31,
                                           .modification_time_seconds = 41,
                                           .modification_time_nanoseconds = 51};
    const core::LocalSourceRevision copied_again{.device = 12,
                                                 .inode = 22,
                                                 .size = 31,
                                                 .modification_time_seconds = 41,
                                                 .modification_time_nanoseconds = 51};
    {
        auto repository = persistence::ListRepository::open(database_path);
        require(repository.has_value(), "the state database opens");
        const persistence::LocalSourceRelocation first{.operation_id = core::StableId::random(),
                                                       .source_reference = source,
                                                       .target_reference = middle,
                                                       .previous_revision = observed,
                                                       .published_revision = copied,
                                                       .published_document = std::nullopt};
        const auto moved = repository->relocate_local_source(first);
        require(moved && !moved->already_applied,
                "a move of a file in no list is journaled all the same");
        const auto replayed = repository->relocate_local_source(first);
        require(replayed && replayed->already_applied, "and its replay is a no-op");
        auto different = first;
        different.target_reference = target;
        const auto conflicting = repository->relocate_local_source(different);
        require(!conflicting && conflicting.error().code == core::ErrorCode::conflict,
                "a replay of the same move to elsewhere is refused");
        require(repository
                    ->relocate_local_source({.operation_id = core::StableId::random(),
                                             .source_reference = middle,
                                             .target_reference = target,
                                             .previous_revision = copied,
                                             .published_revision = copied_again,
                                             .published_document = std::nullopt})
                    .has_value(),
                "the file moves again");
    }
    {
        auto repository = persistence::ListRepository::open(database_path);
        require(repository.has_value(), "the state database opens again");
        const auto changed = observed.size + 1U;
        auto other = observed;
        other.size = changed;
        const auto resolved = repository->resolve_local_source_moves(
            {{source, observed}, {source, other}, {"/music/never-moved.flac", observed}});
        require(resolved && resolved->size() == 3U, "every file asked is answered");
        require((*resolved)[0] && (*resolved)[0]->first == target &&
                    (*resolved)[0]->second == copied_again,
                "a file seen before both moves is found where the last one put it");
        require(!(*resolved)[1], "one seen at another revision is not taken for it");
        require(!(*resolved)[2], "and one never moved is where it was");
    }
    cleanup();
}

} // namespace

namespace {

// The foreign-key check guards migrations, and runs only when one ran. The
// engine opens the database for every library request; checking the whole of
// it each time cost half a second per request on a 66,000-track library.
// Shown by a reference no migration made: opening an up-to-date database
// does not go looking for it.
void an_up_to_date_database_is_opened_without_rechecking_it() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("trackknife-current-schema-" + std::to_string(::getpid()) + ".sqlite3");
    std::filesystem::remove(path);
    require(trackknife::persistence::ListRepository::open(path).has_value(),
            "a fresh database is created at the current schema");
    sqlite3* raw = nullptr;
    require(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "the database opens directly");
    require(sqlite3_exec(raw,
                         "INSERT INTO engine_list_items(list_id,position,entry_id,raw_path) "
                         "VALUES('no-such-list',0,'entry',x'2f')",
                         nullptr, nullptr, nullptr) == SQLITE_OK,
            "a stray reference is written with foreign keys off");
    sqlite3_close(raw);
    require(trackknife::persistence::ListRepository::open(path).has_value(),
            "an up-to-date database opens without a full foreign-key check");
    std::filesystem::remove(path);
}

// ADR-0233: the engine's own lists. Every write carries the revision it was
// made against, so two clients editing one list cannot silently undo each
// other; "Save" is turning a working list into a saved one.
void engine_lists_round_trip_and_refuse_stale_writes() {
    namespace persistence = trackknife::persistence;
    using trackknife::core::ErrorCode;
    using trackknife::core::StableId;
    const auto path = std::filesystem::temp_directory_path() /
                      ("trackknife-engine-lists-" + std::to_string(::getpid()) + ".sqlite3");
    std::filesystem::remove(path);
    auto opened = persistence::ListRepository::open(path);
    require(opened.has_value(), "the repository opens");
    auto& repository = *opened;
    require(repository.load_engine_lists() && repository.load_engine_lists()->empty(),
            "there are no lists at first");

    persistence::EngineListItem cue{
        .entry_id = StableId::random(),
        .raw_path = std::string{"/music/Album/album.flac\xff", 24},
        .logical_reference = std::string{"cue:1"},
        .segment = persistence::ListItemSegment{.start_sample = 44'100, .end_sample = 88'200},
        .source_selection = persistence::ListItemSourceSelection{.audio_stream_index = 1,
                                                                 .subsong_index = std::nullopt},
        .duration_ms = 1'000,
        .title = "One",
        .artist = "Someone",
        .album = "Album",
        .album_artist = "Various",
        .date = "1999",
        .replay_gain = persistence::ListItemReplayGain{.track_gain_db = -7.25,
                                                       .track_peak = 0.98,
                                                       .album_gain_db = std::nullopt,
                                                       .album_peak = std::nullopt}};
    persistence::EngineListItem plain{.entry_id = StableId::random(),
                                      .raw_path = "/music/Album/02.flac",
                                      .logical_reference = std::nullopt,
                                      .segment = std::nullopt,
                                      .source_selection = std::nullopt,
                                      .duration_ms = std::nullopt,
                                      .title = {},
                                      .artist = {},
                                      .album = {},
                                      .album_artist = {},
                                      .date = {},
                                      .replay_gain = std::nullopt};
    const auto id = StableId::random();
    auto created = repository.save_engine_list(id, "Untitled", persistence::EngineListKind::working,
                                               {cue, plain}, 0U, 1'000);
    require(created && created->revision == 1U && created->tracks == 2U &&
                created->kind == persistence::EngineListKind::working,
            "a working list is created at revision 1");
    auto loaded = repository.load_engine_list(id);
    require(loaded && *loaded && (*loaded)->items == std::vector{cue, plain},
            "every field of every entry comes back, raw bytes and all, in order");
    require(repository
                    .save_engine_list(id, "Untitled", persistence::EngineListKind::working, {}, 0U,
                                      1'100)
                    .error()
                    .code == ErrorCode::conflict,
            "creating it again is a conflict");

    // Saved, by name: the same list, now kept.
    auto saved = repository.save_engine_list(id, "Road trip", persistence::EngineListKind::saved,
                                             {plain, cue}, 1U, 2'000);
    require(saved && saved->revision == 2U && saved->kind == persistence::EngineListKind::saved &&
                saved->name == "Road trip" && saved->modified_ms == 2'000,
            "saving promotes it, and the revision goes up");
    require((*repository.load_engine_list(id))->items == std::vector{plain, cue},
            "and the new order is the stored one");

    // A second client still holding revision 1 cannot overwrite that.
    auto stale =
        repository.save_engine_list(id, "Old", persistence::EngineListKind::saved, {}, 1U, 2'100);
    require(!stale && stale.error().code == ErrorCode::conflict,
            "a write against an old revision is refused");
    require(repository.rename_engine_list(id, "Old", 1U, 2'100).error().code == ErrorCode::conflict,
            "so is a rename");
    require(repository.delete_engine_list(id, 1U).error().code == ErrorCode::conflict,
            "and a delete");
    // "Keep mine": written regardless.
    auto forced = repository.save_engine_list(id, "Mine", persistence::EngineListKind::saved,
                                              {plain}, std::nullopt, 2'200);
    require(forced && forced->revision == 3U && forced->tracks == 1U,
            "without a revision it is written whatever changed");

    auto renamed = repository.rename_engine_list(id, "Mine, renamed", 3U, 2'300);
    require(renamed && renamed->revision == 4U && renamed->name == "Mine, renamed",
            "a rename is a write like any other");
    require(repository.rename_engine_list(StableId::random(), "x", std::nullopt, 0).error().code ==
                ErrorCode::not_found,
            "renaming a list that is not there says so");

    const auto other = StableId::random();
    require(repository
                .save_engine_list(other, "Another", persistence::EngineListKind::saved, {cue},
                                  std::nullopt, 3'000)
                .has_value(),
            "a second list");
    const auto all = repository.load_engine_lists();
    require(all && all->size() == 2U && all->front().name == "Another", "lists come by name");

    require(!repository.save_engine_list(StableId::random(), "Twice",
                                         persistence::EngineListKind::working, {plain, plain},
                                         std::nullopt, 0),
            "one entry identity twice in a list is refused");
    require(!repository.save_engine_list(StableId::random(), "",
                                         persistence::EngineListKind::working, {}, std::nullopt, 0),
            "a list needs a name");

    auto deleted = repository.delete_engine_list(id, 4U);
    require(deleted && *deleted, "deleting at the current revision works");
    require(repository.load_engine_list(id) && !*repository.load_engine_list(id),
            "and the list is gone");
    auto again = repository.delete_engine_list(id, std::nullopt);
    require(again && !*again, "deleting what is not there says so, without failing");
    sqlite3* raw = nullptr;
    require(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "the database opens directly");
    sqlite3_stmt* count = nullptr;
    sqlite3_prepare_v2(raw, "SELECT count(*) FROM engine_list_items", -1, &count, nullptr);
    require(sqlite3_step(count) == SQLITE_ROW && sqlite3_column_int(count, 0) == 1,
            "a deleted list's entries go with it");
    sqlite3_finalize(count);
    sqlite3_close(raw);
    std::filesystem::remove(path);
}

// ADR-0256: a list is changed by edits, in one transaction, against the
// revision it was read at.
void engine_lists_are_edited_in_place() {
    namespace persistence = trackknife::persistence;
    using persistence::ListEdit;
    using trackknife::core::ErrorCode;
    using trackknife::core::StableId;
    const auto path = std::filesystem::temp_directory_path() /
                      ("trackknife-engine-list-edits-" + std::to_string(::getpid()) + ".sqlite3");
    std::filesystem::remove(path);
    auto opened = persistence::ListRepository::open(path);
    require(opened.has_value(), "the repository opens");
    auto& repository = *opened;
    std::vector<persistence::EngineListItem> items;
    for (int index = 0; index < 5; ++index) {
        persistence::EngineListItem made;
        made.raw_path = "/music/" + std::to_string(index) + ".flac";
        made.title = "Track " + std::to_string(index);
        items.push_back(std::move(made));
    }
    const auto id = StableId::random();
    require(repository
                .save_engine_list(id, "Edited", persistence::EngineListKind::working, items, 0U, 1)
                .has_value(),
            "a list to edit");

    // Taken from the middle: the rest keep their places.
    auto removed = repository.edit_engine_list(id, 1U,
                                               {ListEdit{.kind = ListEdit::Kind::remove,
                                                         .entries = {items[2].entry_id},
                                                         .items = {},
                                                         .after = {}}},
                                               2);
    const std::vector expected_after_removal{items[0], items[1], items[3], items[4]};
    require(removed && removed->summary.revision == 2U && removed->items == expected_after_removal,
            "a removal answers the list as it is, at the next revision");
    require((*repository.load_engine_list(id))->items == expected_after_removal,
            "and is what is stored");

    // A new one at the start, and the last moved after the first.
    persistence::EngineListItem fresh;
    fresh.raw_path = "/music/new.flac";
    fresh.album_artist = "Someone";
    auto changed = repository.edit_engine_list(
        id, 2U,
        {ListEdit{.kind = ListEdit::Kind::insert, .entries = {}, .items = {fresh}, .after = {}},
         ListEdit{.kind = ListEdit::Kind::move,
                  .entries = {items[4].entry_id},
                  .items = {},
                  .after = fresh.entry_id}},
        3);
    const std::vector expected{fresh, items[4], items[0], items[1], items[3]};
    require(changed && changed->summary.revision == 3U && changed->items == expected &&
                (*repository.load_engine_list(id))->items == expected,
            "inserts and moves apply in order");
    // After a gap left by the removal, positions are still in order: a
    // removal and an insert in the same list read back as written.
    auto after_gap = repository.edit_engine_list(id, 3U,
                                                 {ListEdit{.kind = ListEdit::Kind::remove,
                                                           .entries = {items[0].entry_id},
                                                           .items = {},
                                                           .after = {}},
                                                  ListEdit{.kind = ListEdit::Kind::move,
                                                           .entries = {fresh.entry_id},
                                                           .items = {},
                                                           .after = items[3].entry_id}},
                                                 4);
    require(after_gap && (*repository.load_engine_list(id))->items ==
                             std::vector{items[4], items[1], items[3], fresh},
            "a mixed edit is stored in its order");

    auto stale = repository.edit_engine_list(id, 2U,
                                             {ListEdit{.kind = ListEdit::Kind::remove,
                                                       .entries = {items[1].entry_id},
                                                       .items = {},
                                                       .after = {}}},
                                             5);
    require(!stale && stale.error().code == ErrorCode::conflict,
            "an edit against an old revision is a conflict");

    // Appends, in two inserts chained one after the other: added where the
    // list ends, the rest untouched.
    persistence::EngineListItem tail_one;
    tail_one.raw_path = "/music/tail-1.flac";
    persistence::EngineListItem tail_two;
    tail_two.raw_path = "/music/tail-2.flac";
    persistence::EngineListItem tail_three;
    tail_three.raw_path = "/music/tail-3.flac";
    auto appended = repository.edit_engine_list(id, 4U,
                                                {ListEdit{.kind = ListEdit::Kind::insert,
                                                          .entries = {},
                                                          .items = {tail_one, tail_two},
                                                          .after = fresh.entry_id},
                                                 ListEdit{.kind = ListEdit::Kind::insert,
                                                          .entries = {},
                                                          .items = {tail_three},
                                                          .after = tail_two.entry_id}},
                                                5);
    const std::vector after_appends{items[4], items[1], items[3],  fresh,
                                    tail_one, tail_two, tail_three};
    require(appended && appended->items == after_appends &&
                (*repository.load_engine_list(id))->items == after_appends,
            "appends land at the end");
    auto quiet = repository.edit_engine_list(id, 5U,
                                             {ListEdit{.kind = ListEdit::Kind::insert,
                                                       .entries = {},
                                                       .items = {tail_one},
                                                       .after = tail_three.entry_id}},
                                             6, std::nullopt, std::nullopt, false);
    require(!quiet && (*repository.load_engine_list(id))->items == after_appends,
            "an append of an entry already held is refused, nothing written");
    persistence::EngineListItem tail_four;
    tail_four.raw_path = "/music/tail-4.flac";
    auto without = repository.edit_engine_list(id, 5U,
                                               {ListEdit{.kind = ListEdit::Kind::insert,
                                                         .entries = {},
                                                         .items = {tail_four},
                                                         .after = tail_three.entry_id}},
                                               6, std::nullopt, std::nullopt, false);
    require(without && without->items.empty() && without->summary.tracks == 8U,
            "without items asked for, only the summary comes back");
    // A move near the end rewrites from there; the order read back is right.
    auto near_end = repository.edit_engine_list(id, 6U,
                                                {ListEdit{.kind = ListEdit::Kind::move,
                                                          .entries = {tail_four.entry_id},
                                                          .items = {},
                                                          .after = fresh.entry_id}},
                                                7);
    const std::vector after_move{items[4],  items[1], items[3], fresh,
                                 tail_four, tail_one, tail_two, tail_three};
    require(near_end && (*repository.load_engine_list(id))->items == after_move,
            "a move keeps the lead in place and writes the rest in order");
    auto unknown = repository.edit_engine_list(id, 7U,
                                               {ListEdit{.kind = ListEdit::Kind::remove,
                                                         .entries = {items[1].entry_id},
                                                         .items = {},
                                                         .after = {}},
                                                ListEdit{.kind = ListEdit::Kind::remove,
                                                         .entries = {StableId::random()},
                                                         .items = {},
                                                         .after = {}}},
                                               5);
    require(!unknown && unknown.error().code == ErrorCode::not_found &&
                (*repository.load_engine_list(id))->items.size() == 8U &&
                (*repository.load_engine_lists()).front().revision == 7U,
            "an edit naming an unknown entry writes nothing, not even its first part");
    require(repository.edit_engine_list(StableId::random(), 1U, {}, 5).error().code ==
                ErrorCode::not_found,
            "editing a list that is not there says so");
    std::filesystem::remove(path);
}

} // namespace

// ADR-0259: a saved list's unsaved edits are a draft on the engine.
void a_saved_lists_draft_is_saved_into_it() {
    namespace persistence = trackknife::persistence;
    using persistence::ListEdit;
    using trackknife::core::ErrorCode;
    using trackknife::core::StableId;
    const auto path = std::filesystem::temp_directory_path() /
                      ("trackknife-engine-list-drafts-" + std::to_string(::getpid()) + ".sqlite3");
    std::filesystem::remove(path);
    auto opened = persistence::ListRepository::open(path);
    require(opened.has_value(), "the repository opens");
    auto& repository = *opened;
    const auto item = [](const char* raw_path) {
        return persistence::EngineListItem{.entry_id = StableId::random(),
                                           .raw_path = raw_path,
                                           .logical_reference = std::nullopt,
                                           .segment = std::nullopt,
                                           .source_selection = std::nullopt,
                                           .duration_ms = std::nullopt,
                                           .title = raw_path,
                                           .artist = {},
                                           .album = {},
                                           .album_artist = {},
                                           .date = {},
                                           .replay_gain = std::nullopt};
    };
    const std::vector items{item("/a.flac"), item("/b.flac")};
    const auto saved_id = StableId::random();
    auto saved = repository.save_engine_list(saved_id, "Kept", persistence::EngineListKind::saved,
                                             items, 0U, 1'000);
    require(saved && saved->revision == 1U, "a saved list");
    const auto working_id = StableId::random();
    require(repository
                .save_engine_list(working_id, "Untitled", persistence::EngineListKind::working,
                                  items, 0U, 1'000)
                .has_value(),
            "and a working one");
    require(repository.draft_engine_list(working_id, StableId::random(), 1'100).error().code ==
                ErrorCode::invalid_argument,
            "a working list has no draft");
    require(
        repository.draft_engine_list(StableId::random(), StableId::random(), 1'100).error().code ==
            ErrorCode::not_found,
        "nor does a list that is not there");

    const auto draft_id = StableId::random();
    auto draft = repository.draft_engine_list(saved_id, draft_id, 1'100);
    require(draft && draft->id == draft_id && draft->draft_of == saved_id &&
                draft->draft_base == 1U && draft->kind == persistence::EngineListKind::working &&
                draft->name == "Kept" && draft->tracks == 2U,
            "a draft is a working list naming the list it drafts and its revision");
    require((*repository.load_engine_list(draft_id))->items == items,
            "holding its entries, identities and all");
    auto again = repository.draft_engine_list(saved_id, StableId::random(), 1'200);
    require(again && again->id == draft_id, "a saved list has one draft, whoever asks");
    require(repository.draft_engine_list(draft_id, StableId::random(), 1'200).error().code ==
                ErrorCode::invalid_argument,
            "and a draft has none");

    // Edited, then saved into the list it drafts.
    require(repository
                .edit_engine_list(draft_id, 1U,
                                  {ListEdit{.kind = ListEdit::Kind::remove,
                                            .entries = {items[0].entry_id},
                                            .items = {},
                                            .after = {}}},
                                  1'300, std::string_view{"Kept, shorter"})
                .has_value(),
            "the draft is edited");
    require((*repository.load_engine_list(saved_id))->items == items,
            "the saved list is untouched meanwhile");
    auto committed = repository.commit_engine_list_draft(draft_id, false, 1'400);
    require(committed && committed->id == saved_id && committed->revision == 2U &&
                committed->name == "Kept, shorter" && committed->modified_ms == 1'400 &&
                committed->kind == persistence::EngineListKind::saved && !committed->draft_of,
            "saving a draft writes it to the list it drafts");
    require((*repository.load_engine_list(saved_id))->items == std::vector{items[1]},
            "entries, identities and all");
    require(!*repository.load_engine_list(draft_id), "and the draft is gone");

    // The list moved on meanwhile: a conflict, unless forced.
    auto second = repository.draft_engine_list(saved_id, StableId::random(), 1'500);
    require(second && second->draft_base == 2U, "a new draft begins at the new revision");
    require(repository
                .save_engine_list(saved_id, "Kept", persistence::EngineListKind::saved, items, 2U,
                                  1'600)
                .has_value(),
            "someone saves the list itself");
    require(repository.commit_engine_list_draft(second->id, false, 1'700).error().code ==
                ErrorCode::conflict,
            "saving the draft then is a conflict");
    require(repository.load_engine_list(second->id)->has_value(), "which keeps the draft");
    require(repository.commit_engine_list_draft(second->id, true, 1'800).has_value(),
            "kept regardless, when asked");
    require(repository.commit_engine_list_draft(saved_id, false, 1'900).error().code ==
                ErrorCode::invalid_argument,
            "a list that is no draft cannot be saved as one");

    // Deleting a saved list deletes its draft.
    auto third = repository.draft_engine_list(saved_id, StableId::random(), 2'000);
    require(third.has_value(), "a third draft");
    require(repository.delete_engine_list(saved_id, std::nullopt) == true, "the list is deleted");
    require(!*repository.load_engine_list(third->id), "and its draft with it");
    std::filesystem::remove(path);
}

// ADR-0286: schema 56 sets the working lists and drafts aside -- saved
// playlists stay -- and the down migration puts them back.
// ADR-0339: melody-cli reads the naming layouts Melody saves, by name, with
// SQLite alone -- the same names and expressions, and nothing written.
void melody_cli_reads_the_layouts_melody_saves() {
    namespace core = trackknife::core;
    namespace operations = trackknife::operations;
    namespace persistence = trackknife::persistence;
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-cli-presets-" + core::StableId::random().to_string());
    std::filesystem::create_directories(directory);
    const auto presets = directory / "presets.sqlite";
    const auto missing = trackknife::cli::read_naming_layouts(presets);
    require(!missing && missing.error().code == core::ErrorCode::not_found &&
                !std::filesystem::exists(presets),
            "no presets yet is not found, and nothing made");
    {
        auto repository = persistence::ListRepository::open(presets);
        require(repository.has_value(), "Melody's presets open");
        for (const auto& [name, folders, file] :
             {std::tuple{"Opus copies", "%albumartist%/%album%", "%tracknumber% %title%"},
              std::tuple{"Archive", "$if2(%albumartist%,Various)", "%title%"}}) {
            const persistence::SavedOutputLayoutProfile layout{
                .id = core::StableId::random(),
                .profile = operations::OutputLayoutProfile{.schema_version = 1U,
                                                           .name = name,
                                                           .dialect = {},
                                                           .relative_directory_expression = folders,
                                                           .basename_expression = file,
                                                           .sanitization_policy = {"linux", 1U}},
            };
            require(repository->upsert_output_layout_profile(layout).has_value(),
                    "Melody saves a layout");
        }
    }
    const auto before = std::filesystem::last_write_time(presets);
    const auto read = trackknife::cli::read_naming_layouts(presets);
    require(read && read->size() == 2U && (*read)[0].name == "Archive" &&
                (*read)[0].directory == "$if2(%albumartist%,Various)" &&
                (*read)[0].basename == "%title%" && (*read)[1].name == "Opus copies" &&
                (*read)[1].directory == "%albumartist%/%album%" &&
                (*read)[1].basename == "%tracknumber% %title%",
            "melody-cli reads each by name, with its expressions");
    require(std::filesystem::last_write_time(presets) == before, "and writes nothing");
    std::filesystem::remove_all(directory);
}

namespace {

// Rows of a query, one line each, for comparing two databases' shapes.
[[nodiscard]] std::string query_rows(const std::filesystem::path& path, const char* sql) {
    sqlite3* raw = nullptr;
    require(sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
            "the database opens for reading");
    std::string rows;
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(raw, sql, -1, &statement, nullptr) == SQLITE_OK,
            "the query prepares");
    while (sqlite3_step(statement) == SQLITE_ROW) {
        for (int column = 0; column < sqlite3_column_count(statement); ++column) {
            const auto* text = sqlite3_column_text(statement, column);
            rows += text == nullptr ? "NULL" : reinterpret_cast<const char*>(text);
            rows += '|';
        }
        rows += '\n';
    }
    sqlite3_finalize(statement);
    sqlite3_close(raw);
    return rows;
}

[[nodiscard]] std::string schema_shape(const std::filesystem::path& path) {
    return query_rows(path, "SELECT m.name, p.name, p.type, p.\"notnull\", p.dflt_value, p.pk "
                            "FROM sqlite_schema m JOIN pragma_table_info(m.name) p "
                            "WHERE m.type = 'table' ORDER BY m.name, p.cid") +
           query_rows(path, "SELECT m.name, f.id, f.seq, f.\"table\", f.\"from\", f.\"to\", "
                            "f.on_update, f.on_delete FROM sqlite_schema m "
                            "JOIN pragma_foreign_key_list(m.name) f WHERE m.type = 'table' "
                            "ORDER BY m.name, f.id, f.seq") +
           query_rows(path, "SELECT type, name, tbl_name, sql FROM sqlite_schema "
                            "WHERE type IN ('index', 'trigger', 'view') ORDER BY name");
}

// ADR-0354, amendment of 2026-11-18: a database at the version before is
// upgraded where it is, its rows kept, after a copy of it is kept beside it;
// the result has the shape of one made new. One older than that is refused
// and left exactly as it was, with no copy made.
void a_database_from_the_version_before_is_upgraded_after_a_copy() {
    namespace persistence = trackknife::persistence;
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-schema-upgrade-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto path = directory / "lists.sqlite";
    const auto fresh = directory / "fresh.sqlite";

    std::ifstream fixture{MELODY_STATE_SCHEMA_PREVIOUS};
    const std::string previous{std::istreambuf_iterator<char>{fixture}, {}};
    require(!previous.empty(), "the previous schema's script is read");
    {
        sqlite3* raw = nullptr;
        require(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "a database is made");
        require(sqlite3_exec(raw,
                             "CREATE TABLE schema_version (version INTEGER NOT NULL "
                             "CHECK(version >= 0)); INSERT INTO schema_version VALUES (0);",
                             nullptr, nullptr, nullptr) == SQLITE_OK &&
                    sqlite3_exec(raw, previous.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
                "it is given the previous schema");
        require(sqlite3_exec(raw,
                             "INSERT INTO local_library_roots(raw_path, available) "
                             "VALUES (x'2f6d75736963', 1);",
                             nullptr, nullptr, nullptr) == SQLITE_OK,
                "and a row");
        sqlite3_close(raw);
    }
    require(persistence::ListRepository::open(fresh).has_value(), "a new database is made");

    auto upgraded = persistence::ListRepository::open(path);
    require(upgraded && upgraded->schema_version() == 58U,
            "the database from the version before opens, upgraded");
    upgraded = std::unexpected(trackknife::core::Error{});
    require(query_rows(path, "SELECT hex(raw_path), available FROM local_library_roots") ==
                "2F6D75736963|1|\n",
            "its rows are kept");
    require(schema_shape(path) == schema_shape(fresh),
            "it has the tables, columns, keys and indexes of a new one");
    const auto copy = std::filesystem::path{path.native() + ".v57-backup"};
    require(query_rows(copy, "SELECT version FROM schema_version") == "57|\n" &&
                query_rows(copy, "SELECT hex(raw_path) FROM local_library_roots") ==
                    "2F6D75736963|\n",
            "a copy at the version before is kept beside it");
    require(persistence::ListRepository::open(path).has_value() &&
                !std::filesystem::exists(path.native() + ".v57-backup-2"),
            "opening it again copies nothing");

    const auto old = directory / "old.sqlite";
    {
        sqlite3* raw = nullptr;
        require(sqlite3_open(old.c_str(), &raw) == SQLITE_OK &&
                    sqlite3_exec(raw,
                                 "CREATE TABLE schema_version (version INTEGER NOT NULL); "
                                 "INSERT INTO schema_version VALUES (56);",
                                 nullptr, nullptr, nullptr) == SQLITE_OK,
                "an older database is made");
        sqlite3_close(raw);
    }
    const auto refused = persistence::ListRepository::open(old);
    require(!refused && refused.error().message.find("schema 56") != std::string::npos,
            "one older than the version before is refused, saying which it is");
    require(query_rows(old, "SELECT version FROM schema_version") == "56|\n" &&
                !std::filesystem::exists(old.native() + ".v56-backup"),
            "and left as it was, with no copy");
    std::filesystem::remove_all(directory);
}

} // namespace

int main() {
    saved_searches_are_persistent_and_conflict_checked();
    local_listening_history_is_monotonic_and_persistent();
    local_listening_occurrences_are_idempotent_and_source_qualified();
    metadata_transformation_chains_round_trip_transactionally();
    output_layout_and_destination_profiles_round_trip_transactionally();
    melody_cli_reads_the_layouts_melody_saves();
    encoder_presets_round_trip_transactionally();
    a_move_is_journaled_and_followed_through_later_moves();
    an_up_to_date_database_is_opened_without_rechecking_it();
    a_database_from_the_version_before_is_upgraded_after_a_copy();
    engine_lists_round_trip_and_refuse_stale_writes();
    engine_lists_are_edited_in_place();
    a_saved_lists_draft_is_saved_into_it();
    return EXIT_SUCCESS;
}
