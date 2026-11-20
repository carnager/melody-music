// SPDX-License-Identifier: GPL-3.0-only

#include "test_checks.hpp"
#include "trackknife/musicbrainz/acoustid.hpp"
#include "trackknife/musicbrainz/bandcamp.hpp"
#include "trackknife/musicbrainz/discogs.hpp"
#include "trackknife/musicbrainz/web_service.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using checks::failures;

using namespace trackknife::musicbrainz;

void searchUrlEscapesLuceneAndPercentEncodes() {
    const auto url = build_release_search_url(ReleaseSearchQuery{
        .artist = "AC/DC \"Live\"",
        .release = "Back in Black",
        .track_count = 10U,
        .limit = 5U,
    });
    CHECK(url.has_value());
    CHECK(url->starts_with("https://musicbrainz.org/ws/2/release/?query="));
    CHECK(url->ends_with("&fmt=json&limit=5"));
    // The quote inside the artist phrase is Lucene-escaped, then everything
    // is percent-encoded: \" becomes %5C%22.
    CHECK(url->find("%5C%22Live%5C%22") != std::string::npos);
    CHECK(url->find("tracks%3A10") != std::string::npos);
    CHECK(url->find(' ') == std::string::npos);

    CHECK(!build_release_search_url(ReleaseSearchQuery{}).has_value());
    CHECK(!build_release_search_url(
               ReleaseSearchQuery{.artist = "x", .release = {}, .track_count = {}, .limit = 0U})
               .has_value());
    CHECK(!build_release_search_url(
               ReleaseSearchQuery{.artist = "x", .release = {}, .track_count = {}, .limit = 999U})
               .has_value());
}

void lookupUrlRequiresAReleaseId() {
    const auto url = build_release_lookup_url("2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    CHECK(url.has_value());
    CHECK(*url == "https://musicbrainz.org/ws/2/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc"
                  "?inc=artist-credits+recordings+release-groups+labels"
                  "+isrcs+recording-level-rels+work-rels&fmt=json");
    CHECK(!build_release_lookup_url("").has_value());
    CHECK(!build_release_lookup_url("not-a-uuid").has_value());
    CHECK(!build_release_lookup_url("2f2ac1b7-1111-4f4f-8f8f-123456789ab/../x").has_value());
}

// A release named by its id or its link; nothing else, a release group's
// link included.
void releaseIdsAreFoundInLinks() {
    constexpr std::string_view id = "2f2ac1b7-1111-4f4f-8f8f-123456789abc";
    CHECK(release_id_in(id) == std::string{id});
    CHECK(release_id_in("  2F2AC1B7-1111-4F4F-8F8F-123456789ABC\t") == std::string{id});
    CHECK(release_id_in("https://musicbrainz.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc") ==
          std::string{id});
    CHECK(release_id_in("musicbrainz.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/discids") ==
          std::string{id});
    CHECK(release_id_in("https://beta.musicbrainz.org/release/"
                        "2f2ac1b7-1111-4f4f-8f8f-123456789abc?tport=8000") == std::string{id});
    CHECK(!release_id_in("").has_value());
    CHECK(!release_id_in("Alpha").has_value());
    CHECK(!release_id_in("https://musicbrainz.org/release-group/"
                         "2f2ac1b7-1111-4f4f-8f8f-123456789abc")
               .has_value());
    CHECK(!release_id_in("https://musicbrainz.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abcd")
               .has_value());
    CHECK(!release_id_in("https://notmusicbrainz.org/release/"
                         "2f2ac1b7-1111-4f4f-8f8f-123456789abc")
               .has_value());
    // An album's link names its release group; a bare id is a release's.
    CHECK(release_group_id_in("https://musicbrainz.org/release-group/"
                              "2F2AC1B7-1111-4f4f-8f8f-123456789abc") == std::string{id});
    CHECK(!release_group_id_in(id).has_value());
    CHECK(!release_group_id_in("https://musicbrainz.org/release/"
                               "2f2ac1b7-1111-4f4f-8f8f-123456789abc")
               .has_value());
}

// Every version of an album: a release group's releases, read as a search's.
void releaseGroupReleasesAreListed() {
    const auto url = build_release_group_releases_url("2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    CHECK(url.has_value());
    CHECK(*url == "https://musicbrainz.org/ws/2/release?release-group="
                  "2f2ac1b7-1111-4f4f-8f8f-123456789abc"
                  "&inc=artist-credits+labels+media+release-groups&fmt=json&limit=100");
    CHECK(!build_release_group_releases_url("not-a-uuid").has_value());
    const auto listed = parse_release_search(R"json({"release-count": 140, "releases": [
        {"id": "2f2ac1b7-1111-4f4f-8f8f-123456789abc", "title": "Alpha",
         "media": [{"format": "CD", "track-count": 2}, {"format": "CD", "track-count": 3}]}]})json");
    CHECK(listed.has_value());
    CHECK(listed->total_count == 140U);
    CHECK(listed->releases.size() == 1U);
    CHECK(listed->releases.front().track_count == 5U);
}

