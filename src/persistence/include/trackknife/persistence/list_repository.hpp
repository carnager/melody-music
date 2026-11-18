// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/convert/preset.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/transformation.hpp"
#include "trackknife/operations/output_path_plan.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace trackknife::persistence {

enum class ListKind : std::uint8_t { scratch, saved };

struct SnapshotField {
    std::string name;
    std::string value;
    std::string native_name{};
    metadata::FieldProvenance provenance{metadata::FieldProvenance::cached_snapshot};
    std::optional<std::string> language{};
    std::optional<std::string> description{};

    friend bool operator==(const SnapshotField&, const SnapshotField&) = default;
};

struct ListItemSegment {
    std::int64_t start_sample{0};
    std::optional<std::int64_t> end_sample;

    friend bool operator==(const ListItemSegment&, const ListItemSegment&) = default;
};

struct ListItemSourceSelection {
    std::optional<int> audio_stream_index;
    std::optional<int> subsong_index;

    friend bool operator==(const ListItemSourceSelection&,
                           const ListItemSourceSelection&) = default;
};

struct ListItem {
    // ADR-0221: identity of this entry within its list, distinct from the
    // track it points at. Assigned on construction so every in-memory entry
    // is addressable immediately, replaced by the persisted value on load,
    // and preserved across reordering because ordering lives in the separate
    // position column.
    //
    // Deliberately excluded from equality below: two entries for the same
    // track are equal in value and distinct in identity. Comparisons in this
    // codebase ask whether two rows describe the same track, and that meaning
    // must not change.
    core::StableId entry_id{core::StableId::random()};
    std::optional<core::StableId> profile_id;
    // MPD URIs and local raw OS paths are stored as SQLite BLOBs. No UTF-8 or
    // URL interpretation occurs at this boundary.
    std::string source_reference;
    // Opaque logical identity (for example, cue sheet + file/track indexes)
    // remains distinct from the physical source and survives duplicate paths.
    std::optional<std::string> logical_reference;
    std::optional<ListItemSegment> segment;
    // Explicit decoder selection for independently playable content inside a
    // physical source. Ordinary rows leave this absent.
    std::optional<ListItemSourceSelection> source_selection;
    std::optional<std::int64_t> duration_ms;
    std::optional<core::LocalSourceRevision> source_revision{};
    std::vector<SnapshotField> fields;

    // Value equality over everything but entry_id; see the note above.
    friend bool operator==(const ListItem& left, const ListItem& right) {
        return std::tie(left.profile_id, left.source_reference, left.logical_reference,
                        left.segment, left.source_selection, left.duration_ms, left.source_revision,
                        left.fields) == std::tie(right.profile_id, right.source_reference,
                                                 right.logical_reference, right.segment,
                                                 right.source_selection, right.duration_ms,
                                                 right.source_revision, right.fields);
    }
};

struct ListDocument {
    core::StableId id;
    ListKind kind{ListKind::scratch};
    std::string name;
    bool pinned{false};
    bool dirty{false};
    std::vector<ListItem> items;
    // ADR-0227, ADR-0234: the engine whose files these are, by the id it
    // keeps; empty for this computer's. Its paths name files on that
    // engine's machine, and it plays there. "remote" is the remote of an
    // older release's lists, until that engine has said who it is.
    std::string engine{};

    friend bool operator==(const ListDocument&, const ListDocument&) = default;
};

// A list as a workspace save hands it over: always its header -- name, kind,
// place, flags -- and its items only when they changed since they were last
// saved. A list left out is deleted.
struct ListDocumentWrite {
    ListDocument document;
    bool items{true};
};

// A ReplayGain given with a list entry rather than read from its file.
struct ListItemReplayGain {
    std::optional<double> track_gain_db;
    std::optional<double> track_peak;
    std::optional<double> album_gain_db;
    std::optional<double> album_peak;

    friend bool operator==(const ListItemReplayGain&, const ListItemReplayGain&) = default;
};

