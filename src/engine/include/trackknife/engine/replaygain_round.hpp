// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/loudness/grouping.hpp"
#include "trackknife/loudness/replaygain.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/metadata_apply.hpp"
#include "trackknife/protocol/message.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::engine {

// ADR-0357: one ReplayGain round -- the tracks read, grouped into albums,
// measured, and the gains written into their tags, CUE sheet or sidecar --
// done whole by the engine holding the files, for every client that asks
// (the window's ReplayGain dialog, melody-cli) as the replaygain.apply job.

// A track to measure: a whole file, one stream or subsong of it, a range of
// it, or a CUE sheet's track, which the engine reads from the sheet.
struct ReplayGainItem {
    struct Cue {
        std::string raw_cue_path;
        std::size_t file_index{0U};
        std::size_t track_index{0U};

        friend bool operator==(const Cue&, const Cue&) = default;
    };

    std::string raw_path;
    std::optional<Cue> cue;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> range;

    friend bool operator==(const ReplayGainItem&, const ReplayGainItem&) = default;
};

struct ReplayGainRequest {
    std::vector<ReplayGainItem> items;
    loudness::LoudnessGrouping grouping;
    bool true_peak{false};
    bool sidecar_only{false};
    // The album groups only: nothing measured, nothing written.
    bool preview{false};

    friend bool operator==(const ReplayGainRequest&, const ReplayGainRequest&) = default;
};

// An album programme the tracks fall into, by its first track's tags.
struct ReplayGainGroup {
    std::string album_artist;
    std::string album;
    std::size_t tracks{0U};

    friend bool operator==(const ReplayGainGroup&, const ReplayGainGroup&) = default;
};

// The most groups a round names; the rest are counted.
inline constexpr std::size_t replaygain_named_groups = 200U;
// The most tracks one round takes.
inline constexpr std::size_t replaygain_round_items = 100'000U;

struct ReplayGainRound {
    std::vector<ReplayGainGroup> groups;
    std::size_t more_groups{0U};
    // Tracks in no album: track gain only.
    std::size_t track_only{0U};
    // Tracks whose tags could not be read: left out, neither measured nor
    // written.
    std::vector<loudness::ReplayGainProblem> unreadable;
    std::size_t measured_items{0U};
    std::vector<loudness::ReplayGainProblem> problems;
    std::vector<loudness::ReplayGainRow> rows;
    // Files, CUE sheets and sidecars written, and not.
    std::size_t written{0U};
    std::size_t failed{0U};
    // Nothing written: what was measured is written whole or not at all, and
    // the write had blocking issues (among the problems).
    bool refused{false};
    // What was written, for a client's rows to show it.
    std::optional<operations::MetadataApplyResult> applied;
};

// reading, measuring or writing: how far, of how many.
struct ReplayGainStep {
    std::string phase;
    std::size_t completed{0U};
    std::size_t total{0U};
};
using ReplayGainReport = std::function<void(const ReplayGainStep&)>;

// The gains measured written as the engine writes every plan.
using ReplayGainApplier = std::function<core::Result<operations::MetadataApplyResult>(
    const metadata::MetadataWritePlan&, const operations::MetadataApplyProgressCallback&)>;

// The round, in this process, on this machine's files. Runs on a worker.
[[nodiscard]] core::Result<ReplayGainRound>
run_replaygain_round(const ReplayGainRequest& request, const ReplayGainApplier& apply,
                     const ReplayGainReport& report, const core::CancellationToken& cancellation);

namespace wire {

// replaygain.apply {items: [encoded path | {path, cue?: {sheet, file, track},
// stream?, subsong?, start?, end?}], grouping?: "release" (the default) |
// "discs" | "album" | "track" | "expression", expression?, true_peak?,
// sidecar?, preview?}
[[nodiscard]] protocol::Json encode(const ReplayGainRequest& request);
[[nodiscard]] core::Result<ReplayGainRequest>
decode_replaygain_request(const protocol::Json& value);
// {groups: [{album_artist, album, tracks}], more_groups, track_only,
// unreadable: [{path, message}], measured, tracks: [{path, title,
// integrated_lufs, track_gain, track_peak, album_key, album_gain,
// album_peak, status, peak_kind}], problems: [{path?, message}], written,
// failed, refused?, applied?}
[[nodiscard]] protocol::Json encode(const ReplayGainRound& round);
[[nodiscard]] core::Result<ReplayGainRound> decode_replaygain_round(const protocol::Json& value);
// {phase, completed, total}
[[nodiscard]] core::Result<ReplayGainStep> decode_replaygain_step(const protocol::Json& value);

} // namespace wire

} // namespace trackknife::engine
