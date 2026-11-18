// SPDX-License-Identifier: GPL-3.0-only

#include "test_checks.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/artwork.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"
#include "trackknife/metadata/flac_writer.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <flacfile.h>
#include <flacpicture.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using checks::failures;

class TemporaryDirectory final {
  public:
    TemporaryDirectory()
        : path_{std::filesystem::temp_directory_path() /
                ("trackknife-flac-writer-" + trackknife::core::StableId::random().to_string())} {
        std::error_code error;
        CHECK(std::filesystem::create_directory(path_, error));
        CHECK(!error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        CHECK(!error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::optional<std::vector<unsigned char>>
decode_base64_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    const std::string encoded{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    std::array<int, 256> values{};
    values.fill(-1);
    constexpr std::string_view alphabet{
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    for (std::size_t index = 0U; index < alphabet.size(); ++index) {
        values[static_cast<unsigned char>(alphabet[index])] = static_cast<int>(index);
    }
    std::vector<unsigned char> decoded;
    unsigned accumulator = 0U;
    unsigned bits = 0U;
    for (const auto character : encoded) {
        if (character == '=') {
            break;
        }
        const auto byte = static_cast<unsigned char>(character);
        const auto value = values[byte];
        if (value < 0) {
            if (character == '\r' || character == '\n' || character == ' ' || character == '\t') {
                continue;
            }
            return std::nullopt;
        }
        accumulator = (accumulator << 6U) | static_cast<unsigned>(value);
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            decoded.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFFU));
        }
    }
    return decoded;
}

[[nodiscard]] std::filesystem::path materialize(const std::filesystem::path& fixture_directory,
                                                const std::string_view fixture,
                                                const std::filesystem::path& destination) {
    const auto bytes = decode_base64_file(fixture_directory / fixture);
    CHECK(bytes.has_value());
    if (bytes) {
        std::ofstream output{destination, std::ios::binary};
        output.write(reinterpret_cast<const char*>(bytes->data()),
                     static_cast<std::streamsize>(bytes->size()));
        CHECK(output.good());
    }
    return destination;
}

[[nodiscard]] std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    CHECK(output.good());
}

void add_unknown_application_block(const std::filesystem::path& path) {
    auto bytes = read_bytes(path);
    CHECK(bytes.size() >= 42U);
    CHECK(std::ranges::equal(std::span{bytes}.first<4>(),
                             std::array<unsigned char, 4>{'f', 'L', 'a', 'C'}));
    if (bytes.size() < 42U) {
        return;
    }
    const auto first_length = (static_cast<std::size_t>(bytes[5]) << 16U) |
                              (static_cast<std::size_t>(bytes[6]) << 8U) |
                              static_cast<std::size_t>(bytes[7]);
    const auto insertion = 8U + first_length;
    CHECK(insertion <= bytes.size());
    constexpr std::array<unsigned char, 16> application_block{
        0x02U, 0x00U, 0x00U, 0x0CU, 't', 'k', 'U', 'T',
        0x00U, 0xFFU, 0x7EU, 0x81U, 'p', 'r', 'o', 'b',
    };
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(insertion), application_block.begin(),
                 application_block.end());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    CHECK(output.good());
}

void add_secondary_picture(const std::filesystem::path& path,
                           const std::vector<unsigned char>& encoded) {
    TagLib::FLAC::File file{path.c_str(), false};
    CHECK(file.isValid());
    if (!file.isValid()) {
        return;
    }
    auto* picture = new TagLib::FLAC::Picture;
    picture->setType(TagLib::FLAC::Picture::BackCover);
    picture->setMimeType(TagLib::String{"image/jpeg", TagLib::String::UTF8});
    picture->setDescription(TagLib::String{"Secondary", TagLib::String::UTF8});
    picture->setWidth(8);
    picture->setHeight(6);
    picture->setColorDepth(24);
    picture->setNumColors(0);
    picture->setData(TagLib::ByteVector{reinterpret_cast<const char*>(encoded.data()),
                                        static_cast<unsigned int>(encoded.size())});
    file.addPicture(picture);
    CHECK(file.save());
}

[[nodiscard]] std::vector<std::vector<unsigned char>>
application_payloads(const std::filesystem::path& path) {
    const auto bytes = read_bytes(path);
    std::vector<std::vector<unsigned char>> payloads;
    if (bytes.size() < 8U) {
        return payloads;
    }
    std::size_t offset = 4U;
    bool last = false;
    while (!last && offset + 4U <= bytes.size()) {
        last = (bytes[offset] & 0x80U) != 0U;
        const auto type = bytes[offset] & 0x7FU;
        const auto length = (static_cast<std::size_t>(bytes[offset + 1U]) << 16U) |
                            (static_cast<std::size_t>(bytes[offset + 2U]) << 8U) |
                            static_cast<std::size_t>(bytes[offset + 3U]);
        offset += 4U;
        if (length > bytes.size() - offset) {
            return {};
        }
        if (type == 2U) {
            payloads.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                  bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        }
        offset += length;
    }
    return payloads;
}

[[nodiscard]] std::optional<std::vector<float>> decode_all(const std::filesystem::path& path) {
    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (!decoder) {
        return std::nullopt;
    }
    std::vector<float> samples;
    while (true) {
        auto chunk = decoder->next_chunk();
        CHECK(chunk.has_value());
        if (!chunk) {
            return std::nullopt;
        }
        if (!*chunk) {
            break;
        }
        samples.insert(samples.end(), (*chunk)->interleaved_samples.begin(),
                       (*chunk)->interleaved_samples.end());
    }
    return samples;
}