// ADR-0233: one entry of a list, as its engine stores it. The track
// is named by its path on the engine's machine; the entry keeps its identity
// (ADR-0221). Title, artist and album are what the client that saved it
// showed, for a file the library does not index.
struct EngineListItem {
    core::StableId entry_id{core::StableId::random()};
    std::string raw_path;
    std::optional<std::string> logical_reference;
    std::optional<ListItemSegment> segment;
    std::optional<ListItemSourceSelection> source_selection;
    std::optional<std::int64_t> duration_ms;
    std::string title;
    std::string artist;
    std::string album;
    // ADR-0256: what a queue entry needs that the engine cannot read for
    // itself -- the album shuffle groups by album artist and date, and a
    // sidecar's or CUE sheet's ReplayGain (ADR-0139/0141) is not in the file.
    std::string album_artist;
    std::string date;
    std::optional<ListItemReplayGain> replay_gain;

    friend bool operator==(const EngineListItem&, const EngineListItem&) = default;
};

// A working list goes with the tab it was open in; a saved one stays until
// it is deleted. "Save" turns the one into the other.
enum class EngineListKind : std::uint8_t { working, saved };

struct EngineListSummary {
    core::StableId id;
    std::string name;
    EngineListKind kind{EngineListKind::working};
    // Goes up with every write: a client saves against the revision it read.
    std::uint64_t revision{0};
    std::size_t tracks{0};
    std::int64_t modified_ms{0};
    // ADR-0259: a draft -- a working list holding the unsaved edits of the
    // saved list `draft_of`, begun when that was at revision `draft_base`.
    std::optional<core::StableId> draft_of;
    std::uint64_t draft_base{0};

    friend bool operator==(const EngineListSummary&, const EngineListSummary&) = default;
};

struct ListEdit;

struct EngineList {
    EngineListSummary summary;
    std::vector<EngineListItem> items;

    friend bool operator==(const EngineList&, const EngineList&) = default;
};

struct TrackViewPreset {
    std::string binding;
    std::string header_state;

    friend bool operator==(const TrackViewPreset&, const TrackViewPreset&) = default;
};

struct SavedMetadataTransformationChain {
    core::StableId id;
    metadata::MetadataTransformationChain chain;
    bool automatic{false};

    friend bool operator==(const SavedMetadataTransformationChain&,
                           const SavedMetadataTransformationChain&) = default;
};

struct SavedOutputLayoutProfile {
    core::StableId id;
    operations::OutputLayoutProfile profile;

    friend bool operator==(const SavedOutputLayoutProfile&,
                           const SavedOutputLayoutProfile&) = default;
};

struct SavedDestinationProfile {
    core::StableId id;
    operations::DestinationProfile profile;

    friend bool operator==(const SavedDestinationProfile&,
                           const SavedDestinationProfile&) = default;
};

// A user-defined encode target beside the built-in presets. Editing any
// preset saves a new profile; built-ins are immutable.
struct SavedEncoderPreset {
    core::StableId id;
    convert::EncoderPreset preset;

    friend bool operator==(const SavedEncoderPreset&, const SavedEncoderPreset&) = default;
};

// Versioned query definitions; result lists remain independent snapshots.
enum class SavedSearchScope : std::uint8_t { library, current_tab, server };
struct SavedSearch {
    core::StableId id;
    std::string name;
    std::string expression;
    std::string dialect{"tkq-1"};
    SavedSearchScope scope{SavedSearchScope::library};
    std::uint64_t revision{0U}; // zero creates; loaded revisions gate updates/deletion
    friend bool operator==(const SavedSearch&, const SavedSearch&) = default;
};

// Local listening state uses a repository-owned source identity plus decoder
// selection/range. Verified publications preserve identity (ADR-0207).
struct LocalListeningHistory {
    std::string track_hash;
    std::uint64_t play_count{0U};
    std::int64_t last_played_ms{0};
    std::int64_t resume_position_ms{0};
    std::int64_t updated_at_ms{0};

    friend bool operator==(const LocalListeningHistory&, const LocalListeningHistory&) = default;
};

// Pure identity projection shared by repository and consistent query snapshots.
[[nodiscard]] core::Result<std::string> local_listening_observation(const ListItem& source);
[[nodiscard]] std::string local_listening_track_hash(const std::string& source_id,
                                                     const ListItem& source);

struct LocalSourceRelocation {
    core::StableId operation_id;
    std::string source_reference;
    std::string target_reference;
    core::LocalSourceRevision previous_revision;
    core::LocalSourceRevision published_revision;
    // Present only when publication changed embedded content: the library
    // row is then described from it rather than copied from the source's.
    std::optional<metadata::MetadataDocument> published_document;
};

