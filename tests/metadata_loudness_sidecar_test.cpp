// SPDX-License-Identifier: GPL-3.0-only

#include "test_checks.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/loudness_sidecar.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using checks::failures;

using trackknife::metadata::LoudnessSidecar;
using trackknife::metadata::LoudnessSidecarEntry;

[[nodiscard]] LoudnessSidecar sample_sidecar() {
    LoudnessSidecar sidecar;
    sidecar.source_size = 123'456U;
    sidecar.source_modified_seconds = 1'757'500'000;
    sidecar.source_modified_nanoseconds = 42;
    sidecar.entries = {
        LoudnessSidecarEntry{
            .stream_index = std::nullopt,
            .subsong_index = std::nullopt,
            .start_sample = 0,
            .end_sample = 8'820'000,
            .track_gain_db = -3.46,
            .track_peak = 1.041372,
            .album_gain_db = -5.53,
            .album_peak = 1.041372,
            .true_peak = true,
        },
        LoudnessSidecarEntry{
            .stream_index = 0,
            .subsong_index = 2,
            .start_sample = std::nullopt,
            .end_sample = std::nullopt,
            .track_gain_db = 1.25,
            .track_peak = std::nullopt,
            .album_gain_db = std::nullopt,
            .album_peak = std::nullopt,
        },
    };
    return sidecar;
}

void roundTripsEveryFieldExactly() {
    const auto original = sample_sidecar();
    const auto serialized = trackknife::metadata::serialize_loudness_sidecar(original);
    CHECK(serialized.has_value());
    if (!serialized) {
        return;
    }
    const auto parsed = trackknife::metadata::parse_loudness_sidecar(*serialized);
    CHECK(parsed.has_value());
    CHECK(parsed && *parsed == original);

    trackknife::core::LocalSourceRevision matching{
        .device = 1U,
        .inode = 2U,
        .size = 123'456U,
        .modification_time_seconds = 1'757'500'000,
        .modification_time_nanoseconds = 42,
    };
    CHECK(original.matches(matching));
    // Device/inode differences (copies) do not invalidate the sidecar,
    // size or mtime differences do.
    matching.device = 9U;
    matching.inode = 9U;
    CHECK(original.matches(matching));
    matching.size = 1U;
    CHECK(!original.matches(matching));

    // ADR-0148: a peak kind with no peak value is unserializable, not
    // silently dropped.
    auto kind_without_peak = sample_sidecar();
    kind_without_peak.entries = {LoudnessSidecarEntry{
        .stream_index = std::nullopt,
        .subsong_index = std::nullopt,
        .start_sample = std::nullopt,
        .end_sample = std::nullopt,
        .track_gain_db = -1.0,
        .track_peak = std::nullopt,
        .album_gain_db = std::nullopt,
        .album_peak = std::nullopt,
        .true_peak = true,
    }};
    CHECK(!trackknife::metadata::serialize_loudness_sidecar(kind_without_peak));
}

void failsClosedOnUnknownContent() {
    using trackknife::core::ErrorCode;
    const auto unknown_key = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 1, "source": {"size": 1, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": []}, "extra": 1})");
    CHECK(!unknown_key);
    CHECK(!unknown_key && unknown_key.error().code == ErrorCode::unsupported);

    const auto future_version = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 2, "source": {"size": 1, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": []}})");
    CHECK(!future_version);
    CHECK(!future_version && future_version.error().code == ErrorCode::unsupported);

    const auto missing_section = trackknife::metadata::parse_loudness_sidecar(R"({"tkmeta": 1})");
    CHECK(!missing_section);

    const auto duplicate_identity = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 1, "source": {"size": 1, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": [)"
        R"({"track_gain_db": -1.0}, {"track_gain_db": -2.0}]}})");
    CHECK(!duplicate_identity);

    const auto out_of_range = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 1, "source": {"size": 1, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": [)"
        R"({"track_gain_db": 100.0}]}})");
    CHECK(!out_of_range);

    const auto fractional_integer = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 1, "source": {"size": 1.5, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": []}})");
    CHECK(!fractional_integer);

    const auto not_json = trackknife::metadata::parse_loudness_sidecar("REM GENRE Ambient");
    CHECK(!not_json);

    // ADR-0148: only the one known peak kind is accepted, and it needs a
    // peak value to describe.
    const auto unknown_peak_kind = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 1, "source": {"size": 1, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": [)"
        R"({"track_peak": 0.5, "peak_kind": "loud"}]}})");
    CHECK(!unknown_peak_kind);

    const auto non_string_peak_kind = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 1, "source": {"size": 1, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": [)"
        R"({"track_peak": 0.5, "peak_kind": 1}]}})");
    CHECK(!non_string_peak_kind);

    const auto kind_without_peak = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 1, "source": {"size": 1, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": [)"
        R"({"track_gain_db": -1.0, "peak_kind": "true_peak"}]}})");
    CHECK(!kind_without_peak);

    const auto true_peak_entry = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 1, "source": {"size": 1, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": [)"
        R"({"track_peak": 1.25, "peak_kind": "true_peak"}]}})");
    CHECK(true_peak_entry.has_value());
    CHECK(true_peak_entry && true_peak_entry->entries.size() == 1U &&
          true_peak_entry->entries.front().true_peak);

    // Absence keeps the pre-ADR-0148 meaning: sample peak.
    const auto sample_peak_entry = trackknife::metadata::parse_loudness_sidecar(
        R"({"tkmeta": 1, "source": {"size": 1, "modified_seconds": 1,)"
        R"( "modified_nanoseconds": 0}, "loudness": {"entries": [)"
        R"({"track_peak": 0.5}]}})");
    CHECK(sample_peak_entry.has_value());
    CHECK(sample_peak_entry && !sample_peak_entry->entries.front().true_peak);
}

void readsFilesAndTreatsAbsenceAsEmpty() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("trackknife-sidecar-" + trackknife::core::StableId::random().to_string());
    std::error_code fs_error;
    CHECK(std::filesystem::create_directories(root, fs_error));
    const auto audio = (root / "album.flac").native();

    const auto absent = trackknife::metadata::read_loudness_sidecar(audio);
    CHECK(absent.has_value());
    CHECK(absent && !absent->has_value());

    const auto sidecar = sample_sidecar();
    const auto serialized = trackknife::metadata::serialize_loudness_sidecar(sidecar);
    CHECK(serialized.has_value());
    if (serialized) {
        std::ofstream output{trackknife::metadata::loudness_sidecar_path(audio), std::ios::binary};
        output << *serialized;
    }
    const auto read_back = trackknife::metadata::read_loudness_sidecar(audio);
    CHECK(read_back.has_value());
    CHECK(read_back && read_back->has_value() && **read_back == sidecar);

    {
        std::ofstream output{trackknife::metadata::loudness_sidecar_path(audio), std::ios::binary};
        output << "not json";
    }
    const auto corrupt = trackknife::metadata::read_loudness_sidecar(audio);
    CHECK(!corrupt);

    std::filesystem::remove_all(root, fs_error);
}

} // namespace

int main() {
    roundTripsEveryFieldExactly();
    failsClosedOnUnknownContent();
    readsFilesAndTreatsAbsenceAsEmpty();
    return failures == 0 ? 0 : 1;
}
