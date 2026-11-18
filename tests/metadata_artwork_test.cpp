// SPDX-License-Identifier: GPL-3.0-only

#include "test_checks.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/artwork.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using checks::failures;

class TemporaryDirectory final {
  public:
    TemporaryDirectory()
        : path_{std::filesystem::temp_directory_path() /
                ("trackknife-artwork-" + trackknife::core::StableId::random().to_string())} {
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
    const auto decoded = decode_base64_file(fixture_directory / fixture);
    CHECK(decoded.has_value());
    if (decoded) {
        std::ofstream output{destination, std::ios::binary};
        output.write(reinterpret_cast<const char*>(decoded->data()),
                     static_cast<std::streamsize>(decoded->size()));
        CHECK(output.good());
    }
    return destination;
}

void write_bytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    CHECK(output.good());
}

void inventoriesEmbeddedAndExternalArtwork(const std::filesystem::path& fixture_directory) {
    using trackknife::metadata::ArtworkProvenance;
    using trackknife::metadata::ArtworkRole;

    TemporaryDirectory directory;
    const auto media =
        materialize(fixture_directory, "art-tone-flac.b64", directory.path() / "art-tone.flac");
    auto embedded_only_policy = trackknife::metadata::default_artwork_inventory_policy();
    embedded_only_policy.external_patterns.clear();
    const auto embedded =
        trackknife::metadata::read_local_artwork_inventory(media.native(), embedded_only_policy);
    CHECK(embedded.has_value());
    CHECK(embedded && embedded->raw_media_path == media.native());
    CHECK(embedded && embedded->capabilities.embedded_readable);
    CHECK(embedded && embedded->capabilities.external_readable);
    CHECK(embedded && embedded->embedded_adapter_name == "taglib-flac-picture-v1");
    CHECK(embedded && embedded->issues.empty());
    CHECK(embedded && embedded->items.size() == 1U);
    if (!embedded || embedded->items.size() != 1U) {
        return;
    }

    const auto& picture = embedded->items.front();
    // FFmpeg encoded this fixture's FLAC picture type as Other; inventory
    // must retain that exact native type instead of promoting the first image
    // to a front cover implicitly.
    CHECK(picture.role == ArtworkRole::other);
    CHECK(picture.native_type == "Other");
    CHECK(picture.mime_type == "image/png");
    CHECK(picture.width == 64U);
    CHECK(picture.height == 64U);
    CHECK(picture.byte_size > 8U);
    CHECK(picture.provenance == ArtworkProvenance::embedded);
    CHECK(picture.raw_source_path == media.native());
    CHECK(picture.source_revision == embedded->media_revision);
    CHECK(picture.source_ordinal == 0U);
    CHECK(!picture.duplicate_of.has_value());
    const auto fingerprint =
        trackknife::metadata::artwork_fingerprint_hex(picture.content_fingerprint);
    CHECK(fingerprint.size() == 64U);
    CHECK(std::ranges::any_of(fingerprint, [](const char byte) { return byte != '0'; }));

    const auto encoded = trackknife::formats::load_embedded_artwork(media.native());
    CHECK(encoded.has_value());
    if (!encoded) {
        return;
    }
    CHECK(picture.byte_size == encoded->size());
    const auto external_path = directory.path() / "cover.png";
    write_bytes(external_path, *encoded);

    const auto combined = trackknife::metadata::read_local_artwork_inventory(media.native());
    CHECK(combined.has_value());
    CHECK(combined && combined->issues.empty());
    CHECK(combined && combined->items.size() == 2U);
    if (!combined || combined->items.size() != 2U) {
        return;
    }
    const auto& external = combined->items[1U];
    CHECK(external.role == ArtworkRole::front);
    CHECK(external.native_type.empty());
    CHECK(external.mime_type == "image/png");
    CHECK(external.width == 64U);
    CHECK(external.height == 64U);
    CHECK(external.byte_size == encoded->size());
    CHECK(external.content_fingerprint == combined->items.front().content_fingerprint);
    CHECK(external.provenance == ArtworkProvenance::external);
    CHECK(external.raw_source_path == external_path.native());
    CHECK(external.source_revision.size == encoded->size());
    const auto expected_revision =
        trackknife::core::observe_local_source_revision(external_path.native());
    CHECK(expected_revision.has_value());
    CHECK(expected_revision && external.source_revision == *expected_revision);
    CHECK(external.duplicate_of == 0U);
    CHECK(trackknife::metadata::artwork_role_name(external.role) == "front");
    CHECK(trackknife::metadata::artwork_provenance_name(external.provenance) == "external");
}

