// SPDX-License-Identifier: GPL-3.0-only

#include "test_checks.hpp"
#include "trackknife/metadata/proposal.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace {

using checks::failures;

using trackknife::metadata::MetadataDocument;
using trackknife::metadata::MetadataField;
using trackknife::metadata::StagedMetadataSelection;
using trackknife::metadata::StagedMetadataSource;

MetadataField field(std::string name, std::vector<std::string> values,
                    const trackknife::metadata::FieldProvenance provenance =
                        trackknife::metadata::FieldProvenance::embedded) {
    return MetadataField{
        .canonical_name = trackknife::metadata::canonicalize_field_name(name),
        .native_name = std::move(name),
        .values = std::move(values),
        .qualifier = {},
        .provenance = provenance,
    };
}

StagedMetadataSource source(std::string path, std::vector<MetadataField> fields) {
    return StagedMetadataSource{
        .raw_path = std::move(path),
        .source_revision = std::nullopt,
        .baseline = MetadataDocument{.fields = std::move(fields), .unsupported_native_objects = {}},
    };
}

[[nodiscard]] const trackknife::metadata::ProposedFieldValues*
proposed_field(const trackknife::metadata::MetadataProposalSet& proposals,
               const std::size_t item_index, const std::string_view canonical_field) {
    for (const auto& item : proposals.items) {
        if (item.item_index != item_index) {
            continue;
        }
        for (const auto& value : item.fields) {
            if (value.canonical_field == canonical_field) {
                return &value;
            }
        }
    }
    return nullptr;
}

void consistentAlbumProposesAlbumArtistAndTotals() {
    const auto selection = StagedMetadataSelection::create({
        source("/music/1.flac", {field("ALBUM", {"Alpha"}), field("ARTIST", {"Band"}),
                                 field("TRACKNUMBER", {"01"})}),
        source("/music/2.flac",
               {field("ALBUM", {"Alpha"}), field("ARTIST", {"Band"}), field("TRACKNUMBER", {"2"})}),
        source("/music/3.flac", {field("ALBUM", {"Alpha"}), field("ARTIST", {"Band"}),
                                 field("TRACKNUMBER", {"3/3"})}),
    });
    CHECK(selection.has_value());
    const trackknife::metadata::StagedMetadataPatchSet draft;
    const std::vector<std::size_t> items{0U, 1U, 2U};
    const auto proposals =
        trackknife::metadata::propose_selection_consistency(*selection, draft, items);
    CHECK(proposals.has_value());
    CHECK(proposals->provider_name == "Selection consistency");
    CHECK(proposals->field_proposal_count() == 6U);
    for (std::size_t item = 0U; item < 3U; ++item) {
        const auto* album_artist = proposed_field(*proposals, item, "albumartist");
        const auto* totals = proposed_field(*proposals, item, "totaltracks");
        CHECK(album_artist != nullptr && album_artist->values == std::vector<std::string>{"Band"} &&
              album_artist->confidence == 1.0 && !album_artist->rationale.empty());
        CHECK(totals != nullptr && totals->values == std::vector<std::string>{"3"});
    }
}

void disagreementAndGapsProposeNothing() {
    const auto selection = StagedMetadataSelection::create({
        source("/music/1.flac",
               {field("ALBUM", {"Alpha"}), field("ARTIST", {"Band"}), field("TRACKNUMBER", {"1"})}),
        source("/music/2.flac", {field("ALBUM", {"Alpha"}), field("ARTIST", {"Guest"}),
                                 field("TRACKNUMBER", {"2"})}),
        source("/music/3.flac",
               {field("ALBUM", {"Alpha"}), field("ARTIST", {"Band"}), field("TRACKNUMBER", {"4"})}),
    });
    CHECK(selection.has_value());
    const trackknife::metadata::StagedMetadataPatchSet draft;
    const std::vector<std::size_t> items{0U, 1U, 2U};
    const auto proposals =
        trackknife::metadata::propose_selection_consistency(*selection, draft, items);
    CHECK(proposals.has_value());
    // Artists disagree, so no album artist; 1,2,4 is not contiguous, so no
    // totals. Nothing else may be invented.
    CHECK(proposals->items.empty());
}

void presentAlbumArtistFillsOnlyMissingFiles() {
    const auto selection = StagedMetadataSelection::create({
        source("/music/1.flac", {field("ALBUM", {"Alpha"}), field("ARTIST", {"Band"}),
                                 field("ALBUMARTIST", {"Anthology"})}),
        source("/music/2.flac", {field("ALBUM", {"Alpha"}), field("ARTIST", {"Guest"})}),
    });
    CHECK(selection.has_value());
    const trackknife::metadata::StagedMetadataPatchSet draft;
    const std::vector<std::size_t> items{0U, 1U};
    const auto proposals =
        trackknife::metadata::propose_selection_consistency(*selection, draft, items);
    CHECK(proposals.has_value());
    CHECK(proposals->field_proposal_count() == 1U);
    const auto* album_artist = proposed_field(*proposals, 1U, "albumartist");
    CHECK(album_artist != nullptr && album_artist->values == std::vector<std::string>{"Anthology"});
    CHECK(proposed_field(*proposals, 0U, "albumartist") == nullptr);
}

