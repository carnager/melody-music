// SPDX-License-Identifier: GPL-3.0-only

// The window test: the desktop: notifications, MPRIS, keeping the computer awake.

#include "window_test.hpp"

#include "uicommon/library_tree_view.hpp"
#include "workspace/artwork_session.hpp"
#include "workspace/sources.hpp"

#include <QGroupBox>
#include <QMessageBox>
#include <QPainter>
#include <QStandardItemModel>
#include <sqlite3.h>

namespace trackknife::bench {

class BenchMainWindowTest::Desktop final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void theDesktopNamesWhatPlaysAfterARestart();
    void desktopNotificationsNotifyBackgroundTrackChanges();
    void aNotificationWaitsForItsCover();
    void mprisOrderAndCoverFollowTheWorkspace();
    void longJobsKeepTheComputerAwake();
};

// The desktop -- notifications, media keys -- names what plays as the header
// does, by finding its entry in any open list: after a restart the list
// played from is not known until something plays, and it said only the file
// name.
void BenchMainWindowTest::Desktop::theDesktopNamesWhatPlaysAfterARestart() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr);
    LocalTrackRow row;
    row.raw_path = "/music/Someone/Album/01 file name.flac";
    row.title = "The Title";
    row.artist = "Someone";
    row.album = "Album";
    tab->model->appendRows({row});
    const auto entry = QString::fromStdString(tab->model->rows().front().entry_id.to_string());
    // Restarted: playing, but from no list this window knows of.
    window.workspace_.playback_.anchors = {};
    EnginePlayback::State engine;
    engine.status = QStringLiteral("playing");
    engine.entry = entry;
    engine.path = QStringLiteral("/music/Someone/Album/01 file name.flac");
    const auto state = window.workspace_.desktopStateOf(engine);
    QCOMPARE(state.title, QStringLiteral("The Title"));
    QCOMPARE(state.artist, QStringLiteral("Someone"));
    QCOMPARE(state.album, QStringLiteral("Album"));
    QCOMPARE(state.track_key, entry);
}

