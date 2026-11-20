// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/discogs.hpp"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace trackknife::musicbrainz {
namespace {

constexpr std::string_view api_root = "https://api.discogs.com";

[[nodiscard]] core::Error discogs_error(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

[[nodiscard]] bool is_number(const std::string_view id) {
    return !id.empty() && id.size() <= 12U && id.front() != '0' &&
           std::ranges::all_of(id, [](const unsigned char c) { return std::isdigit(c) != 0; });
}

[[nodiscard]] std::string url_encoded(const std::string& value) {
    const auto encoded = QUrl::toPercentEncoding(QString::fromStdString(value));
    return std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

[[nodiscard]] std::string trimmed(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    value.erase(0, first);
    value.erase(value.find_last_not_of(" \t\r\n") + 1U);
    return value;
}

[[nodiscard]] std::string bounded_text(const QJsonValue& value, const WebServiceLimits& limits) {
    std::string text;
    if (value.isString()) {
        text = value.toString().toStdString();
    } else if (value.isDouble()) {
        // Ids and years come as numbers.
        const auto number = value.toInteger();
        text = number > 0 ? std::to_string(number) : std::string{};
    }
    if (text.size() > limits.text_bytes) {
        text.resize(limits.text_bytes);
    }
    return trimmed(std::move(text));
}

// A small count as text ("2"), or as a number; 0 for anything else.
[[nodiscard]] long long count_of(const QJsonValue& value) {
    if (value.isDouble()) {
        return std::max<qint64>(value.toInteger(0), 0);
    }
    bool ok = false;
    const auto count = value.toString().trimmed().toLongLong(&ok);
    return ok && count > 0 ? count : 0;
}

[[nodiscard]] core::Result<QJsonObject> parse_body(const std::string_view body,
                                                   const WebServiceLimits& limits) {
    if (body.size() > limits.body_bytes) {
        return std::unexpected(discogs_error(core::ErrorCode::limit_exceeded,
                                             "the Discogs response exceeds the size limit"));
    }
    QJsonParseError parse_error{};
    const auto document = QJsonDocument::fromJson(
        QByteArray{body.data(), static_cast<qsizetype>(body.size())}, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(
            discogs_error(core::ErrorCode::backend, "the Discogs response is not a JSON object"));
    }
    return document.object();
}

// A date as Discogs writes it, as tags do: "2004-03-00" is "2004-03",
// "2004-00-00" "2004"; "0" none.
[[nodiscard]] std::string release_date(std::string date) {
    while (date.size() > 4U && date.ends_with("-00")) {
        date.resize(date.size() - 3U);
    }
    if (date.size() < 4U || date == "0" || date.starts_with("0000")) {
        return {};
    }
    return date;
}

// A catalogue number of "none" is none.
[[nodiscard]] std::string catalog_number(std::string number) {
    std::string lowered = number;
    std::ranges::transform(lowered, lowered.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lowered == "none" ? std::string{} : number;
}

// Discogs names a digital release's format "File"; tags, as MusicBrainz,
// "Digital Media".
[[nodiscard]] std::string medium_format(std::string name) {
    if (name == "File") {
        return "Digital Media";
    }
    return name;
}

// A join between artists as Discogs gives it, as a join phrase: "," is
// ", ", a word is spaced.
[[nodiscard]] std::string join_phrase(std::string join) {
    join = trimmed(std::move(join));
    if (join.empty()) {
        return {};
    }
    if (join == ",") {
        return ", ";
    }
    return " " + join + " ";
}

[[nodiscard]] std::vector<ArtistCredit> artist_credits(const QJsonValue& value,
                                                       const WebServiceLimits& limits) {
    std::vector<ArtistCredit> credits;
    if (!value.isArray()) {
        return credits;
    }
    for (const auto& entry : value.toArray()) {
        if (credits.size() == limits.artist_credits) {
            break;
        }
        const auto artist = entry.toObject();
        const auto credited = bounded_text(artist.value(QStringLiteral("anv")), limits);
        auto name =
            credited.empty()
                ? discogs_artist_name(bounded_text(artist.value(QStringLiteral("name")), limits))
                : credited;
        if (name.empty()) {
            continue;
        }
        credits.push_back(ArtistCredit{
            .name = std::move(name),
            .join_phrase = join_phrase(bounded_text(artist.value(QStringLiteral("join")), limits)),
            .artist_id = bounded_text(artist.value(QStringLiteral("id")), limits),
            .sort_name = {},
        });
    }
    // The last joins nothing.
    if (!credits.empty()) {
        credits.back().join_phrase.clear();
    }
    return credits;
}

// "4:05" or "1:02:03" in milliseconds; none for anything else.
[[nodiscard]] std::optional<std::int64_t> duration_ms(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t seconds = 0;
    std::int64_t part = 0;
    bool digits = false;
    for (const auto c : text) {
        if (c >= '0' && c <= '9') {
            part = part * 10 + (c - '0');
            digits = true;
            if (part > 1'000'000) {
                return std::nullopt;
            }
        } else if (c == ':' && digits) {
            seconds = seconds * 60 + part;
            part = 0;
            digits = false;
        } else {
            return std::nullopt;
        }
    }
    if (!digits) {
        return std::nullopt;
    }
    seconds = seconds * 60 + part;
    return seconds > 0 ? std::optional{seconds * 1'000} : std::nullopt;
}

// Release types from a format's descriptions, as MusicBrainz names them.
void release_types(const QJsonValue& formats, Release& release) {
    for (const auto& format : formats.toArray()) {
        for (const auto& description :
             format.toObject().value(QStringLiteral("descriptions")).toArray()) {
            const auto text = description.toString();
            if (release.release_group_primary_type.empty()) {
                if (text == QStringLiteral("Album") || text == QStringLiteral("LP")) {
                    release.release_group_primary_type = "Album";
                } else if (text == QStringLiteral("EP") || text == QStringLiteral("Mini-Album")) {
                    release.release_group_primary_type = "EP";
                } else if (text == QStringLiteral("Single") ||
                           text == QStringLiteral("Maxi-Single")) {
                    release.release_group_primary_type = "Single";
                }
            }
            if (text == QStringLiteral("Compilation") &&
                std::ranges::find(release.release_group_secondary_types, "Compilation") ==
                    release.release_group_secondary_types.end()) {
                release.release_group_secondary_types.emplace_back("Compilation");
            }
        }
    }
}

// The formats of a release, one per disc: "2×CD + DVD" is CD, CD, DVD.
[[nodiscard]] std::vector<std::string> disc_formats(const QJsonValue& formats,
                                                    const WebServiceLimits& limits) {
    std::vector<std::string> discs;
    for (const auto& format : formats.toArray()) {
        const auto object = format.toObject();
        const auto name = medium_format(bounded_text(object.value(QStringLiteral("name")), limits));
        const auto quantity = std::clamp<long long>(count_of(object.value(QStringLiteral("qty"))),
                                                    1, static_cast<long long>(limits.media));
        for (long long disc = 0; disc < quantity && discs.size() < limits.media; ++disc) {
            discs.push_back(name);
        }
    }
    return discs;
}

[[nodiscard]] bool is_record(const std::string& format) {
    return format == "Vinyl" || format == "Shellac" || format == "Lathe Cut" ||
           format == "Acetate" || format == "Flexi-disc" || format == "Cassette";
}

// What a track's position says of its disc.
struct Position {
    // Named: "2-3", "CD2-3", "2.3".
    std::optional<std::size_t> disc;
    // A record's side, from 0: "A1", "B", "C2".
    std::optional<std::size_t> side;
};

[[nodiscard]] Position position_of(const std::string& text) {
    static const QRegularExpression named{
        QStringLiteral(
            R"(^(?:CD|DVD|LP|SACD|DISC|DISK|MC|BD)?\s*(\d{1,3})\s*[-.]\s*\d{1,3}[A-Z]?$)"),
        QRegularExpression::CaseInsensitiveOption};
    static const QRegularExpression side{QStringLiteral(R"(^([A-Za-z])\1?\d{0,3}[a-z]?$)")};
    const auto value = QString::fromStdString(text);
    if (const auto match = named.match(value); match.hasMatch()) {
        const auto disc = match.captured(1).toULongLong();
        if (disc > 0U) {
            return Position{.disc = static_cast<std::size_t>(disc), .side = std::nullopt};
        }
    }
    if (const auto match = side.match(value); match.hasMatch()) {
        const auto letter = match.captured(1).toUpper().at(0).toLatin1();
        return Position{.disc = std::nullopt, .side = static_cast<std::size_t>(letter - 'A')};
    }
    return {};
}

struct ListedTrack {
    std::string position;
    ReleaseTrack track;
    Position where;
};

[[nodiscard]] core::Result<std::vector<ListedTrack>> listed_tracks(const QJsonValue& tracklist,
                                                                   const WebServiceLimits& limits) {
    std::vector<ListedTrack> tracks;
    const auto bound = limits.media * limits.tracks_per_medium;
    for (const auto& entry : tracklist.toArray()) {
        const auto object = entry.toObject();
        const auto kind = object.value(QStringLiteral("type_")).toString(QStringLiteral("track"));
        // Headings name a part of the release, not a track.
        if (kind != QStringLiteral("track") && kind != QStringLiteral("index")) {
            continue;
        }
        if (tracks.size() == bound) {
            return std::unexpected(discogs_error(core::ErrorCode::limit_exceeded,
                                                 "a Discogs release exceeds the track limit"));
        }
        auto position = bounded_text(object.value(QStringLiteral("position")), limits);
        auto length = duration_ms(bounded_text(object.value(QStringLiteral("duration")), limits));
        // An index track is one track: its parts' positions and lengths.
        if (kind == QStringLiteral("index")) {
            std::int64_t parts = 0;
            bool whole = true;
            for (const auto& part : object.value(QStringLiteral("sub_tracks")).toArray()) {
                const auto sub = part.toObject();
                if (position.empty()) {
                    position = bounded_text(sub.value(QStringLiteral("position")), limits);
                }
                const auto sub_length =
                    duration_ms(bounded_text(sub.value(QStringLiteral("duration")), limits));
                whole = whole && sub_length.has_value();
                parts += sub_length.value_or(0);
            }
            if (!length && whole && parts > 0) {
                length = parts;
            }
        }
        auto title = bounded_text(object.value(QStringLiteral("title")), limits);
        if (title.empty()) {
            continue;
        }
        tracks.push_back(ListedTrack{
            .position = position,
            .track =
                ReleaseTrack{
                    .track_id = {},
                    .recording_id = {},
                    .position = 0U,
                    .number = position,
                    .title = std::move(title),
                    .length_ms = length,
                    .artist_credits =
                        artist_credits(object.value(QStringLiteral("artists")), limits),
                    .isrcs = {},
                    .work_ids = {},
                },
            .where = position_of(position),
        });
    }
    return tracks;
}

// The tracks split into discs: by the discs their positions name; else by
// sides, a pair of them a record where the release has as many records;
// else one disc.
[[nodiscard]] std::vector<ReleaseMedium> media_of(std::vector<ListedTrack> tracks,
                                                  const std::vector<std::string>& formats) {
    const auto named =
        std::ranges::any_of(tracks, [](const auto& t) { return t.where.disc.has_value(); });
    const auto sides = !named && !tracks.empty() && std::ranges::all_of(tracks, [](const auto& t) {
        return t.where.side.has_value();
    });
    const auto records = static_cast<std::size_t>(std::ranges::count_if(formats, is_record));
    std::map<std::size_t, ReleaseMedium> discs;
    std::size_t current = 1U;
    for (auto& listed : tracks) {
        if (named) {
            // A track naming no disc stays on the one before.
            current = listed.where.disc.value_or(current);
        } else if (sides && records > 1U) {
            current = std::min(*listed.where.side / 2U + 1U, records);
        }
        auto& medium = discs[current];
        medium.tracks.push_back(std::move(listed.track));
    }
    std::vector<ReleaseMedium> media;
    for (auto& [disc, medium] : discs) {
        medium.position = disc;
        medium.track_count = medium.tracks.size();
        for (std::size_t index = 0U; index < medium.tracks.size(); ++index) {
            auto& track = medium.tracks[index];
            track.position = index + 1U;
            if (track.number.empty()) {
                track.number = std::to_string(index + 1U);
            }
        }
        const auto slot = media.size();
        medium.format =
            formats.empty() ? std::string{} : formats[std::min(slot, formats.size() - 1U)];
        media.push_back(std::move(medium));
    }
    return media;
}

[[nodiscard]] std::string first_label(const QJsonValue& labels, const WebServiceLimits& limits,
                                      std::string* catalog) {
    const auto list = labels.toArray();
    if (list.isEmpty()) {
        return {};
    }
    // A search lists labels as names; a release, as objects.
    if (list.first().isString()) {
        return discogs_artist_name(bounded_text(list.first(), limits));
    }
    const auto first = list.first().toObject();
    if (catalog != nullptr) {
        *catalog = catalog_number(bounded_text(first.value(QStringLiteral("catno")), limits));
    }
    return discogs_artist_name(bounded_text(first.value(QStringLiteral("name")), limits));
}

[[nodiscard]] std::string barcode_of(const QJsonValue& identifiers,
                                     const WebServiceLimits& limits) {
    for (const auto& entry : identifiers.toArray()) {
        const auto object = entry.toObject();
        if (object.value(QStringLiteral("type")).toString() != QStringLiteral("Barcode")) {
            continue;
        }
        auto value = bounded_text(object.value(QStringLiteral("value")), limits);
        std::erase_if(value,
                      [](const unsigned char c) { return std::isspace(c) != 0 || c == '-'; });
        if (!value.empty() && std::ranges::all_of(value, [](const unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            return value;
        }
    }
    return {};
}

[[nodiscard]] Release empty_release() {
    return Release{
        .source = ReleaseSource::discogs,
        .id = {},
        .title = {},
        .status = {},
        .disambiguation = {},
        .date = {},
        .country = {},
        .barcode = {},
        .release_group_id = {},
        .release_group_primary_type = {},
        .release_group_secondary_types = {},
        .release_group_first_release_date = {},
        .script = {},
        .language = {},
        .label = {},
        .catalog_number = {},
        .search_score = 0,
        .track_count = 0U,
        .artist_credits = {},
        .media = {},
    };
}

// What a search or a master's listing says of a format: "CD, Album" or
// ["CD", "Album"] -- its first word the medium's.
void listed_format(const QJsonValue& format, const QJsonValue& quantity, Release& release,
                   const WebServiceLimits& limits) {
    std::string name;
    if (format.isArray() && !format.toArray().isEmpty()) {
        name = bounded_text(format.toArray().first(), limits);
    } else if (format.isString()) {
        name = bounded_text(format, limits);
        name = trimmed(name.substr(0U, name.find(',')));
    }
    if (name.empty()) {
        return;
    }
    const auto count = std::clamp<long long>(quantity.isUndefined() ? 1 : count_of(quantity), 1,
                                             static_cast<long long>(limits.media));
    for (long long disc = 0; disc < count; ++disc) {
        release.media.push_back(ReleaseMedium{.position = static_cast<std::size_t>(disc) + 1U,
                                              .format = medium_format(name),
                                              .title = {},
                                              .track_count = 0U,
                                              .tracks = {}});
    }
}

[[nodiscard]] std::optional<std::string> id_in(const std::string_view text, const QString& kind,
                                               const QChar letter) {
    const auto value =
        QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())).trimmed();
    const QRegularExpression link{
        QStringLiteral(R"(^(?:https?://)?(?:www\.)?discogs\.com/(?:[a-z]{2}(?:-[a-z]{2})?/)?)") +
            kind + QStringLiteral(R"(/(\d{1,12})(?:[-/?#].*)?$)"),
        QRegularExpression::CaseInsensitiveOption};
    const QRegularExpression reference{QStringLiteral(R"(^\[)") + letter +
                                           QStringLiteral(R"((\d{1,12})\]$)"),
                                       QRegularExpression::CaseInsensitiveOption};
    for (const auto* pattern : {&link, &reference}) {
        if (const auto match = pattern->match(value); match.hasMatch()) {
            auto id = match.captured(1).toStdString();
            if (is_number(id)) {
                return id;
            }
        }
    }
    return std::nullopt;
}

} // namespace

bool is_cd_or_digital(const Release& release) {
    if (release.media.empty()) {
        return true;
    }
    return std::ranges::any_of(release.media, [](const ReleaseMedium& medium) {
        return medium.format == "CD" || medium.format == "CDr" || medium.format == "SACD" ||
               medium.format == "Digital Media";
    });
}

void keep_cd_and_digital(ReleaseSearchResult& result, const std::size_t at_most) {
    std::erase_if(result.releases,
                  [](const Release& release) { return !is_cd_or_digital(release); });
    if (result.releases.size() > at_most) {
        result.releases.resize(at_most);
    }
}

namespace {

// A name as compared: folded, its accents and punctuation gone, a leading
// "The" too.
[[nodiscard]] QString compared_name(const std::string_view name) {
    auto text = QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()))
                    .normalized(QString::NormalizationForm_KD)
                    .toCaseFolded();
    QString kept;
    for (const auto character : text) {
        if (character.isLetterOrNumber()) {
            kept.append(character);
        } else if (character.isSpace() && !kept.endsWith(QLatin1Char{' '})) {
            kept.append(QLatin1Char{' '});
        }
    }
    kept = kept.trimmed();
    if (kept.startsWith(QStringLiteral("the "))) {
        kept.remove(0, 4);
    }
    kept.remove(QLatin1Char{' '});
    return kept;
}

} // namespace

void keep_artist(ReleaseSearchResult& result, const std::string_view artist, const bool strictly) {
    const auto wanted = compared_name(artist);
    if (wanted.isEmpty()) {
        return;
    }
    const auto by = [&wanted](const Release& release) {
        std::string credited;
        for (const auto& credit : release.artist_credits) {
            credited += credit.name + credit.join_phrase;
        }
        return compared_name(credited) == wanted;
    };
    if (strictly || std::ranges::any_of(result.releases, by)) {
        std::erase_if(result.releases, [&by](const Release& release) { return !by(release); });
    }
}

std::string discogs_artist_name(const std::string_view name) {
    static const QRegularExpression suffix{QStringLiteral(R"(\s+\(\d+\)$)")};
    auto text = QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())).trimmed();
    text.remove(suffix);
    if (text == QStringLiteral("Various")) {
        return "Various Artists";
    }
    return text.toStdString();
}

