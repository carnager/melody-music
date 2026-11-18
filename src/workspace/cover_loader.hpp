// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"

#include <QCache>
#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

namespace trackknife::bench {

// Covers asked of an engine one at a time, made thumbnails off the UI thread,
// and kept: a tab's by its album group, the library's by its album key. Each
// says how to ask for its cover; this queues the asking and keeps the answers.
class CoverLoader final : public QObject {
    Q_OBJECT

  public:
    // A cover's encoded bytes as an engine answers them: empty, it has none;
    // an error -- the engine not reached -- is not taken for "none" and the
    // cover is asked again when next wanted.
    using Fetch =
        std::function<core::Result<std::vector<unsigned char>>(const core::CancellationToken&)>;

    // At most `kept` covers are kept, the least recently looked at going first.
    explicit CoverLoader(int kept, QObject* parent = nullptr);
    ~CoverLoader() override;
    CoverLoader(const CoverLoader&) = delete;
    CoverLoader& operator=(const CoverLoader&) = delete;

    // The cover, or a null image when there is none; nullopt, not known yet.
    [[nodiscard]] std::optional<QImage> cached(const QString& key) const;
    // Asked for and not answered yet.
    [[nodiscard]] bool pending(const QString& key) const;
    // Asked once: a cover known or on its way is not asked again.
    void request(const QString& key, Fetch fetch);
    // Only these are still wanted: the rest waiting are dropped, and one
    // being asked for is let go.
    void keepOnly(const QSet<QString>& keys);
    // Forgotten -- changed, say -- and an answer on its way discarded: asked
    // afresh when next wanted.
    void forget(const QString& key);
    void clear();
    // Nothing more asked; returns once what is under way has finished.
    void stop();

  signals:
    // An answer: the cover, or a null image when there is none.
    void loaded(const QString& key, const QImage& image);

  private:
    struct Job {
        QString key;
        Fetch fetch;
    };
    struct Answer {
        QImage image;
        bool failed{false};
    };

    void pump();
    void finish();

    mutable QCache<QString, QImage> cache_;
    std::deque<Job> queue_;
    QSet<QString> queued_;
    QString loading_key_;
    bool loading_{false};
    // The answer on its way is for a cover since forgotten.
    bool discard_{false};
    bool stopped_{false};
    core::CancellationSource cancellation_;
    QThreadPool pool_;
    QFutureWatcher<Answer> watcher_;
};

} // namespace trackknife::bench
