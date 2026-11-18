// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/formats/cue_sheet.hpp"

#include "trackknife/core/error.hpp"
#include "trackknife/core/local_sources.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::formats {
namespace {

[[nodiscard]] bool is_ascii_space(const char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

[[nodiscard]] std::string_view trim(const std::string_view value) noexcept {
    std::size_t first = 0U;
    while (first < value.size() && is_ascii_space(value[first])) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && is_ascii_space(value[last - 1U])) {
        --last;
    }
    return value.substr(first, last - first);
}

[[nodiscard]] std::pair<std::string_view, std::string_view>
split_first(const std::string_view value) noexcept {
    const auto clean = trim(value);
    const auto separator = std::find_if(clean.begin(), clean.end(), is_ascii_space);
    const auto offset = static_cast<std::size_t>(separator - clean.begin());
    return {clean.substr(0U, offset), trim(clean.substr(offset))};
}

[[nodiscard]] std::string uppercase_ascii(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(static_cast<char>(std::toupper(byte)));
    }
    return result;
}

[[nodiscard]] core::Error cue_error(const core::ErrorCode code, std::string message,
                                    const std::size_t line = 0U) {
    auto error = core::Error{code, std::move(message), {}};
    if (line != 0U) {
        return std::move(error).with_context("line", std::to_string(line));
    }
    return error;
}

template <typename Integer>
[[nodiscard]] core::Result<Integer>
parse_integer(const std::string_view source, const std::size_t line, const std::string_view label) {
    const auto clean = trim(source);
    Integer result{};
    const auto [end, conversion_error] =
        std::from_chars(clean.data(), clean.data() + clean.size(), result);
    if (clean.empty() || conversion_error != std::errc{} || end != clean.data() + clean.size()) {
        return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                         "CUE " + std::string(label) + " must be a decimal integer",
                                         line));
    }
    return result;
}

[[nodiscard]] core::Result<std::int64_t> parse_cue_time(const std::string_view source,
                                                        const std::size_t line) {
    const auto clean = trim(source);
    const auto first_colon = clean.find(':');
    const auto second_colon =
        first_colon == std::string_view::npos ? first_colon : clean.find(':', first_colon + 1U);
    if (first_colon == std::string_view::npos || second_colon == std::string_view::npos ||
        clean.find(':', second_colon + 1U) != std::string_view::npos) {
        return std::unexpected(
            cue_error(core::ErrorCode::invalid_argument, "CUE time must use MM:SS:FF", line));
    }

    const auto minutes =
        parse_integer<std::int64_t>(clean.substr(0U, first_colon), line, "minutes");
    const auto seconds = parse_integer<std::int64_t>(
        clean.substr(first_colon + 1U, second_colon - first_colon - 1U), line, "seconds");
    const auto frames =
        parse_integer<std::int64_t>(clean.substr(second_colon + 1U), line, "frames");
    if (!minutes || !seconds || !frames) {
        return std::unexpected(!minutes ? minutes.error()
                                        : (!seconds ? seconds.error() : frames.error()));
    }
    if (*minutes < 0 || *seconds < 0 || *seconds >= 60 || *frames < 0 ||
        *frames >= cue_frames_per_second) {
        return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                         "CUE time components are out of range", line));
    }

    constexpr auto frames_per_minute = 60 * cue_frames_per_second;
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if (*minutes > (maximum - (*seconds * cue_frames_per_second) - *frames) / frames_per_minute) {
        return std::unexpected(
            cue_error(core::ErrorCode::limit_exceeded, "CUE time is too large", line));
    }
    return (*minutes * frames_per_minute) + (*seconds * cue_frames_per_second) + *frames;
}

[[nodiscard]] core::Result<std::string>
parse_text(const std::string_view source, const std::size_t line, const std::string_view label) {
    const auto clean = trim(source);
    if (clean.empty()) {
        return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                         "CUE " + std::string(label) + " is empty", line));
    }
    if (clean.front() != '"') {
        return std::string(clean);
    }
    const auto closing_quote = clean.find('"', 1U);
    if (closing_quote == std::string_view::npos ||
        !trim(clean.substr(closing_quote + 1U)).empty()) {
        return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                         "CUE " + std::string(label) + " has invalid quotes",
                                         line));
    }
    return std::string(clean.substr(1U, closing_quote - 1U));
}

struct ParsedFileArguments {
    std::string reference;
    std::string type;
};