void draftValuesDriveTheDerivation() {
    const auto selection = StagedMetadataSelection::create({
        source("/music/1.flac", {field("ALBUM", {"Alpha"}), field("ARTIST", {"Band"})}),
        source("/music/2.flac", {field("ALBUM", {"Alpha"}), field("ARTIST", {"Typo"})}),
    });
    CHECK(selection.has_value());
    trackknife::metadata::StagedMetadataPatchSet draft;
    const auto artist = selection->field_index("artist");
    CHECK(artist.has_value());
    const auto staged = draft.replace_values(*selection, 1U, *artist, {"Band"});
    CHECK(staged.has_value() && *staged);
    const std::vector<std::size_t> items{0U, 1U};
    const auto proposals =
        trackknife::metadata::propose_selection_consistency(*selection, draft, items);
    CHECK(proposals.has_value());
    CHECK(proposals->field_proposal_count() == 2U);
    const auto* album_artist = proposed_field(*proposals, 0U, "albumartist");
    CHECK(album_artist != nullptr && album_artist->values == std::vector<std::string>{"Band"});
}

void pairedSpellingSatisfiesTheLogicalField() {
    // TRACKTOTAL resolves to the one logical totals identity, so files that
    // already carry it are satisfied and the bare file receives exactly one
    // logical proposal; the FLAC writer emits both spellings on its own.
    const auto selection = StagedMetadataSelection::create({
        source("/music/1.flac", {field("ALBUM", {"Alpha"}), field("TRACKNUMBER", {"1"}),
                                 field("TRACKTOTAL", {"3"})}),
        source("/music/2.flac", {field("ALBUM", {"Alpha"}), field("TRACKNUMBER", {"2"}),
                                 field("TRACKTOTAL", {"3"})}),
        source("/music/3.flac", {field("ALBUM", {"Alpha"}), field("TRACKNUMBER", {"3"})}),
    });
    CHECK(selection.has_value());
    CHECK(trackknife::metadata::canonicalize_field_name("TRACKTOTAL") == "totaltracks");
    const trackknife::metadata::StagedMetadataPatchSet draft;
    const std::vector<std::size_t> items{0U, 1U, 2U};
    const auto proposals =
        trackknife::metadata::propose_selection_consistency(*selection, draft, items);
    CHECK(proposals.has_value());
    CHECK(proposals->field_proposal_count() == 1U);
    CHECK(proposed_field(*proposals, 0U, "totaltracks") == nullptr);
    CHECK(proposed_field(*proposals, 1U, "totaltracks") == nullptr);
    const auto* totals = proposed_field(*proposals, 2U, "totaltracks");
    CHECK(totals != nullptr && totals->values == std::vector<std::string>{"3"});
}

void phantomProvenanceDoesNotSatisfy() {
    using trackknife::metadata::FieldProvenance;
    // File 1's totals and album artist exist only as cached/stream
    // projections — visible in the app but absent from the writable file.
    // Both must still be proposed so the real tag gets written.
    const auto selection = StagedMetadataSelection::create({
        source("/music/1.flac", {field("ALBUM", {"Alpha"}), field("TRACKNUMBER", {"1"}),
                                 field("TOTALTRACKS", {"2"}, FieldProvenance::stream),
                                 field("ALBUMARTIST", {"Band"}, FieldProvenance::cached_snapshot)}),
        source("/music/2.flac", {field("ALBUM", {"Alpha"}), field("TRACKNUMBER", {"2"}),
                                 field("ALBUMARTIST", {"Band"})}),
    });
    CHECK(selection.has_value());
    const trackknife::metadata::StagedMetadataPatchSet draft;
    const std::vector<std::size_t> items{0U, 1U};
    const auto proposals =
        trackknife::metadata::propose_selection_consistency(*selection, draft, items);
    CHECK(proposals.has_value());
    const auto* phantom_totals = proposed_field(*proposals, 0U, "totaltracks");
    const auto* phantom_album_artist = proposed_field(*proposals, 0U, "albumartist");
    CHECK(phantom_totals != nullptr && phantom_totals->values == std::vector<std::string>{"2"});
    CHECK(phantom_album_artist != nullptr &&
          phantom_album_artist->values == std::vector<std::string>{"Band"});
    // The file with the real embedded tag receives no album artist proposal.
    CHECK(proposed_field(*proposals, 1U, "albumartist") == nullptr);

    const auto preview =
        trackknife::metadata::metadata_proposal_preview(*selection, draft, *proposals, 0.75);
    CHECK(preview.has_value());
    // The phantom-backed proposals survive the unchanged-drop because the
    // writable state does not match, even though the effective view does.
    const auto phantom_cells = std::ranges::count_if(preview->cells, [](const auto& cell) {
        return cell.item_index == 0U &&
               (cell.canonical_field == "totaltracks" || cell.canonical_field == "albumartist");
    });
    CHECK(phantom_cells == 2);
}

