// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/replaygain_round.hpp"

#include "cue_items.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/formats/cue_sheet.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/loudness_sidecar.hpp"

#include <format>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

namespace trackknife::engine {
namespace {

using protocol::Json;

[[nodiscard]] core::Error malformed(std::string message) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument, .message = std::move(message), .context = {}};
}

// A CUE track's tags as a list shows them (ADR-0356): the sheet's words where
// it has them, the audio file's own where it has not, and the gains the
// sheet holds.
[[nodiscard]] metadata::MetadataDocument
cue_track_document(const persistence::EngineListItem& item) {
    metadata::MetadataDocument document;
    const auto add = [&document](const std::string_view native, const std::string& value,
                                 const metadata::FieldProvenance provenance) {
        if (value.empty()) {
            return;
        }
        document.fields.push_back(metadata::MetadataField{
            .canonical_name = metadata::resolve_text_property_identity(native).canonical_name,
            .native_name = std::string{native},
            .values = {value},
            .qualifier = {},
            .provenance = provenance});
    };
    using metadata::FieldProvenance;
    add("TITLE", item.title, FieldProvenance::sidecar);
    add("ARTIST", item.artist, FieldProvenance::sidecar);
    add("ALBUM", item.album, FieldProvenance::sidecar);
    add("ALBUMARTIST", item.album_artist, FieldProvenance::sidecar);
    add("DATE", item.date, FieldProvenance::sidecar);
    if (item.replay_gain) {
        const auto& gain = *item.replay_gain;
        const auto db = [](const std::optional<double>& value) {
            return value ? std::format("{:.2f} dB", *value) : std::string{};
        };
        const auto peak = [](const std::optional<double>& value) {
            return value ? std::format("{:.6f}", *value) : std::string{};
        };
        add("REPLAYGAIN_TRACK_GAIN", db(gain.track_gain_db), FieldProvenance::segment);
        add("REPLAYGAIN_TRACK_PEAK", peak(gain.track_peak), FieldProvenance::segment);
        add("REPLAYGAIN_ALBUM_GAIN", db(gain.album_gain_db), FieldProvenance::segment);
        add("REPLAYGAIN_ALBUM_PEAK", peak(gain.album_peak), FieldProvenance::segment);
    }
    return document;
}

// The tracks as sources to stage gains on, with where each one's audio is.
struct Gathered {
    std::vector<metadata::StagedMetadataSource> sources;
    std::vector<loudness::ReplayGainAudio> audio;
    std::vector<loudness::ReplayGainProblem> unreadable;
};