[[nodiscard]] core::Result<ParsedFileArguments> parse_file_arguments(const std::string_view source,
                                                                     const std::size_t line) {
    const auto clean = trim(source);
    if (clean.empty()) {
        return std::unexpected(
            cue_error(core::ErrorCode::invalid_argument, "CUE FILE is empty", line));
    }

    if (clean.front() == '"') {
        const auto closing_quote = clean.find('"', 1U);
        if (closing_quote == std::string_view::npos) {
            return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                             "CUE FILE has an unterminated reference", line));
        }
        const auto type = trim(clean.substr(closing_quote + 1U));
        if (type.empty() || std::ranges::any_of(type, is_ascii_space)) {
            return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                             "CUE FILE type is missing or invalid", line));
        }
        return ParsedFileArguments{std::string(clean.substr(1U, closing_quote - 1U)),
                                   uppercase_ascii(type)};
    }

    const auto separator = clean.find_last_of(" \t\r\n");
    if (separator == std::string_view::npos) {
        return std::unexpected(
            cue_error(core::ErrorCode::invalid_argument, "CUE FILE type is missing", line));
    }
    const auto reference = trim(clean.substr(0U, separator));
    const auto type = trim(clean.substr(separator + 1U));
    if (reference.empty() || type.empty() || std::ranges::any_of(type, is_ascii_space)) {
        return std::unexpected(
            cue_error(core::ErrorCode::invalid_argument, "CUE FILE arguments are invalid", line));
    }
    return ParsedFileArguments{std::string(reference), uppercase_ascii(type)};
}

[[nodiscard]] std::optional<std::int64_t> index_one(const CueTrack& track) noexcept {
    const auto found = std::ranges::find(track.indexes, 1, &CueIndex::number);
    if (found == track.indexes.end()) {
        return std::nullopt;
    }
    return found->cue_frame;
}

[[nodiscard]] bool is_audio_track(const CueTrack& track) {
    return uppercase_ascii(track.mode) == "AUDIO";
}

[[nodiscard]] std::optional<std::string> inherit(const std::optional<std::string>& local,
                                                 const std::optional<std::string>& global) {
    return local ? local : global;
}

[[nodiscard]] core::Result<std::int64_t> cue_frame_to_sample(const std::int64_t cue_frame,
                                                             const int sample_rate) {
    if (cue_frame < 0 || sample_rate <= 0) {
        return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                         "CUE frame and sample rate must be positive"));
    }
    const auto whole_seconds = cue_frame / cue_frames_per_second;
    const auto partial_frames = cue_frame % cue_frames_per_second;
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if (whole_seconds > maximum / sample_rate) {
        return std::unexpected(
            cue_error(core::ErrorCode::limit_exceeded, "CUE sample boundary is too large"));
    }
    return (whole_seconds * sample_rate) + ((partial_frames * sample_rate) / cue_frames_per_second);
}

} // namespace