[[nodiscard]] std::optional<trackknife::metadata::StagedMetadataSelection>
selection_for(const trackknife::metadata::LocalMetadataRead& read) {
    auto selection = trackknife::metadata::StagedMetadataSelection::create({
        trackknife::metadata::StagedMetadataSource{
            .raw_path = read.raw_path,
            .source_revision = read.source_revision,
            .baseline = read.document,
        },
    });
    CHECK(selection.has_value());
    if (!selection) {
        return std::nullopt;
    }
    return std::move(*selection);
}

[[nodiscard]] std::optional<trackknife::metadata::MetadataWritePlan>
make_plan(const trackknife::metadata::StagedMetadataSelection& selection,
          const trackknife::metadata::StagedMetadataPatchSet& patches) {
    auto plan = trackknife::metadata::revalidate_metadata_write_plan(selection, patches);
    CHECK(plan.has_value());
    if (!plan) {
        return std::nullopt;
    }
    return std::move(*plan);
}

[[nodiscard]] bool has_issue(const trackknife::metadata::MetadataWritePlanSource& source,
                             const trackknife::metadata::MetadataWritePlanIssueKind kind) {
    return std::ranges::any_of(source.issues,
                               [kind](const auto& issue) { return issue.kind == kind; });
}

[[nodiscard]] std::optional<std::filesystem::path>
prepare_replaygain_source(const std::filesystem::path& fixture_directory,
                          const TemporaryDirectory& directory) {
    const auto source = materialize(fixture_directory, "rich-metadata-flac.b64",
                                    directory.path() / "baseline.flac");
    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return std::nullopt;
    }
    auto selection = selection_for(*read);
    if (!selection) {
        return std::nullopt;
    }
    const auto gain =
        selection->ensure_missing_field("REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_GAIN");
    const auto peak =
        selection->ensure_missing_field("REPLAYGAIN_TRACK_PEAK", "REPLAYGAIN_TRACK_PEAK");
    CHECK(gain.has_value());
    CHECK(peak.has_value());
    if (!gain || !peak) {
        return std::nullopt;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *gain, {"-7.25 dB"}).has_value());
    CHECK(patches.replace_values(*selection, 0U, *peak, {"0.987654"}).has_value());
    auto plan = make_plan(*selection, patches);
    CHECK(plan && plan->ready());
    if (!plan || !plan->ready()) {
        return std::nullopt;
    }
    const auto enriched = directory.path() / "preservation-source.flac";
    const auto prepared = trackknife::metadata::prepare_flac_metadata_write_copy(
        plan->sources.front(), enriched.native());
    CHECK(prepared.has_value());
    if (!prepared) {
        return std::nullopt;
    }
    add_unknown_application_block(enriched);
    return enriched;
}

void preservesNativeFlacWhileApplyingExactTextChanges(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = prepare_replaygain_source(fixture_directory, directory);
    CHECK(source.has_value());
    if (!source) {
        return;
    }
    const auto source_bytes = read_bytes(*source);
    const auto source_applications = application_payloads(*source);
    CHECK(source_applications ==
          (std::vector<std::vector<unsigned char>>{
              {'t', 'k', 'U', 'T', 0x00U, 0xFFU, 0x7EU, 0x81U, 'p', 'r', 'o', 'b'}}));
    const auto source_pcm = decode_all(*source);
    CHECK(source_pcm.has_value());

    const auto read = trackknife::metadata::read_local_metadata(source->native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    auto selection = selection_for(*read);
    if (!selection) {
        return;
    }
    const auto artist = selection->field_index("ARTIST");
    const auto album = selection->field_index("ALBUM");
    const auto recording_id = selection->field_index("MUSICBRAINZ_TRACKID");
    const auto custom = selection->ensure_missing_field("EXPERIMENTAL_FIELD", "EXPERIMENTAL_FIELD");
    CHECK(artist.has_value());
    CHECK(album.has_value());
    CHECK(recording_id.has_value());
    CHECK(custom.has_value());
    if (!artist || !album || !recording_id || !custom) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches
              .replace_values(*selection, 0U, *artist,
                              {"Second Artist", "First Artist", "Second Artist"})
              .has_value());
    CHECK(patches.remove_field(*selection, 0U, *album).has_value());
    CHECK(
        patches
            .replace_values(*selection, 0U, *recording_id, {"99999999-9999-9999-9999-999999999999"})
            .has_value());
    CHECK(patches.replace_values(*selection, 0U, *custom, {"first", "second"}).has_value());

    auto plan = make_plan(*selection, patches);
    CHECK(plan.has_value());
    CHECK(plan && plan->ready());
    CHECK(plan && plan->blocking_issue_count() == 0U);
    CHECK(plan && plan->sources.front().adapter_name == "taglib-flac-v1");
    if (!plan || !plan->ready()) {
        return;
    }
    const auto prepared_path = directory.path() / "prepared.flac";
    const auto prepared = trackknife::metadata::prepare_flac_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    CHECK(prepared.has_value());
    if (!prepared) {
        std::cerr << prepared.error().message << '\n';
        return;
    }
    CHECK(prepared->source_revision == read->source_revision);
    CHECK(prepared->field_change_count == 4U);
    CHECK(read_bytes(*source) == source_bytes);
    CHECK(application_payloads(prepared_path) == source_applications);
    CHECK(prepared->document.effective_values("artist") ==
          (std::vector<std::string>{"Second Artist", "First Artist", "Second Artist"}));
    CHECK(!prepared->document.first_effective_value("album").has_value());
    CHECK(prepared->document.effective_values("experimental_field") ==
          (std::vector<std::string>{"first", "second"}));
    CHECK(prepared->document.effective_values("musicbrainz_trackid") ==
          (std::vector<std::string>{"99999999-9999-9999-9999-999999999999"}));
    CHECK(prepared->document.effective_values("replaygain_track_gain") ==
          (std::vector<std::string>{"-7.25 dB"}));
    CHECK(prepared->document.effective_values("replaygain_track_peak") ==
          (std::vector<std::string>{"0.987654"}));
    const auto prepared_pcm = decode_all(prepared_path);
    CHECK(prepared_pcm == source_pcm);

    const auto prepared_bytes = read_bytes(prepared_path);
    const auto existing = trackknife::metadata::prepare_flac_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    CHECK(!existing.has_value());
    CHECK(!existing && existing.error().code == trackknife::core::ErrorCode::conflict);
    CHECK(read_bytes(prepared_path) == prepared_bytes);
}

