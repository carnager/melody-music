// SPDX-License-Identifier: GPL-3.0-only

#include "window_test.hpp"

namespace trackknife::bench::window_test {

void Fixture::initTestCase() {
    QVERIFY(home_directory_.isValid());
    qputenv("HOME", QFile::encodeName(home_directory_.path()));
    QVERIFY(settings_directory_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("TrackknifeTests"));
    QCoreApplication::setApplicationName(QStringLiteral("trackknife-tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_directory_.path());
    QStandardPaths::setTestModeEnabled(true);
    // ADR-0287: the engine keeps its state in the test's own folder, as the
    // window opens it: not the shared default, nor its socket.
    qputenv("MELODY_STATE_DIR",
            QFile::encodeName(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)));
    QDir{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}.removeRecursively();
}

// ADR-0226: every window runs against an engine, as the application does.
void Fixture::init() { QVERIFY2(engine_.start(), engine_.log().constData()); }

void Fixture::cleanup() {
    // A modifier left held turns the next case's clicks into Shift- or
    // Ctrl-clicks, so a case's outcome would hang on which case ran before
    // it. Released here, and the case that left it fails.
    const auto held = QGuiApplication::keyboardModifiers();
    if (held != Qt::NoModifier) {
        QWidget sink;
        releaseHeldModifiers(&sink);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    engine_.stop();
    QSettings settings;
    settings.clear();
    settings.sync();
    QDir{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}.removeRecursively();
    QDir{QStandardPaths::writableLocation(QStandardPaths::CacheLocation)}.removeRecursively();
    QCOMPARE(held, Qt::NoModifier);
}

} // namespace trackknife::bench::window_test
