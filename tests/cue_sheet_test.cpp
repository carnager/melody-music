// SPDX-License-Identifier: GPL-3.0-only

#include "test_checks.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/cue_sheet.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>

namespace {

using checks::failures;

void append_u16(std::array<unsigned char, 44>& header, const std::size_t offset,
                const std::uint16_t value) {
    header.at(offset) = static_cast<unsigned char>(value & 0xFFU);
    header.at(offset + 1U) = static_cast<unsigned char>((value >> 8U) & 0xFFU);
}

void append_u32(std::array<unsigned char, 44>& header, const std::size_t offset,
                const std::uint32_t value) {
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        header.at(offset + byte) =
            static_cast<unsigned char>((value >> static_cast<unsigned>(byte * 8U)) & 0xFFU);
    }
}

void write_silent_wave(const std::filesystem::path& path, const std::uint32_t frame_count) {
    constexpr std::uint32_t sample_rate = 48'000U;
    constexpr std::uint16_t bits_per_sample = 16U;
    const auto data_bytes = frame_count * (bits_per_sample / 8U);
    std::array<unsigned char, 44> header{};
    std::ranges::copy(std::array{'R', 'I', 'F', 'F'}, header.begin());
    append_u32(header, 4U, 36U + data_bytes);
    std::ranges::copy(std::array{'W', 'A', 'V', 'E'}, header.begin() + 8);
    std::ranges::copy(std::array{'f', 'm', 't', ' '}, header.begin() + 12);
    append_u32(header, 16U, 16U);
    append_u16(header, 20U, 1U);
    append_u16(header, 22U, 1U);
    append_u32(header, 24U, sample_rate);
    append_u32(header, 28U, sample_rate * (bits_per_sample / 8U));
    append_u16(header, 32U, bits_per_sample / 8U);
    append_u16(header, 34U, bits_per_sample);
    std::ranges::copy(std::array{'d', 'a', 't', 'a'}, header.begin() + 36);
    append_u32(header, 40U, data_bytes);

    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    constexpr std::array<char, 2> silence{};
    for (std::uint32_t frame = 0U; frame < frame_count; ++frame) {
        output.write(silence.data(), static_cast<std::streamsize>(silence.size()));
    }
}

