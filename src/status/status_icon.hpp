// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/protocol/client.hpp"

#include <QAction>
#include <QFutureWatcher>
#include <QMenu>
#include <QProcess>
#include <QSystemTrayIcon>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <memory>
#include <mutex>

namespace trackknife::status {

struct RestartCommand {
    QString program;
    QStringList arguments;
};
// A service name is an argument, never shell text.
[[nodiscard]] RestartCommand restartCommand(const QString& service, bool mac, unsigned uid);
[[nodiscard]] QIcon statusIcon(bool running, bool monochrome);

class StatusIcon final : public QObject {
    Q_OBJECT
  public:
    StatusIcon(protocol::Endpoint endpoint, QString service, QObject* parent = nullptr);
    ~StatusIcon() override;
    void show();
    void refresh();
    void rescan();
    void restart();
    [[nodiscard]] QMenu* menu() { return &menu_; }

  private:
    struct Snapshot {
        bool running{false};
        QString name;
        QString error;
    };
    struct Scan {
        std::mutex mutex;
        std::shared_ptr<protocol::Client> client;
        bool stopping{false};
        std::atomic<std::uint64_t> visited{0};
        std::atomic<std::uint64_t> indexed{0};
    };
    void updateActions();
    void fail(const QString& message);

    protocol::Endpoint endpoint_;
    QString service_;
    QThreadPool workers_;
    QFutureWatcher<Snapshot> probe_;
    QFutureWatcher<QString> scan_;
    std::shared_ptr<Scan> scanning_{std::make_shared<Scan>()};
    QProcess restart_;
    QTimer timer_;
    QMenu menu_;
    QSystemTrayIcon tray_;
    QAction* status_;
    QAction* scan_action_;
    QAction* restart_action_;
    QAction* detail_;
    bool running_{false};
};
} // namespace trackknife::status