void freeformFieldNeverAliasesAConventionalNeighbor(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, "rich-metadata-flac.b64",
                                    directory.path() / "alias-source.flac");
    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    CHECK(read && read->document.effective_native_field("ALBUMARTIST").has_value());
    CHECK(read && !read->document.effective_native_field("ALBUM ARTIST").has_value());
    if (!read) {
        return;
    }

    auto add_selection = selection_for(*read);
    CHECK(add_selection.has_value());
    if (!add_selection) {
        return;
    }
    const auto legacy = add_selection->ensure_exact_native_field("ALBUM ARTIST", "ALBUM ARTIST");
    CHECK(legacy.has_value());
    CHECK(legacy && add_selection->field(*legacy).exact_native_name ==
                        std::optional<std::string>{"album artist"});
    if (!legacy) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet add_patches;
    CHECK(add_patches.replace_values(*add_selection, 0U, *legacy, {"Legacy custom"}).has_value());
    auto add_plan = make_plan(*add_selection, add_patches);
    CHECK(add_plan && add_plan->ready());
    CHECK(add_plan && add_plan->sources.front().changes.front().exact_native_name ==
                          std::optional<std::string>{"album artist"});
    if (!add_plan || !add_plan->ready()) {
        return;
    }
    const auto with_legacy = directory.path() / "with-legacy.flac";
    const auto added = trackknife::metadata::prepare_flac_metadata_write_copy(
        add_plan->sources.front(), with_legacy.native());
    CHECK(added.has_value());
    if (!added) {
        return;
    }
    CHECK(added->document.effective_values("albumartist") ==
          std::vector<std::string>{"Album Credit"});
    CHECK(added->document.effective_values("album artist") ==
          std::vector<std::string>{"Legacy custom"});

    const auto reread = trackknife::metadata::read_local_metadata(with_legacy.native());
    CHECK(reread.has_value());
    if (!reread) {
        return;
    }
    auto remove_selection = selection_for(*reread);
    CHECK(remove_selection.has_value());
    if (!remove_selection) {
        return;
    }
    const auto conventional = remove_selection->field_index("albumartist");
    const auto freeform = remove_selection->exact_native_field_index("album artist");
    CHECK(conventional.has_value());
    CHECK(freeform.has_value());
    CHECK(conventional != freeform);
    if (!freeform) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet remove_patches;
    CHECK(remove_patches.remove_field(*remove_selection, 0U, *freeform).has_value());
    auto remove_plan = make_plan(*remove_selection, remove_patches);
    CHECK(remove_plan && remove_plan->ready());
    if (!remove_plan || !remove_plan->ready()) {
        return;
    }
    const auto cleaned = directory.path() / "cleaned.flac";
    const auto removed = trackknife::metadata::prepare_flac_metadata_write_copy(
        remove_plan->sources.front(), cleaned.native());
    CHECK(removed.has_value());
    CHECK(removed && !removed->document.effective_native_field("ALBUM ARTIST").has_value());
    CHECK(removed && removed->document.effective_values("albumartist") ==
                         std::vector<std::string>{"Album Credit"});
}

