// SPDX-License-Identifier: GPL-3.0-only

#include "test_checks.hpp"
#include "trackknife/metadata/field_suggestions.hpp"
#include "trackknife/metadata/staged_selection.hpp"

#include <array>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using checks::failures;

trackknife::metadata::MetadataField field(std::string name, std::vector<std::string> values,
                                          const trackknife::metadata::FieldProvenance provenance =
                                              trackknife::metadata::FieldProvenance::embedded) {
    return trackknife::metadata::MetadataField{
        .canonical_name = trackknife::metadata::canonicalize_field_name(name),
        .native_name = std::move(name),
        .values = std::move(values),
        .qualifier = {},
        .provenance = provenance,
    };
}

void effectiveFieldsRetainOrderAndWinningProvenance() {
    using trackknife::metadata::FieldProvenance;
    const trackknife::metadata::MetadataDocument document{
        .fields =
            {
                field("TITLE", {"cached"}, FieldProvenance::cached_snapshot),
                field("ARTIST", {"one", "two"}),
                field("TITLE", {"embedded"}),
                field("TITLE", {"segment one"}, FieldProvenance::segment),
                field("TITLE", {"segment two"}, FieldProvenance::segment),
            },
        .unsupported_native_objects = {},
    };
    const auto effective = document.effective_fields();
    CHECK(effective.size() == 2U);
    CHECK(effective[0].canonical_name == "title");
    CHECK(effective[0].native_name == "TITLE");
    CHECK(effective[0].values == (std::vector<std::string>{"segment one", "segment two"}));
    CHECK(effective[0].provenance == FieldProvenance::segment);
    CHECK(effective[1].canonical_name == "artist");
    CHECK(effective[1].values == (std::vector<std::string>{"one", "two"}));
}