struct LocalSourceRelocationResult {
    bool already_applied{false};

    friend bool operator==(const LocalSourceRelocationResult&,
                           const LocalSourceRelocationResult&) = default;
};

class ListRepository final {
  public:
    ListRepository(ListRepository&&) noexcept;
    ListRepository& operator=(ListRepository&&) noexcept;
    ListRepository(const ListRepository&) = delete;
    ListRepository& operator=(const ListRepository&) = delete;
    ~ListRepository();

    [[nodiscard]] static core::Result<ListRepository> open(const std::filesystem::path& path);

    [[nodiscard]] core::Result<unsigned> schema_version() const;
    // Records a file's move -- for resolve_local_source_moves -- and moves
    // its library row and listening history with it, in one transaction.
    // The operation identity makes recovery's replay a no-op.
    [[nodiscard]] core::Result<LocalSourceRelocationResult>
    relocate_local_source(const LocalSourceRelocation& relocation);
    [[nodiscard]] core::Result<std::vector<SavedMetadataTransformationChain>>
    load_metadata_transformation_chains() const;
    [[nodiscard]] core::Result<void>
    upsert_metadata_transformation_chain(const SavedMetadataTransformationChain& saved_chain);
    [[nodiscard]] core::Result<void> remove_metadata_transformation_chain(const core::StableId& id);
    [[nodiscard]] core::Result<std::vector<SavedOutputLayoutProfile>>
    load_output_layout_profiles() const;
    [[nodiscard]] core::Result<void>
    upsert_output_layout_profile(const SavedOutputLayoutProfile& saved_profile);
    [[nodiscard]] core::Result<void> remove_output_layout_profile(const core::StableId& id);
    [[nodiscard]] core::Result<std::vector<SavedDestinationProfile>>
    load_destination_profiles() const;
    [[nodiscard]] core::Result<void>
    upsert_destination_profile(const SavedDestinationProfile& saved_profile);
    [[nodiscard]] core::Result<void> remove_destination_profile(const core::StableId& id);

    [[nodiscard]] core::Result<std::vector<SavedEncoderPreset>> load_encoder_presets() const;
    [[nodiscard]] core::Result<void> upsert_encoder_preset(const SavedEncoderPreset& saved_preset);
    [[nodiscard]] core::Result<void> remove_encoder_preset(const core::StableId& id);

    [[nodiscard]] core::Result<std::vector<SavedSearch>> load_saved_searches() const;
    [[nodiscard]] core::Result<void> save_search(const SavedSearch& search);
    [[nodiscard]] core::Result<void> remove_search(const SavedSearch& expected);

    [[nodiscard]] core::Result<std::optional<LocalListeningHistory>>
    load_local_listening_history(std::string_view track_hash) const;
    // Read-only lookup: an unseen qualified source has no history; never creates identity rows.
    [[nodiscard]] core::Result<std::optional<LocalListeningHistory>>
    lookup_local_listening_history(const ListItem& source) const;
    // Resolves a revision-qualified local source; may create its durable identity.
    [[nodiscard]] core::Result<std::string> local_listening_key(const ListItem& source);
    // Atomically deduplicates a qualified playback occurrence and increments history.
    [[nodiscard]] core::Result<void> record_local_listen(const ListItem& source,
                                                         core::StableId occurrence_id,
                                                         std::int64_t played_at_ms);
    // Each call records one distinct qualified listen. Callers must serialize
    // delivery and must not retry a write with an ambiguous outcome.
    [[nodiscard]] core::Result<void> record_local_play(std::string_view track_hash,
                                                       std::int64_t played_at_ms);
    [[nodiscard]] core::Result<void> save_local_resume(std::string_view track_hash,
                                                       std::int64_t position_ms,
                                                       std::int64_t updated_at_ms);

    // ADR-0220: whatever the engine needs to remember between runs, stored as
    // its own document under a versioned key. Deliberately opaque here -- the
    // repository is not the place that knows what a queue is.
    [[nodiscard]] core::Result<void> save_engine_state(std::string_view key, std::string_view value,
                                                       std::int64_t updated_at_ms);
    // Absent is an empty optional, not an error: an engine that has never run
    // has nothing to restore, and that is ordinary.
    [[nodiscard]] core::Result<std::optional<std::string>>
    load_engine_state(std::string_view key) const;