core::Result<CueSheet> parse_cue_sheet(std::string_view source, const CueParseLimits& limits) {
    if (source.size() > limits.source_bytes) {
        return std::unexpected(
            cue_error(core::ErrorCode::limit_exceeded, "CUE source exceeds the byte limit"));
    }
    if (source.starts_with("\xEF\xBB\xBF")) {
        source.remove_prefix(3U);
    }
    if (source.find('\0') != std::string_view::npos) {
        return std::unexpected(
            cue_error(core::ErrorCode::invalid_argument, "CUE source contains a NUL byte"));
    }

    CueSheet sheet;
    CueFile* current_file = nullptr;
    CueTrack* current_track = nullptr;
    std::size_t track_count = 0U;
    std::size_t metadata_count = 0U;
    std::size_t line_number = 0U;
    std::size_t offset = 0U;

    const auto count_metadata = [&]() -> core::Result<void> {
        ++metadata_count;
        if (metadata_count > limits.metadata_fields) {
            return std::unexpected(cue_error(core::ErrorCode::limit_exceeded,
                                             "CUE metadata exceeds the field limit", line_number));
        }
        return {};
    };

    while (offset <= source.size()) {
        ++line_number;
        if (line_number > limits.lines) {
            return std::unexpected(cue_error(core::ErrorCode::limit_exceeded,
                                             "CUE source exceeds the line limit", line_number));
        }
        const auto newline = source.find('\n', offset);
        auto line = source.substr(offset, newline == std::string_view::npos ? source.size() - offset
                                                                            : newline - offset);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1U);
        }
        line = trim(line);
        if (!line.empty()) {
            const auto [raw_directive, argument] = split_first(line);
            const auto directive = uppercase_ascii(raw_directive);

            if (directive == "FILE") {
                const auto parsed = parse_file_arguments(argument, line_number);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                if (sheet.files.size() >= limits.files) {
                    return std::unexpected(cue_error(core::ErrorCode::limit_exceeded,
                                                     "CUE exceeds the file limit", line_number));
                }
                sheet.files.push_back({parsed->reference, parsed->type, {}});
                current_file = &sheet.files.back();
                current_track = nullptr;
            } else if (directive == "TRACK") {
                if (current_file == nullptr) {
                    return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                                     "CUE TRACK appears before FILE", line_number));
                }
                const auto [number_text, remainder] = split_first(argument);
                const auto [mode_text, trailing] = split_first(remainder);
                const auto number = parse_integer<int>(number_text, line_number, "track number");
                if (!number || *number <= 0 || mode_text.empty() || !trailing.empty()) {
                    return std::unexpected(number ? cue_error(core::ErrorCode::invalid_argument,
                                                              "CUE TRACK arguments are invalid",
                                                              line_number)
                                                  : number.error());
                }
                if (!current_file->tracks.empty() &&
                    *number <= current_file->tracks.back().number) {
                    return std::unexpected(
                        cue_error(core::ErrorCode::invalid_argument,
                                  "CUE track numbers must increase within a FILE", line_number));
                }
                ++track_count;
                if (track_count > limits.tracks) {
                    return std::unexpected(cue_error(core::ErrorCode::limit_exceeded,
                                                     "CUE exceeds the track limit", line_number));
                }
                CueTrack parsed_track;
                parsed_track.number = *number;
                parsed_track.mode = uppercase_ascii(mode_text);
                current_file->tracks.push_back(std::move(parsed_track));
                current_track = &current_file->tracks.back();
            } else if (directive == "INDEX") {
                if (current_track == nullptr) {
                    return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                                     "CUE INDEX appears before TRACK",
                                                     line_number));
                }
                const auto [number_text, remainder] = split_first(argument);
                const auto [time_text, trailing] = split_first(remainder);
                const auto number = parse_integer<int>(number_text, line_number, "index number");
                const auto cue_frame = parse_cue_time(time_text, line_number);
                if (!number || !cue_frame || *number < 0 || !trailing.empty()) {
                    return std::unexpected(
                        !number ? number.error()
                                : (!cue_frame ? cue_frame.error()
                                              : cue_error(core::ErrorCode::invalid_argument,
                                                          "CUE INDEX arguments are invalid",
                                                          line_number)));
                }
                if (current_track->indexes.size() >= limits.indexes_per_track) {
                    return std::unexpected(cue_error(core::ErrorCode::limit_exceeded,
                                                     "CUE track exceeds the index limit",
                                                     line_number));
                }
                if (!current_track->indexes.empty() &&
                    (*number <= current_track->indexes.back().number ||
                     *cue_frame < current_track->indexes.back().cue_frame)) {
                    return std::unexpected(cue_error(
                        core::ErrorCode::invalid_argument,
                        "CUE indexes must have increasing numbers and nondecreasing times",
                        line_number));
                }
                current_track->indexes.push_back({*number, *cue_frame});
            } else if (directive == "TITLE" || directive == "PERFORMER" ||
                       directive == "SONGWRITER") {
                const auto value = parse_text(argument, line_number, directive);
                if (!value) {
                    return std::unexpected(value.error());
                }
                const auto counted = count_metadata();
                if (!counted) {
                    return std::unexpected(counted.error());
                }
                auto& metadata =
                    current_track == nullptr ? sheet.metadata : current_track->metadata;
                auto* target = directive == "TITLE"       ? &metadata.title
                               : directive == "PERFORMER" ? &metadata.performer
                                                          : &metadata.songwriter;
                *target = *value;
            } else if (directive == "REM") {
                const auto [name, value] = split_first(argument);
                if (name.empty()) {
                    return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                                     "CUE REM name is empty", line_number));
                }
                const auto counted = count_metadata();
                if (!counted) {
                    return std::unexpected(counted.error());
                }
                auto& metadata =
                    current_track == nullptr ? sheet.metadata : current_track->metadata;
                metadata.remarks.push_back({uppercase_ascii(name), std::string(value)});
            } else if (directive == "PREGAP" || directive == "POSTGAP") {
                if (current_track == nullptr) {
                    return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                                     "CUE gap appears before TRACK", line_number));
                }
                const auto frames = parse_cue_time(argument, line_number);
                if (!frames) {
                    return std::unexpected(frames.error());
                }
                (directive == "PREGAP" ? current_track->pregap_frames
                                       : current_track->postgap_frames) = *frames;
            } else if (directive == "FLAGS") {
                if (current_track == nullptr) {
                    return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                                     "CUE FLAGS appears before TRACK",
                                                     line_number));
                }
                auto remainder = argument;
                while (!trim(remainder).empty()) {
                    const auto [flag, next] = split_first(remainder);
                    const auto counted = count_metadata();
                    if (!counted) {
                        return std::unexpected(counted.error());
                    }
                    current_track->flags.push_back(uppercase_ascii(flag));
                    remainder = next;
                }
            } else if (directive == "ISRC") {
                if (current_track == nullptr) {
                    return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                                     "CUE ISRC appears before TRACK", line_number));
                }
                const auto value = parse_text(argument, line_number, "ISRC");
                if (!value) {
                    return std::unexpected(value.error());
                }
                current_track->isrc = *value;
            } else if (directive == "CATALOG" || directive == "CDTEXTFILE") {
                const auto value = parse_text(argument, line_number, directive);
                if (!value) {
                    return std::unexpected(value.error());
                }
                (directive == "CATALOG" ? sheet.catalog : sheet.cd_text_file) = *value;
            } else {
                sheet.unknown_directives.push_back({line_number, directive, std::string(argument)});
            }
        }

        if (newline == std::string_view::npos) {
            break;
        }
        offset = newline + 1U;
    }

    if (sheet.files.empty()) {
        return std::unexpected(
            cue_error(core::ErrorCode::invalid_argument, "CUE sheet contains no FILE"));
    }
    return sheet;
}