void selectionStatesAreDeterministicAndSparse() {
    using trackknife::metadata::FieldProvenance;
    using trackknife::metadata::MetadataSelectionFieldState;
    using trackknife::metadata::StagedMetadataSelection;
    using trackknife::metadata::StagedMetadataSource;

    trackknife::metadata::MetadataDocument first{
        .fields =
            {
                field("TITLE", {"First"}),
                field("ARTIST", {"Alpha", "Beta"}),
                field("ALBUM", {"Shared album"}),
                field("DATE", {"2026"}),
                field("CUSTOM_FIELD", {"one", "two"}),
            },
        .unsupported_native_objects = {},
    };
    trackknife::metadata::MetadataDocument second{
        .fields =
            {
                field("TITLE", {"Second"}, FieldProvenance::segment),
                field("ARTIST", {"Alpha", "Beta"}, FieldProvenance::sidecar),
                field("ALBUM", {"Shared album"}),
            },
        .unsupported_native_objects = {},
    };
    const trackknife::core::LocalSourceRevision revision{
        .device = 1U,
        .inode = 2U,
        .size = 3U,
        .modification_time_seconds = 4,
        .modification_time_nanoseconds = 5,
    };
    const std::vector<std::string_view> preferred{"Title", "Artist", "Album",
                                                  "Date",  "Genre",  "ALBUM_ARTIST"};
    const auto selection_result = StagedMetadataSelection::create(
        {
            StagedMetadataSource{.raw_path = "/music/disc.flac",
                                 .source_revision = revision,
                                 .baseline = std::move(first)},
            StagedMetadataSource{.raw_path = "/music/disc.flac",
                                 .source_revision = std::nullopt,
                                 .baseline = std::move(second)},
        },
        preferred);
    CHECK(selection_result.has_value());
    if (!selection_result) {
        return;
    }
    const auto& selection = *selection_result;

    CHECK(selection.item_count() == 2U);
    CHECK(selection.distinct_source_count() == 1U);
    CHECK(selection.item_revision_count() == 1U);
    CHECK(selection.field_count() == 7U);
    CHECK(selection.field(0U).canonical_name == "title");
    CHECK(selection.field(1U).canonical_name == "artist");
    CHECK(selection.field(5U).canonical_name == "albumartist");
    CHECK(selection.field(6U).canonical_name == "customfield");
    CHECK(selection.field(0U).representative_item_index == std::optional<std::size_t>{0U});
    CHECK(selection.field(4U).representative_item_index == std::nullopt);
    CHECK(selection.field(0U).state == MetadataSelectionFieldState::mixed);
    CHECK(selection.field(1U).state == MetadataSelectionFieldState::common);
    CHECK(selection.field(2U).state == MetadataSelectionFieldState::common);
    CHECK(selection.field(3U).state == MetadataSelectionFieldState::partial);
    CHECK(selection.field(4U).state == MetadataSelectionFieldState::missing);
    CHECK(selection.field(5U).state == MetadataSelectionFieldState::missing);
    CHECK(selection.field(6U).state == MetadataSelectionFieldState::partial);
    CHECK(selection.field(3U).present_item_count == 1U);

    const auto artist = selection.field_index("ART_IST");
    CHECK(artist == std::optional<std::size_t>{1U});
    CHECK(artist && selection.cell(0U, *artist) != nullptr);
    CHECK(artist &&
          selection.cell(0U, *artist)->values == (std::vector<std::string>{"Alpha", "Beta"}));
    CHECK(artist && selection.cell(1U, *artist)->provenance == FieldProvenance::sidecar);
    const auto genre = selection.field_index("genre");
    CHECK(genre && selection.cell(0U, *genre) == nullptr);
    CHECK(genre && selection.cell(1U, *genre) == nullptr);
    CHECK(!selection.field_index("not present").has_value());

    const std::array first_item{std::size_t{0U}};
    const auto first_summary = selection.summarize_items(first_item);
    CHECK(first_summary.has_value());
    if (first_summary) {
        CHECK((*first_summary)[0U].state == MetadataSelectionFieldState::common);
        CHECK((*first_summary)[0U].representative_item_index == std::optional<std::size_t>{0U});
        CHECK((*first_summary)[3U].state == MetadataSelectionFieldState::common);
        CHECK((*first_summary)[4U].state == MetadataSelectionFieldState::missing);
        CHECK((*first_summary)[6U].state == MetadataSelectionFieldState::common);
    }

    const std::array both_items{std::size_t{1U}, std::size_t{0U}};
    const auto both_summary = selection.summarize_items(both_items);
    CHECK(both_summary.has_value());
    if (both_summary) {
        CHECK((*both_summary)[0U].state == MetadataSelectionFieldState::mixed);
        CHECK((*both_summary)[1U].state == MetadataSelectionFieldState::common);
        CHECK((*both_summary)[3U].state == MetadataSelectionFieldState::partial);
        CHECK((*both_summary)[3U].present_item_count == 1U);
    }

    const std::array<std::size_t, 0U> no_items{};
    const auto empty_summary = selection.summarize_items(no_items);
    CHECK(empty_summary.has_value());
    if (empty_summary) {
        CHECK(empty_summary->size() == selection.field_count());
        CHECK((*empty_summary)[0U].state == MetadataSelectionFieldState::missing);
        CHECK((*empty_summary)[0U].present_item_count == 0U);
        CHECK((*empty_summary)[0U].representative_item_index == std::nullopt);
    }

    const std::array invalid_item{std::size_t{2U}};
    const auto invalid_summary = selection.summarize_items(invalid_item);
    CHECK(!invalid_summary.has_value());
    CHECK(invalid_summary.error().code == trackknife::core::ErrorCode::invalid_argument);

    auto extended = selection;
    const auto mood = extended.ensure_missing_field("MOOD", "Mood");
    CHECK(mood.has_value());
    CHECK(selection.field_count() == 7U);
    CHECK(extended.field_count() == 8U);
    CHECK(mood && extended.field(*mood).canonical_name == "mood");
    CHECK(mood && extended.field(*mood).display_name == "Mood");
    CHECK(mood && extended.cell(0U, *mood) == nullptr);
    CHECK(mood && extended.cell(1U, *mood) == nullptr);
    const auto same_mood = extended.ensure_missing_field("m_o-o d", "Ignored duplicate");
    CHECK(same_mood.has_value());
    CHECK(same_mood != mood);
    CHECK(same_mood &&
          extended.field(*same_mood).exact_native_name == std::optional<std::string>{"m_o-o d"});
    const auto empty_field = extended.ensure_missing_field(" _- ", "Empty");
    CHECK(!empty_field.has_value());
    const auto field_limit = extended.ensure_missing_field("Energy", "Energy", 8U);
    CHECK(!field_limit.has_value());
    CHECK(field_limit.error().code == trackknife::core::ErrorCode::limit_exceeded);
}

