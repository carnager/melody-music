// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/musicbrainz/web_service.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace trackknife::musicbrainz {

// ADR-0360: Discogs as Identify's second source. Its releases fill the same
// Release model as MusicBrainz's, marked ReleaseSource::discogs.

// Discogs allows sixty requests a minute with a token; paced a little slower.
inline constexpr int discogs_minimum_request_interval_ms = 1'100;
// How many of Discogs' releases a search lists, after MusicBrainz's.
inline constexpr std::size_t discogs_search_limit = 10U;
// How many of a master's versions are listed: one page.
inline constexpr std::size_t discogs_master_versions_limit = 100U;

// Qt-free, for an engine: Discogs' interface, which is given the token,
// and its images, which are not.
[[nodiscard]] bool is_discogs_api(std::string_view url);
[[nodiscard]] bool is_discogs_image(std::string_view url);

// The database search for releases by artist and title; at least one must
// be given. Lists discogs_search_limit.
[[nodiscard]] core::Result<std::string> build_discogs_search_url(const ReleaseSearchQuery& query);
// A release, and a master's versions; the id is a Discogs number.
[[nodiscard]] core::Result<std::string> build_discogs_release_url(std::string_view release_id);
[[nodiscard]] core::Result<std::string>
build_discogs_master_versions_url(std::string_view master_id);

// The release or master a person names: discogs.com/release/N (any
// language's page of it, its slug or none), or [rN]; discogs.com/master/N
// or [mN]. None for anything else.
[[nodiscard]] std::optional<std::string> discogs_release_id_in(std::string_view text);
[[nodiscard]] std::optional<std::string> discogs_master_id_in(std::string_view text);

// A search's releases, without their tracks: Discogs' search does not say
// how many there are (track_count 0).
[[nodiscard]] core::Result<ReleaseSearchResult>
parse_discogs_search(std::string_view body, const WebServiceLimits& limits = {});
// A master's versions, without artists or tracks.
[[nodiscard]] core::Result<ReleaseSearchResult>
parse_discogs_master_versions(std::string_view body, const WebServiceLimits& limits = {});
// A release with its tracks, split into discs by their positions.
[[nodiscard]] core::Result<Release> parse_discogs_release(std::string_view body,
                                                          const WebServiceLimits& limits = {});
// A release's images as a cover listing: its primary image the front.
[[nodiscard]] core::Result<CoverArtListing>
parse_discogs_images(std::string_view body, const WebServiceLimits& limits = {});

// An artist's name as Discogs gives it, as it is tagged: without the
// " (N)" telling artists of one name apart; "Various" is "Various Artists".
[[nodiscard]] std::string discogs_artist_name(std::string_view name);

} // namespace trackknife::musicbrainz