constexpr std::string_view search_fixture = R"json({
  "count": 42,
  "offset": 0,
  "releases": [
    {
      "id": "11111111-2222-3333-4444-555555555555",
      "score": 100,
      "title": "Alpha",
      "status": "Official",
      "disambiguation": "remastered",
      "date": "1999-09-09",
      "country": "DE",
      "barcode": "0123456789012",
      "track-count": 10,
      "artist-credit": [
        {"name": "Band", "joinphrase": " feat. ",
         "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
                    "name": "Band", "sort-name": "Band, The"}},
        {"name": "Guest", "artist": {"id": "ffffffff-0000-1111-2222-333333333333",
                                     "name": "Guest", "sort-name": "Guest"}}
      ],
      "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
      "label-info": [
        {"catalog-number": "CAT-123", "label": {"name": "Label Records"}}
      ],
      "media": [{"format": "CD", "track-count": 10}]
    },
    {
      "id": "21111111-2222-3333-4444-555555555555",
      "score": 87,
      "title": "Alpha",
      "status": "Official",
      "date": "2001-01-01",
      "country": "US",
      "track-count": 11,
      "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
      "media": [{"format": "12\" Vinyl", "track-count": 11}]
    }
  ]
})json";

void searchParsingCarriesVersionDetail() {
    const auto result = parse_release_search(search_fixture);
    CHECK(result.has_value());
    if (!result) {
        return;
    }
    CHECK(result->total_count == 42U);
    CHECK(result->releases.size() == 2U);
    const auto& first = result->releases.front();
    CHECK(first.id == "11111111-2222-3333-4444-555555555555");
    CHECK(first.title == "Alpha");
    CHECK(first.search_score == 100);
    CHECK(first.disambiguation == "remastered");
    CHECK(first.date == "1999-09-09");
    CHECK(first.country == "DE");
    CHECK(first.barcode == "0123456789012");
    CHECK(first.release_group_id == "99999999-8888-7777-6666-555555555555");
    CHECK(first.label == "Label Records");
    CHECK(first.catalog_number == "CAT-123");
    CHECK(first.track_count == 10U);
    CHECK(first.artist_credits.size() == 2U);
    CHECK(first.artist_credits[0].name == "Band");
    CHECK(first.artist_credits[0].join_phrase == " feat. ");
    CHECK(first.artist_credits[0].artist_id == "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    CHECK(first.artist_credits[0].sort_name == "Band, The");
    CHECK(first.media.size() == 1U && first.media.front().format == "CD");
    // Two versions of the same release group stay distinct candidates.
    const auto& second = result->releases.back();
    CHECK(second.release_group_id == first.release_group_id);
    CHECK(second.country == "US" && second.media.front().format == "12\" Vinyl");
}

constexpr std::string_view lookup_fixture = R"json({
  "id": "11111111-2222-3333-4444-555555555555",
  "title": "Alpha",
  "status": "Official",
  "date": "1999-09-09",
  "country": "DE",
  "release-group": {"id": "99999999-8888-7777-6666-555555555555",
                    "primary-type": "Album", "secondary-types": ["Live"],
                    "first-release-date": "1990-05-01"},
  "text-representation": {"script": "Latn", "language": "eng"},
  "artist-credit": [{"name": "Band",
                     "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
                                "name": "Band", "sort-name": "Band, The"}}],
  "media": [
    {
      "position": 1,
      "format": "CD",
      "title": "First Night",
      "track-count": 2,
      "tracks": [
        {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
         "title": "One", "length": 61000,
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One",
                       "isrcs": ["DEA119900001"],
                       "relations": [
                         {"type": "performance",
                          "work": {"id": "cccc1111-0000-0000-0000-000000000001",
                                   "title": "One (work)"}},
                         {"type": "performance", "work": {"id": "not-a-uuid"}}
                       ]}},
        {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000002",
                       "title": "Two (recording)", "length": 59000}}
      ]
    }
  ]
})json";