[[nodiscard]] Gathered gather(const std::vector<ReplayGainItem>& items,
                              const core::CancellationToken& cancellation) {
    Gathered gathered;
    gathered.sources.reserve(items.size());
    gathered.audio.reserve(items.size());
    std::unordered_map<std::string, core::Result<formats::ResolvedCueSheet>> sheets;
    std::unordered_map<std::string, std::optional<metadata::MetadataDocument>> embedded;
    const auto left_out = [&gathered](const std::string& raw_path, std::string message) {
        gathered.unreadable.push_back(
            loudness::ReplayGainProblem{.raw_path = raw_path, .message = std::move(message)});
    };
    for (const auto& item : items) {
        if (cancellation.is_cancellation_requested()) {
            break;
        }
        if (item.cue) {
            // ADR-0139: a sheet's track takes its gains in the sheet, read
            // here, where it is (its revision observed as the files are read).
            const auto& cue_path = item.cue->raw_cue_path;
            auto found_sheet = sheets.find(cue_path);
            if (found_sheet == sheets.end()) {
                found_sheet =
                    sheets
                        .emplace(cue_path,
                                 formats::resolve_external_cue_sheet(cue_path, cancellation))
                        .first;
            }
            if (!found_sheet->second) {
                left_out(cue_path, found_sheet->second.error().message);
                continue;
            }
            const auto& sheet = *found_sheet->second;
            const auto track = std::ranges::find_if(
                sheet.tracks, [&item](const formats::ResolvedCueTrack& candidate) {
                    return candidate.cue.file_index == item.cue->file_index &&
                           candidate.cue.track_index == item.cue->track_index;
                });
            if (track == sheet.tracks.end()) {
                left_out(cue_path, "the sheet has no such track any more");
                continue;
            }
            auto found_embedded = embedded.find(track->raw_source_path);
            if (found_embedded == embedded.end()) {
                auto read = metadata::read_local_metadata(track->raw_source_path, cancellation);
                found_embedded =
                    embedded
                        .emplace(track->raw_source_path,
                                 read ? std::optional{std::move(read->document)} : std::nullopt)
                        .first;
            }
            const auto listed = detail::opened_cue_item(
                sheet, *track, found_embedded->second ? &*found_embedded->second : nullptr);
            gathered.sources.push_back(metadata::StagedMetadataSource{
                .raw_path = track->raw_source_path,
                .source_revision = std::nullopt,
                .baseline = cue_track_document(listed),
                .logical_track = true,
                .cue_sheet = metadata::StagedCueSheetBinding{.raw_cue_path = cue_path,
                                                             .cue_revision = std::nullopt,
                                                             .file_index = item.cue->file_index,
                                                             .track_index = item.cue->track_index},
                .logical_identity =
                    metadata::StagedLogicalIdentity{.stream_index = std::nullopt,
                                                    .subsong_index = std::nullopt,
                                                    .start_sample =
                                                        track->sample_range.start_sample,
                                                    .end_sample = track->sample_range.end_sample},
                .needs_metadata_capture = false});
            gathered.audio.push_back(
                loudness::ReplayGainAudio{.selection = {}, .range = track->sample_range});
            continue;
        }
        const auto logical = item.range.has_value() || item.selection.stream_index.has_value() ||
                             item.selection.subsong_index.has_value();
        if (!logical) {
            // Read below, in batches, with the reading told as it goes.
            gathered.sources.push_back(
                metadata::StagedMetadataSource{.raw_path = item.raw_path,
                                               .source_revision = std::nullopt,
                                               .baseline = {},
                                               .logical_track = false,
                                               .cue_sheet = std::nullopt,
                                               .logical_identity = std::nullopt,
                                               .needs_metadata_capture = true});
            gathered.audio.emplace_back();
            continue;
        }
        // ADR-0141: a stream, subsong or range of a file takes its gains in
        // the file's sidecar, keyed by where in the file it is.
        metadata::MetadataDocument baseline;
        std::optional<core::LocalSourceRevision> revision;
        if (auto read = metadata::read_local_metadata(item.raw_path, cancellation)) {
            baseline = std::move(read->document);
            revision = read->source_revision;
        } else if (auto observed = core::observe_local_source_revision(item.raw_path)) {
            revision = *observed;
        } else {
            left_out(item.raw_path, observed.error().message);
            continue;
        }
        gathered.sources.push_back(metadata::StagedMetadataSource{
            .raw_path = item.raw_path,
            .source_revision = revision,
            .baseline = std::move(baseline),
            .logical_track = true,
            .cue_sheet = std::nullopt,
            .logical_identity =
                metadata::StagedLogicalIdentity{
                    .stream_index = item.selection.stream_index,
                    .subsong_index = item.selection.subsong_index,
                    .start_sample =
                        item.range ? std::optional{item.range->start_sample} : std::nullopt,
                    .end_sample = item.range ? item.range->end_sample : std::nullopt},
            .needs_metadata_capture = false});
        gathered.audio.push_back(
            loudness::ReplayGainAudio{.selection = item.selection, .range = item.range});
    }
    return gathered;
}

