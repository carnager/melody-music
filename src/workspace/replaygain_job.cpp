// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/replaygain_job.hpp"
#include "workspace/keep_awake.hpp"

#include "bench/metadata_dialog_helpers.hpp"
#include "trackknife/core/local_sources.hpp"

#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace trackknife::bench {
namespace {

// "1 track", "2 tracks".
[[nodiscard]] QString tracks(const long long count) {
    return count == 1 ? QStringLiteral("1 track") : QStringLiteral("%1 tracks").arg(count);
}

[[nodiscard]] QString problem_text(const loudness::ReplayGainProblem& problem) {
    return QStringLiteral("%1: %2").arg(
        problem.raw_path ? display_utf8(core::display_raw_path(*problem.raw_path))
                         : QStringLiteral("Album group"),
        display_utf8(problem.message));
}

} // namespace

ReplayGainJob::ReplayGainJob(const std::size_t item_count, ReplayGainItemReader items,
                             std::shared_ptr<engine::RemoteFileWork> work,
                             MetadataApplyObserver apply_observer, QObject* parent)
    : QObject(parent), item_count_(item_count), items_(std::move(items)), work_(std::move(work)),
      apply_observer_(std::move(apply_observer)),
      groups_{QStringLiteral("Preview groups before scanning to check album boundaries.")} {
    // The same persisted policies the tag editor exposes (ADR-0146/0148).
    const QSettings settings;
    grouping_ = settings.value(QStringLiteral("replaygain/grouping"), 0).toInt();
    expression_ = settings.value(QStringLiteral("replaygain/grouping-expression")).toString();
    sidecar_only_ = settings.value(QStringLiteral("replaygain/sidecar-only"), false).toBool();
    true_peak_ = settings.value(QStringLiteral("replaygain/true-peak"), false).toBool();
    status_ = QStringLiteral("%1 selected. Scan writes tags when measurement finishes.")
                  .arg(tracks(static_cast<long long>(item_count_)));
    progress_timer_.setInterval(100);
    connect(&progress_timer_, &QTimer::timeout, this, &ReplayGainJob::showProgress);
    connect(&round_watcher_, &QFutureWatcherBase::finished, this, &ReplayGainJob::finishRound);
}

ReplayGainJob::~ReplayGainJob() {
    cancellation_.request_cancellation();
    round_watcher_.waitForFinished();
}

QStringList ReplayGainJob::groupings() {
    return {QStringLiteral("Albums by release tags"),
            QStringLiteral("Albums — combine disc editions"),
            QStringLiteral("Selection as one album"), QStringLiteral("Track gain only"),
            QStringLiteral("Custom album grouping…")};
}

QString ReplayGainJob::title() const {
    return QStringLiteral("ReplayGain %1 track%2")
        .arg(item_count_)
        .arg(item_count_ == 1U ? QString{} : QStringLiteral("s"));
}

void ReplayGainJob::groupsChanged() {
    groups_ =
        QStringList{QStringLiteral("Grouping changed — preview again to check album boundaries.")};
    emit changed();
}

void ReplayGainJob::setGrouping(const int index) {
    if (grouping_ != index) {
        grouping_ = index;
        groupsChanged();
    }
}

void ReplayGainJob::setExpression(const QString& expression) {
    if (expression_ != expression) {
        expression_ = expression;
        groupsChanged();
    }
}

void ReplayGainJob::setSidecarOnly(const bool on) {
    sidecar_only_ = on;
    emit changed();
}

void ReplayGainJob::setTruePeak(const bool on) {
    true_peak_ = on;
    emit changed();
}

void ReplayGainJob::setStatusText(const QString& text) {
    status_ = text;
    emit changed();
}

void ReplayGainJob::setRunning(const bool running) {
    running_ = running;
    awake_ = running ? KeepAwake::hold(QStringLiteral("Measuring ReplayGain")) : nullptr;
    stopping_ = false;
    if (!running) {
        progress_timer_.stop();
    }
    emit changed();
}

void ReplayGainJob::preview() { start(true); }

void ReplayGainJob::run() { start(false); }

void ReplayGainJob::stop() {
    if (!running_) {
        return;
    }
    cancellation_.request_cancellation();
    stopping_ = true;
    progress_timer_.stop();
    setStatusText(QStringLiteral("Stopping… Any completed tag writes are retained."));
}