void parsesMetadataAndPlansAdjacentTracks() {
    constexpr std::string_view source = "\xEF\xBB\xBFREM GENRE Ambient\r\n"
                                        "PERFORMER \"Album Artist\"\r\n"
                                        "TITLE \"The Album\"\r\n"
                                        "FILE \"disc.flac\" WAVE\r\n"
                                        "  TRACK 01 AUDIO\r\n"
                                        "    TITLE \"First\"\r\n"
                                        "    INDEX 00 00:00:00\r\n"
                                        "    INDEX 01 00:00:00\r\n"
                                        "  TRACK 02 AUDIO\r\n"
                                        "    TITLE Second\r\n"
                                        "    PERFORMER \"Guest\"\r\n"
                                        "    SONGWRITER \"Writer\"\r\n"
                                        "    FLAGS DCP PRE\r\n"
                                        "    ISRC TEST00000002\r\n"
                                        "    INDEX 01 00:02:00\r\n"
                                        "  TRACK 03 AUDIO\r\n"
                                        "    TITLE \"Third\"\r\n"
                                        "    INDEX 01 00:04:00\r\n";

    const auto sheet = trackknife::formats::parse_cue_sheet(source);
    CHECK(sheet.has_value());
    if (!sheet) {
        return;
    }
    CHECK(sheet->metadata.title == "The Album");
    CHECK(sheet->metadata.performer == "Album Artist");
    CHECK(sheet->metadata.remarks.size() == 1U);
    CHECK(sheet->metadata.remarks.front().name == "GENRE");
    CHECK(sheet->metadata.remarks.front().value == "Ambient");
    CHECK(sheet->files.size() == 1U);
    CHECK(sheet->files.front().raw_reference == "disc.flac");
    CHECK(sheet->files.front().type == "WAVE");
    CHECK(sheet->files.front().tracks.size() == 3U);
    CHECK(sheet->files.front().tracks[1].flags.size() == 2U);
    CHECK(sheet->files.front().tracks[1].isrc == "TEST00000002");

    const auto tracks = trackknife::formats::plan_cue_logical_tracks(*sheet);
    CHECK(tracks.has_value());
    if (!tracks) {
        return;
    }
    CHECK(tracks->size() == 3U);
    CHECK((*tracks)[0].start_cue_frame == 0);
    CHECK((*tracks)[0].end_cue_frame == 150);
    CHECK((*tracks)[0].performer == "Album Artist");
    CHECK((*tracks)[1].start_cue_frame == 150);
    CHECK((*tracks)[1].end_cue_frame == 300);
    CHECK((*tracks)[1].performer == "Guest");
    CHECK((*tracks)[1].songwriter == "Writer");
    CHECK((*tracks)[1].album_title == "The Album");
    CHECK((*tracks)[2].start_cue_frame == 300);
    CHECK(!(*tracks)[2].end_cue_frame.has_value());

    const auto first = trackknife::formats::cue_track_sample_range((*tracks)[0], 44'100);
    const auto second = trackknife::formats::cue_track_sample_range((*tracks)[1], 44'100);
    const auto third = trackknife::formats::cue_track_sample_range((*tracks)[2], 44'100, 220'500);
    CHECK(first && first->start_sample == 0 && first->end_sample == 88'200);
    CHECK(second && second->start_sample == 88'200 && second->end_sample == 176'400);
    CHECK(third && third->start_sample == 176'400 && third->end_sample == 220'500);
    CHECK(first && second && first->end_sample == second->start_sample);
    CHECK(second && third && second->end_sample == third->start_sample);
}

void preservesRawFileBytesAndUsesTheNextPhysicalBoundary() {
    std::string source = "FILE \"disc-";
    source.push_back(static_cast<char>(0xFF));
    source += ".flac\" WAVE\n"
              "TRACK 01 AUDIO\n"
              "INDEX 01 00:00:00\n"
              "TRACK 02 MODE1/2352\n"
              "INDEX 01 00:01:00\n"
              "TRACK 03 AUDIO\n"
              "INDEX 01 00:02:00\n"
              "FILE second.wav WAVE\n"
              "TRACK 04 AUDIO\n"
              "INDEX 01 00:00:00\n"
              "MYSTERY preserved bytes\n";

    const auto sheet = trackknife::formats::parse_cue_sheet(source);
    CHECK(sheet.has_value());
    if (!sheet) {
        return;
    }
    CHECK(sheet->files.size() == 2U);
    CHECK(sheet->files[0].raw_reference.size() == 11U);
    CHECK(static_cast<unsigned char>(sheet->files[0].raw_reference[5]) == 0xFFU);
    CHECK(sheet->unknown_directives.size() == 1U);
    CHECK(sheet->unknown_directives.front().name == "MYSTERY");
    CHECK(sheet->unknown_directives.front().argument == "preserved bytes");

    const auto tracks = trackknife::formats::plan_cue_logical_tracks(*sheet);
    CHECK(tracks.has_value());
    if (!tracks) {
        return;
    }
    CHECK(tracks->size() == 3U);
    CHECK((*tracks)[0].track_number == 1);
    CHECK((*tracks)[0].end_cue_frame == 75);
    CHECK((*tracks)[1].track_number == 3);
    CHECK(!(*tracks)[1].end_cue_frame.has_value());
    CHECK((*tracks)[2].file_index == 1U);
    CHECK((*tracks)[2].start_cue_frame == 0);
}

void rejectsMalformedAndUnboundedInputs() {
    const auto before_file = trackknife::formats::parse_cue_sheet("TRACK 01 AUDIO\n");
    CHECK(!before_file);
    CHECK(before_file.error().code == trackknife::core::ErrorCode::invalid_argument);
    CHECK(!before_file.error().context.empty());

    const auto bad_time = trackknife::formats::parse_cue_sheet(
        "FILE disc.flac WAVE\nTRACK 01 AUDIO\nINDEX 01 00:60:00\n");
    CHECK(!bad_time);

    const auto duplicate_index = trackknife::formats::parse_cue_sheet(
        "FILE disc.flac WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
        "INDEX 01 00:01:00\n");
    CHECK(!duplicate_index);

    const auto missing_index =
        trackknife::formats::parse_cue_sheet("FILE disc.flac WAVE\nTRACK 01 AUDIO\n");
    CHECK(missing_index.has_value());
    if (missing_index) {
        const auto planned = trackknife::formats::plan_cue_logical_tracks(*missing_index);
        CHECK(!planned);
    }

    auto limits = trackknife::formats::CueParseLimits{};
    limits.source_bytes = 8U;
    const auto too_large = trackknife::formats::parse_cue_sheet("FILE x WAVE", limits);
    CHECK(!too_large);
    CHECK(too_large.error().code == trackknife::core::ErrorCode::limit_exceeded);
}

void mapsFractionalCueFramesWithoutBoundaryDrift() {
    auto first = trackknife::formats::CueLogicalTrack{};
    first.start_cue_frame = 1;
    first.end_cue_frame = 2;
    auto second = trackknife::formats::CueLogicalTrack{};
    second.start_cue_frame = 2;

    const auto first_range = trackknife::formats::cue_track_sample_range(first, 44'117);
    const auto second_range = trackknife::formats::cue_track_sample_range(second, 44'117, 44'117);
    CHECK(first_range && first_range->start_sample == 588);
    CHECK(first_range && first_range->end_sample == 1'176);
    CHECK(second_range && second_range->start_sample == 1'176);
    CHECK(first_range && second_range && first_range->end_sample == second_range->start_sample);

    second.start_cue_frame = 150;
    const auto outside = trackknife::formats::cue_track_sample_range(second, 44'117, 80'000);
    CHECK(!outside);
    CHECK(outside.error().code == trackknife::core::ErrorCode::invalid_argument);
}

void resolvesContainedPhysicalSourcesAndExactDurations() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-cue-resolution-" + trackknife::core::StableId::random().to_string());
    std::error_code error;
    CHECK(std::filesystem::create_directories(root / "nested", error));
    const auto wave = root / "disc.wav";
    const auto cue = root / "album.cue";
    write_silent_wave(wave, 96'000U);
    {
        std::ofstream output{cue, std::ios::binary};
        output << "REM DATE 2026\n"
                  "PERFORMER \"Album Artist\"\n"
                  "TITLE \"Album\"\n"
                  "FILE \"disc.wav\" WAVE\n"
                  "TRACK 01 AUDIO\n"
                  "TITLE \"First\"\n"
                  "INDEX 01 00:00:00\n"
                  "TRACK 02 AUDIO\n"
                  "TITLE \"Second\"\n"
                  "INDEX 01 00:01:00\n";
    }

    const auto resolved = trackknife::formats::resolve_external_cue_sheet(cue.native());
    CHECK(resolved.has_value());
    if (resolved) {
        CHECK(resolved->raw_cue_path == std::filesystem::canonical(cue).native());
        CHECK(resolved->physical_sources.size() == 1U);
        CHECK(resolved->physical_sources.front() == std::filesystem::canonical(wave).native());
        CHECK(resolved->tracks.size() == 2U);
        CHECK(resolved->tracks[0].sample_range.start_sample == 0);
        CHECK(resolved->tracks[0].sample_range.end_sample == 48'000);
        CHECK(resolved->tracks[0].duration_ms == 1'000);
        CHECK(resolved->tracks[1].sample_range.start_sample == 48'000);
        CHECK(resolved->tracks[1].sample_range.end_sample == 96'000);
        CHECK(resolved->tracks[1].duration_ms == 1'000);
        CHECK(resolved->tracks[1].cue.remarks.size() == 1U);
        CHECK(resolved->tracks[1].cue.remarks.front().name == "DATE");
        CHECK(resolved->tracks[1].cue.remarks.front().value == "2026");
    }

    const auto escaping_cue = root / "nested" / "escaping.cue";
    {
        std::ofstream output{escaping_cue, std::ios::binary};
        output << "FILE \"../disc.wav\" WAVE\n"
                  "TRACK 01 AUDIO\n"
                  "INDEX 01 00:00:00\n";
    }
    const auto escaping = trackknife::formats::resolve_external_cue_sheet(escaping_cue.native());
    CHECK(!escaping);
    CHECK(escaping.error().code == trackknife::core::ErrorCode::invalid_argument);

    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

void insertsReplayGainLinesPreservingBytes() {
    constexpr std::string_view source = "\xEF\xBB\xBFPERFORMER \"AA\"\r\n"
                                        "FILE \"disc.flac\" WAVE\r\n"
                                        "  TRACK 01 AUDIO\r\n"
                                        "    TITLE \"First\"\r\n"
                                        "    INDEX 01 00:00:00\r\n"
                                        "  TRACK 02 AUDIO\r\n"
                                        "    INDEX 01 00:02:00\r\n";
    const trackknife::formats::CueReplayGainUpdate update{
        .album_gain_db = {.update = true, .value = -5.53},
        .album_peak = {.update = true, .value = 0.994629},
        .tracks =
            {
                {.file_index = 0U,
                 .track_index = 0U,
                 .track_gain_db = {.update = true, .value = -3.46},
                 .track_peak = {.update = true, .value = 0.994629}},
                {.file_index = 0U,
                 .track_index = 1U,
                 .track_gain_db = {.update = true, .value = 1.25},
                 .track_peak = {.update = true, .value = 1.0}},
            },
    };
    const auto rewritten = trackknife::formats::rewrite_cue_replay_gain(source, update);
    CHECK(rewritten.has_value());
    if (!rewritten) {
        return;
    }
    constexpr std::string_view expected = "\xEF\xBB\xBFPERFORMER \"AA\"\r\n"
                                          "REM REPLAYGAIN_ALBUM_GAIN -5.53 dB\r\n"
                                          "REM REPLAYGAIN_ALBUM_PEAK 0.994629\r\n"
                                          "FILE \"disc.flac\" WAVE\r\n"
                                          "  TRACK 01 AUDIO\r\n"
                                          "    TITLE \"First\"\r\n"
                                          "    REM REPLAYGAIN_TRACK_GAIN -3.46 dB\r\n"
                                          "    REM REPLAYGAIN_TRACK_PEAK 0.994629\r\n"
                                          "    INDEX 01 00:00:00\r\n"
                                          "  TRACK 02 AUDIO\r\n"
                                          "    REM REPLAYGAIN_TRACK_GAIN 1.25 dB\r\n"
                                          "    REM REPLAYGAIN_TRACK_PEAK 1.000000\r\n"
                                          "    INDEX 01 00:02:00\r\n";
    CHECK(rewritten->bytes == expected);
    CHECK(rewritten->inserted_lines == 6U);
    CHECK(rewritten->replaced_lines == 0U);
    CHECK(rewritten->removed_lines == 0U);
}

void replacesRemovesAndDeduplicatesReplayGainLines() {
    constexpr std::string_view source = "REM REPLAYGAIN_ALBUM_GAIN -2.00 dB\n"
                                        "REM REPLAYGAIN_ALBUM_PEAK 0.900000\n"
                                        "FILE disc.flac WAVE\n"
                                        "TRACK 01 AUDIO\n"
                                        "REM REPLAYGAIN_TRACK_GAIN -1.00 dB\n"
                                        "REM REPLAYGAIN_TRACK_GAIN -1.50 dB\n"
                                        "REM REPLAYGAIN_TRACK_PEAK 0.800000\n"
                                        "INDEX 01 00:00:00\n"
                                        "TRACK 02 AUDIO\n"
                                        "REM REPLAYGAIN_TRACK_GAIN -4.00 dB\n"
                                        "INDEX 01 00:01:00\n";
    const trackknife::formats::CueReplayGainUpdate update{
        .album_gain_db = {.update = true, .value = -2.5},
        .album_peak = {.update = true, .value = std::nullopt},
        .tracks = {{.file_index = 0U,
                    .track_index = 0U,
                    .track_gain_db = {.update = true, .value = -1.75},
                    .track_peak = {}}},
    };
    const auto rewritten = trackknife::formats::rewrite_cue_replay_gain(source, update);
    CHECK(rewritten.has_value());
    if (!rewritten) {
        return;
    }
    constexpr std::string_view expected = "REM REPLAYGAIN_ALBUM_GAIN -2.50 dB\n"
                                          "FILE disc.flac WAVE\n"
                                          "TRACK 01 AUDIO\n"
                                          "REM REPLAYGAIN_TRACK_GAIN -1.75 dB\n"
                                          "REM REPLAYGAIN_TRACK_PEAK 0.800000\n"
                                          "INDEX 01 00:00:00\n"
                                          "TRACK 02 AUDIO\n"
                                          "REM REPLAYGAIN_TRACK_GAIN -4.00 dB\n"
                                          "INDEX 01 00:01:00\n";
    CHECK(rewritten->bytes == expected);
    CHECK(rewritten->replaced_lines == 2U);
    CHECK(rewritten->removed_lines == 2U);
    CHECK(rewritten->inserted_lines == 0U);
}

void insertsBeforeFileAndKeepsMissingTrailingNewline() {
    const auto rewritten = trackknife::formats::rewrite_cue_replay_gain(
        "FILE disc.flac WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00",
        {.album_gain_db = {.update = true, .value = -1.0}, .album_peak = {}, .tracks = {}});
    CHECK(rewritten.has_value());
    if (rewritten) {
        CHECK(rewritten->bytes == "REM REPLAYGAIN_ALBUM_GAIN -1.00 dB\n"
                                  "FILE disc.flac WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00");
        CHECK(rewritten->inserted_lines == 1U);
    }
}

void rejectsInvalidReplayGainUpdates() {
    constexpr std::string_view source = "FILE disc.flac WAVE\n"
                                        "TRACK 01 AUDIO\n"
                                        "INDEX 01 00:00:00\n";
    const auto missing = trackknife::formats::rewrite_cue_replay_gain(
        source, {.album_gain_db = {},
                 .album_peak = {},
                 .tracks = {{.file_index = 0U,
                             .track_index = 5U,
                             .track_gain_db = {.update = true, .value = -1.0},
                             .track_peak = {}}}});
    CHECK(!missing);
    CHECK(missing.error().code == trackknife::core::ErrorCode::invalid_argument);

    const auto duplicated = trackknife::formats::rewrite_cue_replay_gain(
        source,
        {.album_gain_db = {},
         .album_peak = {},
         .tracks = {{.file_index = 0U, .track_index = 0U, .track_gain_db = {}, .track_peak = {}},
                    {.file_index = 0U, .track_index = 0U, .track_gain_db = {}, .track_peak = {}}}});
    CHECK(!duplicated);

    const auto out_of_range = trackknife::formats::rewrite_cue_replay_gain(
        source,
        {.album_gain_db = {.update = true, .value = 100.0}, .album_peak = {}, .tracks = {}});
    CHECK(!out_of_range);

    const auto negative_peak = trackknife::formats::rewrite_cue_replay_gain(
        source, {.album_gain_db = {}, .album_peak = {.update = true, .value = -0.5}, .tracks = {}});
    CHECK(!negative_peak);
}

void parsesReplayGainTextLeniently() {
    CHECK(trackknife::formats::parse_replay_gain_decibels("+1.20 dB") == 1.2);
    CHECK(trackknife::formats::parse_replay_gain_decibels("-3.46dB") == -3.46);
    CHECK(trackknife::formats::parse_replay_gain_decibels("2.5") == 2.5);
    CHECK(!trackknife::formats::parse_replay_gain_decibels("70 dB").has_value());
    CHECK(!trackknife::formats::parse_replay_gain_decibels("loud").has_value());
    CHECK(trackknife::formats::parse_replay_gain_peak("0.994629") == 0.994629);
    CHECK(!trackknife::formats::parse_replay_gain_peak("-1").has_value());
    CHECK(!trackknife::formats::parse_replay_gain_peak("0").has_value());
}

} // namespace

int main() {
    parsesMetadataAndPlansAdjacentTracks();
    preservesRawFileBytesAndUsesTheNextPhysicalBoundary();
    rejectsMalformedAndUnboundedInputs();
    mapsFractionalCueFramesWithoutBoundaryDrift();
    resolvesContainedPhysicalSourcesAndExactDurations();
    insertsReplayGainLinesPreservingBytes();
    replacesRemovesAndDeduplicatesReplayGainLines();
    insertsBeforeFileAndKeepsMissingTrailingNewline();
    rejectsInvalidReplayGainUpdates();
    parsesReplayGainTextLeniently();
    return failures == 0 ? 0 : 1;
}