// The album groups the tracks fall into, by their first track's tags.
void name_groups(ReplayGainRound& round, const loudness::LoudnessGrouping& grouping,
                 const metadata::StagedMetadataSelection& selection,
                 const core::CancellationToken& cancellation, core::Result<void>& failed) {
    std::vector<const metadata::MetadataDocument*> documents;
    documents.reserve(selection.item_count());
    for (std::size_t index = 0U; index < selection.item_count(); ++index) {
        documents.push_back(&selection.source(index).baseline);
    }
    const auto keys = loudness::assign_loudness_groups(grouping, documents, cancellation);
    if (!keys) {
        failed = std::unexpected(keys.error());
        return;
    }
    std::map<std::string, ReplayGainGroup> groups;
    for (std::size_t index = 0U; index < keys->size(); ++index) {
        if (!(*keys)[index]) {
            ++round.track_only;
            continue;
        }
        auto& group = groups[*(*keys)[index]];
        if (group.tracks++ == 0U) {
            const auto& document = *documents[index];
            group.album_artist =
                document.first_effective_value("albumartist")
                    .value_or(document.first_effective_value("artist").value_or(std::string{}));
            group.album = document.first_effective_value("album").value_or(std::string{});
        }
    }
    for (auto& [key, group] : groups) {
        (void)key;
        if (round.groups.size() < replaygain_named_groups) {
            round.groups.push_back(std::move(group));
        } else {
            ++round.more_groups;
        }
    }
}

} // namespace