void preservesEmbeddedArtworkAndDecodedAudio(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source =
        materialize(fixture_directory, "art-tone-flac.b64", directory.path() / "art-source.flac");
    const auto artwork = trackknife::formats::load_embedded_artwork(source.native());
    auto artwork_policy = trackknife::metadata::default_artwork_inventory_policy();
    artwork_policy.external_patterns.clear();
    const auto inventory =
        trackknife::metadata::read_local_artwork_inventory(source.native(), artwork_policy);
    const auto pcm = decode_all(source);
    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(artwork.has_value());
    CHECK(inventory.has_value());
    CHECK(pcm.has_value());
    CHECK(read.has_value());
    if (!artwork || !inventory || !pcm || !read) {
        return;
    }
    auto selection = selection_for(*read);
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("TITLE");
    CHECK(title.has_value());
    if (!title) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"Artwork preserved"}).has_value());
    auto plan = make_plan(*selection, patches);
    CHECK(plan && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto destination = directory.path() / "art-prepared.flac";
    const auto prepared = trackknife::metadata::prepare_flac_metadata_write_copy(
        plan->sources.front(), destination.native());
    CHECK(prepared.has_value());
    const auto prepared_artwork = trackknife::formats::load_embedded_artwork(destination.native());
    const auto prepared_inventory =
        trackknife::metadata::read_local_artwork_inventory(destination.native(), artwork_policy);
    CHECK(prepared_artwork == artwork);
    CHECK(prepared_inventory.has_value());
    CHECK(prepared_inventory && prepared_inventory->items.size() == inventory->items.size());
    if (prepared_inventory && prepared_inventory->items.size() == inventory->items.size()) {
        for (std::size_t index = 0U; index < inventory->items.size(); ++index) {
            const auto& before = inventory->items[index];
            const auto& after = prepared_inventory->items[index];
            CHECK(after.role == before.role);
            CHECK(after.native_type == before.native_type);
            CHECK(after.mime_type == before.mime_type);
            CHECK(after.description == before.description);
            CHECK(after.width == before.width);
            CHECK(after.height == before.height);
            CHECK(after.byte_size == before.byte_size);
            CHECK(after.content_fingerprint == before.content_fingerprint);
            CHECK(after.provenance == before.provenance);
            CHECK(after.source_ordinal == before.source_ordinal);
            CHECK(after.duplicate_of == before.duplicate_of);
        }
    }
    CHECK(decode_all(destination) == pcm);
}

