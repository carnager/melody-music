// SPDX-License-Identifier: GPL-3.0-only

#include "test_checks.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/artwork.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/operations/artwork_export.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
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
                ("trackknife-artwork-export-" + trackknife::core::StableId::random().to_string())} {
        std::error_code error;
        CHECK(std::filesystem::create_directory(path_, error));
        CHECK(!error);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        CHECK(!error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

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
        const auto value = values[static_cast<unsigned char>(character)];
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

[[nodiscard]] std::filesystem::path materialize(const std::filesystem::path& fixtures,
                                                const std::string_view fixture,
                                                const std::filesystem::path& destination) {
    const auto bytes = decode_base64_file(fixtures / fixture);
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

void exportsWithoutOverwriteAndReportsOrderedPartialResults(const std::filesystem::path& fixtures) {
    using trackknife::operations::ArtworkExportItemState;
    using trackknife::operations::ArtworkExportRequest;

    TemporaryDirectory directory;
    const auto source =
        materialize(fixtures, "art-tone-flac.b64", directory.path() / "source.flac");
    auto policy = trackknife::metadata::default_artwork_inventory_policy();
    policy.external_patterns.clear();
    const auto inventory =
        trackknife::metadata::read_local_artwork_inventory(source.native(), policy);
    const auto expected = trackknife::formats::load_embedded_artwork(source.native());
    CHECK(inventory && inventory->items.size() == 1U && expected);
    if (!inventory || inventory->items.size() != 1U || !expected) {
        return;
    }
    const auto successful_path = directory.path() / "exported.png";
    const auto existing_path = directory.path() / "existing.png";
    const std::vector<unsigned char> sentinel{'k', 'e', 'e', 'p'};
    {
        std::ofstream output{existing_path, std::ios::binary};
        output.write(reinterpret_cast<const char*>(sentinel.data()),
                     static_cast<std::streamsize>(sentinel.size()));
    }
    std::vector<trackknife::operations::ArtworkExportProgress> progress;
    const auto exported = trackknife::operations::export_artwork_items(
        {
            ArtworkExportRequest{.source = inventory->items.front(),
                                 .destination_raw_path = successful_path.native()},
            ArtworkExportRequest{.source = inventory->items.front(),
                                 .destination_raw_path = existing_path.native()},
        },
        [&progress](const auto& update) { progress.push_back(update); });
    CHECK(exported && exported->items.size() == 2U);
    CHECK(exported && exported->items[0].state == ArtworkExportItemState::exported);
    CHECK(exported && exported->items[1].state == ArtworkExportItemState::failed);
    CHECK(exported && exported->exported_item_count() == 1U && exported->failed_item_count() == 1U);
    CHECK(read_bytes(successful_path) == *expected);
    CHECK(read_bytes(existing_path) == sentinel);
    CHECK(!progress.empty());

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled_path = directory.path() / "cancelled.png";
    const auto cancelled = trackknife::operations::export_artwork_items(
        {ArtworkExportRequest{.source = inventory->items.front(),
                              .destination_raw_path = cancelled_path.native()}},
        {}, cancellation.token());
    CHECK(cancelled && cancelled->cancellation_requested &&
          cancelled->items.front().state == ArtworkExportItemState::cancelled);
    CHECK(!std::filesystem::exists(cancelled_path));

    const auto source_time = std::filesystem::last_write_time(source);
    std::filesystem::last_write_time(source, source_time + std::chrono::seconds{1});
    const auto stale_path = directory.path() / "stale.png";
    const auto stale = trackknife::operations::export_artwork_items({ArtworkExportRequest{
        .source = inventory->items.front(), .destination_raw_path = stale_path.native()}});
    CHECK(stale && stale->failed_item_count() == 1U);
    CHECK(!std::filesystem::exists(stale_path));
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    if (argc == 2) {
        exportsWithoutOverwriteAndReportsOrderedPartialResults(argv[1]);
    }
    return failures == 0 ? 0 : 1;
}