core::Result<std::string> build_discogs_search_url(const ReleaseSearchQuery& query) {
    if (query.artist.empty() && query.release.empty()) {
        return std::unexpected(discogs_error(core::ErrorCode::invalid_argument,
                                             "a Discogs search needs an artist or a title"));
    }
    std::string url{api_root};
    url += "/database/search?type=release";
    if (!query.artist.empty()) {
        url += "&artist=" + url_encoded(query.artist);
    }
    if (!query.release.empty()) {
        url += "&release_title=" + url_encoded(query.release);
    }
    url += "&per_page=" + std::to_string(std::clamp<std::size_t>(query.limit, 1U, 100U));
    return url;
}

core::Result<std::string> build_discogs_release_url(const std::string_view release_id) {
    if (!is_number(release_id)) {
        return std::unexpected(
            discogs_error(core::ErrorCode::invalid_argument, "a Discogs release id is a number"));
    }
    return std::string{api_root} + "/releases/" + std::string{release_id};
}

core::Result<std::string> build_discogs_master_versions_url(const std::string_view master_id) {
    if (!is_number(master_id)) {
        return std::unexpected(
            discogs_error(core::ErrorCode::invalid_argument, "a Discogs master id is a number"));
    }
    return std::string{api_root} + "/masters/" + std::string{master_id} +
           "/versions?per_page=" + std::to_string(discogs_master_versions_limit);
}