core::Result<std::vector<CueLogicalTrack>> plan_cue_logical_tracks(const CueSheet& sheet) {
    std::vector<CueLogicalTrack> result;
    for (std::size_t file_index = 0U; file_index < sheet.files.size(); ++file_index) {
        const auto& file = sheet.files[file_index];
        std::optional<std::int64_t> previous_start;
        for (std::size_t track_index = 0U; track_index < file.tracks.size(); ++track_index) {
            const auto& track = file.tracks[track_index];
            const auto start = index_one(track);
            if (is_audio_track(track) && !start) {
                return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                                 "CUE AUDIO track " + std::to_string(track.number) +
                                                     " has no INDEX 01"));
            }
            if (!start) {
                continue;
            }
            if (previous_start && *start <= *previous_start) {
                return std::unexpected(
                    cue_error(core::ErrorCode::invalid_argument,
                              "CUE INDEX 01 boundaries must increase within a FILE"));
            }
            previous_start = start;
            if (!is_audio_track(track)) {
                continue;
            }

            std::optional<std::int64_t> end;
            for (auto following = track_index + 1U; following < file.tracks.size(); ++following) {
                end = index_one(file.tracks[following]);
                if (end) {
                    break;
                }
            }
            if (end && *end <= *start) {
                return std::unexpected(
                    cue_error(core::ErrorCode::invalid_argument,
                              "CUE logical track end must follow its INDEX 01 boundary"));
            }

            result.push_back(CueLogicalTrack{
                .file_index = file_index,
                .track_index = track_index,
                .track_number = track.number,
                .raw_file_reference = file.raw_reference,
                .file_type = file.type,
                .start_cue_frame = *start,
                .end_cue_frame = end,
                .title = track.metadata.title,
                .performer = inherit(track.metadata.performer, sheet.metadata.performer),
                .songwriter = inherit(track.metadata.songwriter, sheet.metadata.songwriter),
                .album_title = sheet.metadata.title,
                .album_performer = sheet.metadata.performer,
                .isrc = track.isrc,
                .remarks = sheet.metadata.remarks,
            });
            result.back().remarks.insert(result.back().remarks.end(),
                                         track.metadata.remarks.begin(),
                                         track.metadata.remarks.end());
        }
    }
    return result;
}

core::Result<SampleRange>
cue_track_sample_range(const CueLogicalTrack& track, const int sample_rate,
                       const std::optional<std::int64_t> physical_duration_samples) {
    const auto start = cue_frame_to_sample(track.start_cue_frame, sample_rate);
    if (!start) {
        return std::unexpected(start.error());
    }

    std::optional<std::int64_t> end;
    if (track.end_cue_frame) {
        const auto converted = cue_frame_to_sample(*track.end_cue_frame, sample_rate);
        if (!converted) {
            return std::unexpected(converted.error());
        }
        end = *converted;
    } else {
        end = physical_duration_samples;
    }
    if (end && *end <= *start) {
        return std::unexpected(
            cue_error(core::ErrorCode::invalid_argument, "CUE sample range is empty or reversed"));
    }
    if (physical_duration_samples &&
        (*physical_duration_samples < 0 || *start >= *physical_duration_samples ||
         (end && *end > *physical_duration_samples))) {
        return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                         "CUE sample range lies outside the physical source"));
    }
    return SampleRange{*start, end};
}

