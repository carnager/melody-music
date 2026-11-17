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