std::optional<std::string> discogs_release_id_in(const std::string_view text) {
    return id_in(text, QStringLiteral("release"), QLatin1Char{'r'});
}

std::optional<std::string> discogs_master_id_in(const std::string_view text) {
    return id_in(text, QStringLiteral("master"), QLatin1Char{'m'});
}

core::Result<ReleaseSearchResult> parse_discogs_search(const std::string_view body,
                                                       const WebServiceLimits& limits) {
    auto root = parse_body(body, limits);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    ReleaseSearchResult result{
        .total_count =
            static_cast<std::size_t>(std::max<qint64>(0, root->value(QStringLiteral("pagination"))
                                                             .toObject()
                                                             .value(QStringLiteral("items"))
                                                             .toInteger(0))),
        .releases = {},
    };
    for (const auto& entry : root->value(QStringLiteral("results")).toArray()) {
        if (result.releases.size() == limits.releases) {
            break;
        }
        const auto object = entry.toObject();
        auto release = empty_release();
        release.id = bounded_text(object.value(QStringLiteral("id")), limits);
        // "Artist - Title".
        const auto both = bounded_text(object.value(QStringLiteral("title")), limits);
        const auto split = both.find(" - ");
        release.title = split == std::string::npos ? both : trimmed(both.substr(split + 3U));
        if (split != std::string::npos) {
            release.artist_credits.push_back(ArtistCredit{
                .name = discogs_artist_name(both.substr(0U, split)),
                .join_phrase = {},
                .artist_id = {},
                .sort_name = {},
            });
        }
        if (!is_number(release.id) || release.title.empty()) {
            continue;
        }
        release.date = release_date(bounded_text(object.value(QStringLiteral("year")), limits));
        release.country = bounded_text(object.value(QStringLiteral("country")), limits);
        release.label = first_label(object.value(QStringLiteral("label")), limits, nullptr);
        release.catalog_number =
            catalog_number(bounded_text(object.value(QStringLiteral("catno")), limits));
        release.release_group_id = bounded_text(object.value(QStringLiteral("master_id")), limits);
        listed_format(object.value(QStringLiteral("format")),
                      object.value(QStringLiteral("format_quantity")), release, limits);
        result.releases.push_back(std::move(release));
    }
    return result;
}

