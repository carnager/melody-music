// SPDX-License-Identifier: GPL-3.0-only

// A damaged packet in an otherwise sound file -- an MP3 frame without its
// header, a truncated last ALAC packet -- is passed over and counted, as
// FFmpeg's own tools do, not taken for the whole track (ADR-0354). A file
// of nothing but damage still fails, and a converter's output is still
// held to having none.

#include "trackknife/convert/convert.hpp"
#include "trackknife/convert/preset.hpp"
#include "trackknife/formats/decoder.hpp"

#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace formats = trackknife::formats;

int failures = 0;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

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
            continue;
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

// The MPEG audio frames of a file: where each header is, walked from the
// first by the lengths the headers give (MPEG-1 Layer III).
[[nodiscard]] std::vector<std::size_t> mp3_frames(const std::vector<unsigned char>& bytes) {
    constexpr std::array<int, 16> kbps{0,   32,  40,  48,  56,  64,  80,  96,
                                       112, 128, 160, 192, 224, 256, 320, 0};
    constexpr std::array<int, 4> rates{44'100, 48'000, 32'000, 0};
    std::vector<std::size_t> frames;
    std::size_t at = 0U;
    // Past an ID3v2 tag.
    if (bytes.size() > 10U && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3') {
        at = 10U + ((bytes[6] & 0x7FU) << 21U | (bytes[7] & 0x7FU) << 14U |
                    (bytes[8] & 0x7FU) << 7U | (bytes[9] & 0x7FU));
    }
    while (at + 4U <= bytes.size() && bytes[at] == 0xFFU && (bytes[at + 1U] & 0xFEU) == 0xFAU) {
        const auto rate = rates[(bytes[at + 2U] >> 2U) & 0x3U];
        const auto bitrate = kbps[bytes[at + 2U] >> 4U];
        if (rate == 0 || bitrate == 0) {
            break;
        }
        frames.push_back(at);
        const auto padding = static_cast<int>((bytes[at + 2U] >> 1U) & 1U);
        at += static_cast<std::size_t>(144 * bitrate * 1000 / rate + padding);
    }
    return frames;
}

[[nodiscard]] std::filesystem::path write(const std::filesystem::path& path,
                                          const std::vector<unsigned char>& bytes) {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    CHECK(output.good());
    return path;
}

// Every frame decoded, or the error that stopped it.
struct Decoded {
    std::int64_t frames{0};
    std::uint64_t damaged{0U};
    std::optional<std::string> error;
};

[[nodiscard]] Decoded decode(const std::filesystem::path& path) {
    Decoded result;
    auto decoder = formats::AudioDecoder::open(path.native(), {});
    if (!decoder) {
        result.error = decoder.error().message;
        return result;
    }
    const auto channels = decoder->output_format().channels;
    while (true) {
        auto chunk = decoder->next_chunk();
        if (!chunk) {
            result.error = chunk.error().message;
            break;
        }
        if (!*chunk) {
            break;
        }
        result.frames += static_cast<std::int64_t>((*chunk)->frame_count(channels));
    }
    result.damaged = decoder->damaged_packets();
    return result;
}

} // namespace