void missingAndMalformedExternalArtworkRemainExplicit(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto media =
        materialize(fixture_directory, "tagged-tone-flac.b64", directory.path() / "plain.flac");
    auto policy = trackknife::metadata::default_artwork_inventory_policy();
    policy.external_patterns = {
        {.raw_basename = "front.jpg", .role = trackknife::metadata::ArtworkRole::front},
    };

    const auto missing = trackknife::metadata::read_local_artwork_inventory(media.native(), policy);
    CHECK(missing.has_value());
    CHECK(missing && missing->capabilities.embedded_readable);
    CHECK(missing && missing->items.empty());
    CHECK(missing && missing->issues.empty());

    const auto jpeg_path =
        materialize(fixture_directory, "external-blue-jpeg.b64", directory.path() / "front.jpg");
    CHECK(jpeg_path == directory.path() / "front.jpg");
    const auto jpeg = trackknife::metadata::read_local_artwork_inventory(media.native(), policy);
    CHECK(jpeg.has_value());
    CHECK(jpeg && jpeg->issues.empty());
    CHECK(jpeg && jpeg->items.size() == 1U);
    CHECK(jpeg && jpeg->items.front().mime_type == "image/jpeg");
    CHECK(jpeg && jpeg->items.front().width == 8U);
    CHECK(jpeg && jpeg->items.front().height == 6U);

    const std::vector<unsigned char> malformed{'n', 'o', 't', '-', 'j', 'p', 'e', 'g'};
    write_bytes(directory.path() / "front.jpg", malformed);
    const auto rejected =
        trackknife::metadata::read_local_artwork_inventory(media.native(), policy);
    CHECK(rejected.has_value());
    CHECK(rejected && rejected->items.empty());
    CHECK(rejected && rejected->issues.size() == 1U);
    CHECK(rejected &&
          rejected->issues.front().error.code == trackknife::core::ErrorCode::unsupported);

    policy.external_patterns.front().raw_basename = "nested/front.jpg";
    const auto invalid = trackknife::metadata::read_local_artwork_inventory(media.native(), policy);
    CHECK(!invalid.has_value());
    CHECK(!invalid && invalid.error().code == trackknife::core::ErrorCode::invalid_argument);

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled = trackknife::metadata::read_local_artwork_inventory(
        media.native(), trackknife::metadata::default_artwork_inventory_policy(),
        cancellation.token());
    CHECK(!cancelled.has_value());
    CHECK(!cancelled && cancelled.error().code == trackknife::core::ErrorCode::cancelled);
}

void limitsFailClosed(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto media =
        materialize(fixture_directory, "art-tone-flac.b64", directory.path() / "limited.flac");
    const auto encoded = trackknife::formats::load_embedded_artwork(media.native());
    CHECK(encoded.has_value());
    if (!encoded || encoded->empty()) {
        return;
    }
    auto policy = trackknife::metadata::default_artwork_inventory_policy();
    policy.external_patterns.clear();
    policy.maximum_item_bytes = encoded->size() - 1U;
    const auto limited = trackknife::metadata::read_local_artwork_inventory(media.native(), policy);
    CHECK(!limited.has_value());
    CHECK(!limited && limited.error().code == trackknife::core::ErrorCode::limit_exceeded);
}

[[nodiscard]] bool has_plan_issue(const trackknife::metadata::ArtworkWritePlanSource& source,
                                  const trackknife::metadata::ArtworkWritePlanIssueKind kind) {
    return std::ranges::any_of(source.issues,
                               [kind](const auto& issue) { return issue.kind == kind; });
}