core::Result<ResolvedCueSheet>
resolve_external_cue_sheet(std::string raw_cue_path, const core::CancellationToken& cancellation,
                           const CueParseLimits& limits) {
    if (raw_cue_path.empty()) {
        return std::unexpected(
            cue_error(core::ErrorCode::invalid_argument, "CUE sheet path is empty"));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cue_error(core::ErrorCode::cancelled, "CUE resolution cancelled"));
    }

    std::error_code filesystem_error;
    const auto resolved_cue =
        std::filesystem::canonical(std::filesystem::path{raw_cue_path}, filesystem_error);
    if (filesystem_error) {
        return std::unexpected(
            std::move(cue_error(filesystem_error == std::errc::no_such_file_or_directory
                                    ? core::ErrorCode::not_found
                                    : core::ErrorCode::io,
                                "CUE sheet could not be resolved: " + filesystem_error.message()))
                .with_context("path", core::escape_raw_path(raw_cue_path)));
    }
    if (!std::filesystem::is_regular_file(resolved_cue, filesystem_error) || filesystem_error) {
        return std::unexpected(
            std::move(
                cue_error(core::ErrorCode::invalid_argument, "CUE sheet is not a regular file"))
                .with_context("path", core::escape_raw_path(resolved_cue.native())));
    }
    const auto source_size = std::filesystem::file_size(resolved_cue, filesystem_error);
    if (filesystem_error) {
        return std::unexpected(
            std::move(cue_error(core::ErrorCode::io,
                                "CUE sheet size could not be read: " + filesystem_error.message()))
                .with_context("path", core::escape_raw_path(resolved_cue.native())));
    }
    if (source_size > limits.source_bytes ||
        source_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        source_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::unexpected(
            std::move(
                cue_error(core::ErrorCode::limit_exceeded, "CUE source exceeds the byte limit"))
                .with_context("path", core::escape_raw_path(resolved_cue.native())));
    }

    std::string source(static_cast<std::size_t>(source_size), '\0');
    std::ifstream input{resolved_cue, std::ios::binary};
    if (!input || (!source.empty() &&
                   !input.read(source.data(), static_cast<std::streamsize>(source.size())))) {
        return std::unexpected(
            std::move(cue_error(core::ErrorCode::io, "CUE sheet could not be read"))
                .with_context("path", core::escape_raw_path(resolved_cue.native())));
    }
    auto sheet = parse_cue_sheet(source, limits);
    if (!sheet) {
        auto error = std::move(sheet.error());
        error.context.push_back({"path", core::escape_raw_path(resolved_cue.native())});
        return std::unexpected(std::move(error));
    }
    auto logical_tracks = plan_cue_logical_tracks(*sheet);
    if (!logical_tracks) {
        auto error = std::move(logical_tracks.error());
        error.context.push_back({"path", core::escape_raw_path(resolved_cue.native())});
        return std::unexpected(std::move(error));
    }
    if (logical_tracks->empty()) {
        return std::unexpected(
            std::move(cue_error(core::ErrorCode::unsupported,
                                "CUE sheet contains no playable AUDIO tracks"))
                .with_context("path", core::escape_raw_path(resolved_cue.native())));
    }

    struct SourceInfo {
        std::string raw_path;
        int sample_rate{0};
        std::optional<std::int64_t> duration_samples;
    };
    std::vector<std::optional<SourceInfo>> sources(sheet->files.size());
    ResolvedCueSheet result{
        .raw_cue_path = resolved_cue.native(), .physical_sources = {}, .tracks = {}};
    result.tracks.reserve(logical_tracks->size());
    const auto root = resolved_cue.parent_path();

    for (auto& logical : *logical_tracks) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(
                cue_error(core::ErrorCode::cancelled, "CUE resolution cancelled"));
        }
        auto& source_info = sources.at(logical.file_index);
        if (!source_info) {
            const auto candidate =
                (root / std::filesystem::path{logical.raw_file_reference}).native();
            auto contained = core::revalidate_contained_source(root.native(), candidate);
            if (!contained) {
                auto error = std::move(contained.error());
                error.context.push_back({"cue", core::escape_raw_path(resolved_cue.native())});
                return std::unexpected(std::move(error));
            }
            auto decoder = AudioDecoder::open(contained->resolved_path, cancellation);
            if (!decoder) {
                auto error = std::move(decoder.error());
                error.context.push_back({"cue", core::escape_raw_path(resolved_cue.native())});
                return std::unexpected(std::move(error));
            }
            source_info = SourceInfo{.raw_path = contained->resolved_path,
                                     .sample_rate = decoder->output_format().sample_rate,
                                     .duration_samples = decoder->duration_samples()};
            if (std::ranges::find(result.physical_sources, source_info->raw_path) ==
                result.physical_sources.end()) {
                result.physical_sources.push_back(source_info->raw_path);
            }
        }
        auto range = cue_track_sample_range(logical, source_info->sample_rate,
                                            source_info->duration_samples);
        if (!range) {
            auto error = std::move(range.error());
            error.context.push_back({"source", core::escape_raw_path(source_info->raw_path)});
            return std::unexpected(std::move(error));
        }
        std::optional<std::int64_t> duration_ms;
        if (range->end_sample) {
            const auto duration_samples = *range->end_sample - range->start_sample;
            duration_ms =
                (duration_samples / source_info->sample_rate) * 1'000 +
                ((duration_samples % source_info->sample_rate) * 1'000) / source_info->sample_rate;
        }
        result.tracks.push_back(ResolvedCueTrack{.cue = std::move(logical),
                                                 .raw_source_path = source_info->raw_path,
                                                 .sample_range = *range,
                                                 .duration_ms = duration_ms});
    }
    return result;
}

namespace {

constexpr std::array<std::string_view, 4> replay_gain_remark_names{
    "REPLAYGAIN_ALBUM_GAIN",
    "REPLAYGAIN_ALBUM_PEAK",
    "REPLAYGAIN_TRACK_GAIN",
    "REPLAYGAIN_TRACK_PEAK",
};

[[nodiscard]] bool is_replay_gain_remark_name(const std::string_view name) noexcept {
    return std::ranges::find(replay_gain_remark_names, name) != replay_gain_remark_names.end();
}

[[nodiscard]] std::string fixed_decimal_text(const double value, const int precision) {
    std::array<char, 32> buffer{};
    const auto ends = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                    std::chars_format::fixed, precision);
    return std::string{buffer.data(), ends.ptr};
}

struct CueRewriteLine {
    std::size_t begin{0U};
    std::size_t content_end{0U};
    std::size_t end{0U};
    std::size_t leading_whitespace{0U};
    std::ptrdiff_t file_ordinal{-1};
    std::ptrdiff_t track_ordinal{-1};
    std::optional<std::string> replay_gain_name;
};

