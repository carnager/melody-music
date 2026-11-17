// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/operations/preparation_plan.hpp"

#include <algorithm>
#include <ranges>
#include <string>
#include <utility>

namespace trackknife::operations {
namespace {

[[nodiscard]] core::Error invalid_plan(std::string message) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument,
        .message = std::move(message),
        .context = {},
    };
}

} // namespace

bool PreparationPlan::has_path_operation() const noexcept {
    return operations.rename_files || operations.move_files;
}

bool PreparationPlan::ready() const noexcept {
    if (std::ranges::any_of(issues, &PreparationPlanIssue::blocking)) {
        return false;
    }
    if (operations.save_tags && metadata_context_change_count > 0U &&
        (!metadata || !metadata->ready())) {
        return false;
    }
    if (has_path_operation() &&
        (!output_paths || !output_paths->ready() || !path_preflight || !path_preflight->ready())) {
        return false;
    }
    return (operations.save_tags && metadata_context_change_count > 0U) || has_path_operation();
}

std::size_t PreparationPlan::blocking_issue_count() const noexcept {
    auto count =
        static_cast<std::size_t>(std::ranges::count_if(issues, &PreparationPlanIssue::blocking));
    if (operations.save_tags && metadata) {
        count += metadata->blocking_issue_count();
    }
    if (output_paths) {
        count += static_cast<std::size_t>(
            std::ranges::count_if(output_paths->issues, &OutputPathPlanIssue::blocking));
    }
    if (path_preflight) {
        count += static_cast<std::size_t>(
            std::ranges::count_if(path_preflight->issues, &OutputPathPreflightIssue::blocking));
    }
    return count;
}

core::Result<PreparationPlan>
assemble_preparation_plan(const PreparationOperationSelection operations,
                          const std::size_t metadata_context_change_count,
                          std::optional<metadata::MetadataWritePlan> metadata_plan,
                          std::optional<OutputPathPlan> output_path_plan,
                          std::optional<OutputPathPreflight> output_path_preflight) {
    const auto has_path = operations.rename_files || operations.move_files;
    if (!operations.save_tags && !has_path && !operations.replaygain) {
        return std::unexpected(invalid_plan("preparation requires at least one operation"));
    }
    if (!operations.save_tags && (metadata_context_change_count > 0U || metadata_plan)) {
        return std::unexpected(
            invalid_plan("metadata drafts cannot influence Rename or Move while Save tags is off"));
    }
    if (!has_path && (output_path_plan || output_path_preflight)) {
        return std::unexpected(invalid_plan("an output-path component requires Rename or Move"));
    }
    if (output_path_preflight && !output_path_plan) {
        return std::unexpected(
            invalid_plan("an output-path preflight requires its pure path plan"));
    }
    if (output_path_plan && (output_path_plan->operations.rename_files != operations.rename_files ||
                             output_path_plan->operations.move_files != operations.move_files)) {
        return std::unexpected(
            invalid_plan("output-path operation choices disagree with the preparation plan"));
    }
    if (output_path_preflight && output_path_preflight->plan != *output_path_plan) {
        return std::unexpected(
            invalid_plan("output-path preflight does not retain the reviewed pure plan"));
    }

    PreparationPlan plan{
        .operations = operations,
        .metadata_context_change_count = metadata_context_change_count,
        .metadata = std::move(metadata_plan),
        .output_paths = std::move(output_path_plan),
        .path_preflight = std::move(output_path_preflight),
        .issues = {},
    };
    const auto add_issue = [&plan](const PreparationPlanIssueKind kind, std::string message) {
        plan.issues.push_back(PreparationPlanIssue{
            .kind = kind,
            .blocking = true,
            .message = std::move(message),
        });
    };
    if (operations.replaygain) {
        add_issue(PreparationPlanIssueKind::replaygain_unavailable,
                  "ReplayGain analysis is not qualified until M7");
    }
    if (operations.save_tags && metadata_context_change_count > 0U && !plan.metadata) {
        add_issue(PreparationPlanIssueKind::metadata_plan_missing,
                  "Save tags has changes but no revalidated metadata write plan");
    }
    if (has_path && !plan.output_paths) {
        add_issue(PreparationPlanIssueKind::path_plan_missing,
                  "Rename or Move has no immutable output-path plan");
    } else if (has_path && plan.output_paths->ready() && !plan.path_preflight) {
        add_issue(PreparationPlanIssueKind::path_preflight_missing,
                  "Ready output paths have not passed fresh filesystem preflight");
    }
    if (operations.save_tags && metadata_context_change_count > 0U && has_path && plan.metadata &&
        plan.output_paths) {
        for (const auto& source : plan.metadata->sources) {
            const auto path_source = std::ranges::find(plan.output_paths->sources, source.raw_path,
                                                       &PlannedOutputPathSource::source_raw_path);
            if (path_source == plan.output_paths->sources.end() || !source.expected_revision ||
                !source.observed_revision ||
                *source.expected_revision != *source.observed_revision ||
                path_source->source_revision != *source.observed_revision) {
                add_issue(PreparationPlanIssueKind::combined_source_mismatch,
                          "A changed metadata source does not match the exact reviewed path source "
                          "revision");
                break;
            }
        }
    }
    if (operations.save_tags && metadata_context_change_count == 0U && !has_path) {
        add_issue(PreparationPlanIssueKind::no_effect,
                  "Save tags has no staged or automatic metadata changes");
    }
    return plan;
}

} // namespace trackknife::operations