void emptySelectionStillExposesPreferredMissingFields() {
    const std::vector<std::string_view> preferred{"Title", "Artist"};
    const auto selection_result =
        trackknife::metadata::StagedMetadataSelection::create({}, preferred);
    CHECK(selection_result.has_value());
    if (!selection_result) {
        return;
    }
    const auto& selection = *selection_result;
    CHECK(selection.item_count() == 0U);
    CHECK(selection.field_count() == 2U);
    CHECK(selection.field(0U).state == trackknife::metadata::MetadataSelectionFieldState::missing);
}

void selectionConstructionIsExplicitlyBounded() {
    using trackknife::metadata::StagedMetadataSelection;
    using trackknife::metadata::StagedMetadataSelectionLimits;
    using trackknife::metadata::StagedMetadataSource;

    const auto too_many_items =
        StagedMetadataSelection::create({StagedMetadataSource{}, StagedMetadataSource{}}, {},
                                        StagedMetadataSelectionLimits{.items = 1U, .fields = 10U});
    CHECK(!too_many_items.has_value());
    CHECK(too_many_items.error().code == trackknife::core::ErrorCode::limit_exceeded);

    const std::vector<std::string_view> fields{"Title", "Artist"};
    const auto too_many_fields = StagedMetadataSelection::create(
        {}, fields, StagedMetadataSelectionLimits{.items = 1U, .fields = 1U});
    CHECK(!too_many_fields.has_value());
    CHECK(too_many_fields.error().code == trackknife::core::ErrorCode::limit_exceeded);
}

void committedArtworkRevisionsPreserveSnapshotsAndRejectBrokenChains() {
    using trackknife::metadata::StagedMetadataSelection;
    using trackknife::metadata::StagedMetadataSource;
    const trackknife::core::LocalSourceRevision before{.device = 1,
                                                       .inode = 2,
                                                       .size = 3,
                                                       .modification_time_seconds = 4,
                                                       .modification_time_nanoseconds = 5};
    auto after = before;
    ++after.inode;
    const StagedMetadataSource original{
        .raw_path = "/music/album.flac",
        .source_revision = before,
        .baseline = {.fields = {field("TITLE", {"Original"})}, .unsupported_native_objects = {}}};
    auto other = original;
    other.raw_path = "/music/other.flac";
    const auto captured = StagedMetadataSelection::create({original, other, original});
    CHECK(captured.has_value());
    if (!captured) {
        return;
    }
    auto revised = *captured;
    CHECK(revised.ensure_missing_field("NEW_FIELD", "New field").has_value());
    const auto advanced = revised.advance_source_revision(original.raw_path, before, after);
    CHECK(advanced.has_value() && *advanced == 2U);
    CHECK(revised.source(0).source_revision == after);
    CHECK(revised.source(2).source_revision == after);
    CHECK(revised.source(1) == other);
    CHECK(revised.source(0).baseline == original.baseline);
    CHECK(captured->source(0) == original);
    CHECK(captured->source(2) == original);
    CHECK(revised.exact_native_field_index("NEW_FIELD").has_value());
    CHECK(revised.item_revision_count() == captured->item_revision_count());
    auto next = after;
    ++next.inode;
    CHECK(!revised.advance_source_revision(original.raw_path, before, next));
    CHECK(revised.source(0).source_revision == after);
    CHECK(revised.advance_source_revision(original.raw_path, after, next).has_value());
    CHECK(revised.source(2).source_revision == next);

    auto stale = original;
    stale.source_revision = after;
    auto inconsistent = StagedMetadataSelection::create({original, stale});
    CHECK(inconsistent.has_value());
    CHECK(!inconsistent->advance_source_revision(original.raw_path, before, after));
    CHECK(inconsistent->source(0) == original);
    CHECK(inconsistent->source(1) == stale);
    stale.source_revision.reset();
    auto uncaptured = StagedMetadataSelection::create({stale});
    CHECK(!uncaptured->advance_source_revision(original.raw_path, before, after));
}

