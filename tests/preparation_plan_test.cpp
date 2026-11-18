// SPDX-License-Identifier: GPL-3.0-only

#include "test_checks.hpp"
#include "trackknife/metadata/draft_document.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/operations/preparation_plan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace {

using checks::failures;

trackknife::metadata::MetadataField field(std::string native_name,
                                          std::vector<std::string> values) {
    const auto identity = trackknife::metadata::resolve_text_property_identity(native_name);
    return trackknife::metadata::MetadataField{
        .canonical_name = identity.canonical_name,
        .native_name = std::move(native_name),
        .values = std::move(values),
        .qualifier = {},
        .provenance = trackknife::metadata::FieldProvenance::embedded,
    };
}

trackknife::core::LocalSourceRevision revision() {
    return {.device = 7U,
            .inode = 11U,
            .size = 1'024U,
            .modification_time_seconds = 12,
            .modification_time_nanoseconds = 13};
}

trackknife::operations::OutputPathPlan path_plan() {
    return trackknife::operations::OutputPathPlan{
        .layout = {.schema_version = 1U,
                   .name = "Title",
                   .dialect = {},
                   .relative_directory_expression = {},
                   .basename_expression = "%title%",
                   .sanitization_policy = {"linux", 1U}},
        .destination = std::nullopt,
        .operations = {.rename_files = true, .move_files = false},
        .sources = {trackknife::operations::PlannedOutputPathSource{
            .source_raw_path = "/music/old.flac",
            .source_revision = revision(),
            .target_raw_path = "/music/new.flac",
            .raw_relative_directory = {},
            .sanitized_relative_directory = {},
            .raw_basename = "new",
            .sanitized_basename = "new",
            .item_indexes = {0U},
            .sanitized = false,
            .no_change = false,
        }},
        .issues = {},
    };
}

trackknife::operations::OutputPathPreflight preflight() {
    auto plan = path_plan();
    return trackknife::operations::OutputPathPreflight{
        .plan = plan,
        .sources = {trackknife::operations::OutputPathPreflightSource{
            .planned = plan.sources.front(),
            .observed_revision = revision(),
            .publication =
                trackknife::operations::OutputPathPublicationKind::same_filesystem_rename,
            .target_filesystem_device = 7U,
            .missing_directory_raw_paths = {},
        }},
        .issues = {},
    };
}

trackknife::metadata::MetadataWritePlan metadata_plan() {
    return trackknife::metadata::MetadataWritePlan{
        .sources = {trackknife::metadata::MetadataWritePlanSource{
            .raw_path = "/music/old.flac",
            .occurrence_indexes = {0U},
            .expected_revision = revision(),
            .observed_revision = revision(),
            .adapter_name = "taglib-flac-v1",
            .changes = {},
            .issues = {},
        }},
        .patch_count = 1U,
    };
}

void draftProjectionPreservesSemanticAndExactNativeIdentity() {
    using namespace trackknife;
    auto selection = metadata::StagedMetadataSelection::create({metadata::StagedMetadataSource{
        .raw_path = "/music/old.flac",
        .source_revision = revision(),
        .baseline =
            metadata::MetadataDocument{
                .fields = {field("TITLE", {"Old"}), field("ALBUMARTIST", {"Semantic"}),
                           field("ALBUM ARTIST", {"Legacy freeform"})},
                .unsupported_native_objects = {{"PICTURE:front"}},
            },
    }});
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("title");
    const auto freeform = selection->exact_native_field_index("ALBUM ARTIST");
    CHECK(title.has_value());
    CHECK(freeform.has_value());
    metadata::StagedMetadataPatchSet draft;
    CHECK(draft.replace_values(*selection, 0U, *title, {"New/Title"}).has_value());
    CHECK(draft.remove_field(*selection, 0U, *freeform).has_value());
    const std::array<std::size_t, 1U> items{0U};
    const auto projected = metadata::materialize_metadata_draft(*selection, draft, items);
    CHECK(projected.has_value());
    if (!projected) {
        return;
    }
    CHECK(projected->front().effective_values("title") == std::vector<std::string>{"New/Title"});
    CHECK(projected->front().effective_values("albumartist") ==
          std::vector<std::string>{"Semantic"});
    CHECK(!projected->front().effective_native_field("ALBUM ARTIST"));
    CHECK(projected->front().unsupported_native_objects ==
          std::vector<metadata::NativeObjectIdentity>{{"PICTURE:front"}});
    CHECK(selection->source(0U).baseline.effective_values("title") ==
          std::vector<std::string>{"Old"});
}

void pathOnlyAndCombinedContentPreparationAreReady() {
    using namespace trackknife;
    auto pure = path_plan();
    auto checked = preflight();
    auto path_only = operations::assemble_preparation_plan(
        {.save_tags = false, .rename_files = true, .move_files = false, .replaygain = false}, 0U,
        std::nullopt, pure, checked);
    CHECK(path_only.has_value());
    CHECK(path_only && path_only->ready());
    CHECK(path_only && !path_only->metadata.has_value());
    CHECK(path_only && path_only->blocking_issue_count() == 0U);

    auto synthetic_naming = operations::assemble_preparation_plan(
        {.save_tags = false, .rename_files = true, .move_files = false, .replaygain = false}, 1U,
        metadata_plan(), path_plan(), preflight());
    CHECK(!synthetic_naming.has_value());
    CHECK(synthetic_naming.error().message ==
          "metadata drafts cannot influence Rename or Move while Save tags is off");

    auto combined = operations::assemble_preparation_plan(
        {.save_tags = true, .rename_files = true, .move_files = false, .replaygain = false}, 1U,
        metadata_plan(), path_plan(), preflight());
    CHECK(combined.has_value());
    CHECK(combined && combined->ready());
    CHECK(combined && combined->blocking_issue_count() == 0U);

    auto mismatched_metadata = metadata_plan();
    mismatched_metadata.sources.front().observed_revision->inode += 1U;
    auto mismatched = operations::assemble_preparation_plan(
        {.save_tags = true, .rename_files = true, .move_files = false, .replaygain = false}, 1U,
        std::move(mismatched_metadata), path_plan(), preflight());
    CHECK(mismatched.has_value());
    CHECK(mismatched && !mismatched->ready());
    CHECK(mismatched && std::ranges::any_of(mismatched->issues, [](const auto& issue) {
              return issue.kind == operations::PreparationPlanIssueKind::combined_source_mismatch;
          }));
}

void metadataOnlyPreparationRetainsExistingApplyBoundary() {
    using namespace trackknife;
    auto prepared = operations::assemble_preparation_plan(
        {.save_tags = true, .rename_files = false, .move_files = false, .replaygain = false}, 1U,
        metadata_plan(), std::nullopt, std::nullopt);
    CHECK(prepared.has_value());
    CHECK(prepared && prepared->ready());
    CHECK(prepared && !prepared->has_path_operation());
}

} // namespace

int main() {
    draftProjectionPreservesSemanticAndExactNativeIdentity();
    pathOnlyAndCombinedContentPreparationAreReady();
    metadataOnlyPreparationRetainsExistingApplyBoundary();
    return failures == 0 ? 0 : 1;
}
