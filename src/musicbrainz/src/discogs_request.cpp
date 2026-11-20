// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/discogs.hpp"

namespace trackknife::musicbrainz {

bool is_discogs_api(const std::string_view url) {
    return url.starts_with("https://api.discogs.com/");
}

bool is_discogs_image(const std::string_view url) {
    return url.starts_with("https://i.discogs.com/");
}

} // namespace trackknife::musicbrainz