// ADR-0277: a long file job holds idle sleep off, one platform hold however
// many jobs run, let go with the last. Where logind answers, it lists the
// hold while it lasts.
void BenchMainWindowTest::Desktop::longJobsKeepTheComputerAwake() {
    const auto ours = [] {
        auto bus = QDBusConnection::systemBus();
        if (!bus.isConnected()) {
            return -1;
        }
        auto call = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
            QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("ListInhibitors"));
        const auto reply = bus.call(call, QDBus::Block, 2'000);
        if (reply.type() != QDBusMessage::ReplyMessage) {
            return -1;
        }
        const auto listed = reply.arguments().front().value<QDBusArgument>();
        int count = 0;
        listed.beginArray();
        while (!listed.atEnd()) {
            QString what;
            QString who;
            QString why;
            QString mode;
            uint uid = 0;
            uint pid = 0;
            listed.beginStructure();
            listed >> what >> who >> why >> mode >> uid >> pid;
            listed.endStructure();
            if (who == QStringLiteral("Melody") &&
                pid == static_cast<uint>(QCoreApplication::applicationPid())) {
                ++count;
            }
        }
        listed.endArray();
        return count;
    };
    QCOMPARE(KeepAwake::holds(), 0);
    auto first = KeepAwake::hold(QStringLiteral("Writing tags"));
    auto second = KeepAwake::hold(QStringLiteral("Measuring ReplayGain"));
    QCOMPARE(KeepAwake::holds(), 2);
    const bool logind = ours() >= 0;
    if (logind) {
        QTRY_COMPARE_WITH_TIMEOUT(ours(), 1, 3'000);
    }
    first.reset();
    QCOMPARE(KeepAwake::holds(), 1);
    if (logind) {
        QTest::qWait(200);
        QCOMPARE(ours(), 1);
    }
    second.reset();
    QCOMPARE(KeepAwake::holds(), 0);
    if (logind) {
        QTRY_COMPARE_WITH_TIMEOUT(ours(), 0, 3'000);
    }
}

// ADR-0144: notifications fire only for track changes while playing in
// the background, never retro-notify, and the menu toggle persists.
void BenchMainWindowTest::Desktop::desktopNotificationsNotifyBackgroundTrackChanges() {
    DesktopNotifier notifier;
    notifier.setBackgroundOnly(true);
    QStringList sent;
    notifier.setSendOverride([&sent](const QString& summary, const QString& body) {
        sent << summary + QStringLiteral("|") + body;
    });
    MprisPlaybackState playing;
    playing.status = QStringLiteral("Playing");
    playing.title = QStringLiteral("First Song");
    playing.artist = QStringLiteral("Artist");
    playing.album = QStringLiteral("Album");
    playing.track_key = QStringLiteral("key-1");
    // Disabled: tracked but silent; enabling never retro-notifies.
    QVERIFY(!notifier.publish(playing, false));
    notifier.setEnabled(true);
    QVERIFY(!notifier.publish(playing, false));
    // A background track change notifies with a markup-escaped body.
    playing.track_key = QStringLiteral("key-2");
    playing.title = QStringLiteral("Second Song");
    playing.artist = QStringLiteral("A & B");
    QVERIFY(notifier.publish(playing, false));
    QCOMPARE(notifier.sentCount(), 1ULL);
    QCOMPARE(sent.size(), 1);
#ifdef __APPLE__
    QCOMPARE(sent.front(), QStringLiteral("Second Song|A & B — Album"));
#else
    QCOMPARE(sent.front(), QStringLiteral("Second Song|A &amp; B — Album"));
#endif
    // A change while the window is active stays silent and is tracked.
    playing.track_key = QStringLiteral("key-3");
    QVERIFY(!notifier.publish(playing, true));
    QVERIFY(!notifier.publish(playing, false));
    // Pause never notifies; resuming into a new track does.
    playing.track_key = QStringLiteral("key-4");
    playing.status = QStringLiteral("Paused");
    QVERIFY(!notifier.publish(playing, false));
    playing.status = QStringLiteral("Playing");
    QVERIFY(notifier.publish(playing, false));
    QCOMPARE(notifier.sentCount(), 2ULL);
    notifier.setBackgroundOnly(false);
    playing.track_key = QStringLiteral("key-5");
    QVERIFY(notifier.publish(playing, true));
    QCOMPARE(notifier.sentCount(), 3ULL);
    QSignalSpy delivered(&notifier, &DesktopNotifier::deliveryFinished);
    notifier.setEnabled(false);
    notifier.sendTest();
    QCOMPARE(delivered.size(), 1);
    QVERIFY(delivered.front().front().toString().isEmpty());

    BenchMainWindow window;
    window.show();
    auto* action = window.findChild<QAction*>(QStringLiteral("action-desktop-notifications"));
    QVERIFY(action != nullptr);
    QVERIFY(action->isCheckable());
    QVERIFY(!action->isChecked());
    action->setChecked(true);
    QCOMPARE(QSettings{}.value(QStringLiteral("desktop/notifications"), false).toBool(), true);
    action->setChecked(false);
    QCOMPARE(QSettings{}.value(QStringLiteral("desktop/notifications"), true).toBool(), false);
}

// An album queued and played by another client -- melody-cli -- is in no
// list this window has shown, so its cover is not at hand when it starts.
// The notification waits for the cover rather than going out without it.
void BenchMainWindowTest::Desktop::aNotificationWaitsForItsCover() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    // A minute long, so it is still playing when the window looks, with its
    // cover beside it as an album folder has.
    const auto path = media.filePath(QStringLiteral("track.wav"));
    write_wave(path, wave_sample_rate * 60U);
    QImage cover{64, 64, QImage::Format_RGB32};
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(media.filePath(QStringLiteral("cover.jpg")), "JPEG"));

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    QVERIFY(window.notifier_ != nullptr);
    window.notifier_->setEnabled(true);
    window.notifier_->setBackgroundOnly(false);
    QStringList sent;
    window.notifier_->setSendOverride(
        [&sent](const QString& summary, const QString&) { sent << summary; });

    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = remote.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    const auto entry = core::StableId::random().to_string();
    auto entries = protocol::Json::array();
    entries.push_back(
        protocol::Json{{"entry", entry},
                       {"path", protocol::encode_raw_path(QFile::encodeName(path).toStdString())},
                       {"title", "With a cover"},
                       {"group", protocol::Json{{"album_artist", "Band"},
                                                {"artist", "Band"},
                                                {"album", "Covered"},
                                                {"date", "1992"}}}});
    QVERIFY((*other)
                ->call("playback.replace_queue", protocol::Json{{"entries", std::move(entries)}})
                .has_value());
    QVERIFY((*other)->call("playback.play", protocol::Json{{"entry", entry}}).has_value());

    QTRY_VERIFY_WITH_TIMEOUT(!sent.isEmpty(), 10'000);
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.front(), QStringLiteral("With a cover"));
    QVERIFY2(!window.notifier_->lastImage().isEmpty(), "the notification shows the cover");
    QVERIFY(QFileInfo::exists(window.notifier_->lastImage()));
}