[[nodiscard]] core::Result<void> validate_replay_gain_field(const CueReplayGainField& field,
                                                            const bool gain,
                                                            const std::string_view label) {
    if (!field.update || !field.value) {
        return {};
    }
    const auto value = *field.value;
    if (!std::isfinite(value) || (gain ? std::abs(value) > 60.0 : value < 0.0)) {
        return std::unexpected(
            cue_error(core::ErrorCode::invalid_argument,
                      "CUE ReplayGain " + std::string(label) + " value is out of range"));
    }
    return {};
}

// The four-name remark values one scope carries, keyed by name. Order
// inside the sheet is deliberately not part of the proof; presence,
// multiplicity, and exact value text are.
[[nodiscard]] std::map<std::string, std::vector<std::string>>
replay_gain_remark_values(const std::vector<CueMetadataField>& remarks) {
    std::map<std::string, std::vector<std::string>> values;
    for (const auto& remark : remarks) {
        if (is_replay_gain_remark_name(remark.name)) {
            values[remark.name].push_back(remark.value);
        }
    }
    return values;
}

void strip_replay_gain_remarks(CueMetadata& metadata) {
    std::erase_if(metadata.remarks, [](const CueMetadataField& remark) {
        return is_replay_gain_remark_name(remark.name);
    });
}

[[nodiscard]] CueSheet stripped_of_replay_gain_remarks(CueSheet sheet) {
    strip_replay_gain_remarks(sheet.metadata);
    for (auto& file : sheet.files) {
        for (auto& track : file.tracks) {
            strip_replay_gain_remarks(track.metadata);
        }
    }
    return sheet;
}

void apply_expected_field(std::map<std::string, std::vector<std::string>>& expected,
                          const std::string_view name, const CueReplayGainField& field,
                          const bool gain) {
    if (!field.update) {
        return;
    }
    if (field.value) {
        expected[std::string(name)] = {gain ? replay_gain_decibel_text(*field.value)
                                            : replay_gain_peak_text(*field.value)};
    } else {
        expected.erase(std::string(name));
    }
}

[[nodiscard]] core::Result<void> verify_cue_replay_gain_rewrite(const CueSheet& original,
                                                                const std::string& rewritten,
                                                                const CueReplayGainUpdate& update,
                                                                const CueParseLimits& limits) {
    const auto fail = [] {
        return std::unexpected(cue_error(core::ErrorCode::conflict,
                                         "CUE ReplayGain rewrite failed its preservation proof"));
    };
    const auto reparsed = parse_cue_sheet(rewritten, limits);
    if (!reparsed) {
        return fail();
    }
    if (stripped_of_replay_gain_remarks(original) != stripped_of_replay_gain_remarks(*reparsed)) {
        return fail();
    }

    auto expected_album = replay_gain_remark_values(original.metadata.remarks);
    apply_expected_field(expected_album, "REPLAYGAIN_ALBUM_GAIN", update.album_gain_db, true);
    apply_expected_field(expected_album, "REPLAYGAIN_ALBUM_PEAK", update.album_peak, false);
    if (expected_album != replay_gain_remark_values(reparsed->metadata.remarks)) {
        return fail();
    }

    for (std::size_t file_index = 0U; file_index < original.files.size(); ++file_index) {
        const auto& file = original.files[file_index];
        for (std::size_t track_index = 0U; track_index < file.tracks.size(); ++track_index) {
            auto expected = replay_gain_remark_values(file.tracks[track_index].metadata.remarks);
            const auto entry = std::ranges::find_if(
                update.tracks, [file_index, track_index](const CueTrackReplayGainUpdate& track) {
                    return track.file_index == file_index && track.track_index == track_index;
                });
            if (entry != update.tracks.end()) {
                apply_expected_field(expected, "REPLAYGAIN_TRACK_GAIN", entry->track_gain_db, true);
                apply_expected_field(expected, "REPLAYGAIN_TRACK_PEAK", entry->track_peak, false);
            }
            const auto actual = replay_gain_remark_values(
                reparsed->files[file_index].tracks[track_index].metadata.remarks);
            if (expected != actual) {
                return fail();
            }
        }
    }
    return {};
}

} // namespace

std::string replay_gain_decibel_text(const double value) {
    return fixed_decimal_text(value, 2) + " dB";
}

std::string replay_gain_peak_text(const double value) { return fixed_decimal_text(value, 6); }