int main(const int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: decoder_damaged_packets_test FIXTURE_DIRECTORY\n";
        return EXIT_FAILURE;
    }
    const auto source = decode_base64_file(std::filesystem::path{argv[1]} / "gapless-tone-mp3.b64");
    CHECK(source.has_value());
    if (!source) {
        return EXIT_FAILURE;
    }
    const auto frames = mp3_frames(*source);
    CHECK(frames.size() >= 5U);
    if (frames.size() < 5U) {
        return EXIT_FAILURE;
    }
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-damaged-" + std::to_string(::getpid()));
    std::filesystem::create_directories(directory);

    const auto sound = decode(write(directory / "sound.mp3", *source));
    CHECK(!sound.error);
    CHECK(sound.damaged == 0U);
    CHECK(sound.frames > 0);

    // One frame's header gone, as in real files: FFmpeg's MP3 decoder says
    // "Header missing" and refuses that packet as invalid data. Not one of
    // the first frames, which the demuxer reads to recognise the file and
    // would skip as junk; frames[0] is the encoder's info frame.
    auto one = *source;
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        one[frames[3] + byte] = 0U;
    }
    const auto flawed = decode(write(directory / "one-damaged.mp3", one));
    if (flawed.error) {
        std::cerr << "one damaged frame: " << *flawed.error << '\n';
    }
    CHECK(!flawed.error);
    CHECK(flawed.damaged == 1U);
    // Its frame is missing, and nothing else: an MPEG-1 Layer III frame is
    // 1152 samples.
    CHECK(flawed.frames < sound.frames);
    CHECK(sound.frames - flawed.frames <= 1152 * 2);

    // Nothing but damage is no sound file: every frame after the first
    // loses its header, and the first is the encoder's silent info frame.
    auto ruined = *source;
    for (const auto frame : frames) {
        for (std::size_t byte = 1U; byte < 4U; ++byte) {
            ruined[frame + byte] = 0U;
        }
    }
    const auto garbage = decode(write(directory / "all-damaged.mp3", ruined));
    CHECK(garbage.error.has_value() || garbage.frames == 0);

    // Converted, the flaw is passed over and said; the output it made is
    // read back strictly, damage-free.
    const auto flac = trackknife::convert::find_encoder_preset("flac");
    CHECK(flac.has_value());
    if (flac) {
        const auto destination = directory / "one-damaged.flac";
        const auto converted = trackknife::convert::convert_audio_file(
            {.source_raw_path = (directory / "one-damaged.mp3").native(),
             .source_selection = {},
             .source_range = {},
             .destination_raw_path = destination.native(),
             .preset = *flac,
             .target_sample_rate = {},
             .sample_rate_cap = {},
             .target_bit_depth = {},
             .keep_source_bit_depth = false,
             .metadata = {},
             .artwork = {}},
            [](std::uint64_t, std::optional<std::uint64_t>) {});
        if (!converted) {
            std::cerr << "converting one damaged frame: " << converted.error().message << '\n';
        }
        CHECK(converted.has_value());
        if (converted) {
            CHECK(converted->damaged_source_packets == 1U);
            const auto output = decode(destination);
            CHECK(!output.error);
            CHECK(output.damaged == 0U);
        }
    }

    // A mono frame among stereo ones -- reached in real files past a
    // damaged packet -- is mixed up to the stream's stereo, not read as
    // stereo: its second plane does not exist.
    const auto mixed_source =
        decode_base64_file(std::filesystem::path{argv[1]} / "mono-frame-in-stereo-mp3.b64");
    CHECK(mixed_source.has_value());
    if (mixed_source) {
        const auto mixed_path = write(directory / "mono-frame-in-stereo.mp3", *mixed_source);
        auto decoder = formats::AudioDecoder::open(mixed_path.native(), {});
        CHECK(decoder.has_value());
        if (decoder) {
            CHECK(decoder->output_format().channels == 2);
        }
        const auto mixed = decode(mixed_path);
        if (mixed.error) {
            std::cerr << "mono frame in stereo: " << *mixed.error << '\n';
        }
        CHECK(!mixed.error);
        // Fourteen frames of 1152 samples, less the encoder's delay.
        CHECK(mixed.frames > 1152 * 12);
        CHECK(mixed.frames <= 1152 * 14);
        if (flac) {
            const auto destination = directory / "mono-frame-in-stereo.flac";
            const auto converted = trackknife::convert::convert_audio_file(
                {.source_raw_path = mixed_path.native(),
                 .source_selection = {},
                 .source_range = {},
                 .destination_raw_path = destination.native(),
                 .preset = *flac,
                 .target_sample_rate = {},
                 .sample_rate_cap = {},
                 .target_bit_depth = {},
                 .keep_source_bit_depth = false,
                 .metadata = {},
                 .artwork = {}},
                [](std::uint64_t, std::optional<std::uint64_t>) {});
            if (!converted) {
                std::cerr << "converting mono frame in stereo: " << converted.error().message << '\n';
            }
            CHECK(converted.has_value());
        }
    }

    std::error_code removed;
    std::filesystem::remove_all(directory, removed);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
