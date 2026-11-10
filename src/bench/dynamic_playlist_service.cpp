// SPDX-License-Identifier: GPL-3.0-only
#include "bench/dynamic_playlist_service.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/persistence/local_library.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSettings>
#include <algorithm>

namespace trackknife::bench {
namespace {
core::Error error(const QString& message) {
    return {
        .code = core::ErrorCode::invalid_argument, .message = message.toStdString(), .context = {}};
}
QString key(const QString& profile) { return QStringLiteral("dynamic-playlists/v1/") + profile; }
bool validSource(const QString& source) {
    return source == QStringLiteral("rules") || source == QStringLiteral("similar") ||
           source == QStringLiteral("loved") || source == QStringLiteral("top") ||
           source == QStringLiteral("tag");
}
bool validCount(const int count) { return count >= 0 && count <= 500; }
bool validSelection(const DynamicPlaylistDefinition& d) {
    return d.limit >= 1 && d.limit <= 500 && validCount(d.groups) && validCount(d.per_group) &&
           (d.groups == 0 || !d.group_by.trimmed().isEmpty());
}
} // namespace

void adoptDynamicPlaylists(const QString& profile) {
    const auto done = QStringLiteral("dynamic-playlists/adopted-v1");
    QSettings settings;
    if (settings.value(done).toBool())
        return;
    settings.beginGroup(QStringLiteral("dynamic-playlists/v1"));
    const auto profiles = settings.allKeys();
    settings.endGroup();
    auto kept = loadDynamicPlaylists(profile);
    if (!kept)
        return;
    QStringList moved;
    for (const auto& other : profiles) {
        if (other == profile)
            continue;
        const auto found = loadDynamicPlaylists(other);
        if (!found)
            continue;
        for (auto definition : *found) {
            if (std::ranges::any_of(
                    *kept, [&definition](const auto& known) { return known.id == definition.id; }))
                continue;
            definition.profile = profile;
            kept->push_back(std::move(definition));
        }
        moved.push_back(other);
    }
    if (!moved.isEmpty() && !saveDynamicPlaylists(profile, *kept))
        return;
    for (const auto& other : moved)
        settings.remove(key(other));
    settings.setValue(done, true);
}

core::Result<QVector<DynamicPlaylistDefinition>> loadDynamicPlaylists(const QString& profile) {
    QVector<DynamicPlaylistDefinition> result;
    const auto bytes = QSettings{}.value(key(profile)).toByteArray();
    if (bytes.isEmpty())
        return result;
    const auto doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject() || doc.object().value(QStringLiteral("version")).toInt() != 1 ||
        !doc.object().value(QStringLiteral("definitions")).isArray())
        return std::unexpected(
            error(QStringLiteral("Cannot read this dynamic-playlist catalog version")));
    for (const auto& entry : doc.object().value(QStringLiteral("definitions")).toArray()) {
        const auto o = entry.toObject();
        DynamicPlaylistDefinition d{.id = o["id"].toString(),
                                    .name = o["name"].toString(),
                                    .profile = profile,
                                    .source = o["source"].toString(),
                                    .query = o["query"].toString(),
                                    .artist = o["artist"].toString(),
                                    .track = o["track"].toString(),
                                    .user = o["user"].toString(),
                                    .tag = o["tag"].toString(),
                                    .limit = o["limit"].toInt(),
                                    .shuffle = o["shuffle"].toBool(),
                                    .group_by = o["group_by"].toString(),
                                    .groups = o["groups"].toInt(),
                                    .per_group = o["per_group"].toInt()};
        if (d.id.isEmpty() || d.name.isEmpty() || d.shipped() || !validSource(d.source) ||
            !validSelection(d) || o["dialect"].toString() != QStringLiteral("tkq") ||
            o["dialect_version"].toInt() != 1 || o["compiler_schema"].toInt() != 1)
            return std::unexpected(
                error(QStringLiteral("Invalid or unsupported dynamic-playlist definition")));
        result.push_back(std::move(d));
    }
    return result;
}
core::Result<void> saveDynamicPlaylists(const QString& profile,
                                        const QVector<DynamicPlaylistDefinition>& definitions) {
    if (profile.isEmpty())
        return std::unexpected(error(QStringLiteral("Connect to a saved server profile first")));
    if (const auto existing = loadDynamicPlaylists(profile); !existing)
        return std::unexpected(existing.error());
    QJsonArray entries;
    for (const auto& d : definitions) {
        if (d.profile != profile || d.id.isEmpty() || d.shipped() || d.name.trimmed().isEmpty() ||
            !validSource(d.source) || !validSelection(d))
            return std::unexpected(error(QStringLiteral("Invalid dynamic-playlist definition")));
        entries.append(QJsonObject{{"id", d.id},
                                   {"name", d.name},
                                   {"source", d.source},
                                   {"query", d.query},
                                   {"artist", d.artist},
                                   {"track", d.track},
                                   {"user", d.user},
                                   {"tag", d.tag},
                                   {"limit", d.limit},
                                   {"shuffle", d.shuffle},
                                   {"group_by", d.group_by},
                                   {"groups", d.groups},
                                   {"per_group", d.per_group},
                                   {"dialect", "tkq"},
                                   {"dialect_version", 1},
                                   {"compiler_schema", 1}});
    }
    QSettings settings;
    settings.setValue(key(profile),
                      QJsonDocument(QJsonObject{{"version", 1}, {"definitions", entries}})
                          .toJson(QJsonDocument::Compact));
    // Uses the same settings persistence contract as the rest of the workspace.
    return {};
}
DynamicPlaylistService::DynamicPlaylistService(Pick pick, QObject* parent)
    : QObject(parent), pick_(std::move(pick)) {}

void DynamicPlaylistService::cancel() {
    ++generation_;
    cancellation_.request_cancellation();
    cancellation_ = core::CancellationSource{};
}

void DynamicPlaylistService::refresh(DynamicPlaylistDefinition definition) {
    cancel();
    const auto generation = generation_;
    const bool from_library = definition.source == QStringLiteral("rules");
    emit progress(from_library ? QStringLiteral("Updating from library tags and ratings…")
                               : QStringLiteral("Asking Last.fm…"));
    QPointer<DynamicPlaylistService> self(this);
    pick_(
        std::move(definition), cancellation_.token(),
        [self, generation](const std::size_t done, const std::size_t of) {
            if (self && generation == self->generation_ && of > 0U)
                emit self->progress(
                    QStringLiteral("Matching library tracks: %1 / %2").arg(done).arg(of));
        },
        [self, generation](core::Result<Picked> picked) {
            if (!self || generation != self->generation_)
                return;
            if (!picked) {
                emit self->finished({}, 0, QString::fromStdString(picked.error().message));
                return;
            }
            self->matched_pool_size_ = picked->matched;
            emit self->finished(picked->tracks, static_cast<int>(picked->unmatched), {});
        });
}

DynamicPlaylistService::Result dynamicRows(const engine::Catalogue& catalogue,
                                           const std::vector<std::string>& paths,
                                           const core::CancellationToken& cancellation) {
    auto cached = catalogue.cached_tracks(paths, cancellation);
    if (!cached)
        return std::unexpected(cached.error());
    std::vector<LocalTrackRow> rows;
    rows.reserve(cached->size());
    for (auto& entry : *cached)
        rows.push_back(cached_library_row(std::move(entry)));
    return rows;
}
} // namespace trackknife::bench