// ADR-0135: MPRIS's play order is the workspace's modes -- repeat and single
// loop the track, repeat the list, random or album random shuffle -- both
// ways; and the playing album's cover is a file, written once per album.
void BenchMainWindowTest::Desktop::mprisOrderAndCoverFollowTheWorkspace() {
    BenchMainWindow window;
    auto& workspace = window.workspace_;
    workspace.setLoopStatus(QStringLiteral("Track"));
    QCOMPARE(workspace.loopStatus(), QStringLiteral("Track"));
    QVERIFY(workspace.modeTexts().repeat.checked && workspace.modeTexts().single.checked);
    workspace.setLoopStatus(QStringLiteral("Playlist"));
    QCOMPARE(workspace.loopStatus(), QStringLiteral("Playlist"));
    QVERIFY(!workspace.modeTexts().single.checked);
    workspace.setLoopStatus(QStringLiteral("None"));
    QCOMPARE(workspace.loopStatus(), QStringLiteral("None"));
    QVERIFY(!workspace.modeTexts().repeat.checked);
    workspace.setAlbumRandom(true);
    QVERIFY(workspace.shuffled());
    workspace.setShuffled(false);
    QVERIFY(!workspace.shuffled() && !workspace.modeTexts().album_random.checked);
    workspace.setShuffled(true);
    QVERIFY(workspace.shuffled() && workspace.modeTexts().random.checked);
    workspace.setShuffled(false);

    // The cover: a file per album, the last one gone when the album changes.
    QTRY_VERIFY(!window.list_tabs_.empty());
    auto& tab = *window.list_tabs_.front();
    const auto row = [](const char* album) {
        LocalTrackRow track;
        track.raw_path = std::string{"/covers/"} + album + ".flac";
        track.title = "Song";
        track.artist = "Band";
        track.album = album;
        track.probed = true;
        return track;
    };
    tab.model->replaceRows({row("First"), row("Second")});
    QImage red{40, 40, QImage::Format_RGB32};
    red.fill(Qt::red);
    tab.model->setArtwork(tab.model->groupKey(0), red);
    QImage blue{40, 40, QImage::Format_RGB32};
    blue.fill(Qt::blue);
    tab.model->setArtwork(tab.model->groupKey(1), blue);
    const auto first = workspace.desktopCoverPath(tab.model->rows()[0], EngineKey::local());
    QVERIFY(!first.isEmpty() && QFileInfo::exists(first));
    QCOMPARE(QImage{first}.pixelColor(0, 0), QColor{Qt::red});
    QCOMPARE(workspace.desktopCoverPath(tab.model->rows()[0], EngineKey::local()), first);
    const auto second = workspace.desktopCoverPath(tab.model->rows()[1], EngineKey::local());
    QVERIFY(second != first && QFileInfo::exists(second));
    QVERIFY(!QFileInfo::exists(first));
    QFile::remove(second);
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::Desktop)

#include "window_desktop_test.moc"
