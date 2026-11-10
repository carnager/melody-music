// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/dynamic_playlist_definition.hpp"
#include "bench/local_list_model.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/engine/catalogue.hpp"
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>
#include <vector>

namespace trackknife::bench {

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

// A rule refreshed into tracks. ADR-0350: the engine picks -- from its
// library, or what Last.fm suggests that its library has -- and this shows
// what it picked.
class DynamicPlaylistService final : public QObject {
    Q_OBJECT
  public:
    using Tracks = std::vector<LocalTrackRow>;
    using Result = core::Result<Tracks>;
    // What the engine picked, named from its library.
    struct Picked {
        Tracks tracks;
        // How many tracks there were to choose from.
        std::size_t matched{0U};
        // Last.fm's suggestions the library does not have.
        std::size_t unmatched{0U};
    };
    using Completion = std::function<void(core::Result<Picked>)>;
    // While Last.fm's suggestions are matched: how many, of how many.
    using Progress = std::function<void(std::size_t done, std::size_t of)>;
    // Asks the engine to pick for a rule, saved or not. Both callbacks are
    // called on this object's thread.
    using Pick = std::function<void(DynamicPlaylistDefinition, core::CancellationToken, Progress,
                                    Completion)>;
    explicit DynamicPlaylistService(Pick pick, QObject* parent = nullptr);
    void refresh(DynamicPlaylistDefinition definition);
    void cancel();
    std::size_t matchedPoolSize() const { return matched_pool_size_; }
  signals:
    void progress(const QString& message);
    void finished(const Tracks& tracks, int unmatched, const QString& error);

  private:
    Pick pick_;
    core::CancellationSource cancellation_;
    quint64 generation_{0};
    std::size_t matched_pool_size_{0};
};
// The library's rows for files it picked -- this process's or an engine's,
// never a database opened here: the cached tags of only those, in order.
DynamicPlaylistService::Result dynamicRows(const engine::Catalogue& catalogue,
                                           const std::vector<std::string>& paths,
                                           const core::CancellationToken& cancellation);
} // namespace trackknife::bench