void previewStagesOnlyConfidentChanges() {
    const auto selection = StagedMetadataSelection::create({
        source("/music/1.flac", {field("ALBUM", {"Alpha"}), field("ALBUMARTIST", {"Band"})}),
    });
    CHECK(selection.has_value());
    const trackknife::metadata::StagedMetadataPatchSet draft;
    const trackknife::metadata::MetadataProposalSet proposals{
        .provider_name = "Test provider",
        .provider_detail = "synthetic",
        .items = {trackknife::metadata::MetadataProposalItem{
            .item_index = 0U,
            .fields =
                {
                    trackknife::metadata::ProposedFieldValues{
                        .canonical_field = "albumartist",
                        .display_field = "Album Artist",
                        .match_mode = trackknife::metadata::MetadataFieldMatchMode::logical,
                        .values = {"Band"},
                        .confidence = 1.0,
                        .rationale = "unchanged",
                    },
                    trackknife::metadata::ProposedFieldValues{
                        .canonical_field = "genre",
                        .display_field = "Genre",
                        .match_mode = trackknife::metadata::MetadataFieldMatchMode::logical,
                        .values = {"Rock"},
                        .confidence = 0.4,
                        .rationale = "too weak",
                    },
                    trackknife::metadata::ProposedFieldValues{
                        .canonical_field = "totaltracks",
                        .display_field = "Total Tracks",
                        .match_mode = trackknife::metadata::MetadataFieldMatchMode::logical,
                        .values = {"1"},
                        .confidence = 0.9,
                        .rationale = "stageable",
                    },
                },
            .artwork = {},
        }},
    };
    const auto preview =
        trackknife::metadata::metadata_proposal_preview(*selection, draft, proposals, 0.5);
    CHECK(preview.has_value());
    CHECK(preview->chain.name == "Test provider");
    // The unchanged album artist is counted, the low-confidence genre is
    // dropped, and only the totals cell survives as a stageable change.
    CHECK(preview->cells.size() == 1U);
    CHECK(preview->cells.front().canonical_field == "totaltracks");
    CHECK(!preview->cells.front().before.has_value());
    CHECK(preview->cells.front().after == std::vector<std::string>{"1"});
    CHECK(preview->changed_item_count == 1U);
    CHECK(preview->unchanged_present_cell_count == 1U);
}

void previewRejectsMalformedProposals() {
    const auto selection = StagedMetadataSelection::create({
        source("/music/1.flac", {field("ALBUM", {"Alpha"})}),
    });
    CHECK(selection.has_value());
    const trackknife::metadata::StagedMetadataPatchSet draft;
    trackknife::metadata::MetadataProposalSet out_of_range{
        .provider_name = "Test provider",
        .provider_detail = {},
        .items = {trackknife::metadata::MetadataProposalItem{
            .item_index = 5U, .fields = {}, .artwork = {}}},
    };
    CHECK(!trackknife::metadata::metadata_proposal_preview(*selection, draft, out_of_range, 0.0)
               .has_value());

    trackknife::metadata::MetadataProposalSet duplicate_field{
        .provider_name = "Test provider",
        .provider_detail = {},
        .items = {trackknife::metadata::MetadataProposalItem{
            .item_index = 0U,
            .fields =
                {
                    trackknife::metadata::ProposedFieldValues{
                        .canonical_field = "genre",
                        .display_field = "Genre",
                        .match_mode = trackknife::metadata::MetadataFieldMatchMode::logical,
                        .values = {"Rock"},
                        .confidence = 1.0,
                        .rationale = {},
                    },
                    trackknife::metadata::ProposedFieldValues{
                        .canonical_field = "genre",
                        .display_field = "Genre",
                        .match_mode = trackknife::metadata::MetadataFieldMatchMode::logical,
                        .values = {"Pop"},
                        .confidence = 1.0,
                        .rationale = {},
                    },
                },
            .artwork = {},
        }},
    };
    CHECK(!trackknife::metadata::metadata_proposal_preview(*selection, draft, duplicate_field, 0.0)
               .has_value());
}

} // namespace

int main() {
    consistentAlbumProposesAlbumArtistAndTotals();
    disagreementAndGapsProposeNothing();
    presentAlbumArtistFillsOnlyMissingFiles();
    draftValuesDriveTheDerivation();
    pairedSpellingSatisfiesTheLogicalField();
    phantomProvenanceDoesNotSatisfy();
    previewStagesOnlyConfidentChanges();
    previewRejectsMalformedProposals();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