core::Result<ReplayGainRound> run_replaygain_round(const ReplayGainRequest& request,
                                                   const ReplayGainApplier& apply,
                                                   const ReplayGainReport& report,
                                                   const core::CancellationToken& cancellation) {
    const auto told = [&report](std::string phase, const std::size_t completed,
                                const std::size_t total) {
        if (report) {
            report(
                ReplayGainStep{.phase = std::move(phase), .completed = completed, .total = total});
        }
    };
    told("reading", 0U, request.items.size());
    auto gathered = gather(request.items, cancellation);
    ReplayGainRound round;
    round.unreadable = std::move(gathered.unreadable);
    auto captured = metadata::capture_metadata_sources(
        std::move(gathered.sources), metadata::local_metadata_file_access(), cancellation,
        [&told](const std::size_t read, const std::size_t total) { told("reading", read, total); });
    if (!captured) {
        return std::unexpected(std::move(captured.error()));
    }
    // Said, and left out: a file whose tags were not read has no baseline
    // to write gains onto, and would hold up the rest.
    std::set<std::string> unread;
    for (auto& [raw_path, error] : captured->unreadable) {
        unread.insert(raw_path);
        round.unreadable.push_back(
            loudness::ReplayGainProblem{.raw_path = raw_path, .message = std::move(error.message)});
    }
    std::vector<metadata::StagedMetadataSource> sources;
    std::vector<loudness::ReplayGainAudio> audio;
    sources.reserve(captured->sources.size());
    audio.reserve(captured->sources.size());
    for (std::size_t index = 0U; index < captured->sources.size(); ++index) {
        auto& source = captured->sources[index];
        if (!source.logical_track && unread.contains(source.raw_path)) {
            continue;
        }
        // Gains already in a file's sidecar are part of what it says, as
        // Melody reads it: the same gains again change nothing.
        if (source.source_revision && !source.cue_sheet) {
            const auto sidecar = metadata::read_loudness_sidecar(source.raw_path);
            if (sidecar && *sidecar && (*sidecar)->matches(*source.source_revision)) {
                loudness::project_loudness_sidecar(source.baseline, **sidecar,
                                                   gathered.audio[index].selection,
                                                   gathered.audio[index].range);
            }
        }
        sources.push_back(std::move(source));
        audio.push_back(gathered.audio[index]);
    }
    auto selection = metadata::StagedMetadataSelection::create(std::move(sources), {});
    if (!selection) {
        return std::unexpected(std::move(selection.error()));
    }
    core::Result<void> grouped;
    name_groups(round, request.grouping, *selection, cancellation, grouped);
    if (!grouped) {
        return std::unexpected(std::move(grouped.error()));
    }
    if (request.preview || selection->item_count() == 0U) {
        return round;
    }

    std::vector<std::size_t> items(selection->item_count());
    for (std::size_t index = 0U; index < items.size(); ++index) {
        items[index] = index;
    }
    round.measured_items = items.size();
    told("measuring", 0U, items.size());
    auto measured = loudness::measure_replaygain(
        *selection, metadata::StagedMetadataPatchSet{}, items, audio,
        loudness::ReplayGainSettings{.grouping = request.grouping,
                                     .true_peak = request.true_peak,
                                     .sidecar_only = request.sidecar_only},
        [&told](const loudness::LoudnessScanProgress& progress) {
            told("measuring", progress.completed_items, progress.total_items);
        },
        cancellation);
    if (!measured) {
        return std::unexpected(std::move(measured.error()));
    }
    round.problems = std::move(measured->problems);
    round.rows = std::move(measured->rows);
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::cancelled,
                                           .message = "ReplayGain was stopped",
                                           .context = {}});
    }
    auto staged = loudness::stage_replaygain(*selection, measured->proposals, cancellation);
    if (!staged) {
        return std::unexpected(std::move(staged.error()));
    }
    if (staged->staged_fields == 0U) {
        return round;
    }
    auto plan = metadata::build_metadata_write_plan(
        staged->selection, staged->patches, metadata::local_metadata_file_access(), cancellation,
        metadata::MetadataWritePlanOptions{.sidecar_loudness = request.sidecar_only,
                                           .true_peak_loudness = request.true_peak});
    if (!plan) {
        return std::unexpected(std::move(plan.error()));
    }
    // What was measured is written whole or not at all.
    if (!plan->ready()) {
        for (const auto& source : plan->sources) {
            for (const auto& issue : source.issues) {
                round.problems.push_back(loudness::ReplayGainProblem{
                    .raw_path = source.raw_path, .message = issue.error.message});
            }
        }
        for (const auto& sheet : plan->cue_sheets) {
            for (const auto& issue : sheet.issues) {
                round.problems.push_back(loudness::ReplayGainProblem{
                    .raw_path = sheet.raw_cue_path, .message = issue.error.message});
            }
        }
        for (const auto& sidecar : plan->sidecars) {
            for (const auto& issue : sidecar.issues) {
                round.problems.push_back(loudness::ReplayGainProblem{
                    .raw_path = sidecar.raw_audio_path, .message = issue.error.message});
            }
        }
        round.refused = true;
        return round;
    }
    told("writing", 0U, plan->sources.size() + plan->cue_sheets.size() + plan->sidecars.size());
    auto applied = apply(*plan, [&told](const operations::MetadataApplyProgress& progress) {
        told("writing", progress.completed_sources, progress.total_sources);
    });
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    // Files whose gains went into their tags, a CUE sheet or a sidecar:
    // each counts once, wherever they went.
    const auto count = [&round](const operations::MetadataApplySourceState state,
                                const std::string& raw_path,
                                const std::optional<core::Error>& issue) {
        if (state == operations::MetadataApplySourceState::committed) {
            ++round.written;
        } else if (state == operations::MetadataApplySourceState::failed) {
            ++round.failed;
        }
        if (issue) {
            round.problems.push_back(
                loudness::ReplayGainProblem{.raw_path = raw_path, .message = issue->message});
        }
    };
    for (const auto& source : applied->sources) {
        count(source.state, source.raw_path, source.issue);
    }
    for (const auto& sheet : applied->cue_sheets) {
        count(sheet.state, sheet.raw_cue_path, sheet.issue);
    }
    for (const auto& sidecar : applied->sidecars) {
        count(sidecar.state, sidecar.raw_audio_path, sidecar.issue);
    }
    round.applied = std::move(*applied);
    return round;
}