void preparesVerifiedArtworkReplaceAndRemove(const std::filesystem::path& fixture_directory) {
    using trackknife::metadata::ArtworkProvenance;
    using trackknife::metadata::ArtworkWritePlanIntent;
    using trackknife::metadata::ArtworkWritePlanIntentKind;

    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, "art-tone-flac.b64",
                                    directory.path() / "artwork-source.flac");
    const auto replacement = materialize(fixture_directory, "external-blue-jpeg.b64",
                                         directory.path() / "replacement.jpg");
    const auto replacement_bytes = read_bytes(replacement);
    add_secondary_picture(source, replacement_bytes);
    add_unknown_application_block(source);

    auto artwork_policy = trackknife::metadata::default_artwork_inventory_policy();
    artwork_policy.external_patterns.clear();
    const auto source_inventory =
        trackknife::metadata::read_local_artwork_inventory(source.native(), artwork_policy);
    const auto source_document = trackknife::metadata::read_local_metadata(source.native());
    const auto source_pcm = decode_all(source);
    const auto source_applications = application_payloads(source);
    const auto source_bytes = read_bytes(source);
    CHECK(source_inventory.has_value());
    CHECK(source_inventory && source_inventory->items.size() == 2U);
    CHECK(source_document.has_value());
    CHECK(source_pcm.has_value());
    CHECK(source_applications.size() == 1U);
    if (!source_inventory || source_inventory->items.size() != 2U || !source_document ||
        !source_pcm) {
        return;
    }
    CHECK(source_inventory->items[1].role == trackknife::metadata::ArtworkRole::back);
    CHECK(source_inventory->items[1].native_type == "Back Cover");
    CHECK(source_inventory->items[1].description == "Secondary");

    const ArtworkWritePlanIntent replace_intent{
        .occurrence_index = 3U,
        .raw_media_path = source.native(),
        .expected_media_revision = source_inventory->media_revision,
        .target_ordinal = 0U,
        .expected_target_fingerprint = source_inventory->items[0].content_fingerprint,
        .kind = ArtworkWritePlanIntentKind::replace,
        .replacement_raw_path = replacement.native(),
        .added_role = trackknife::metadata::ArtworkRole::front,
        .added_description = {},
        .replacement_embedded_source = std::nullopt,
    };
    const auto replace_plan = trackknife::metadata::revalidate_artwork_write_plan({replace_intent});
    CHECK(replace_plan.has_value());
    CHECK(replace_plan && replace_plan->ready());
    if (!replace_plan || !replace_plan->ready()) {
        return;
    }
    const auto replaced_path = directory.path() / "replaced.flac";
    const auto replaced = trackknife::metadata::prepare_flac_artwork_write_copy(
        replace_plan->sources.front(), replaced_path.native());
    CHECK(replaced.has_value());
    if (!replaced) {
        std::cerr << replaced.error().message << '\n';
        return;
    }
    CHECK(read_bytes(source) == source_bytes);
    CHECK(application_payloads(replaced_path) == source_applications);
    CHECK(replaced->document == source_document->document);
    CHECK(replaced->inventory.items.size() == 2U);
    CHECK(replaced->inventory.items[0].mime_type == "image/jpeg");
    CHECK(replaced->inventory.items[0].width == 8U);
    CHECK(replaced->inventory.items[0].height == 6U);
    CHECK(replaced->inventory.items[0].content_fingerprint ==
          replace_plan->sources.front().change.replacement->content_fingerprint);
    CHECK(replaced->inventory.items[0].native_type == source_inventory->items[0].native_type);
    CHECK(replaced->inventory.items[0].description == source_inventory->items[0].description);
    CHECK(replaced->inventory.items[1].content_fingerprint ==
          source_inventory->items[1].content_fingerprint);
    CHECK(replaced->inventory.items[1].native_type == source_inventory->items[1].native_type);
    CHECK(replaced->inventory.items[1].description == source_inventory->items[1].description);
    CHECK(replaced->inventory.items[1].source_ordinal == 1U);
    CHECK(decode_all(replaced_path) == source_pcm);

    auto distinct_replacement_bytes = replacement_bytes;
    distinct_replacement_bytes.push_back(0U);
    const auto add_replacement = directory.path() / "add-replacement.jpg";
    write_bytes(add_replacement, distinct_replacement_bytes);
    auto add_intent = replace_intent;
    add_intent.kind = ArtworkWritePlanIntentKind::add;
    add_intent.target_ordinal = 999U;
    add_intent.expected_target_fingerprint = {};
    add_intent.replacement_raw_path = add_replacement.native();
    add_intent.added_role = trackknife::metadata::ArtworkRole::artist;
    add_intent.added_description = "Tour portrait";
    const auto add_plan = trackknife::metadata::revalidate_artwork_write_plan({add_intent});
    CHECK(add_plan.has_value());
    CHECK(add_plan && add_plan->ready());
    if (!add_plan || !add_plan->ready()) {
        return;
    }
    const auto added_path = directory.path() / "added.flac";
    const auto added = trackknife::metadata::prepare_flac_artwork_write_copy(
        add_plan->sources.front(), added_path.native());
    CHECK(added.has_value());
    if (!added) {
        std::cerr << added.error().message << '\n';
        return;
    }
    CHECK(read_bytes(source) == source_bytes);
    CHECK(application_payloads(added_path) == source_applications);
    CHECK(added->document == source_document->document);
    CHECK(added->inventory.items.size() == 3U);
    CHECK(added->inventory.items[0].content_fingerprint ==
          source_inventory->items[0].content_fingerprint);
    CHECK(added->inventory.items[0].native_type == source_inventory->items[0].native_type);
    CHECK(added->inventory.items[0].description == source_inventory->items[0].description);
    CHECK(added->inventory.items[1].content_fingerprint ==
          source_inventory->items[1].content_fingerprint);
    CHECK(added->inventory.items[1].native_type == source_inventory->items[1].native_type);
    CHECK(added->inventory.items[1].description == source_inventory->items[1].description);
    CHECK(added->inventory.items[2].role == trackknife::metadata::ArtworkRole::artist);
    CHECK(added->inventory.items[2].native_type == "Artist");
    CHECK(added->inventory.items[2].description == "Tour portrait");
    CHECK(added->inventory.items[2].mime_type == "image/jpeg");
    CHECK(added->inventory.items[2].content_fingerprint ==
          add_plan->sources.front().change.replacement->content_fingerprint);
    CHECK(added->inventory.items[2].source_ordinal == 2U);
    CHECK(decode_all(added_path) == source_pcm);

    const auto copy_target = materialize(fixture_directory, "tagged-tone-flac.b64",
                                         directory.path() / "embedded-copy-target.flac");
    const auto copy_target_inventory =
        trackknife::metadata::read_local_artwork_inventory(copy_target.native(), artwork_policy);
    CHECK(copy_target_inventory && copy_target_inventory->items.empty());
    if (!copy_target_inventory) {
        return;
    }
    const ArtworkWritePlanIntent copy_intent{
        .occurrence_index = 8U,
        .raw_media_path = copy_target.native(),
        .expected_media_revision = copy_target_inventory->media_revision,
        .target_ordinal = 0U,
        .expected_target_fingerprint = {},
        .kind = ArtworkWritePlanIntentKind::add,
        .replacement_raw_path = source.native(),
        .added_role = source_inventory->items[0].role,
        .added_description = source_inventory->items[0].description,
        .replacement_embedded_source = source_inventory->items[0],
    };
    const auto copy_plan = trackknife::metadata::revalidate_artwork_write_plan({copy_intent});
    CHECK(copy_plan && copy_plan->ready());
    if (!copy_plan || !copy_plan->ready()) {
        return;
    }
    const auto copied_path = directory.path() / "embedded-copied.flac";
    const auto copied = trackknife::metadata::prepare_flac_artwork_write_copy(
        copy_plan->sources.front(), copied_path.native());
    CHECK(copied && copied->inventory.items.size() == 1U &&
          copied->inventory.items.front().content_fingerprint ==
              source_inventory->items.front().content_fingerprint &&
          copied->inventory.items.front().role == source_inventory->items.front().role);

    auto remove_intent = replace_intent;
    remove_intent.kind = ArtworkWritePlanIntentKind::remove;
    remove_intent.replacement_raw_path.reset();
    const auto remove_plan = trackknife::metadata::revalidate_artwork_write_plan({remove_intent});
    CHECK(remove_plan.has_value());
    CHECK(remove_plan && remove_plan->ready());
    if (!remove_plan || !remove_plan->ready()) {
        return;
    }
    const auto removed_path = directory.path() / "removed.flac";
    const auto removed = trackknife::metadata::prepare_flac_artwork_write_copy(
        remove_plan->sources.front(), removed_path.native());
    CHECK(removed.has_value());
    if (!removed) {
        std::cerr << removed.error().message << '\n';
        return;
    }
    CHECK(read_bytes(source) == source_bytes);
    CHECK(application_payloads(removed_path) == source_applications);
    CHECK(removed->document == source_document->document);
    CHECK(removed->inventory.items.size() == 1U);
    CHECK(removed->inventory.items.front().provenance == ArtworkProvenance::embedded);
    CHECK(removed->inventory.items.front().content_fingerprint ==
          source_inventory->items[1].content_fingerprint);
    CHECK(removed->inventory.items.front().native_type == source_inventory->items[1].native_type);
    CHECK(removed->inventory.items.front().description == source_inventory->items[1].description);
    CHECK(removed->inventory.items.front().source_ordinal == 0U);
    CHECK(decode_all(removed_path) == source_pcm);

    const auto replaced_bytes = read_bytes(replaced_path);
    const auto existing = trackknife::metadata::prepare_flac_artwork_write_copy(
        replace_plan->sources.front(), replaced_path.native());
    CHECK(!existing.has_value());
    CHECK(!existing && existing.error().code == trackknife::core::ErrorCode::conflict);
    CHECK(read_bytes(replaced_path) == replaced_bytes);

    const auto replacement_time = std::filesystem::last_write_time(replacement);
    std::filesystem::last_write_time(replacement, replacement_time + std::chrono::seconds{1});
    const auto stale_path = directory.path() / "stale-artwork.flac";
    const auto stale = trackknife::metadata::prepare_flac_artwork_write_copy(
        replace_plan->sources.front(), stale_path.native());
    CHECK(!stale.has_value());
    CHECK(!stale && stale.error().code == trackknife::core::ErrorCode::conflict);
    CHECK(!std::filesystem::exists(stale_path));

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled_path = directory.path() / "cancelled-artwork.flac";
    const auto cancelled = trackknife::metadata::prepare_flac_artwork_write_copy(
        remove_plan->sources.front(), cancelled_path.native(), cancellation.token());
    CHECK(!cancelled.has_value());
    CHECK(!cancelled && cancelled.error().code == trackknife::core::ErrorCode::cancelled);
    CHECK(!std::filesystem::exists(cancelled_path));
}