    // ADR-0233: lists, the engine's own -- working and saved -- not list_documents,
    // which is Trackknife's workspace and rewritten whole by it.
    [[nodiscard]] core::Result<std::vector<EngineListSummary>> load_engine_lists() const;
    // ADR-0286: where each file now is, by the move journal -- a file moved
    // by this engine since a client saw it at that path and revision; empty
    // for one not moved. What a tab kept in a client follows a move by.
    [[nodiscard]] core::Result<
        std::vector<std::optional<std::pair<std::string, core::LocalSourceRevision>>>>
    resolve_local_source_moves(
        const std::vector<std::pair<std::string, core::LocalSourceRevision>>& sources) const;
    // Empty when there is no such list.
    [[nodiscard]] core::Result<std::optional<EngineList>>
    load_engine_list(const core::StableId& id) const;
    // Creates it, or replaces its name and items. With `expected_revision`
    // the write is refused as a conflict unless the list is still at
    // that revision -- or, for 0, does not exist yet; without, it is written
    // whatever changed meanwhile.
    [[nodiscard]] core::Result<EngineListSummary>
    save_engine_list(const core::StableId& id, std::string_view name, EngineListKind kind,
                     const std::vector<EngineListItem>& items,
                     std::optional<std::uint64_t> expected_revision, std::int64_t now_ms);
    // ADR-0256: applies `edits` in order to the list at `expected_revision`,
    // with its name or kind when given -- "Save" is a change of kind -- in
    // one transaction, and answers the list as it is afterwards -- its items
    // only `with_items`, as reading a long one back costs. Refused
    // as a conflict at any other revision, as not_found without the list,
    // and as apply_list_edits refuses; nothing is written then.
    [[nodiscard]] core::Result<EngineList>
    edit_engine_list(const core::StableId& id, std::uint64_t expected_revision,
                     const std::vector<ListEdit>& edits, std::int64_t now_ms,
                     std::optional<std::string_view> name = std::nullopt,
                     std::optional<EngineListKind> kind = std::nullopt, bool with_items = true);
    [[nodiscard]] core::Result<EngineListSummary>
    rename_engine_list(const core::StableId& id, std::string_view name,
                       std::optional<std::uint64_t> expected_revision, std::int64_t now_ms);
    // False when there was no such list. A saved list's draft goes with it.
    [[nodiscard]] core::Result<bool>
    delete_engine_list(const core::StableId& id, std::optional<std::uint64_t> expected_revision);
    // ADR-0259: the list's revision and the path of each of `entries`, in
    // that order; nothing for an entry it does not hold. Not found without
    // the list.
    [[nodiscard]] core::Result<std::pair<std::uint64_t, std::vector<std::optional<std::string>>>>
    engine_list_entry_paths(const core::StableId& id,
                            const std::vector<core::StableId>& entries) const;
    // ADR-0259: the draft of the saved list `of` -- the one there is, or a
    // new working list `id` with its name and entries, entry identities
    // kept, begun at its revision. Refused as not_found without the list and
    // as invalid_argument for one that is not saved.
    [[nodiscard]] core::Result<EngineListSummary>
    draft_engine_list(const core::StableId& of, const core::StableId& id, std::int64_t now_ms);
    // Writes the draft `id` -- its name and entries -- to the saved list it
    // drafts, and deletes the draft, answering the saved list. Refused as a
    // conflict when the saved list moved on since the draft began, unless
    // `force`; as not_found when it is gone; as invalid_argument for a list
    // that is no draft.
    [[nodiscard]] core::Result<EngineListSummary>
    commit_engine_list_draft(const core::StableId& id, bool force, std::int64_t now_ms);
    // ADR-0233: files moved or renamed. Every entry of every list naming a
    // `from` names its `to` instead, in one transaction; each list changed
    // gets a new revision, and is returned.
    [[nodiscard]] core::Result<std::vector<EngineListSummary>>
    relocate_engine_list_paths(const std::vector<std::pair<std::string, std::string>>& moves,
                               std::int64_t now_ms);

  private:
    struct Impl;
    explicit ListRepository(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::persistence
