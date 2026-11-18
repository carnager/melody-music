// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/keep_awake.hpp"

#include "trackknife/engine/remote_file_work.hpp"
#include "workspace/tagger_session.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace trackknife::bench {

// The selected tracks as the engine is asked to measure them, read from the
// window's list when a round starts.
using ReplayGainItemReader = std::function<std::vector<engine::ReplayGainItem>()>;

// ADR-0156, ADR-0357: the selected tracks' loudness measured and their
// ReplayGain written without the tag editor's review grid -- grouped into
// albums as chosen, previewed first if wanted. The engine holding the files
// does the whole round (replaygain.apply); this asks it and tells how far
// it has got. The options are the tag editor's settings. The window's
// ReplayGain dialog draws it.
class ReplayGainJob final : public QObject {
    Q_OBJECT

  public:
    ReplayGainJob(std::size_t item_count, ReplayGainItemReader items,
                  std::shared_ptr<engine::RemoteFileWork> work,
                  MetadataApplyObserver apply_observer, QObject* parent = nullptr);
    ~ReplayGainJob() override;

    [[nodiscard]] static QStringList groupings();
    [[nodiscard]] QString title() const;
    [[nodiscard]] int grouping() const { return grouping_; }
    [[nodiscard]] QString expression() const { return expression_; }
    [[nodiscard]] bool sidecarOnly() const { return sidecar_only_; }
    [[nodiscard]] bool truePeak() const { return true_peak_; }
    [[nodiscard]] QStringList groups() const { return groups_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QStringList problems() const { return problems_; }
    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] bool canStop() const { return running_ && !stopping_; }
    [[nodiscard]] int progressValue() const { return progress_value_; }
    // 0: under way, without a count.
    [[nodiscard]] int progressMaximum() const { return progress_maximum_; }

    void setGrouping(int index);
    void setExpression(const QString& expression);
    void setSidecarOnly(bool on);
    void setTruePeak(bool on);
    // The album groups the selection falls into, without measuring.
    void preview();
    // Measured and written.
    void run();
    void stop();

  signals:
    void changed();

  private:
    // How far the engine has got, told on a worker.
    struct Progress {
        // 0 reading, 1 measuring, 2 writing.
        std::atomic_int phase{0};
        std::atomic_size_t completed{0U};
        std::atomic_size_t total{0U};
    };
    using Outcome = std::shared_ptr<core::Result<engine::ReplayGainRound>>;

    void groupsChanged();
    void setStatusText(const QString& text);
    void start(bool preview);
    void showProgress();
    void finishRound();
    void setRunning(bool running);

    std::size_t item_count_{0U};
    ReplayGainItemReader items_;
    std::shared_ptr<engine::RemoteFileWork> work_;
    MetadataApplyObserver apply_observer_;

    int grouping_{0};
    QString expression_;
    bool sidecar_only_{false};
    bool true_peak_{false};
    QStringList groups_;
    QString status_;
    QStringList problems_;
    bool preview_only_{false};
    bool running_{false};
    // ADR-0277: the computer kept awake while this runs.
    KeepAwake::Hold awake_;
    bool stopping_{false};
    int progress_value_{0};
    int progress_maximum_{0};
    loudness::LoudnessGroupingMode mode_{loudness::LoudnessGroupingMode::release};

    QFutureWatcher<Outcome> round_watcher_;
    core::CancellationSource cancellation_;
    std::shared_ptr<Progress> progress_;
    QTimer progress_timer_;
};

} // namespace trackknife::bench
