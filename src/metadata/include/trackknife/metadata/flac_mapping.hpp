// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/metadata/staged_patch.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::metadata {

struct FlacTextFieldMapping {
    std::string property_name;

    friend bool operator==(const FlacTextFieldMapping&, const FlacTextFieldMapping&) = default;
};

struct TextPropertyIdentity {
    std::string canonical_name;
    bool conventional{false};

    friend bool operator==(const TextPropertyIdentity&, const TextPropertyIdentity&) = default;
};

// Resolves only repository-owned, explicitly enumerated property mappings.
// Unknown names retain their case-folded native spelling, including spaces,
// underscores, hyphens, and punctuation; similarity never creates an alias.
[[nodiscard]] TextPropertyIdentity resolve_text_property_identity(std::string_view native_name);
// FFmpeg exposes a few format-native fields through generic AVDictionary
// spellings ("track", "disc", "album_artist", "encoder"). Those are
// projections of tags the tag reader already has, not further identities:
// the canonical name each stands for, or nothing for any other name.
[[nodiscard]] std::optional<std::string_view> stream_tag_alias(std::string_view native_name);
[[nodiscard]] bool is_conventional_metadata_field(std::string_view canonical_name);

// Picard-paired native spellings for one canonical identity, primary first.
// Empty for fields written under a single name.
[[nodiscard]] std::vector<std::string> paired_flac_property_names(std::string_view canonical_name);

// Resolves one Trackknife field to the exact Xiph-comment key used by the
// native FLAC adapter. Existing native spelling wins; known newly added fields
// use the repository-owned conventional mapping; arbitrary additions retain
// their display spelling with ASCII letters uppercased. Exact empty values and
// artwork conventions are rejected instead of being silently reinterpreted.
[[nodiscard]] core::Result<FlacTextFieldMapping>
map_flac_text_field(std::string_view canonical_name, std::string_view display_name,
                    std::string_view existing_native_name, StagedMetadataPatchKind kind,
                    std::span<const std::string> values);

} // namespace trackknife::metadata