void plansNativeFlacReplaceAndRemove(const std::filesystem::path& fixture_directory) {
    using trackknife::metadata::ArtworkWritePlanIntent;
    using trackknife::metadata::ArtworkWritePlanIntentKind;
    using trackknife::metadata::ArtworkWritePlanIssueKind;

    TemporaryDirectory directory;
    const auto media =
        materialize(fixture_directory, "art-tone-flac.b64", directory.path() / "planned.flac");
    const auto replacement = materialize(fixture_directory, "external-blue-jpeg.b64",
                                         directory.path() / "replacement.jpg");
    auto policy = trackknife::metadata::default_artwork_inventory_policy();
    policy.external_patterns.clear();
    const auto inventory =
        trackknife::metadata::read_local_artwork_inventory(media.native(), policy);
    const auto replacement_evidence =
        trackknife::metadata::read_artwork_image_file(replacement.native());
    CHECK(inventory.has_value());
    CHECK(inventory && inventory->items.size() == 1U);
    CHECK(replacement_evidence.has_value());
    CHECK(replacement_evidence && replacement_evidence->mime_type == "image/jpeg");
    CHECK(replacement_evidence && replacement_evidence->width == 8U);
    CHECK(replacement_evidence && replacement_evidence->height == 6U);
    if (!inventory || inventory->items.size() != 1U || !replacement_evidence) {
        return;
    }

    const ArtworkWritePlanIntent first{
        .occurrence_index = 2U,
        .raw_media_path = media.native(),
        .expected_media_revision = inventory->media_revision,
        .target_ordinal = 0U,
        .expected_target_fingerprint = inventory->items.front().content_fingerprint,
        .kind = ArtworkWritePlanIntentKind::replace,
        .replacement_raw_path = replacement.native(),
        .added_role = trackknife::metadata::ArtworkRole::front,
        .added_description = {},
        .replacement_embedded_source = std::nullopt,
    };
    auto duplicate = first;
    duplicate.occurrence_index = 7U;
    const auto replace = trackknife::metadata::revalidate_artwork_write_plan({first, duplicate});
    CHECK(replace.has_value());
    CHECK(replace && replace->ready());
    CHECK(replace && replace->logical_intent_count == 2U);
    CHECK(replace && replace->sources.size() == 1U);
    CHECK(replace &&
          replace->sources.front().occurrence_indexes == (std::vector<std::size_t>{2U, 7U}));
    CHECK(replace && replace->sources.front().adapter_name == "taglib-flac-picture-v1");
    CHECK(replace && replace->sources.front().change.original == inventory->items.front());
    CHECK(replace && replace->sources.front().change.replacement == replacement_evidence);
    CHECK(replace && replace->blocking_issue_count() == 0U);

    auto add_intent = first;
    add_intent.kind = ArtworkWritePlanIntentKind::add;
    add_intent.target_ordinal = 999U;
    add_intent.expected_target_fingerprint = {};
    add_intent.added_role = trackknife::metadata::ArtworkRole::back;
    add_intent.added_description = "Selected back cover";
    const auto add = trackknife::metadata::revalidate_artwork_write_plan({add_intent});
    CHECK(add.has_value());
    CHECK(add && add->ready());
    CHECK(add && add->sources.front().change.kind == ArtworkWritePlanIntentKind::add);
    CHECK(add && add->sources.front().change.target_ordinal == 1U);
    CHECK(add && !add->sources.front().change.original.has_value());
    CHECK(add && add->sources.front().change.replacement == replacement_evidence);
    CHECK(add && add->sources.front().change.added_role == trackknife::metadata::ArtworkRole::back);
    CHECK(add && add->sources.front().change.added_description == "Selected back cover");
    CHECK(add && trackknife::metadata::artwork_write_plan_intent_kind_name(
                     add->sources.front().change.kind) == "add");

    const auto copy_target = materialize(fixture_directory, "tagged-tone-flac.b64",
                                         directory.path() / "copy-target.flac");
    const auto copy_target_inventory =
        trackknife::metadata::read_local_artwork_inventory(copy_target.native(), policy);
    CHECK(copy_target_inventory && copy_target_inventory->items.empty());
    if (!copy_target_inventory) {
        return;
    }
    const ArtworkWritePlanIntent copy_intent{
        .occurrence_index = 12U,
        .raw_media_path = copy_target.native(),
        .expected_media_revision = copy_target_inventory->media_revision,
        .target_ordinal = 0U,
        .expected_target_fingerprint = {},
        .kind = ArtworkWritePlanIntentKind::add,
        .replacement_raw_path = inventory->items.front().raw_source_path,
        .added_role = inventory->items.front().role,
        .added_description = inventory->items.front().description,
        .replacement_embedded_source = inventory->items.front(),
    };
    const auto copy = trackknife::metadata::revalidate_artwork_write_plan({copy_intent});
    CHECK(copy && copy->ready());
    CHECK(copy && copy->sources.front().change.replacement &&
          copy->sources.front().change.replacement->embedded_source_ordinal == 0U);
    const auto copied_bytes = copy && copy->sources.front().change.replacement
                                  ? trackknife::metadata::read_artwork_image_bytes(
                                        *copy->sources.front().change.replacement)
                                  : trackknife::core::Result<std::vector<unsigned char>>{
                                        std::unexpected(trackknife::core::Error{
                                            .code = trackknife::core::ErrorCode::invariant,
                                            .message = "copy plan did not retain donor evidence",
                                            .context = {},
                                        })};
    const auto donor_bytes = trackknife::formats::load_embedded_artwork(media.native());
    CHECK(copied_bytes && donor_bytes && *copied_bytes == *donor_bytes);

    auto remove_intent = first;
    remove_intent.kind = ArtworkWritePlanIntentKind::remove;
    remove_intent.replacement_raw_path.reset();
    const auto remove = trackknife::metadata::revalidate_artwork_write_plan({remove_intent});
    CHECK(remove.has_value());
    CHECK(remove && remove->ready());
    CHECK(remove && !remove->sources.front().change.replacement.has_value());
    CHECK(remove && trackknife::metadata::artwork_write_plan_intent_kind_name(
                        remove->sources.front().change.kind) == "remove");

    auto stale = first;
    ++stale.expected_media_revision->size;
    const auto stale_plan = trackknife::metadata::revalidate_artwork_write_plan({stale});
    CHECK(stale_plan.has_value());
    CHECK(stale_plan && !stale_plan->ready());
    CHECK(stale_plan &&
          has_plan_issue(stale_plan->sources.front(), ArtworkWritePlanIssueKind::source_changed));

    auto changed_target = first;
    changed_target.expected_target_fingerprint = {};
    const auto changed_plan = trackknife::metadata::revalidate_artwork_write_plan({changed_target});
    CHECK(changed_plan.has_value());
    CHECK(changed_plan && !changed_plan->ready());
    CHECK(changed_plan &&
          has_plan_issue(changed_plan->sources.front(), ArtworkWritePlanIssueKind::target_changed));

    const auto conflicting =
        trackknife::metadata::revalidate_artwork_write_plan({first, remove_intent});
    CHECK(conflicting.has_value());
    CHECK(conflicting && !conflicting->ready());
    CHECK(conflicting && has_plan_issue(conflicting->sources.front(),
                                        ArtworkWritePlanIssueKind::conflicting_logical_intents));

    const auto embedded_bytes = trackknife::formats::load_embedded_artwork(media.native());
    CHECK(embedded_bytes.has_value());
    if (embedded_bytes) {
        const auto unchanged_path = directory.path() / "unchanged.png";
        write_bytes(unchanged_path, *embedded_bytes);
        auto unchanged = first;
        unchanged.replacement_raw_path = unchanged_path.native();
        const auto unchanged_plan =
            trackknife::metadata::revalidate_artwork_write_plan({unchanged});
        CHECK(unchanged_plan.has_value());
        CHECK(unchanged_plan && !unchanged_plan->ready());
        CHECK(unchanged_plan && has_plan_issue(unchanged_plan->sources.front(),
                                               ArtworkWritePlanIssueKind::replacement_unchanged));
        auto duplicate_add = add_intent;
        duplicate_add.replacement_raw_path = unchanged_path.native();
        const auto duplicate_add_plan =
            trackknife::metadata::revalidate_artwork_write_plan({duplicate_add});
        CHECK(duplicate_add_plan.has_value());
        CHECK(duplicate_add_plan && !duplicate_add_plan->ready());
        CHECK(duplicate_add_plan &&
              has_plan_issue(duplicate_add_plan->sources.front(),
                             ArtworkWritePlanIssueKind::replacement_unchanged));
    }

    const auto malformed_path = directory.path() / "malformed.jpg";
    write_bytes(malformed_path, {'n', 'o', 't', '-', 'a', 'n', '-', 'i', 'm', 'a', 'g', 'e'});
    auto malformed = first;
    malformed.replacement_raw_path = malformed_path.native();
    const auto malformed_plan = trackknife::metadata::revalidate_artwork_write_plan({malformed});
    CHECK(malformed_plan.has_value());
    CHECK(malformed_plan && !malformed_plan->ready());
    CHECK(malformed_plan && has_plan_issue(malformed_plan->sources.front(),
                                           ArtworkWritePlanIssueKind::replacement_unsupported));

    const auto alias = directory.path() / "alias.flac";
    std::error_code link_error;
    std::filesystem::create_hard_link(media, alias, link_error);
    CHECK(!link_error);
    auto alias_intent = remove_intent;
    alias_intent.occurrence_index = 11U;
    alias_intent.raw_media_path = alias.native();
    const auto alias_plan =
        trackknife::metadata::revalidate_artwork_write_plan({remove_intent, alias_intent});
    CHECK(alias_plan.has_value());
    CHECK(alias_plan && !alias_plan->ready());
    CHECK(alias_plan && alias_plan->sources.size() == 2U);
    CHECK(alias_plan &&
          has_plan_issue(alias_plan->sources[0], ArtworkWritePlanIssueKind::physical_source_alias));
    CHECK(alias_plan &&
          has_plan_issue(alias_plan->sources[1], ArtworkWritePlanIssueKind::physical_source_alias));

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled =
        trackknife::metadata::revalidate_artwork_write_plan({first}, cancellation.token());
    CHECK(!cancelled.has_value());
    CHECK(!cancelled && cancelled.error().code == trackknife::core::ErrorCode::cancelled);
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    if (argc == 2) {
        const std::filesystem::path fixture_directory{argv[1]};
        inventoriesEmbeddedAndExternalArtwork(fixture_directory);
        missingAndMalformedExternalArtworkRemainExplicit(fixture_directory);
        limitsFailClosed(fixture_directory);
        plansNativeFlacReplaceAndRemove(fixture_directory);
    }
    return failures == 0 ? 0 : 1;
}