core::Result<CueReplayGainRewrite> rewrite_cue_replay_gain(const std::string_view source,
                                                           const CueReplayGainUpdate& update,
                                                           const CueParseLimits& limits) {
    auto original = parse_cue_sheet(source, limits);
    if (!original) {
        return std::unexpected(std::move(original.error()));
    }

    if (auto checked = validate_replay_gain_field(update.album_gain_db, true, "album gain");
        !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = validate_replay_gain_field(update.album_peak, false, "album peak");
        !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    std::set<std::pair<std::size_t, std::size_t>> seen_tracks;
    for (const auto& track : update.tracks) {
        if (track.file_index >= original->files.size() ||
            track.track_index >= original->files[track.file_index].tracks.size()) {
            return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                             "CUE ReplayGain update addresses a missing track"));
        }
        if (!seen_tracks.emplace(track.file_index, track.track_index).second) {
            return std::unexpected(cue_error(core::ErrorCode::invalid_argument,
                                             "CUE ReplayGain update addresses a track twice"));
        }
        if (auto checked = validate_replay_gain_field(track.track_gain_db, true, "track gain");
            !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        if (auto checked = validate_replay_gain_field(track.track_peak, false, "track peak");
            !checked) {
            return std::unexpected(std::move(checked.error()));
        }
    }

    const std::size_t bom = source.starts_with("\xEF\xBB\xBF") ? 3U : 0U;

    std::vector<CueRewriteLine> lines;
    std::optional<std::size_t> first_file_line;
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> track_lines;
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> first_index_lines;
    std::ptrdiff_t file_ordinal = -1;
    std::ptrdiff_t track_ordinal = -1;
    std::size_t offset = bom;
    while (offset < source.size()) {
        CueRewriteLine record;
        record.begin = offset;
        const auto newline = source.find('\n', offset);
        if (newline == std::string_view::npos) {
            record.content_end = source.size();
            record.end = source.size();
        } else {
            record.end = newline + 1U;
            record.content_end =
                newline > offset && source[newline - 1U] == '\r' ? newline - 1U : newline;
        }
        auto content = source.substr(record.begin, record.content_end - record.begin);
        while (record.leading_whitespace < content.size() &&
               (content[record.leading_whitespace] == ' ' ||
                content[record.leading_whitespace] == '\t')) {
            ++record.leading_whitespace;
        }
        const auto trimmed = trim(content);
        if (!trimmed.empty()) {
            const auto [raw_directive, argument] = split_first(trimmed);
            const auto directive = uppercase_ascii(raw_directive);
            if (directive == "FILE") {
                ++file_ordinal;
                track_ordinal = -1;
                if (!first_file_line) {
                    first_file_line = lines.size();
                }
            } else if (directive == "TRACK") {
                ++track_ordinal;
                if (file_ordinal >= 0) {
                    track_lines.emplace(std::pair{static_cast<std::size_t>(file_ordinal),
                                                  static_cast<std::size_t>(track_ordinal)},
                                        lines.size());
                }
            } else if (directive == "INDEX") {
                if (file_ordinal >= 0 && track_ordinal >= 0) {
                    first_index_lines.emplace(std::pair{static_cast<std::size_t>(file_ordinal),
                                                        static_cast<std::size_t>(track_ordinal)},
                                              lines.size());
                }
            } else if (directive == "REM") {
                const auto [name, value] = split_first(argument);
                const auto uppercased = uppercase_ascii(name);
                if (is_replay_gain_remark_name(uppercased)) {
                    record.replay_gain_name = uppercased;
                }
            }
        }
        record.file_ordinal = file_ordinal;
        record.track_ordinal = track_ordinal;
        offset = record.end;
        lines.push_back(std::move(record));
    }

    const auto line_terminator = [&source](const CueRewriteLine& line) -> std::string_view {
        return std::string_view{source}.substr(line.content_end, line.end - line.content_end);
    };
    std::string_view default_terminator = "\n";
    for (const auto& line : lines) {
        if (line.end > line.content_end) {
            default_terminator = line_terminator(line);
            break;
        }
    }

    std::map<std::size_t, std::string> replaced;
    std::set<std::size_t> dropped;
    struct PendingInsertion {
        std::size_t anchor_line{0U};
        bool at_end{false};
        std::string content;
    };
    std::vector<PendingInsertion> insertions;

    const auto plan_field = [&](const CueReplayGainField& field, const std::string_view name,
                                const bool gain, const auto& in_scope,
                                const std::optional<std::size_t> anchor,
                                const std::size_t anchor_whitespace_from) {
        if (!field.update) {
            return;
        }
        std::optional<std::size_t> first_occurrence;
        for (std::size_t index = 0U; index < lines.size(); ++index) {
            const auto& line = lines[index];
            if (!line.replay_gain_name || *line.replay_gain_name != name || !in_scope(line)) {
                continue;
            }
            if (field.value && !first_occurrence) {
                first_occurrence = index;
            } else {
                dropped.insert(index);
            }
        }
        if (!field.value) {
            return;
        }
        const auto value_text =
            gain ? replay_gain_decibel_text(*field.value) : replay_gain_peak_text(*field.value);
        auto content = "REM " + std::string(name) + " " + value_text;
        if (first_occurrence) {
            replaced.emplace(*first_occurrence, std::move(content));
            return;
        }
        std::string indented;
        if (anchor_whitespace_from < lines.size()) {
            const auto& reference = lines[anchor_whitespace_from];
            indented = std::string(source.substr(reference.begin, reference.leading_whitespace));
        }
        insertions.push_back(PendingInsertion{
            .anchor_line = anchor.value_or(0U),
            .at_end = !anchor.has_value(),
            .content = std::move(indented) + std::move(content),
        });
    };

    const auto album_scope = [](const CueRewriteLine& line) { return line.track_ordinal < 0; };
    plan_field(update.album_gain_db, "REPLAYGAIN_ALBUM_GAIN", true, album_scope, first_file_line,
               first_file_line.value_or(lines.size()));
    plan_field(update.album_peak, "REPLAYGAIN_ALBUM_PEAK", false, album_scope, first_file_line,
               first_file_line.value_or(lines.size()));
    for (const auto& track : update.tracks) {
        const auto key = std::pair{track.file_index, track.track_index};
        const auto track_scope = [key](const CueRewriteLine& line) {
            return line.file_ordinal >= 0 && line.track_ordinal >= 0 &&
                   static_cast<std::size_t>(line.file_ordinal) == key.first &&
                   static_cast<std::size_t>(line.track_ordinal) == key.second;
        };
        const auto index_line = first_index_lines.find(key);
        const auto track_line = track_lines.find(key);
        std::optional<std::size_t> anchor;
        std::size_t whitespace_from = lines.size();
        if (index_line != first_index_lines.end()) {
            anchor = index_line->second;
            whitespace_from = index_line->second;
        } else if (track_line != track_lines.end() && track_line->second + 1U < lines.size()) {
            anchor = track_line->second + 1U;
            whitespace_from = track_line->second;
        }
        plan_field(track.track_gain_db, "REPLAYGAIN_TRACK_GAIN", true, track_scope, anchor,
                   whitespace_from);
        plan_field(track.track_peak, "REPLAYGAIN_TRACK_PEAK", false, track_scope, anchor,
                   whitespace_from);
    }

    CueReplayGainRewrite result;
    result.bytes.reserve(source.size() + insertions.size() * 48U);
    if (bom != 0U) {
        result.bytes.append(source.substr(0U, bom));
    }
    const auto emit_insertions_for = [&](const std::size_t line_index) {
        for (const auto& insertion : insertions) {
            if (insertion.at_end || insertion.anchor_line != line_index) {
                continue;
            }
            const auto terminator =
                line_index < lines.size() && lines[line_index].end > lines[line_index].content_end
                    ? line_terminator(lines[line_index])
                    : default_terminator;
            result.bytes.append(insertion.content);
            result.bytes.append(terminator);
            ++result.inserted_lines;
        }
    };
    for (std::size_t index = 0U; index < lines.size(); ++index) {
        emit_insertions_for(index);
        const auto& line = lines[index];
        if (dropped.contains(index)) {
            ++result.removed_lines;
            continue;
        }
        const auto replacement = replaced.find(index);
        if (replacement != replaced.end()) {
            result.bytes.append(source.substr(line.begin, line.leading_whitespace));
            result.bytes.append(replacement->second);
            result.bytes.append(line_terminator(line));
            ++result.replaced_lines;
            continue;
        }
        result.bytes.append(source.substr(line.begin, line.end - line.begin));
    }
    for (const auto& insertion : insertions) {
        if (!insertion.at_end) {
            continue;
        }
        if (!result.bytes.empty() && result.bytes.back() != '\n') {
            result.bytes.append(default_terminator);
        }
        result.bytes.append(insertion.content);
        result.bytes.append(default_terminator);
        ++result.inserted_lines;
    }

    if (auto proven = verify_cue_replay_gain_rewrite(*original, result.bytes, update, limits);
        !proven) {
        return std::unexpected(std::move(proven.error()));
    }
    return result;
}