void fieldSuggestionsAreFuzzyDeterministicAndOpenEnded() {
    using trackknife::metadata::MetadataFieldSuggestionCandidate;
    using trackknife::metadata::MetadataFieldSuggestionKind;
    using trackknife::metadata::suggest_metadata_field_names;

    const std::array candidates{
        MetadataFieldSuggestionCandidate{"CUSTOM_FIELD", MetadataFieldSuggestionKind::present},
        MetadataFieldSuggestionCandidate{"Mood", MetadataFieldSuggestionKind::recent},
        MetadataFieldSuggestionCandidate{"Title", MetadataFieldSuggestionKind::conventional},
        MetadataFieldSuggestionCandidate{"TITLE", MetadataFieldSuggestionKind::present},
        MetadataFieldSuggestionCandidate{"Album Artist", MetadataFieldSuggestionKind::conventional},
        MetadataFieldSuggestionCandidate{"ALBUM ARTIST", MetadataFieldSuggestionKind::present},
        MetadataFieldSuggestionCandidate{"MusicBrainz Track Id",
                                         MetadataFieldSuggestionKind::musicbrainz},
        MetadataFieldSuggestionCandidate{"MusicBrainz Release Track Id",
                                         MetadataFieldSuggestionKind::musicbrainz},
    };

    const auto exact = suggest_metadata_field_names("t_i-tle", candidates);
    CHECK(exact.size() == 1U);
    CHECK(exact[0U].display_name == "TITLE");
    CHECK(exact[0U].canonical_name == "title");
    CHECK(exact[0U].kind == MetadataFieldSuggestionKind::present);

    const auto fuzzy = suggest_metadata_field_names("alb art", candidates);
    CHECK(fuzzy.size() == 2U);
    CHECK(fuzzy[0U].display_name == "ALBUM ARTIST");
    CHECK(fuzzy[0U].canonical_name == "album artist");
    CHECK(fuzzy[1U].display_name == "Album Artist");
    CHECK(fuzzy[1U].canonical_name == "albumartist");

    const auto musicbrainz = suggest_metadata_field_names("mb track", candidates);
    CHECK(musicbrainz.size() == 2U);
    CHECK(musicbrainz[0U].display_name == "MusicBrainz Track Id");
    CHECK(musicbrainz[1U].display_name == "MusicBrainz Release Track Id");

    const auto initial = suggest_metadata_field_names({}, candidates, 3U);
    CHECK(initial.size() == 3U);
    CHECK(initial[0U].display_name == "CUSTOM_FIELD");
    CHECK(initial[1U].display_name == "TITLE");
    CHECK(initial[2U].display_name == "ALBUM ARTIST");
    CHECK(suggest_metadata_field_names("no such field", candidates).empty());
    CHECK(suggest_metadata_field_names("title", candidates, 0U).empty());

    const auto catalog = trackknife::metadata::metadata_field_suggestion_catalog();
    const auto catalog_match = suggest_metadata_field_names("origdate", catalog);
    CHECK(!catalog_match.empty());
    CHECK(catalog_match[0U].display_name == "Original Date");
}

} // namespace

int main() {
    effectiveFieldsRetainOrderAndWinningProvenance();
    selectionStatesAreDeterministicAndSparse();
    emptySelectionStillExposesPreferredMissingFields();
    selectionConstructionIsExplicitlyBounded();
    committedArtworkRevisionsPreserveSnapshotsAndRejectBrokenChains();
    fieldSuggestionsAreFuzzyDeterministicAndOpenEnded();
    return failures == 0 ? 0 : 1;
}