namespace wire {
namespace {

[[nodiscard]] Json encode_problems(const std::vector<loudness::ReplayGainProblem>& problems) {
    auto encoded = Json::array();
    for (const auto& problem : problems) {
        encoded.push_back(
            Json{{"path",
                  problem.raw_path ? Json(protocol::encode_raw_path(*problem.raw_path)) : Json()},
                 {"message", encode_text(problem.message)}});
    }
    return encoded;
}

[[nodiscard]] core::Result<std::vector<loudness::ReplayGainProblem>>
decode_problems(const Json& value) {
    std::vector<loudness::ReplayGainProblem> problems;
    if (!value.is_array()) {
        return std::unexpected(malformed("problems are a list"));
    }
    for (const auto& entry : value) {
        loudness::ReplayGainProblem problem;
        if (!entry.is_object()) {
            return std::unexpected(malformed("a problem is an object"));
        }
        if (const auto path = entry.find("path"); path != entry.end() && path->is_string()) {
            auto decoded = protocol::decode_raw_path(path->get<std::string>());
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            problem.raw_path = std::move(*decoded);
        }
        auto message = decode_text(entry.value("message", Json("")));
        if (!message) {
            return std::unexpected(std::move(message.error()));
        }
        problem.message = std::move(*message);
        problems.push_back(std::move(problem));
    }
    return problems;
}

[[nodiscard]] std::string grouping_name(const loudness::LoudnessGroupingMode mode) {
    switch (mode) {
    case loudness::LoudnessGroupingMode::track:
        return "track";
    case loudness::LoudnessGroupingMode::selection_album:
        return "album";
    case loudness::LoudnessGroupingMode::format_expression:
        return "expression";
    case loudness::LoudnessGroupingMode::release_merged_discs:
        return "discs";
    case loudness::LoudnessGroupingMode::release:
        break;
    }
    return "release";
}

[[nodiscard]] std::optional<loudness::LoudnessGroupingMode> grouping_mode(const std::string& name) {
    using loudness::LoudnessGroupingMode;
    static const std::map<std::string, LoudnessGroupingMode, std::less<>> modes{
        {"release", LoudnessGroupingMode::release},
        {"discs", LoudnessGroupingMode::release_merged_discs},
        {"album", LoudnessGroupingMode::selection_album},
        {"track", LoudnessGroupingMode::track},
        {"expression", LoudnessGroupingMode::format_expression}};
    const auto found = modes.find(name);
    return found == modes.end() ? std::nullopt : std::optional{found->second};
}

template <typename T>
[[nodiscard]] std::optional<T> optional_number(const Json& object, const char* name) {
    const auto found = object.find(name);
    if (found == object.end() || !found->is_number_integer()) {
        return std::nullopt;
    }
    return found->get<T>();
}

} // namespace

Json encode(const ReplayGainRequest& request) {
    auto items = Json::array();
    for (const auto& item : request.items) {
        const auto bare = !item.cue && !item.range && !item.selection.stream_index &&
                          !item.selection.subsong_index;
        if (bare) {
            items.push_back(protocol::encode_raw_path(item.raw_path));
            continue;
        }
        Json encoded{{"path", protocol::encode_raw_path(item.raw_path)}};
        if (item.cue) {
            encoded["cue"] = Json{{"sheet", protocol::encode_raw_path(item.cue->raw_cue_path)},
                                  {"file", item.cue->file_index},
                                  {"track", item.cue->track_index}};
        }
        if (item.selection.stream_index) {
            encoded["stream"] = *item.selection.stream_index;
        }
        if (item.selection.subsong_index) {
            encoded["subsong"] = *item.selection.subsong_index;
        }
        if (item.range) {
            encoded["start"] = item.range->start_sample;
            if (item.range->end_sample) {
                encoded["end"] = *item.range->end_sample;
            }
        }
        items.push_back(std::move(encoded));
    }
    Json encoded{{"items", std::move(items)},
                 {"grouping", grouping_name(request.grouping.mode)},
                 {"true_peak", request.true_peak},
                 {"sidecar", request.sidecar_only},
                 {"preview", request.preview}};
    if (request.grouping.mode == loudness::LoudnessGroupingMode::format_expression) {
        encoded["expression"] = request.grouping.expression;
    }
    return encoded;
}

core::Result<ReplayGainRequest> decode_replaygain_request(const Json& value) {
    if (!value.is_object()) {
        return std::unexpected(malformed("a ReplayGain round is an object"));
    }
    const auto listed = value.find("items");
    if (listed == value.end() || !listed->is_array() || listed->empty() ||
        listed->size() > replaygain_round_items) {
        return std::unexpected(malformed(std::format(
            "between 1 and {} items (tracks to measure) are required", replaygain_round_items)));
    }
    ReplayGainRequest request;
    request.items.reserve(listed->size());
    const auto path_of = [](const Json& encoded) -> core::Result<std::string> {
        if (!encoded.is_string()) {
            return std::unexpected(malformed("a path is not an encoded path"));
        }
        return protocol::decode_raw_path(encoded.get<std::string>());
    };
    for (const auto& entry : *listed) {
        ReplayGainItem item;
        if (entry.is_string()) {
            auto path = path_of(entry);
            if (!path) {
                return std::unexpected(std::move(path.error()));
            }
            item.raw_path = std::move(*path);
            request.items.push_back(std::move(item));
            continue;
        }
        if (!entry.is_object() || !entry.contains("path")) {
            return std::unexpected(malformed("an item is a path or {path, ...}"));
        }
        auto path = path_of(entry.at("path"));
        if (!path) {
            return std::unexpected(std::move(path.error()));
        }
        item.raw_path = std::move(*path);
        if (const auto cue = entry.find("cue"); cue != entry.end()) {
            if (!cue->is_object() || !cue->contains("sheet")) {
                return std::unexpected(malformed("a CUE track is {sheet, file, track}"));
            }
            auto sheet = path_of(cue->at("sheet"));
            const auto file = optional_number<std::size_t>(*cue, "file");
            const auto track = optional_number<std::size_t>(*cue, "track");
            if (!sheet || !file || !track) {
                return std::unexpected(malformed("a CUE track is {sheet, file, track}"));
            }
            item.cue = ReplayGainItem::Cue{
                .raw_cue_path = std::move(*sheet), .file_index = *file, .track_index = *track};
        }
        item.selection.stream_index = optional_number<int>(entry, "stream");
        item.selection.subsong_index = optional_number<int>(entry, "subsong");
        if (const auto start = optional_number<std::int64_t>(entry, "start")) {
            item.range = formats::SampleRange{
                .start_sample = *start, .end_sample = optional_number<std::int64_t>(entry, "end")};
        }
        request.items.push_back(std::move(item));
    }
    const auto mode = grouping_mode(value.value("grouping", std::string{"release"}));
    if (!mode) {
        return std::unexpected(malformed("grouping is release, discs, album, track or expression"));
    }
    request.grouping.mode = *mode;
    if (*mode == loudness::LoudnessGroupingMode::format_expression) {
        request.grouping.expression = value.value("expression", std::string{});
        if (request.grouping.expression.empty()) {
            return std::unexpected(malformed("an expression grouping needs its expression"));
        }
    }
    request.true_peak = value.value("true_peak", false);
    request.sidecar_only = value.value("sidecar", false);
    request.preview = value.value("preview", false);
    return request;
}

Json encode(const ReplayGainRound& round) {
    auto groups = Json::array();
    for (const auto& group : round.groups) {
        groups.push_back(Json{{"album_artist", encode_text(group.album_artist)},
                              {"album", encode_text(group.album)},
                              {"tracks", group.tracks}});
    }
    auto tracks = Json::array();
    for (const auto& row : round.rows) {
        tracks.push_back(Json{{"path", protocol::encode_raw_path(row.raw_path)},
                              {"title", encode_text(row.title)},
                              {"integrated_lufs", row.integrated_lufs},
                              {"track_gain", row.track_gain},
                              {"track_peak", row.track_peak},
                              {"album_key", encode_text(row.album_key)},
                              {"album_gain", row.album_gain},
                              {"album_peak", row.album_peak},
                              {"status", row.status},
                              {"peak_kind", row.peak_kind}});
    }
    Json encoded{{"groups", std::move(groups)},
                 {"more_groups", round.more_groups},
                 {"track_only", round.track_only},
                 {"unreadable", encode_problems(round.unreadable)},
                 {"measured", round.measured_items},
                 {"tracks", std::move(tracks)},
                 {"problems", encode_problems(round.problems)},
                 {"written", round.written},
                 {"failed", round.failed}};
    if (round.refused) {
        encoded["refused"] = true;
    }
    if (round.applied) {
        encoded["applied"] = wire::encode(*round.applied);
    }
    return encoded;
}

core::Result<ReplayGainRound> decode_replaygain_round(const Json& value) {
    if (!value.is_object()) {
        return std::unexpected(malformed("a ReplayGain round's result is an object"));
    }
    ReplayGainRound round;
    for (const auto& group : value.value("groups", Json::array())) {
        auto artist = decode_text(group.value("album_artist", Json("")));
        auto album = decode_text(group.value("album", Json("")));
        if (!artist || !album) {
            return std::unexpected(malformed("a group is {album_artist, album, tracks}"));
        }
        round.groups.push_back(ReplayGainGroup{.album_artist = std::move(*artist),
                                               .album = std::move(*album),
                                               .tracks = group.value("tracks", std::size_t{0})});
    }
    round.more_groups = value.value("more_groups", std::size_t{0});
    round.track_only = value.value("track_only", std::size_t{0});
    round.measured_items = value.value("measured", std::size_t{0});
    auto unreadable = decode_problems(value.value("unreadable", Json::array()));
    auto problems = decode_problems(value.value("problems", Json::array()));
    if (!unreadable || !problems) {
        return std::unexpected(malformed("a round's problems are [{path?, message}]"));
    }
    round.unreadable = std::move(*unreadable);
    round.problems = std::move(*problems);
    for (const auto& track : value.value("tracks", Json::array())) {
        auto path = protocol::decode_raw_path(track.value("path", std::string{}));
        auto title = decode_text(track.value("title", Json("")));
        auto album_key = decode_text(track.value("album_key", Json("")));
        if (!path || !title || !album_key) {
            return std::unexpected(malformed("a measured track is malformed"));
        }
        round.rows.push_back(loudness::ReplayGainRow{
            .title = std::move(*title),
            .raw_path = std::move(*path),
            .integrated_lufs = track.value("integrated_lufs", std::string{}),
            .track_gain = track.value("track_gain", std::string{}),
            .track_peak = track.value("track_peak", std::string{}),
            .album_key = std::move(*album_key),
            .album_gain = track.value("album_gain", std::string{}),
            .album_peak = track.value("album_peak", std::string{}),
            .status = track.value("status", std::string{}),
            .peak_kind = track.value("peak_kind", std::string{})});
    }
    round.written = value.value("written", std::size_t{0});
    round.failed = value.value("failed", std::size_t{0});
    round.refused = value.value("refused", false);
    if (const auto applied = value.find("applied"); applied != value.end()) {
        auto decoded = decode_apply_result(*applied);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        round.applied = std::move(*decoded);
    }
    return round;
}

core::Result<ReplayGainStep> decode_replaygain_step(const Json& value) {
    if (!value.is_object() || !value.contains("phase")) {
        return std::unexpected(malformed("a ReplayGain step is {phase, completed, total}"));
    }
    return ReplayGainStep{.phase = value.value("phase", std::string{}),
                          .completed = value.value("completed", std::size_t{0}),
                          .total = value.value("total", std::size_t{0})};
}

} // namespace wire

} // namespace trackknife::engine
