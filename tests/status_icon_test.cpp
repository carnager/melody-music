// SPDX-License-Identifier: GPL-3.0-only

#include "status_icon.hpp"

#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QtTest>

namespace {
using trackknife::protocol::Json;
using trackknife::status::StatusIcon;

class FakeEngine final : public QLocalServer {
  public:
    FakeEngine() {
        connect(this, &QLocalServer::newConnection, this, [this] {
            while (auto* socket = nextPendingConnection()) {
                connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QLocalSocket::readyRead, socket, [this, socket] {
                    while (socket->canReadLine()) {
                        const auto request = Json::parse(socket->readLine().toStdString());
                        Json result = {{"name", "Test engine"}};
                        const auto method = request.at("method").get<std::string>();
                        if (method == "job.submit") {
                            ++scans;
                            last_job = request.at("params").value("job", "");
                            result = {{"job_id", "test-scan"}};
                        }
                        write(socket, {{"id", request.at("id")}, {"result", result}});
                        if (method == "job.submit" && finish_scan) {
                            QTimer::singleShot(50, socket, [this, socket] {
                                Json outcome = {{"visited", 12}, {"indexed", 3}, {"failed", 0}};
                                if (fail_scan) {
                                    outcome["incomplete"] = true;
                                }
                                write(socket,
                                      {{"event", "job.finished"},
                                       {"data", {{"job_id", "test-scan"}, {"outcome", outcome}}}});
                            });
                        }
                    }
                });
            }
        });
    }
    static void write(QLocalSocket* socket, const Json& document) {
        const auto line = document.dump() + "\n";
        socket->write(line.data(), static_cast<qint64>(line.size()));
        socket->flush();
    }
    int scans{0};
    std::string last_job;
    bool finish_scan{true};
    bool fail_scan{false};
};

class StatusIconTest final : public QObject {
    Q_OBJECT
  private slots:
    void serviceCommandsAreNotShellCommands() {
        const auto mac =
            trackknife::status::restartCommand(QStringLiteral("com.example.melodyd"), true, 501);
        QCOMPARE(mac.program, QStringLiteral("/bin/launchctl"));
        QCOMPARE(mac.arguments, (QStringList{"kickstart", "-k", "gui/501/com.example.melodyd"}));
        QVERIFY(trackknife::status::restartCommand({}, true, 501).program.isEmpty());
        const auto linux = trackknife::status::restartCommand({}, false, 1000);
        QCOMPARE(linux.program, QStringLiteral("systemctl"));
        QCOMPARE(linux.arguments, (QStringList{"--user", "restart", "--", "melodyd.service"}));
        const auto literal =
            trackknife::status::restartCommand(QStringLiteral("unit; touch /tmp/no"), false, 1000);
        QCOMPARE(literal.arguments.back(), QStringLiteral("unit; touch /tmp/no"));
    }

    void monochromeIconHasTransparencyAndAnOfflineState() {
        const auto running = trackknife::status::statusIcon(true, true);
        const auto offline = trackknife::status::statusIcon(false, true);
        QVERIFY(running.isMask());
        QVERIFY(!running.isNull());
        const auto on = running.pixmap(44, 44).toImage();
        const auto off = offline.pixmap(44, 44).toImage();
        int transparent = 0;
        int opaque = 0;
        for (int y = 0; y < on.height(); ++y) {
            for (int x = 0; x < on.width(); ++x) {
                const auto color = on.pixelColor(x, y);
                if (color.alpha() == 0) {
                    ++transparent;
                } else {
                    ++opaque;
                    QCOMPARE(color.red(), 0);
                    QCOMPARE(color.green(), 0);
                    QCOMPARE(color.blue(), 0);
                    QVERIFY(off.pixelColor(x, y).alpha() <= color.alpha());
                }
            }
        }
        QVERIFY(transparent > 0 && opaque > 0);
    }

    void statusRescanAndReconnect() {
        QTemporaryDir temporary(QStringLiteral("/tmp/melody-status-XXXXXX"));
        QVERIFY(temporary.isValid());
        const auto path = temporary.filePath(QStringLiteral("engine.sock"));
        FakeEngine engine;
        QVERIFY(engine.listen(path));
        StatusIcon icon({.socket = path.toStdString(), .host = {}, .port = 0, .token = {}}, {});
        auto* scan = icon.menu()->findChild<QAction*>(QStringLiteral("rescan-library"));
        auto* status = icon.menu()->findChild<QAction*>(QStringLiteral("daemon-status"));
        auto* detail = icon.menu()->findChild<QAction*>(QStringLiteral("status-detail"));
        QVERIFY(scan && status && detail);
        QTRY_VERIFY(scan->isEnabled());
        QVERIFY(status->text().contains(QStringLiteral("Test engine")));
        scan->trigger();
        QVERIFY(!scan->isEnabled());
        icon.rescan(); // A second click cannot start a second job.
        QTRY_COMPARE(engine.scans, 1);
        QCOMPARE(engine.last_job, std::string{"catalogue.scan"});
        QTRY_VERIFY(scan->isEnabled());
        QVERIFY(detail->text().contains(QStringLiteral("3 read, 12 visited")));
        engine.fail_scan = true;
        scan->trigger();
        QTRY_VERIFY(scan->isEnabled());
        QVERIFY(detail->text().contains(QStringLiteral("unavailable folders")));
        engine.close();
        icon.refresh();
        QTRY_VERIFY(!scan->isEnabled());
        QVERIFY(status->text().contains(QStringLiteral("not responding")));
        QLocalServer::removeServer(path);
        QVERIFY(engine.listen(path));
        icon.refresh();
        QTRY_VERIFY(scan->isEnabled());
    }

    void quittingTheIconDoesNotWaitForOrCancelTheScan() {
        QTemporaryDir temporary(QStringLiteral("/tmp/melody-status-XXXXXX"));
        QVERIFY(temporary.isValid());
        const auto path = temporary.filePath(QStringLiteral("engine.sock"));
        FakeEngine engine;
        engine.finish_scan = false;
        QVERIFY(engine.listen(path));
        auto icon = std::make_unique<StatusIcon>(
            trackknife::protocol::Endpoint{
                .socket = path.toStdString(), .host = {}, .port = 0, .token = {}},
            QString{});
        auto* scan = icon->menu()->findChild<QAction*>(QStringLiteral("rescan-library"));
        QTRY_VERIFY(scan->isEnabled());
        icon->rescan();
        QTRY_COMPARE(engine.scans, 1);
        QElapsedTimer elapsed;
        elapsed.start();
        icon.reset();
        QVERIFY(elapsed.elapsed() < 1500);
        QVERIFY(engine.isListening());
    }
};
} // namespace

QTEST_MAIN(StatusIconTest)
#include "status_icon_test.moc"