std::string cue_track_logical_reference(const std::string& raw_cue_path,
                                        const std::size_t file_index,
                                        const std::size_t track_index) {
    std::string reference{"cue-v1"};
    reference.push_back('\0');
    reference += raw_cue_path;
    reference.push_back('\0');
    reference += std::to_string(file_index);
    reference.push_back('\0');
    reference += std::to_string(track_index);
    return reference;
}

std::optional<CueLogicalReferenceParts> parse_cue_logical_reference(const std::string& reference) {
    constexpr std::string_view prefix{"cue-v1"};
    if (!reference.starts_with(prefix) || reference.size() <= prefix.size() + 1U ||
        reference[prefix.size()] != '\0') {
        return std::nullopt;
    }
    // Raw paths cannot contain NUL, so the remaining separators are exact.
    const auto body = std::string_view{reference}.substr(prefix.size() + 1U);
    const auto first = body.find('\0');
    const auto second = first == std::string_view::npos ? first : body.find('\0', first + 1U);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        body.find('\0', second + 1U) != std::string_view::npos) {
        return std::nullopt;
    }
    CueLogicalReferenceParts parts;
    parts.raw_cue_path = std::string{body.substr(0U, first)};
    const auto file_text = body.substr(first + 1U, second - first - 1U);
    const auto track_text = body.substr(second + 1U);
    const auto file_parsed =
        std::from_chars(file_text.data(), file_text.data() + file_text.size(), parts.file_index);
    const auto track_parsed = std::from_chars(
        track_text.data(), track_text.data() + track_text.size(), parts.track_index);
    if (parts.raw_cue_path.empty() || file_parsed.ec != std::errc{} ||
        file_parsed.ptr != file_text.data() + file_text.size() || track_parsed.ec != std::errc{} ||
        track_parsed.ptr != track_text.data() + track_text.size()) {
        return std::nullopt;
    }
    return parts;
}

} // namespace trackknife::formats
