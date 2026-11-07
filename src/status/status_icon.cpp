// SPDX-License-Identifier: GPL-3.0-only

#include "status_icon.hpp"

#include <QApplication>
#include <QPainter>
#include <QSvgRenderer>
#include <QtConcurrent/QtConcurrentRun>

#include <unistd.h>

namespace trackknife::status {
namespace {
#ifdef Q_OS_MACOS
constexpr bool mac = true;
#else
constexpr bool mac = false;
#endif
using protocol::Json;
} // namespace

RestartCommand restartCommand(const QString& service, const bool on_mac, const unsigned uid) {
    if (on_mac) {
        if (service.isEmpty()) {
            return {};
        }
        return {QStringLiteral("/bin/launchctl"),
                {QStringLiteral("kickstart"), QStringLiteral("-k"),
                 QStringLiteral("gui/%1/%2").arg(uid).arg(service)}};
    }
    return {QStringLiteral("systemctl"),
            {QStringLiteral("--user"), QStringLiteral("restart"), QStringLiteral("--"),
             service.isEmpty() ? QStringLiteral("melodyd.service") : service}};
}

QIcon statusIcon(const bool running, const bool monochrome) {
    QSvgRenderer svg(monochrome ? QStringLiteral(":/melody/status/melody-symbolic.svg")
                                : QStringLiteral(":/melody/status/melody.svg"));
    QIcon icon;
    for (const int size : {18, 22, 36, 44, 72}) {
        QPixmap pixels(size, size);
        pixels.fill(Qt::transparent);
        QPainter painter(&pixels);
        painter.setOpacity(running ? 1.0 : 0.35);
        svg.render(&painter);
        painter.end();
        icon.addPixmap(pixels);
    }
    icon.setIsMask(monochrome);
    return icon;
}

StatusIcon::StatusIcon(protocol::Endpoint endpoint, QString service, QObject* parent)
    : QObject(parent), endpoint_(std::move(endpoint)), service_(std::move(service)) {
    workers_.setMaxThreadCount(2);
    status_ = menu_.addAction(tr("Checking melodyd…"));
    status_->setObjectName(QStringLiteral("daemon-status"));
    status_->setEnabled(false);
    menu_.addSeparator();
    scan_action_ = menu_.addAction(tr("Rescan Library"));
    scan_action_->setObjectName(QStringLiteral("rescan-library"));
    restart_action_ = menu_.addAction(tr("Restart melodyd"));
    restart_action_->setObjectName(QStringLiteral("restart-daemon"));
    detail_ = menu_.addAction(QString{});
    detail_->setObjectName(QStringLiteral("status-detail"));
    detail_->setEnabled(false);
    detail_->setVisible(false);
    menu_.addSeparator();
    auto* quit = menu_.addAction(tr("Quit Status Icon"));
    quit->setToolTip(tr("Leave melodyd running"));
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);
    connect(scan_action_, &QAction::triggered, this, &StatusIcon::rescan);
    connect(restart_action_, &QAction::triggered, this, &StatusIcon::restart);
    tray_.setContextMenu(&menu_);
    tray_.setIcon(statusIcon(false, mac));
    tray_.setToolTip(tr("Checking melodyd…"));
    connect(&probe_, &QFutureWatcher<Snapshot>::finished, this, [this] {
        const auto result = probe_.result();
        running_ = result.running;
        const auto text = running_ ? tr("melodyd is running — %1").arg(result.name)
                                   : tr("melodyd is not responding");
        status_->setText(text);
        status_->setToolTip(result.error);
        tray_.setToolTip(text);
        tray_.setIcon(statusIcon(running_, mac));
        updateActions();
    });
    connect(&scan_, &QFutureWatcher<QString>::finished, this, [this] {
        const auto error = scan_.result();
        if (error.isEmpty()) {
            detail_->setText(tr("Scan complete: %1 read, %2 visited")
                                 .arg(scanning_->indexed.load())
                                 .arg(scanning_->visited.load()));
            detail_->setVisible(true);
        } else {
            fail(error);
        }
        updateActions();
    });
    connect(&restart_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exit) {
        if (code != 0 || exit != QProcess::NormalExit) {
            fail(tr("Restart failed: %1")
                     .arg(QString::fromUtf8(restart_.readAllStandardError()).trimmed()));
        } else {
            detail_->setText(tr("Restart requested"));
            detail_->setVisible(true);
        }
        refresh();
        updateActions();
    });
    connect(&restart_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        fail(tr("Restart failed: %1").arg(restart_.errorString()));
        updateActions();
    });
    connect(&timer_, &QTimer::timeout, this, [this] {
        if (scan_.isRunning()) {
            detail_->setText(tr("Scanning: %1 read, %2 visited")
                                 .arg(scanning_->indexed.load())
                                 .arg(scanning_->visited.load()));
            detail_->setVisible(true);
        }
        refresh();
    });
    timer_.setInterval(2000);
    timer_.start();
    updateActions();
    refresh();
}