void blocksUnrepresentablePlansAndStaleOrCancelledWrites(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source =
        materialize(fixture_directory, "rich-metadata-flac.b64", directory.path() / "source.flac");
    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }

    auto empty_selection = selection_for(*read);
    const auto artist = empty_selection ? empty_selection->field_index("ARTIST") : std::nullopt;
    CHECK(empty_selection.has_value());
    CHECK(artist.has_value());
    if (!empty_selection || !artist) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet empty_patches;
    CHECK(empty_patches.replace_values(*empty_selection, 0U, *artist, {""}).has_value());
    auto empty_plan = make_plan(*empty_selection, empty_patches);
    CHECK(empty_plan.has_value());
    CHECK(empty_plan && !empty_plan->ready());
    CHECK(empty_plan &&
          has_issue(empty_plan->sources.front(),
                    trackknife::metadata::MetadataWritePlanIssueKind::unsupported_field_mapping));

    auto invalid_key_selection = selection_for(*read);
    CHECK(invalid_key_selection.has_value());
    if (!invalid_key_selection) {
        return;
    }
    const auto invalid_key = invalid_key_selection->ensure_missing_field("BAD~FIELD", "BAD~FIELD");
    CHECK(invalid_key.has_value());
    if (!invalid_key) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet invalid_key_patches;
    CHECK(invalid_key_patches.replace_values(*invalid_key_selection, 0U, *invalid_key, {"value"})
              .has_value());
    auto invalid_key_plan = make_plan(*invalid_key_selection, invalid_key_patches);
    CHECK(invalid_key_plan && !invalid_key_plan->ready());
    CHECK(invalid_key_plan &&
          has_issue(invalid_key_plan->sources.front(),
                    trackknife::metadata::MetadataWritePlanIssueKind::unsupported_field_mapping));

    auto invalid_utf8_selection = selection_for(*read);
    CHECK(invalid_utf8_selection.has_value());
    const auto invalid_utf8_title =
        invalid_utf8_selection ? invalid_utf8_selection->field_index("TITLE") : std::nullopt;
    CHECK(invalid_utf8_title.has_value());
    if (!invalid_utf8_selection || !invalid_utf8_title) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet invalid_utf8_patches;
    CHECK(invalid_utf8_patches
              .replace_values(*invalid_utf8_selection, 0U, *invalid_utf8_title,
                              {std::string{"\xC3\x28", 2U}})
              .has_value());
    auto invalid_utf8_plan = make_plan(*invalid_utf8_selection, invalid_utf8_patches);
    CHECK(invalid_utf8_plan && !invalid_utf8_plan->ready());
    CHECK(invalid_utf8_plan &&
          has_issue(invalid_utf8_plan->sources.front(),
                    trackknife::metadata::MetadataWritePlanIssueKind::unsupported_field_mapping));

    auto selection = selection_for(*read);
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("TITLE");
    CHECK(title.has_value());
    if (!title) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"Changed"}).has_value());
    auto plan = make_plan(*selection, patches);
    CHECK(plan && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled_path = directory.path() / "cancelled.flac";
    const auto cancelled = trackknife::metadata::prepare_flac_metadata_write_copy(
        plan->sources.front(), cancelled_path.native(), cancellation.token());
    CHECK(!cancelled.has_value());
    CHECK(!cancelled && cancelled.error().code == trackknife::core::ErrorCode::cancelled);
    CHECK(!std::filesystem::exists(cancelled_path));

    auto mismatched_plan = plan->sources.front();
    mismatched_plan.changes.front().original_values = {"Not the fresh original"};
    const auto mismatch_path = directory.path() / "mismatch.flac";
    const auto mismatch = trackknife::metadata::prepare_flac_metadata_write_copy(
        mismatched_plan, mismatch_path.native());
    CHECK(!mismatch.has_value());
    CHECK(!mismatch && mismatch.error().code == trackknife::core::ErrorCode::conflict);
    CHECK(!std::filesystem::exists(mismatch_path));

    const auto old_time = std::filesystem::last_write_time(source);
    std::filesystem::last_write_time(source, old_time + std::chrono::seconds{1});
    const auto stale_path = directory.path() / "stale.flac";
    const auto stale = trackknife::metadata::prepare_flac_metadata_write_copy(plan->sources.front(),
                                                                              stale_path.native());
    CHECK(!stale.has_value());
    CHECK(!stale && stale.error().code == trackknife::core::ErrorCode::conflict);
    CHECK(!std::filesystem::exists(stale_path));
}

} // namespace

