// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/formats/cue_sheet.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/persistence/list_repository.hpp"

namespace trackknife::engine::detail {

// ADR-0356: a CUE sheet's track as a list item -- the sheet's words where it
// has them, the audio file's own tags (`embedded`, if read) where it has not,
// and the gains the sheet holds.
[[nodiscard]] persistence::EngineListItem
opened_cue_item(const formats::ResolvedCueSheet& sheet, const formats::ResolvedCueTrack& track,
                const metadata::MetadataDocument* embedded);

} // namespace trackknife::engine::detail
