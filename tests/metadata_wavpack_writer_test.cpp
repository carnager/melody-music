// SPDX-License-Identifier: GPL-3.0-only

#include "test_checks.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/wavpack_writer.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <apeitem.h>
#include <apetag.h>
#include <tbytevector.h>
#include <wavpackfile.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using checks::failures;

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("trackknife-wavpack-writer-" + trackknife::core::StableId::random().to_string());
        std::filesystem::create_directory(path_);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
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
    std::ofstream output{destination, std::ios::binary};
    if (decoded) {
        output.write(reinterpret_cast<const char*>(decoded->data()),
                     static_cast<std::streamsize>(decoded->size()));
    }
    return destination;
}

[[nodiscard]] std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] bool bytes_contain(const std::vector<unsigned char>& bytes,
                                 const std::string_view needle) {
    return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
}

[[nodiscard]] std::optional<trackknife::metadata::StagedMetadataSelection>
selection_for(const trackknife::metadata::LocalMetadataRead& read) {
    auto selection = trackknife::metadata::StagedMetadataSelection::create(
        {trackknife::metadata::StagedMetadataSource{
            .raw_path = read.raw_path,
            .source_revision = read.source_revision,
            .baseline = read.document,
        }});
    if (!selection) {
        std::cerr << selection.error().message << '\n';
        return std::nullopt;
    }
    return std::move(*selection);
}

[[nodiscard]] std::optional<trackknife::metadata::MetadataWritePlan>
make_plan(const trackknife::metadata::StagedMetadataSelection& selection,
          const trackknife::metadata::StagedMetadataPatchSet& patches) {
    auto plan = trackknife::metadata::revalidate_metadata_write_plan(selection, patches);
    if (!plan) {
        std::cerr << plan.error().message << '\n';
        return std::nullopt;
    }
    return std::move(*plan);
}

constexpr std::string_view binary_item_key = "Cover Art (Front)";
const TagLib::ByteVector binary_item_bytes{"\x89PNG-not-really-an-image-payload", 32};

void embed_binary_ape_item(const std::filesystem::path& path) {
    TagLib::WavPack::File file{path.c_str(), false};
    CHECK(file.isValid());
    auto* tag = file.APETag(true);
    CHECK(tag != nullptr);
    if (tag != nullptr) {
        TagLib::APE::Item item{TagLib::String{binary_item_key.data()}, binary_item_bytes, true};
        tag->setItem(binary_item_key.data(), item);
        CHECK(file.save());
    }
}