core::Result<ReleaseSearchResult> parse_discogs_master_versions(const std::string_view body,
                                                                const WebServiceLimits& limits) {
    auto root = parse_body(body, limits);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    ReleaseSearchResult result{
        .total_count =
            static_cast<std::size_t>(std::max<qint64>(0, root->value(QStringLiteral("pagination"))
                                                             .toObject()
                                                             .value(QStringLiteral("items"))
                                                             .toInteger(0))),
        .releases = {},
    };
    for (const auto& entry : root->value(QStringLiteral("versions")).toArray()) {
        if (result.releases.size() == limits.releases) {
            break;
        }
        const auto object = entry.toObject();
        auto release = empty_release();
        release.id = bounded_text(object.value(QStringLiteral("id")), limits);
        release.title = bounded_text(object.value(QStringLiteral("title")), limits);
        if (!is_number(release.id) || release.title.empty()) {
            continue;
        }
        release.date = release_date(bounded_text(object.value(QStringLiteral("released")), limits));
        release.country = bounded_text(object.value(QStringLiteral("country")), limits);
        release.label =
            discogs_artist_name(bounded_text(object.value(QStringLiteral("label")), limits));
        release.catalog_number =
            catalog_number(bounded_text(object.value(QStringLiteral("catno")), limits));
        // Its medium in major_formats ("Vinyl"); format says its size ("12\"").
        const auto major = object.value(QStringLiteral("major_formats"));
        listed_format(major.isArray() && !major.toArray().isEmpty()
                          ? major
                          : object.value(QStringLiteral("format")),
                      QJsonValue{1}, release, limits);
        result.releases.push_back(std::move(release));
    }
    return result;
}