void lookupParsingAlignsTracksAndRecordings() {
    const auto release = parse_release_lookup(lookup_fixture);
    CHECK(release.has_value());
    if (!release) {
        return;
    }
    CHECK(release->media.size() == 1U);
    const auto& medium = release->media.front();
    CHECK(medium.position == 1U && medium.format == "CD");
    CHECK(medium.tracks.size() == 2U);
    CHECK(medium.tracks[0].position == 1U && medium.tracks[0].title == "One");
    CHECK(medium.tracks[0].length_ms == std::optional<std::int64_t>{61'000});
    CHECK(medium.tracks[0].recording_id == "bbbb1111-0000-0000-0000-000000000001");
    // Track title/length fall back to the recording when absent.
    CHECK(medium.tracks[1].title == "Two (recording)");
    CHECK(medium.tracks[1].length_ms == std::optional<std::int64_t>{59'000});
    CHECK(release->track_count == 2U);

    // Picard-parity detail: release-group dates and types, the text
    // representation, medium titles, ISRCs, and valid work ids only.
    CHECK(release->release_group_primary_type == "Album");
    CHECK(release->release_group_secondary_types == std::vector<std::string>{"Live"});
    CHECK(release->release_group_first_release_date == "1990-05-01");
    CHECK(release->script == "Latn");
    CHECK(release->language == "eng");
    CHECK(medium.title == "First Night");
    CHECK(medium.tracks[0].isrcs == std::vector<std::string>{"DEA119900001"});
    CHECK(medium.tracks[0].work_ids ==
          std::vector<std::string>{"cccc1111-0000-0000-0000-000000000001"});
    CHECK(medium.tracks[1].isrcs.empty());
    CHECK(medium.tracks[1].work_ids.empty());
}

void malformedAndOversizedPayloadsFailTyped() {
    CHECK(!parse_release_search("not json").has_value());
    CHECK(!parse_release_search("[1,2,3]").has_value());
    CHECK(!parse_release_search(R"({"count": 1})").has_value());
    CHECK(!parse_release_search(R"({"count": 1, "releases": [{"title": "No id"}]})").has_value());
    WebServiceLimits tiny;
    tiny.body_bytes = 8U;
    const auto oversized = parse_release_search(search_fixture, tiny);
    CHECK(!oversized.has_value() &&
          oversized.error().code == trackknife::core::ErrorCode::limit_exceeded);
}

constexpr std::string_view cover_art_fixture = R"json({
  "images": [
    {"id": 987654321, "front": false, "back": true, "approved": true,
     "comment": "reverse", "types": ["Back"],
     "image": "http://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/987654321.jpg"},
    {"id": "123456789", "front": true, "back": false, "approved": true,
     "comment": "", "types": ["Front"],
     "image": "http://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/123456789.png",
     "thumbnails": {"250": "http://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/123456789-250.jpg",
                    "small": "https://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/123456789-small.jpg"}},
    {"id": 42, "front": false, "back": false, "approved": false, "types": ["Booklet"],
     "image": "ftp://nowhere.example/booklet.png"}
  ],
  "release": "https://musicbrainz.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc"
})json";

void coverArtUrlRequiresAReleaseId() {
    const auto url = build_cover_art_listing_url("2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    CHECK(url.has_value());
    CHECK(*url == "https://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    // ADR-0332: and a release group's, for another release's cover.
    const auto group_url =
        build_release_group_cover_art_listing_url("2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    CHECK(group_url && *group_url == "https://coverartarchive.org/release-group/"
                                     "2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    CHECK(!build_release_group_cover_art_listing_url("not-a-uuid").has_value());
    CHECK(!build_cover_art_listing_url("").has_value());
    CHECK(!build_cover_art_listing_url("not-a-uuid").has_value());
}

void coverArtParsingUpgradesAndSelectsTheFront() {
    const auto listing = parse_cover_art_listing(cover_art_fixture);
    CHECK(listing.has_value());
    // The ftp entry has no usable https image and is dropped.
    CHECK(listing->images.size() == 2U);
    CHECK(listing->images[0].id == "987654321");
    CHECK(listing->images[0].back);
    CHECK(listing->images[0].comment == "reverse");
    CHECK(listing->images[1].id == "123456789");
    CHECK(listing->images[1].front);
    CHECK(listing->images[1].types == std::vector<std::string>{"Front"});
    CHECK(listing->images[1].image_url ==
          "https://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/"
          "123456789.png");
    // The 250px thumbnail wins and is https-upgraded; the back image has
    // none.
    CHECK(listing->images[1].thumbnail_url ==
          "https://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/"
          "123456789-250.jpg");
    CHECK(listing->images[0].thumbnail_url.empty());
    CHECK(select_front_cover(*listing) == std::optional<std::size_t>{1U});

    // Without a flagged front image, an approved image is preferred and an
    // empty listing selects nothing.
    auto no_front = *listing;
    no_front.images[1].front = false;
    no_front.images[1].types = {"Booklet"};
    no_front.images[1].approved = false;
    CHECK(select_front_cover(no_front) == std::optional<std::size_t>{0U});
    CHECK(select_front_cover(CoverArtListing{}) == std::nullopt);

    CHECK(!parse_cover_art_listing("{}").has_value());
    WebServiceLimits tiny;
    tiny.cover_art_images = 1U;
    const auto oversized = parse_cover_art_listing(cover_art_fixture, tiny);
    CHECK(!oversized.has_value() &&
          oversized.error().code == trackknife::core::ErrorCode::limit_exceeded);
}

constexpr std::string_view acoustid_fixture = R"json({
  "status": "ok",
  "results": [
    {"id": "aaa", "score": 0.65,
     "recordings": [{"id": "bbbb1111-0000-0000-0000-000000000002",
                     "releases": [{"id": "11111111-2222-3333-4444-555555555555"}]}]},
    {"id": "bbb", "score": 0.98,
     "recordings": [
       {"id": "bbbb1111-0000-0000-0000-000000000001",
        "releases": [{"id": "11111111-2222-3333-4444-555555555555"},
                     {"id": "99999999-8888-7777-6666-555555555555"},
                     {"id": "not-a-uuid"}]},
       {"id": "not-a-uuid", "releases": [{"id": "11111111-2222-3333-4444-555555555555"}]}
     ]}
  ]
})json";

void acoustidBodyAndParsingAreBounded() {
    const auto body = build_acoustid_lookup_body("key", 331U, "AQADtKISJUzUR/Ph+3lM+A==");
    CHECK(body.has_value());
    CHECK(body->find("client=key") != std::string::npos);
    CHECK(body->find("meta=recordings+releases") != std::string::npos);
    CHECK(body->find("duration=331") != std::string::npos);
    // The fingerprint is percent-encoded into the form body.
    CHECK(body->find("AQADtKISJUzUR%2FPh%2B3lM%2BA%3D%3D") != std::string::npos);
    CHECK(!build_acoustid_lookup_body("", 331U, "fp").has_value());
    CHECK(!build_acoustid_lookup_body("key", 0U, "fp").has_value());
    CHECK(!build_acoustid_lookup_body("key", 331U, "").has_value());

    const auto lookup = parse_acoustid_lookup(acoustid_fixture);
    CHECK(lookup.has_value());
    if (!lookup) {
        return;
    }
    // Results arrive sorted by score, invalid recording ids are dropped,
    // and release lists keep only valid UUIDs.
    CHECK(lookup->results.size() == 2U);
    CHECK(lookup->results[0].score > lookup->results[1].score);
    CHECK(lookup->results[0].recordings.size() == 1U);
    CHECK(lookup->results[0].recordings[0].id == "bbbb1111-0000-0000-0000-000000000001");
    CHECK(lookup->results[0].recordings[0].release_ids ==
          (std::vector<std::string>{"11111111-2222-3333-4444-555555555555",
                                    "99999999-8888-7777-6666-555555555555"}));

    const auto rejected = parse_acoustid_lookup(
        R"json({"status": "error", "error": {"message": "invalid API key"}})json");
    CHECK(!rejected.has_value());
    CHECK(!rejected && rejected.error().message.find("invalid API key") != std::string::npos);
}

// ADR-0326: Bandcamp's album pages and images, and nothing that only looks
// like them -- an engine fetches what these allow.
void bandcampPagesAndCoversAreRecognised() {
    CHECK(is_bandcamp_album_page("https://band.bandcamp.com/album/beta"));
    CHECK(is_bandcamp_album_page("https://Some-Band.bandcamp.com/album/x?from=y"));
    CHECK(!is_bandcamp_album_page("http://band.bandcamp.com/album/beta"));
    CHECK(!is_bandcamp_album_page("https://band.bandcamp.com/track/beta"));
    CHECK(!is_bandcamp_album_page("https://band.bandcamp.com/album/"));
    CHECK(!is_bandcamp_album_page("https://bandcamp.com/album/beta"));
    CHECK(!is_bandcamp_album_page("https://band.bandcamp.com.example.org/album/beta"));
    CHECK(!is_bandcamp_album_page("https://evil.org/x.bandcamp.com/album/beta"));
    CHECK(!is_bandcamp_album_page("https://user@band.bandcamp.com/album/beta"));
    CHECK(!is_bandcamp_album_page("https://band.bandcamp.com:8443/album/beta"));
    CHECK(!is_bandcamp_album_page("https://a.b.bandcamp.com/album/beta"));
    CHECK(is_bandcamp_image("https://f4.bcbits.com/img/a0123456789_10.jpg"));
    CHECK(!is_bandcamp_image("https://f4.bcbits.com/other/a.jpg"));
    CHECK(!is_bandcamp_image("https://f4.bcbits.com.evil.org/img/a.jpg"));

    CHECK(bandcamp_album_page(
              {"https://www.discogs.com/master/1", "http://band.bandcamp.com/album/beta"}) ==
          std::optional<std::string>{"https://band.bandcamp.com/album/beta"});
    CHECK(!bandcamp_album_page({"https://band.bandcamp.com/"}));

    const auto urls = bandcamp_cover_urls(
        R"(<meta name="x"><meta property="og:image" content="https://f4.bcbits.com/img/a42_5.jpg">)");
    CHECK(urls && urls->image == "https://f4.bcbits.com/img/a42_10.jpg");
    CHECK(urls && urls->thumbnail == "https://f4.bcbits.com/img/a42_2.jpg");
    CHECK(!bandcamp_cover_urls(R"(<meta property="og:image" content="https://evil.org/a_5.jpg">)"));
    CHECK(!bandcamp_cover_urls("<html>no cover</html>"));

    const auto cover = bandcamp_cover(
        R"(<meta property="og:image" content="https://f4.bcbits.com/img/a42_5.jpg">)");
    CHECK(cover && cover->front && cover->source == "Bandcamp");

    const auto links = parse_release_links(R"json({"relations": [
        {"url": {"resource": "https://band.bandcamp.com/album/beta"}},
        {"url": {"resource": "https://www.discogs.com/release/1"}}],
        "release-group": {"id": "88888888-8888-7777-6666-555555555555"}})json");
    CHECK(links && links->urls.size() == 2U);
    CHECK(links && links->release_group_id == "88888888-8888-7777-6666-555555555555");
    CHECK(build_release_links_url("11111111-2222-3333-4444-555555555555").value_or("") ==
          "https://musicbrainz.org/ws/2/release/11111111-2222-3333-4444-555555555555"
          "?inc=url-rels+release-groups&fmt=json");
    CHECK(!build_release_group_links_url("not an id"));
}

// ADR-0360: Discogs' links, search, masters and releases, as the one model.
void discogsUrlsAndLinksAreStrict() {
    CHECK(is_discogs_api("https://api.discogs.com/releases/1"));
    CHECK(!is_discogs_api("https://api.discogs.com.evil.example/releases/1"));
    CHECK(!is_discogs_api("http://api.discogs.com/releases/1"));
    CHECK(is_discogs_image("https://i.discogs.com/x/1.jpg"));
    CHECK(!is_discogs_image("https://i.discogs.com.evil.example/1.jpg"));

    const auto search = build_discogs_search_url(ReleaseSearchQuery{
        .artist = "AC/DC", .release = "Back in Black", .track_count = {}, .limit = 10U});
    CHECK(search.value_or("") == "https://api.discogs.com/database/search?type=release"
                                 "&artist=AC%2FDC&release_title=Back%20in%20Black&per_page=10");
    CHECK(!build_discogs_search_url(ReleaseSearchQuery{}));
    CHECK(build_discogs_release_url("249504").value_or("") ==
          "https://api.discogs.com/releases/249504");
    CHECK(!build_discogs_release_url("../users"));
    CHECK(!build_discogs_release_url("0"));
    CHECK(build_discogs_master_versions_url("96559").value_or("") ==
          "https://api.discogs.com/masters/96559/versions?per_page=100");

    CHECK(discogs_release_id_in("https://www.discogs.com/release/249504-Rick-Astley-Never") ==
          std::optional<std::string>{"249504"});
    CHECK(discogs_release_id_in("discogs.com/de/release/249504") ==
          std::optional<std::string>{"249504"});
    CHECK(discogs_release_id_in(" [r249504] ") == std::optional<std::string>{"249504"});
    CHECK(discogs_release_id_in("https://www.discogs.com/release/249504?ev=rr") ==
          std::optional<std::string>{"249504"});
    CHECK(!discogs_release_id_in("https://www.discogs.com/master/96559"));
    CHECK(!discogs_release_id_in("https://www.discogs.com.evil.example/release/1"));
    CHECK(!discogs_release_id_in("249504"));
    CHECK(discogs_master_id_in("https://www.discogs.com/master/96559-Rick-Astley") ==
          std::optional<std::string>{"96559"});
    CHECK(discogs_master_id_in("[m96559]") == std::optional<std::string>{"96559"});
    CHECK(!discogs_master_id_in("[r96559]"));

    CHECK(discogs_artist_name("Alpha (2)") == "Alpha");
    CHECK(discogs_artist_name("Various") == "Various Artists");
    CHECK(discogs_artist_name("Blink-182") == "Blink-182");
}

void discogsSearchAndVersionsAreListed() {
    const auto found = parse_discogs_search(R"json({
        "pagination": {"items": 31},
        "results": [
          {"id": 11, "type": "release", "title": "Alpha (2) - Beta", "year": "2004",
           "country": "UK", "format": ["Vinyl", "LP", "Album"], "format_quantity": 2,
           "label": ["Warp Records", "Warp"], "catno": "WARP 1", "master_id": 7},
          {"id": 12, "type": "release", "title": "No artist split", "year": "",
           "format": ["CD"], "label": [], "catno": "none", "master_id": 0},
          {"id": "x", "type": "release", "title": "Alpha - Broken"}
        ]})json");
    CHECK(found.has_value());
    CHECK(found && found->total_count == 31U && found->releases.size() == 2U);
    if (found && found->releases.size() == 2U) {
        const auto& first = found->releases[0];
        CHECK(first.source == ReleaseSource::discogs);
        CHECK(first.id == "11" && first.title == "Beta" && first.date == "2004");
        CHECK(first.artist_credits.size() == 1U && first.artist_credits[0].name == "Alpha");
        CHECK(first.label == "Warp Records" && first.catalog_number == "WARP 1");
        CHECK(first.release_group_id == "7" && first.country == "UK");
        CHECK(first.media.size() == 2U && first.media[0].format == "Vinyl");
        CHECK(first.track_count == 0U);
        const auto& second = found->releases[1];
        CHECK(second.title == "No artist split" && second.artist_credits.empty());
        CHECK(second.catalog_number.empty() && second.release_group_id.empty());
    }
    CHECK(!parse_discogs_search("not json"));

    const auto versions = parse_discogs_master_versions(R"json({
        "pagination": {"items": 2},
        "versions": [
          {"id": 21, "title": "Beta", "format": "File, FLAC, Album", "label": "Self (3)",
           "country": "Germany", "released": "2004-03-00", "catno": "none"},
          {"id": 22, "title": "Beta", "format": "12\", 33 ⅓ RPM, Album",
           "major_formats": ["Vinyl"], "label": "Warp",
           "country": "UK", "released": "2004-00-00", "catno": "WARPCD1"}
        ]})json");
    CHECK(versions && versions->releases.size() == 2U);
    if (versions && versions->releases.size() == 2U) {
        CHECK(versions->releases[0].media.size() == 1U &&
              versions->releases[0].media[0].format == "Digital Media");
        CHECK(versions->releases[0].date == "2004-03");
        CHECK(versions->releases[0].label == "Self");
        CHECK(versions->releases[0].catalog_number.empty());
        CHECK(versions->releases[1].date == "2004");
        CHECK(versions->releases[1].media.size() == 1U &&
              versions->releases[1].media[0].format == "Vinyl");
    }

    // Only CDs and digital releases are listed, at most as many as asked;
    // a release of no known format is kept.
    if (versions) {
        auto kept = *versions;
        Release unknown = kept.releases.front();
        unknown.id = "23";
        unknown.media.clear();
        Release compact = kept.releases.front();
        compact.id = "24";
        compact.media[0].format = "CD";
        kept.releases.push_back(unknown);
        kept.releases.push_back(compact);
        keep_cd_and_digital(kept, 10U);
        CHECK(kept.releases.size() == 3U);
        CHECK(kept.releases.size() == 3U && kept.releases[0].id == "21" &&
              kept.releases[1].id == "23" && kept.releases[2].id == "24");
        keep_cd_and_digital(kept, 1U);
        CHECK(kept.releases.size() == 1U && kept.releases[0].id == "21");
    }

    // The artist searched for, not others whose names hold the word -- as
    // long as Discogs has any of theirs.
    const auto by = [](const char* id, const char* artist) {
        Release release{};
        release.source = ReleaseSource::discogs;
        release.id = id;
        release.artist_credits.push_back(
            ArtistCredit{.name = artist, .join_phrase = {}, .artist_id = {}, .sort_name = {}});
        return release;
    };
    ReleaseSearchResult loose{.total_count = 4U,
                              .releases = {by("1", "3 Prong Outlet"), by("2", "The Prong"),
                                           by("3", "Bob Prong"), by("4", "PRÖNG!")}};
    keep_artist(loose, "Prong");
    CHECK(loose.releases.size() == 2U && loose.releases[0].id == "2" &&
          loose.releases[1].id == "4");
    ReleaseSearchResult none{.total_count = 1U, .releases = {by("1", "3 Prong Outlet")}};
    keep_artist(none, "Prong");
    CHECK(none.releases.size() == 1U);
    keep_artist(none, "");
    CHECK(none.releases.size() == 1U);
}

[[nodiscard]] std::string discogs_release(const std::string& formats, const std::string& tracks) {
    return R"json({"id": 31, "title": "Gamma", "released": "1999-05-01", "country": "US",
        "master_id": 9,
        "artists": [{"name": "Alpha (2)", "anv": "", "join": "&", "id": 101},
                    {"name": "Delta", "anv": "D.", "join": "", "id": 102}],
        "labels": [{"name": "Label (4)", "catno": "LBL 1", "id": 5}],
        "identifiers": [{"type": "Matrix / Runout", "value": "X"},
                        {"type": "Barcode", "value": "5 021603 154212"}],
        "formats": )json" +
           formats + R"json(, "tracklist": )json" + tracks + "}";
}

void discogsReleasesSplitIntoDiscs() {
    // Numbered, with a heading, an index track and a track of its own artist.
    const auto plain = parse_discogs_release(
        discogs_release(R"([{"name": "CD", "qty": "1", "descriptions": ["Album", "Compilation"]}])",
                        R"json([
          {"type_": "heading", "position": "", "title": "Part One"},
          {"type_": "track", "position": "1", "title": "One", "duration": "4:05"},
          {"type_": "index", "position": "", "title": "Suite",
           "sub_tracks": [{"type_": "track", "position": "2.a", "title": "I", "duration": "1:00"},
                          {"type_": "track", "position": "2.b", "title": "II", "duration": "2:00"}]},
          {"type_": "track", "position": "3", "title": "Three", "duration": "",
           "artists": [{"name": "Echo (5)", "anv": "", "join": "", "id": 103}]}
        ])json"));
    CHECK(plain.has_value());
    if (plain) {
        CHECK(plain->source == ReleaseSource::discogs && plain->id == "31");
        CHECK(plain->title == "Gamma" && plain->date == "1999-05-01" && plain->country == "US");
        CHECK(plain->artist_credits.size() == 2U);
        if (plain->artist_credits.size() == 2U) {
            CHECK(plain->artist_credits[0].name == "Alpha");
            CHECK(plain->artist_credits[0].join_phrase == " & ");
            CHECK(plain->artist_credits[0].artist_id == "101");
            CHECK(plain->artist_credits[1].name == "D.");
            CHECK(plain->artist_credits[1].join_phrase.empty());
        }
        CHECK(plain->label == "Label" && plain->catalog_number == "LBL 1");
        CHECK(plain->barcode == "5021603154212");
        CHECK(plain->release_group_id == "9");
        CHECK(plain->release_group_primary_type == "Album");
        CHECK(plain->release_group_secondary_types == std::vector<std::string>{"Compilation"});
        CHECK(plain->media.size() == 1U && plain->track_count == 3U);
        if (plain->media.size() == 1U && plain->media[0].tracks.size() == 3U) {
            const auto& tracks = plain->media[0].tracks;
            CHECK(plain->media[0].format == "CD" && plain->media[0].position == 1U);
            CHECK(tracks[0].title == "One" && tracks[0].length_ms == 245'000);
            CHECK(tracks[0].position == 1U && tracks[0].number == "1");
            CHECK(tracks[1].title == "Suite" && tracks[1].length_ms == 180'000);
            CHECK(tracks[1].position == 2U);
            CHECK(tracks[2].length_ms == std::nullopt && tracks[2].position == 3U);
            CHECK(tracks[2].artist_credits.size() == 1U &&
                  tracks[2].artist_credits[0].name == "Echo");
        }
    }

    // Discs named in the positions.
    const auto discs = parse_discogs_release(
        discogs_release(R"([{"name": "CD", "qty": "2"}, {"name": "DVD", "qty": "1"}])",
                        R"json([
          {"position": "1-1", "title": "A"}, {"position": "1-2", "title": "B"},
          {"position": "CD2-1", "title": "C"}, {"position": "2.2", "title": "D"},
          {"position": "3-1", "title": "E"}
        ])json"));
    CHECK(discs && discs->media.size() == 3U);
    if (discs && discs->media.size() == 3U) {
        CHECK(discs->media[0].tracks.size() == 2U && discs->media[1].tracks.size() == 2U);
        CHECK(discs->media[1].position == 2U && discs->media[1].tracks[1].title == "D");
        CHECK(discs->media[1].tracks[1].position == 2U);
        CHECK(discs->media[1].format == "CD" && discs->media[2].format == "DVD");
    }

    // Sides of one record are one disc, numbered on, the side kept.
    const auto record = parse_discogs_release(
        discogs_release(R"([{"name": "Vinyl", "qty": "1", "descriptions": ["LP"]}])",
                        R"json([
          {"position": "A1", "title": "A"}, {"position": "A2", "title": "B"},
          {"position": "B1", "title": "C"}, {"position": "B2", "title": "D"}
        ])json"));
    CHECK(record && record->media.size() == 1U);
    if (record && record->media.size() == 1U && record->media[0].tracks.size() == 4U) {
        CHECK(record->media[0].tracks[2].number == "B1");
        CHECK(record->media[0].tracks[2].position == 3U);
        CHECK(record->media[0].format == "Vinyl");
    }

    // Two records: a pair of sides each.
    const auto double_record =
        parse_discogs_release(discogs_release(R"([{"name": "Vinyl", "qty": "2"}])", R"json([
          {"position": "A", "title": "A"}, {"position": "B", "title": "B"},
          {"position": "C", "title": "C"}, {"position": "D1", "title": "D"},
          {"position": "D2", "title": "E"}
        ])json"));
    CHECK(double_record && double_record->media.size() == 2U);
    if (double_record && double_record->media.size() == 2U) {
        CHECK(double_record->media[0].tracks.size() == 2U);
        CHECK(double_record->media[1].tracks.size() == 3U);
        CHECK(double_record->media[1].tracks[0].position == 1U);
        CHECK(double_record->media[1].tracks[0].number == "C");
    }

    // Positions that say nothing are one disc, in order.
    const auto loose =
        parse_discogs_release(discogs_release(R"([{"name": "File", "qty": "1"}])", R"json([
          {"position": "", "title": "A"}, {"position": "Video", "title": "B"}
        ])json"));
    CHECK(loose && loose->media.size() == 1U && loose->media[0].format == "Digital Media");
    if (loose && loose->media.size() == 1U && loose->media[0].tracks.size() == 2U) {
        CHECK(loose->media[0].tracks[0].number == "1");
        CHECK(loose->media[0].tracks[1].position == 2U);
    }

    // A year alone, and no identity.
    const auto year = parse_discogs_release(R"json({"id": 4, "title": "T", "released": "2001-00-00",
        "tracklist": []})json");
    CHECK(year && year->date == "2001" && year->media.empty());
    CHECK(!parse_discogs_release(R"json({"title": "T"})json"));
    CHECK(!parse_discogs_release(R"json({"id": 4})json"));

    const auto images = parse_discogs_images(R"json({"images": [
        {"type": "secondary", "uri": "https://i.discogs.com/b.jpg", "uri150": "https://i.discogs.com/b150.jpg"},
        {"type": "primary", "uri": "https://i.discogs.com/a.jpg", "uri150": "https://i.discogs.com/a150.jpg"},
        {"type": "primary", "uri": "https://evil.example/c.jpg"}]})json");
    CHECK(images && images->images.size() == 2U);
    if (images && images->images.size() == 2U) {
        CHECK(!images->images[0].front && images->images[1].front);
        CHECK(images->images[1].source == "Discogs");
        CHECK(images->images[1].thumbnail_url == "https://i.discogs.com/a150.jpg");
        CHECK(select_front_cover(*images) == std::optional<std::size_t>{1U});
    }
}

} // namespace

int main() {
    searchUrlEscapesLuceneAndPercentEncodes();
    lookupUrlRequiresAReleaseId();
    releaseIdsAreFoundInLinks();
    releaseGroupReleasesAreListed();
    searchParsingCarriesVersionDetail();
    lookupParsingAlignsTracksAndRecordings();
    malformedAndOversizedPayloadsFailTyped();
    coverArtUrlRequiresAReleaseId();
    coverArtParsingUpgradesAndSelectsTheFront();
    acoustidBodyAndParsingAreBounded();
    bandcampPagesAndCoversAreRecognised();
    discogsUrlsAndLinksAreStrict();
    discogsSearchAndVersionsAreListed();
    discogsReleasesSplitIntoDiscs();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
