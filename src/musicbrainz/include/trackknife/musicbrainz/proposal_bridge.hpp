// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/metadata/proposal.hpp"
#include "trackknife/musicbrainz/matching.hpp"
#include "trackknife/musicbrainz/web_service.hpp"

#include <cstddef>
#include <span>

namespace trackknife::musicbrainz {

struct ReleaseProposalOptions {
    // Aligned tracks below this confidence receive no proposals at all;
    // partial certainty never writes a wrong title.
    double minimum_track_confidence{0.5};
    bool include_identifiers{true};
};

// Converts one aligned release into ADR-0086 proposals: per-track title,
// artist, and numbering; release-level album, album artist, date, and
// totals; and the release's identifiers -- MusicBrainz's, or a Discogs
// release's own (ADR-0360) -- all carrying the alignment's
// confidence and a rationale naming the exact release version. item_indexes
// maps each local descriptor position to its selection item. The result
// stages as an ordinary colored, undoable draft — nothing here writes.
[[nodiscard]] core::Result<metadata::MetadataProposalSet>
release_metadata_proposals(const Release& release, const ReleaseAlignment& alignment,
                           std::span<const std::size_t> item_indexes,
                           const ReleaseProposalOptions& options = {});

} // namespace trackknife::musicbrainz