[[nodiscard]] bool bytes_contain(const std::vector<unsigned char>& bytes,
                                 const std::string_view needle) {
    return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
}

void writesPairedTotalsSpellingsLikePicard(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, "rich-metadata-flac.b64",
                                    directory.path() / "paired-totals.flac");
    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    auto selection = selection_for(*read);
    if (!selection) {
        return;
    }
    const auto totals = selection->ensure_missing_field("Total Tracks", "Total Tracks");
    const auto disc_totals = selection->ensure_missing_field("Total Discs", "Total Discs");
    CHECK(totals.has_value());
    CHECK(disc_totals.has_value());
    if (!totals || !disc_totals) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *totals, {"9"}).has_value());
    CHECK(patches.replace_values(*selection, 0U, *disc_totals, {"2"}).has_value());
    auto plan = make_plan(*selection, patches);
    CHECK(plan && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto prepared_path = directory.path() / "paired-written.flac";
    const auto prepared = trackknife::metadata::prepare_flac_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    CHECK(prepared.has_value());
    if (!prepared) {
        std::cerr << prepared.error().message << '\n';
        return;
    }
    // Picard write rule: both spellings land in the file for every consumer.
    const auto written = read_bytes(prepared_path);
    CHECK(bytes_contain(written, "TOTALTRACKS=9"));
    CHECK(bytes_contain(written, "TRACKTOTAL=9"));
    CHECK(bytes_contain(written, "TOTALDISCS=2"));
    CHECK(bytes_contain(written, "DISCTOTAL=2"));

    // Picard load rule: one logical field surfaces, primary spelling first.
    const auto reread = trackknife::metadata::read_local_metadata(prepared_path.native());
    CHECK(reread.has_value());
    if (!reread) {
        return;
    }
    const auto totals_fields =
        std::ranges::count_if(reread->document.fields, [](const auto& field) {
            return field.canonical_name == "totaltracks";
        });
    CHECK(totals_fields == 1);
    const auto found = std::ranges::find_if(reread->document.fields, [](const auto& field) {
        return field.canonical_name == "totaltracks";
    });
    CHECK(found != reread->document.fields.end() && found->native_name == "TOTALTRACKS");
    CHECK(reread->document.effective_values("totaltracks") == (std::vector<std::string>{"9"}));
    CHECK(reread->document.effective_values("tracktotal") == (std::vector<std::string>{"9"}));

    // A later edit refreshes both spellings, and removal deletes both.
    auto second_selection = selection_for(*reread);
    if (!second_selection) {
        return;
    }
    const auto second_totals = second_selection->field_index("totaltracks");
    CHECK(second_totals.has_value());
    if (!second_totals) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet second_patches;
    CHECK(second_patches.replace_values(*second_selection, 0U, *second_totals, {"10"}).has_value());
    auto second_plan = make_plan(*second_selection, second_patches);
    CHECK(second_plan && second_plan->ready());
    if (!second_plan || !second_plan->ready()) {
        return;
    }
    const auto updated_path = directory.path() / "paired-updated.flac";
    const auto updated = trackknife::metadata::prepare_flac_metadata_write_copy(
        second_plan->sources.front(), updated_path.native());
    CHECK(updated.has_value());
    if (!updated) {
        std::cerr << updated.error().message << '\n';
        return;
    }
    const auto updated_bytes = read_bytes(updated_path);
    CHECK(bytes_contain(updated_bytes, "TOTALTRACKS=10"));
    CHECK(bytes_contain(updated_bytes, "TRACKTOTAL=10"));
    CHECK(!bytes_contain(updated_bytes, "TOTALTRACKS=9"));
    CHECK(!bytes_contain(updated_bytes, "TRACKTOTAL=9"));

    const auto third_read = trackknife::metadata::read_local_metadata(updated_path.native());
    CHECK(third_read.has_value());
    if (!third_read) {
        return;
    }
    auto third_selection = selection_for(*third_read);
    if (!third_selection) {
        return;
    }
    const auto third_totals = third_selection->field_index("tracktotal");
    CHECK(third_totals.has_value());
    if (!third_totals) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet third_patches;
    CHECK(third_patches.remove_field(*third_selection, 0U, *third_totals).has_value());
    auto third_plan = make_plan(*third_selection, third_patches);
    CHECK(third_plan && third_plan->ready());
    if (!third_plan || !third_plan->ready()) {
        return;
    }
    const auto removed_path = directory.path() / "paired-removed.flac";
    const auto removed = trackknife::metadata::prepare_flac_metadata_write_copy(
        third_plan->sources.front(), removed_path.native());
    CHECK(removed.has_value());
    if (!removed) {
        std::cerr << removed.error().message << '\n';
        return;
    }
    const auto removed_bytes = read_bytes(removed_path);
    CHECK(!bytes_contain(removed_bytes, "TOTALTRACKS="));
    CHECK(!bytes_contain(removed_bytes, "TRACKTOTAL="));
    CHECK(bytes_contain(removed_bytes, "TOTALDISCS=2"));
    CHECK(bytes_contain(removed_bytes, "DISCTOTAL=2"));
}