StatusIcon::~StatusIcon() {
    timer_.stop();
    std::shared_ptr<protocol::Client> client;
    {
        const std::lock_guard lock(scanning_->mutex);
        scanning_->stopping = true;
        client = scanning_->client;
    }
    // Disconnect the observer, not the engine's scan. Never wait for the whole library.
    if (client) {
        client->close();
    }
    workers_.waitForDone();
}

void StatusIcon::show() { tray_.show(); }

void StatusIcon::refresh() {
    if (probe_.isRunning()) {
        return;
    }
    probe_.setFuture(QtConcurrent::run(&workers_, [endpoint = endpoint_] {
        Snapshot snapshot;
        auto client = protocol::Client::connect(endpoint);
        if (!client) {
            snapshot.error = QString::fromStdString(client.error().message);
            return snapshot;
        }
        const auto info = (*client)->call("engine.info", Json::object(), std::chrono::seconds{1});
        snapshot.running =
            info && info->is_object() && info->contains("name") && (*info)["name"].is_string();
        if (snapshot.running) {
            snapshot.name = QString::fromStdString((*info)["name"].get<std::string>());
        }
        return snapshot;
    }));
}

void StatusIcon::updateActions() {
    const bool restarting = restart_.state() != QProcess::NotRunning;
    scan_action_->setEnabled(running_ && !scan_.isRunning() && !restarting);
    scan_action_->setText(scan_.isRunning() ? tr("Rescanning Library…") : tr("Rescan Library"));
    const auto command = restartCommand(service_, mac, static_cast<unsigned>(::getuid()));
    restart_action_->setEnabled(!command.program.isEmpty() && !scan_.isRunning() && !restarting);
    restart_action_->setToolTip(
        command.program.isEmpty()
            ? tr("Start melody-status with --service followed by the melodyd LaunchAgent label")
            : tr("Restart the melodyd service; its playback may briefly pause"));
}

void StatusIcon::fail(const QString& message) {
    detail_->setText(message);
    detail_->setVisible(true);
    tray_.showMessage(tr("Melody engine"), message, QSystemTrayIcon::Warning);
}

void StatusIcon::restart() {
    if (!restart_action_->isEnabled()) {
        return;
    }
    const auto command = restartCommand(service_, mac, static_cast<unsigned>(::getuid()));
    restart_.start(command.program, command.arguments);
    updateActions();
}

void StatusIcon::rescan() {
    if (!scan_action_->isEnabled()) {
        return;
    }
    scanning_->visited.store(0);
    scanning_->indexed.store(0);
    scan_.setFuture(QtConcurrent::run(&workers_, [endpoint = endpoint_, state = scanning_] {
        auto connected = protocol::Client::connect(endpoint);
        if (!connected) {
            return QString::fromStdString(connected.error().message);
        }
        std::shared_ptr<protocol::Client> client = std::move(*connected);
        {
            const std::lock_guard lock(state->mutex);
            if (state->stopping) {
                return QString{};
            }
            state->client = client;
        }
        const auto outcome =
            client->run_job("catalogue.scan", Json::object(), [state](const Json& progress) {
                state->visited.store(progress.value("visited", std::uint64_t{0}));
                state->indexed.store(progress.value("indexed", std::uint64_t{0}));
            });
        {
            const std::lock_guard lock(state->mutex);
            state->client.reset();
        }
        if (!outcome) {
            return QString::fromStdString(outcome.error().message);
        }
        state->visited.store(outcome->value("visited", std::uint64_t{0}));
        state->indexed.store(outcome->value("indexed", std::uint64_t{0}));
        if (outcome->contains("error")) {
            return QString::fromStdString((*outcome)["error"].dump());
        }
        if (outcome->value("cancelled", false) || outcome->value("incomplete", false) ||
            outcome->value("failed", std::uint64_t{0}) != 0) {
            return tr("The scan finished with unavailable folders, failed files, or cancellation.");
        }
        return QString{};
    }));
    updateActions();
}
} // namespace trackknife::status