void ReplayGainJob::start(const bool preview) {
    if (running_ || item_count_ == 0U || !items_ || !work_) {
        return;
    }
    engine::ReplayGainRequest request;
    switch (grouping_) {
    case 0:
        request.grouping.mode = loudness::LoudnessGroupingMode::release;
        break;
    case 1:
        request.grouping.mode = loudness::LoudnessGroupingMode::release_merged_discs;
        break;
    case 2:
        request.grouping.mode = loudness::LoudnessGroupingMode::selection_album;
        break;
    case 3:
        request.grouping.mode = loudness::LoudnessGroupingMode::track;
        break;
    default:
        request.grouping.mode = loudness::LoudnessGroupingMode::format_expression;
        request.grouping.expression = expression_.trimmed().toStdString();
        if (request.grouping.expression.empty()) {
            setStatusText(QStringLiteral("Enter a tkfmt-1 grouping expression, e.g. %album%"));
            return;
        }
        break;
    }
    request.sidecar_only = sidecar_only_;
    request.true_peak = true_peak_;
    request.preview = preview;
    request.items = items_();
    if (request.items.empty()) {
        setStatusText(QStringLiteral("Nothing to scan."));
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("replaygain/grouping"), grouping_);
    settings.setValue(QStringLiteral("replaygain/grouping-expression"), expression_);
    settings.setValue(QStringLiteral("replaygain/sidecar-only"), sidecar_only_);
    settings.setValue(QStringLiteral("replaygain/true-peak"), true_peak_);

    preview_only_ = preview;
    mode_ = request.grouping.mode;
    problems_.clear();
    cancellation_ = core::CancellationSource{};
    progress_ = std::make_shared<Progress>();
    status_ = QStringLiteral("Reading the selection…");
    progress_maximum_ = 0;
    progress_value_ = 0;
    setRunning(true);
    progress_timer_.start();
    round_watcher_.setFuture(
        QtConcurrent::run([work = work_, request = std::move(request), progress = progress_,
                           token = cancellation_.token()]() -> Outcome {
            return std::make_shared<core::Result<engine::ReplayGainRound>>(work->replaygain(
                request,
                [&progress](const engine::ReplayGainStep& step) {
                    progress->phase.store(step.phase == "writing"     ? 2
                                          : step.phase == "measuring" ? 1
                                                                      : 0);
                    progress->total.store(step.total);
                    progress->completed.store(step.completed);
                },
                token));
        }));
}

void ReplayGainJob::showProgress() {
    if (!progress_ || stopping_) {
        return;
    }
    const auto total = progress_->total.load();
    const auto completed = progress_->completed.load();
    switch (progress_->phase.load()) {
    case 0:
        progress_maximum_ = 0;
        status_ = total > 0U
                      ? QStringLiteral("Reading tags · %L1 of %L2 files…").arg(completed).arg(total)
                      : QStringLiteral("Reading the selection…");
        break;
    case 1:
        progress_maximum_ = static_cast<int>(total);
        progress_value_ = static_cast<int>(completed);
        status_ = QStringLiteral("Measuring loudness · %1 of %2 files…").arg(completed).arg(total);
        break;
    default:
        progress_maximum_ = 0;
        status_ = QStringLiteral("Writing ReplayGain tags… Audio samples are not changed.");
        break;
    }
    emit changed();
}

void ReplayGainJob::finishRound() {
    progress_timer_.stop();
    const auto outcome = round_watcher_.result();
    const auto stopped = cancellation_.is_cancellation_requested();
    setRunning(false);
    if (!outcome || !*outcome) {
        setStatusText(stopped || (outcome && outcome->error().code == core::ErrorCode::cancelled)
                          ? QStringLiteral("Stopped. No tags written.")
                      : outcome ? display_utf8(outcome->error().message)
                                : QStringLiteral("ReplayGain returned no result"));
        return;
    }
    const auto& round = **outcome;
    QStringList groups;
    for (const auto& group : round.groups) {
        const auto name = mode_ == loudness::LoudnessGroupingMode::selection_album
                              ? QStringLiteral("Selection as one album")
                              : (group.album_artist.empty() ? QStringLiteral("Unknown artist")
                                                            : display_utf8(group.album_artist)) +
                                    QStringLiteral(" — ") +
                                    (group.album.empty() ? QStringLiteral("Untitled album")
                                                         : display_utf8(group.album));
        groups << QStringLiteral("%1 · %2").arg(name, tracks(static_cast<long long>(group.tracks)));
    }
    if (round.more_groups > 0U) {
        groups << QStringLiteral("… %1 more album groups").arg(round.more_groups);
    }
    if (round.track_only > 0U) {
        groups << QStringLiteral("%1 without album grouping — track gain only")
                      .arg(tracks(static_cast<long long>(round.track_only)));
    }
    groups_ = groups;
    QStringList problems;
    for (const auto& problem : round.unreadable) {
        problems << problem_text(problem) + QStringLiteral(" (not read, left out)");
    }
    for (const auto& problem : round.problems) {
        problems << problem_text(problem);
    }
    problems_ = problems;
    const auto readable = item_count_ - std::min(item_count_, round.unreadable.size());
    if (preview_only_) {
        setStatusText(readable == 0U ? QStringLiteral("Nothing to scan.")
                                     : QStringLiteral("%1 ready. Scan and write tags to measure "
                                                      "loudness and save the results.")
                                           .arg(tracks(static_cast<long long>(readable))));
        return;
    }
    if (round.measured_items == 0U) {
        setStatusText(QStringLiteral("Nothing to scan."));
        return;
    }
    if (round.refused) {
        setStatusText(QStringLiteral("Nothing written: the write plan is blocked."));
        return;
    }
    if (!round.applied) {
        setStatusText(QStringLiteral("Nothing measurable to write."));
        return;
    }
    if (apply_observer_) {
        apply_observer_(*round.applied);
    }
    auto text = QStringLiteral("Saved ReplayGain tags to %1 target%2. Audio samples unchanged.")
                    .arg(round.written)
                    .arg(round.written == 1U ? QString{} : QStringLiteral("s"));
    if (round.failed > 0U) {
        text += QStringLiteral(" %1 failed.").arg(round.failed);
    }
    setStatusText(text);
}

} // namespace trackknife::bench
