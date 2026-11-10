// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/dynamic_playlist_definition.hpp"
#include "bench/local_list_model.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/dynamic_selection.hpp"
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <functional>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;

namespace trackknife::bench {

inline constexpr int lastfm_candidate_limit = 500;

struct RecommendationTrack {
    QString artist{};
    QString title;
    QString mbid;
};

// The definitions the window kept in its settings before ADR-0349, by
// profile, independently versioned from ordinary playlists. No key or
// password is stored in a definition, and no shipped one. They are handed to
// each engine once; the engines keep the rules now.
core::Result<QVector<DynamicPlaylistDefinition>> loadDynamicPlaylists(const QString& profile);
core::Result<void> saveDynamicPlaylists(const QString& profile,
                                        const QVector<DynamicPlaylistDefinition>& definitions);
// ADR-0258: moves definitions saved under any other profile -- the retired
// MPD backend kept its own -- into `profile`, once.
void adoptDynamicPlaylists(const QString& profile);
core::Result<QVector<RecommendationTrack>> parseLastFmTracks(const QByteArray& data, int limit);

class DynamicPlaylistService final : public QObject {
    Q_OBJECT
  public:
    using Tracks = std::vector<LocalTrackRow>;
    using Result = core::Result<Tracks>;
    using Completion = std::function<void(Result)>;
    // ADR-0258: a library's selection, as its engine makes it.
    using Search =
        std::function<void(engine::DynamicSelection, core::CancellationToken, Completion)>;
    DynamicPlaylistService(Search search, QObject* parent = nullptr);
    void refresh(DynamicPlaylistDefinition definition, const QString& lastfm_key);
    void cancel();
    std::size_t matchedPoolSize() const { return matched_pool_size_; }
    // Same matching path as a provider response, without network access.
    void matchRecommendations(DynamicPlaylistDefinition definition,
                              QVector<RecommendationTrack> candidates);
  signals:
    void progress(const QString& message);
    void finished(const Tracks& tracks, int unmatched, const QString& error);

  private:
    void matchNext(quint64 generation);
    void complete();
    void fail(const QString& message);
    Search search_;
    core::CancellationSource cancellation_;
    QNetworkAccessManager* network_;
    QPointer<QNetworkReply> reply_;
    quint64 generation_{0};
    DynamicPlaylistDefinition definition_;
    QVector<RecommendationTrack> candidates_;
    qsizetype candidate_index_{0};
    int unmatched_{0};
    Tracks result_;
    std::size_t matched_pool_size_{0};
};
// The library the catalogue source chose -- this process's or an engine's --
// never a database opened here: its selection, and the cached tags of only
// what it selected.
DynamicPlaylistService::Result selectDynamicLibrary(const engine::Catalogue& catalogue,
                                                    const engine::DynamicSelection& selection,
                                                    const core::CancellationToken& cancellation);
} // namespace trackknife::bench