core::Result<Release> parse_discogs_release(const std::string_view body,
                                            const WebServiceLimits& limits) {
    auto root = parse_body(body, limits);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    auto release = empty_release();
    release.id = bounded_text(root->value(QStringLiteral("id")), limits);
    release.title = bounded_text(root->value(QStringLiteral("title")), limits);
    if (!is_number(release.id) || release.title.empty()) {
        return std::unexpected(discogs_error(core::ErrorCode::backend,
                                             "a Discogs release lacks its identity or title"));
    }
    release.artist_credits = artist_credits(root->value(QStringLiteral("artists")), limits);
    release.date = release_date(bounded_text(root->value(QStringLiteral("released")), limits));
    if (release.date.empty()) {
        release.date = release_date(bounded_text(root->value(QStringLiteral("year")), limits));
    }
    release.country = bounded_text(root->value(QStringLiteral("country")), limits);
    release.label =
        first_label(root->value(QStringLiteral("labels")), limits, &release.catalog_number);
    release.barcode = barcode_of(root->value(QStringLiteral("identifiers")), limits);
    release.release_group_id = bounded_text(root->value(QStringLiteral("master_id")), limits);
    release_types(root->value(QStringLiteral("formats")), release);
    auto tracks = listed_tracks(root->value(QStringLiteral("tracklist")), limits);
    if (!tracks) {
        return std::unexpected(std::move(tracks.error()));
    }
    release.media =
        media_of(std::move(*tracks), disc_formats(root->value(QStringLiteral("formats")), limits));
    if (release.media.size() > limits.media ||
        std::ranges::any_of(release.media, [&limits](const ReleaseMedium& medium) {
            return medium.tracks.size() > limits.tracks_per_medium;
        })) {
        return std::unexpected(discogs_error(core::ErrorCode::limit_exceeded,
                                             "a Discogs release exceeds the track limit"));
    }
    for (const auto& medium : release.media) {
        release.track_count += medium.tracks.size();
    }
    return release;
}