// The user-reported apply failure (Picard-tagged files carrying both totals
// spellings, an automatic chain removing "totaltracks" in exact-native
// mode): the partner spelling must never survive to resurrect the value,
// and verification must accept the pair-wide result.
void exactNativeTotalsMutationsAddressThePair(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, "rich-metadata-flac.b64",
                                    directory.path() / "exact-pair.flac");
    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    auto selection = selection_for(*read);
    if (!selection) {
        return;
    }
    const auto totals = selection->ensure_missing_field("Total Tracks", "Total Tracks");
    CHECK(totals.has_value());
    if (!totals) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *totals, {"9"}).has_value());
    auto plan = make_plan(*selection, patches);
    CHECK(plan && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto paired_path = directory.path() / "exact-pair-written.flac";
    const auto prepared = trackknife::metadata::prepare_flac_metadata_write_copy(
        plan->sources.front(), paired_path.native());
    CHECK(prepared.has_value());
    if (!prepared) {
        std::cerr << prepared.error().message << '\n';
        return;
    }

    // Exact-native replace addressed at the secondary spelling refreshes
    // both spellings, exactly like the logical write.
    const auto reread = trackknife::metadata::read_local_metadata(paired_path.native());
    CHECK(reread.has_value());
    if (!reread) {
        return;
    }
    auto replace_selection = selection_for(*reread);
    if (!replace_selection) {
        return;
    }
    const auto secondary = replace_selection->ensure_exact_native_field("TRACKTOTAL", "TRACKTOTAL");
    CHECK(secondary.has_value());
    if (!secondary) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet replace_patches;
    CHECK(replace_patches.replace_values(*replace_selection, 0U, *secondary, {"12"}).has_value());
    auto replace_plan = make_plan(*replace_selection, replace_patches);
    CHECK(replace_plan && replace_plan->ready());
    if (!replace_plan || !replace_plan->ready()) {
        return;
    }
    const auto replaced_path = directory.path() / "exact-pair-replaced.flac";
    const auto replaced = trackknife::metadata::prepare_flac_metadata_write_copy(
        replace_plan->sources.front(), replaced_path.native());
    CHECK(replaced.has_value());
    if (!replaced) {
        std::cerr << replaced.error().message << '\n';
        return;
    }
    const auto replaced_bytes = read_bytes(replaced_path);
    CHECK(bytes_contain(replaced_bytes, "TOTALTRACKS=12"));
    CHECK(bytes_contain(replaced_bytes, "TRACKTOTAL=12"));
    CHECK(!bytes_contain(replaced_bytes, "TOTALTRACKS=9"));
    CHECK(!bytes_contain(replaced_bytes, "TRACKTOTAL=9"));

    // Exact-native removal addressed at the primary clears the whole pair;
    // the reread verification accepts the file it produced.
    const auto remove_read = trackknife::metadata::read_local_metadata(replaced_path.native());
    CHECK(remove_read.has_value());
    if (!remove_read) {
        return;
    }
    auto remove_selection = selection_for(*remove_read);
    if (!remove_selection) {
        return;
    }
    const auto exact_totals =
        remove_selection->ensure_exact_native_field("TOTALTRACKS", "TOTALTRACKS");
    CHECK(exact_totals.has_value());
    if (!exact_totals) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet remove_patches;
    CHECK(remove_patches.remove_field(*remove_selection, 0U, *exact_totals).has_value());
    auto remove_plan = make_plan(*remove_selection, remove_patches);
    CHECK(remove_plan && remove_plan->ready());
    if (!remove_plan || !remove_plan->ready()) {
        return;
    }
    const auto removed_path = directory.path() / "exact-pair-removed.flac";
    const auto removed = trackknife::metadata::prepare_flac_metadata_write_copy(
        remove_plan->sources.front(), removed_path.native());
    CHECK(removed.has_value());
    if (!removed) {
        std::cerr << removed.error().message << '\n';
        return;
    }
    const auto removed_bytes = read_bytes(removed_path);
    CHECK(!bytes_contain(removed_bytes, "TOTALTRACKS="));
    CHECK(!bytes_contain(removed_bytes, "TRACKTOTAL="));
}

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    if (argc == 2) {
        const std::filesystem::path fixture_directory{argv[1]};
        preservesNativeFlacWhileApplyingExactTextChanges(fixture_directory);
        freeformFieldNeverAliasesAConventionalNeighbor(fixture_directory);
        preservesEmbeddedArtworkAndDecodedAudio(fixture_directory);
        preparesVerifiedArtworkReplaceAndRemove(fixture_directory);
        writesPairedTotalsSpellingsLikePicard(fixture_directory);
        exactNativeTotalsMutationsAddressThePair(fixture_directory);
        blocksUnrepresentablePlansAndStaleOrCancelledWrites(fixture_directory);
    }
    return failures == 0 ? 0 : 1;
}