void roundTripsTextEditsPreservingAudioAndBinaryItems(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, "tagged-tone-wavpack.b64",
                                    directory.path() / "roundtrip.wv");
    embed_binary_ape_item(source);
    const auto source_bytes = read_bytes(source);

    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    CHECK(read->adapter_name == "taglib-wavpack-v1");
    CHECK(read->capabilities.fields_writable);
    CHECK(std::ranges::any_of(read->document.unsupported_native_objects,
                              [](const trackknife::metadata::NativeObjectIdentity& object) {
                                  return object.identity.find("COVER ART") != std::string::npos ||
                                         object.identity.find("Cover Art") != std::string::npos;
                              }));
    CHECK(read->document.first_effective_value("title") ==
          std::optional<std::string>{"Fixture Tone"});

    auto selection = selection_for(*read);
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("title");
    const auto artist = selection->field_index("artist");
    const auto track_id =
        selection->ensure_missing_field("musicbrainztrackid", "MUSICBRAINZ_TRACKID");
    const auto totals = selection->ensure_missing_field("Total Tracks", "Total Tracks");
    CHECK(title.has_value() && artist.has_value());
    CHECK(track_id.has_value() && totals.has_value());
    if (!title || !artist || !track_id || !totals) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"Prepared Tone"}).has_value());
    CHECK(patches.remove_field(*selection, 0U, *artist).has_value());
    CHECK(
        patches.replace_values(*selection, 0U, *track_id, {"01b43c31-1b23-42f0-a5d5-275288538230"})
            .has_value());
    CHECK(patches.replace_values(*selection, 0U, *totals, {"9"}).has_value());

    auto plan = make_plan(*selection, patches);
    CHECK(plan && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto prepared_path = directory.path() / "roundtrip-prepared.wv";
    const auto prepared = trackknife::metadata::prepare_wavpack_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    CHECK(prepared.has_value());
    if (!prepared) {
        std::cerr << prepared.error().message << '\n';
        return;
    }
    CHECK(prepared->field_change_count == 4U);

    const auto reread = trackknife::metadata::read_local_metadata(prepared_path.native());
    CHECK(reread.has_value());
    if (!reread) {
        return;
    }
    CHECK(reread->document.first_effective_value("title") ==
          std::optional<std::string>{"Prepared Tone"});
    CHECK(!reread->document.first_effective_value("artist"));
    CHECK(reread->document.first_effective_value("musicbrainztrackid") ==
          std::optional<std::string>{"01b43c31-1b23-42f0-a5d5-275288538230"});
    // Picard-paired totals apply to APEv2 exactly like Vorbis comments.
    CHECK(reread->document.effective_values("totaltracks") == (std::vector<std::string>{"9"}));
    CHECK(reread->document.effective_values("tracktotal") == (std::vector<std::string>{"9"}));
    const auto prepared_bytes = read_bytes(prepared_path);
    CHECK(bytes_contain(prepared_bytes, "TOTALTRACKS"));
    CHECK(bytes_contain(prepared_bytes, "TRACKTOTAL"));

    // The WavPack audio blocks are byte-identical up to the APEv2 trailer.
    const auto audio_probe = std::min<std::size_t>(source_bytes.size() / 2U, 4'096U);
    CHECK(std::equal(source_bytes.begin(),
                     source_bytes.begin() + static_cast<std::ptrdiff_t>(audio_probe),
                     prepared_bytes.begin()));

    // The binary APEv2 item survived byte-exactly.
    TagLib::WavPack::File prepared_file{prepared_path.c_str(), false};
    CHECK(prepared_file.isValid());
    auto* prepared_tag = prepared_file.APETag(false);
    CHECK(prepared_tag != nullptr);
    if (prepared_tag != nullptr) {
        // TagLib folds APE item keys to upper case in its item map.
        const auto items = prepared_tag->itemListMap();
        CHECK(items.contains("COVER ART (FRONT)"));
        if (items.contains("COVER ART (FRONT)")) {
            CHECK(items["COVER ART (FRONT)"].binaryData() == binary_item_bytes);
        }
    }
    CHECK(reread->document.unsupported_native_objects == read->document.unsupported_native_objects);
}

void rejectsId3v1Trailers(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source =
        materialize(fixture_directory, "tagged-tone-wavpack.b64", directory.path() / "id3.wv");
    {
        std::ofstream append{source, std::ios::binary | std::ios::app};
        std::array<char, 128> id3v1{};
        id3v1[0] = 'T';
        id3v1[1] = 'A';
        id3v1[2] = 'G';
        append.write(id3v1.data(), static_cast<std::streamsize>(id3v1.size()));
    }
    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    auto selection = selection_for(*read);
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("title");
    CHECK(title.has_value());
    if (!title) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"Blocked"}).has_value());
    auto plan = make_plan(*selection, patches);
    CHECK(plan && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto prepared_path = directory.path() / "id3-prepared.wv";
    const auto prepared = trackknife::metadata::prepare_wavpack_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    CHECK(!prepared.has_value());
    CHECK(!prepared && prepared.error().code == trackknife::core::ErrorCode::unsupported);
    CHECK(!prepared && prepared.error().message.find("ID3v1") != std::string::npos);
    CHECK(!std::filesystem::exists(prepared_path));
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    if (argc == 2) {
        const std::filesystem::path fixture_directory{argv[1]};
        roundTripsTextEditsPreservingAudioAndBinaryItems(fixture_directory);
        rejectsId3v1Trailers(fixture_directory);
    }
    return failures == 0 ? 0 : 1;
}
