// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/output_path_preflight.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::operations {

struct PreparationOperationSelection {
    bool save_tags{false};
    bool rename_files{false};
    bool move_files{false};
    bool replaygain{false};

    friend bool operator==(const PreparationOperationSelection&,
                           const PreparationOperationSelection&) = default;
};

enum class PreparationPlanIssueKind : std::uint8_t {
    no_effect,
    metadata_plan_missing,
    path_plan_missing,
    path_preflight_missing,
    combined_source_mismatch,
    replaygain_unavailable,
};

struct PreparationPlanIssue {
    PreparationPlanIssueKind kind{PreparationPlanIssueKind::no_effect};
    bool blocking{true};
    std::string message;

    friend bool operator==(const PreparationPlanIssue&, const PreparationPlanIssue&) = default;
};

// One immutable preparation review. Metadata and output-path components retain
// their complete native plans so the UI never rebuilds execution input from
// displayed strings. A combined content/path plan retains exact source pairing
// so Apply can prepare one final artifact directly at each changed destination.
struct PreparationPlan {
    PreparationOperationSelection operations;
    std::size_t metadata_context_change_count{0U};
    std::optional<metadata::MetadataWritePlan> metadata;
    std::optional<OutputPathPlan> output_paths;
    std::optional<OutputPathPreflight> path_preflight;
    std::vector<PreparationPlanIssue> issues;

    [[nodiscard]] bool has_path_operation() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::size_t blocking_issue_count() const noexcept;
};

// Validates and combines independently computed immutable components. Blocked
// component plans remain reviewable; malformed combinations are rejected. A
// path-only plan cannot carry metadata context: Rename/Move uses captured
// source tags unless Save tags is part of the same reviewed operation.
[[nodiscard]] core::Result<PreparationPlan>
assemble_preparation_plan(PreparationOperationSelection operations,
                          std::size_t metadata_context_change_count,
                          std::optional<metadata::MetadataWritePlan> metadata_plan,
                          std::optional<OutputPathPlan> output_path_plan,
                          std::optional<OutputPathPreflight> output_path_preflight);

} // namespace trackknife::operations