core::Result<CoverArtListing> parse_discogs_images(const std::string_view body,
                                                   const WebServiceLimits& limits) {
    auto root = parse_body(body, limits);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    CoverArtListing listing;
    bool front = false;
    for (const auto& entry : root->value(QStringLiteral("images")).toArray()) {
        if (listing.images.size() == limits.cover_art_images) {
            break;
        }
        const auto object = entry.toObject();
        auto image = bounded_text(object.value(QStringLiteral("uri")), limits);
        if (!is_discogs_image(image)) {
            continue;
        }
        auto thumbnail = bounded_text(object.value(QStringLiteral("uri150")), limits);
        const auto primary =
            !front && object.value(QStringLiteral("type")).toString() == QStringLiteral("primary");
        front = front || primary;
        listing.images.push_back(CoverArtImage{
            .id = std::to_string(listing.images.size() + 1U),
            .front = primary,
            .back = false,
            .approved = true,
            .comment = {},
            .types = primary ? std::vector<std::string>{"Front"} : std::vector<std::string>{},
            .image_url = std::move(image),
            .thumbnail_url = is_discogs_image(thumbnail) ? std::move(thumbnail) : std::string{},
            .source = "Discogs",
        });
    }
    return listing;
}

} // namespace trackknife::musicbrainz
