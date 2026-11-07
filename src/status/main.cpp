// SPDX-License-Identifier: GPL-3.0-only

#include "status_icon.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QStandardPaths>

#include <iostream>

#ifdef Q_OS_MACOS
void melodyStatusMenuOnly();
#endif

int main(int argc, char** argv) {
#ifdef Q_OS_LINUX
    if (qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") &&
        qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        std::cerr << "melody-status: a graphical desktop login is required\n";
        return 1;
    }
#endif
    QApplication app(argc, argv);
#ifdef Q_OS_MACOS
    melodyStatusMenuOnly();
#endif
    app.setOrganizationName(QStringLiteral("melody"));
    app.setApplicationName(QStringLiteral("melody-status"));
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Menu-bar/system-tray status for a local melodyd"));
    parser.addHelpOption();
    const auto runtime = qEnvironmentVariable("XDG_RUNTIME_DIR", QDir::tempPath());
    parser.addOption({QStringLiteral("socket"), QStringLiteral("Local engine socket"),
                      QStringLiteral("path"),
                      QDir(runtime).filePath(QStringLiteral("melodyd.sock"))});
    parser.addOption({QStringLiteral("service"),
                      QStringLiteral("LaunchAgent label (macOS) or user unit (Linux) to restart"),
                      QStringLiteral("name")});
    parser.process(app);
    const auto path = QFile::encodeName(parser.value(QStringLiteral("socket")));
    const auto key = QCryptographicHash::hash(path, QCryptographicHash::Sha256).toHex();
    QLockFile lock(QDir(runtime).filePath(
        QStringLiteral("melody-status-%1.lock").arg(QString::fromLatin1(key))));
    if (!lock.tryLock(0)) {
        return 0;
    }
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        std::cerr << "melody-status: no system tray is available; run in a desktop login\n";
        return 1;
    }
    trackknife::protocol::Endpoint endpoint{
        .socket = path.toStdString(), .host = {}, .port = 0, .token = {}};
    trackknife::status::StatusIcon icon(std::move(endpoint),
                                        parser.value(QStringLiteral("service")));
    icon.show();
    return app.exec();
}
