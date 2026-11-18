// SPDX-License-Identifier: GPL-3.0-only

// The window test: playback: the playing row, modes, ratings, outputs, quitting.

#include "window_test.hpp"

#include "trackknife/core/engine_state.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "uicommon/library_tree_view.hpp"
#include "workspace/artwork_session.hpp"
#include "workspace/sources.hpp"
#include "workspace/startup.hpp"
#include "workspace/tab_store.hpp"

#include <QCheckBox>
#include <QGroupBox>
#include <QMessageBox>
#include <QPainter>
#include <QStandardItemModel>
#include <sqlite3.h>

namespace trackknife::bench {

class BenchMainWindowTest::Playback final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void thePlayingRowFollowsItsList();
    void followPlaybackAndJumpRespectBrowsing();
    void quittingStopsTheEngineForGood();
    void theHeartMakesWhatPlaysAFavourite();
    void playbackBufferProfilesPersistAndExposeDiagnostics();
    void theDeviceMenuChoosesAnOutputAgent();
    void muteRestoresLocalVolumeAcrossBrowsing();
    void autoAdvancesOncePerFinishedTrack();
    void localPlaybackModesPersistAndStayLocal();
    void localPlaybackModesAdvance_data();
    void localPlaybackModesAdvance();
    void localTrackRatingsPersistByContentIdentity();
};

// The row of what plays is looked for where it was last found, several
// times a refresh: wherever the list has moved it since, or edited it, it is
// that entry's row -- and once it has left the list, none.
void BenchMainWindowTest::Playback::thePlayingRowFollowsItsList() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr);
    std::vector<LocalTrackRow> rows;
    for (const auto* title : {"One", "Two", "Three"}) {
        LocalTrackRow row;
        row.raw_path = std::string{"/music/"} + title + ".flac";
        row.title = title;
        rows.push_back(std::move(row));
    }
    tab->model->appendRows(std::move(rows));
    QCOMPARE(tab->model->rows().size(), 3U);
    auto& workspace = window.workspace_;
    workspace.playback_.anchors.document = tab->document.id;
    const auto playing = tab->model->rows()[2].entry_id;
    const auto entry = QString::fromStdString(playing.to_string());
    const auto* found = workspace.playingRow(entry);
    QVERIFY(found != nullptr);
    QCOMPARE(found->title, std::string{"Three"});
    QCOMPARE(workspace.playingRow(entry), found);

    // Moved to the top: found there, not at the row it was.
    QVERIFY(tab->model->applyPermutation({2, 0, 1}, QStringLiteral("Move")));
    found = workspace.playingRow(entry);
    QVERIFY(found != nullptr);
    QCOMPARE(found->entry_id, playing);
    QCOMPARE(found, &tab->model->rows()[0]);
    QCOMPARE(workspace.playingRow(entry), found);

    // Another entry now where it was found: not taken for it.
    QVERIFY(tab->model->applyPermutation({1, 2, 0}, QStringLiteral("Move")));
    found = workspace.playingRow(entry);
    QVERIFY(found != nullptr);
    QCOMPARE(found->entry_id, playing);
    QCOMPARE(found->title, std::string{"Three"});

    // Gone from every list: none.
    const auto at = static_cast<int>(found - tab->model->rows().data());
    tab->model->removeRowIndexes({at});
    QCOMPARE(workspace.playingRow(entry), nullptr);
}

// Quit stops this computer's engine -- and it stays stopped. The window's
// engine connection starts its engine again when it goes away, every few
// seconds, and the event loop ran on for a moment after Quit: the engine
// came back, and the music with it.
void BenchMainWindowTest::Playback::quittingStopsTheEngineForGood() {
    // The window's own engine, as the application starts it, in place of
    // the one every other case runs against: same data directory, a socket
    // of its own. cleanup() clears the settings again.
    engine_.stop();
    QSettings settings;
    settings.remove(QLatin1String(SettingsDialog::library_local_engine_socket_key));
    saveRemoteEngines({});
    settings.sync();
    const auto program = qgetenv("MELODY_ENGINE");
    qputenv("MELODY_ENGINE", MELODY_ENGINE_BINARY);
    allowLocalEngine(true);
    const auto engine = localEngine();
    QVERIFY(engine.has_value());
    const auto restore = qScopeGuard([&] {
        static_cast<void>(stopLocalEngine(*engine));
        allowLocalEngine(false);
        qputenv("MELODY_ENGINE", program);
    });

    qputenv("MELODY_ENGINE_RETRY_MS", "200");
    const auto retry = qScopeGuard([] { qunsetenv("MELODY_ENGINE_RETRY_MS"); });
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY2_WITH_TIMEOUT(window.localPlayback() != nullptr && window.localPlayback()->active(),
                              qPrintable(window.localCatalogue()
                                             ? window.localCatalogue()->describe()
                                             : QStringLiteral("no source")),
                              15'000);
    QVERIFY(!lockHolders(engine->state / "engine.lock").empty());

    window.quitAndStopEngine();
    QVERIFY(lockHolders(engine->state / "engine.lock").empty());
    // The loop turns on past several of the reconnect timer's ticks, as it
    // does while the process ends.
    QTest::qWait(1'000);
    QVERIFY2(lockHolders(engine->state / "engine.lock").empty(),
             "quitting must not leave anything that starts the engine again");
}

void BenchMainWindowTest::Playback::followPlaybackAndJumpRespectBrowsing() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(!window.list_tabs_.empty());
    auto& playing = *window.list_tabs_.front();
    LocalTrackRow row;
    row.raw_path = "/missing/cursor.flac";
    row.title = "Cursor track";
    row.probed = true;
    playing.model->replaceRows({row, row});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    tabs->setCurrentWidget(playing.view);
    window.playback_.anchors.document = playing.document.id;
    window.playback_.anchors.current = playing.model->rows().at(0).entry_id;
    window.playback_.row = 0;
    auto* follow = window.findChild<QAction*>(QStringLiteral("action-follow-playback"));
    auto* jump = window.findChild<QAction*>(QStringLiteral("action-jump-to-playing"));
    QVERIFY(follow && jump);
    follow->setChecked(true);
    QCOMPARE(playing.view->currentIndex().row(), 0);
    playing.view->selectRow(1);
    window.refreshPlaybackCursor();
    QCOMPARE(playing.view->currentIndex().row(), 1); // No reselection on every timer tick.
    window.playback_.anchors.current = playing.model->rows().at(1).entry_id;
    window.playback_.row = 1;
    playing.view->selectRow(0);
    window.refreshPlaybackCursor();
    QCOMPARE(playing.view->currentIndex().row(), 1);
    auto other = playing.document;
    other.id = core::StableId::random();
    other.name = "Browsing";
    auto* browsing = window.addListTab(std::move(other), true);
    QVERIFY(browsing);
    window.playback_.anchors.document = playing.document.id;
    window.playback_.anchors.current = playing.model->rows().at(0).entry_id;
    window.playback_.row = 0;
    window.refreshPlaybackCursor();
    QCOMPARE(tabs->currentWidget(), browsing->view);
    follow->setChecked(false);
    jump->trigger();
    QCOMPARE(tabs->currentWidget(), playing.view);
    QCOMPARE(playing.view->currentIndex().row(), 0);
    QCOMPARE(jump->shortcut(), QKeySequence(QStringLiteral("Ctrl+J")));

    QVERIFY(!QSettings{}.value(QStringLiteral("workspace/follow-playback")).toBool());
}

// ADR-0226: the buffer is the engine's. The window shows what the engine
// reports, and a restarted window finds the engine's choice, not its own.
void BenchMainWindowTest::Playback::playbackBufferProfilesPersistAndExposeDiagnostics() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.playingOnEngine());

    auto* menu = window.findChild<QMenu*>(QStringLiteral("bench-buffer-menu"));
    auto* responsive = window.findChild<QAction*>(QStringLiteral("action-buffer-responsive"));
    auto* balanced = window.findChild<QAction*>(QStringLiteral("action-buffer-balanced"));
    auto* resilient = window.findChild<QAction*>(QStringLiteral("action-buffer-resilient"));
    auto* custom = window.findChild<QAction*>(QStringLiteral("action-buffer-custom"));
    auto* device = window.findChild<QToolButton*>(QStringLiteral("bench-device"));
    QVERIFY(menu != nullptr);
    QVERIFY(responsive != nullptr);
    QVERIFY(balanced != nullptr);
    QVERIFY(resilient != nullptr);
    QVERIFY(custom != nullptr);
    QVERIFY(device != nullptr);
    // An engine that has never been told starts balanced.
    QTRY_VERIFY(balanced->isChecked());
    responsive->trigger();
    QTRY_VERIFY(responsive->isChecked());
    QVERIFY(!balanced->isChecked());
    QCOMPARE(responsive->toolTip(), QStringLiteral("250 ms capacity; playback starts at 50 ms"));
    QCOMPARE(custom->text(), QStringLiteral("Custom…"));
    QTRY_COMPARE(window.property("trackknife-player-buffer-capacity-ms").toLongLong(), 250);
    QVERIFY(!window.property("trackknife-player-buffer-pending").toBool());
    QCOMPARE(window.property("trackknife-player-underruns").toULongLong(), 0ULL);

    resilient->trigger();
    QVERIFY(resilient->isChecked());
    QTRY_COMPARE(window.property("trackknife-player-buffer-capacity-ms").toLongLong(), 2'000);
    QTRY_VERIFY(device->toolTip().contains(QStringLiteral("Resilient")));
    QVERIFY(device->toolTip().contains(QStringLiteral("Underruns: 0")));

    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-profile")).toString(),
             QStringLiteral("resilient"));
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-capacity-ms")).toInt(), 2'000);
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-start-threshold-ms")).toInt(), 250);

    custom->trigger();
    auto* dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    auto* profile = dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-buffer-profile"));
    QCOMPARE(profile->currentData().toString(), QStringLiteral("custom"));
    auto* capacity = dialog->findChild<QSpinBox*>(QStringLiteral("bench-settings-buffer-capacity"));
    auto* threshold =
        dialog->findChild<QSpinBox*>(QStringLiteral("bench-settings-buffer-threshold"));
    QVERIFY(capacity && threshold);
    capacity->setValue(1234);
    threshold->setValue(234);
    dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    QVERIFY(custom->isChecked());
    QTRY_COMPARE(window.property("trackknife-player-buffer-capacity-ms").toLongLong(), 1'234);
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-profile")).toString(),
             QStringLiteral("custom"));
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-capacity-ms")).toInt(), 1'234);
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-start-threshold-ms")).toInt(), 234);
    QVERIFY(window.close());

    BenchMainWindow restored;
    restored.show();
    auto* restored_custom = restored.findChild<QAction*>(QStringLiteral("action-buffer-custom"));
    QVERIFY(restored_custom != nullptr);
    QVERIFY(restored_custom->isChecked());
    QTRY_COMPARE(restored.property("trackknife-player-buffer-capacity-ms").toLongLong(), 1'234);
}

// ADR-0297: the heart by what plays -- into the Favorites playlist of the
// engine playing it, and out again; shown only while something plays.
void BenchMainWindowTest::Playback::theHeartMakesWhatPlaysAFavourite() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music));
    const auto file = music + QStringLiteral("/art.flac");
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), file));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    QVERIFY(window.localCatalogue()->open()->add_root(QFile::encodeName(music).toStdString()));
    // Told here what plays; the engine, in this test, plays nothing.
    QTest::qWait(500);
    window.transport_timer_->stop();
    auto* heart = window.findChild<QToolButton*>(QStringLiteral("bench-favorite"));
    auto* action = window.findChild<QAction*>(QStringLiteral("action-favorite"));
    QVERIFY(heart != nullptr && action != nullptr);
    QVERIFY(heart->isHidden());
    EnginePlayback::State state;
    state.status = QStringLiteral("playing");
    state.entry = QStringLiteral("e1");
    state.path = file;
    window.refreshFavorite(state);
    QVERIFY(!heart->isHidden());
    QCOMPARE(heart->text(), QStringLiteral("♡"));
    action->trigger();
    QTRY_COMPARE(heart->text(), QStringLiteral("♥"));
    QVERIFY(action->isChecked());
    // The engine's: a Favorites playlist with the track.
    bool asked = false;
    window.localPlayback()->request(
        QStringLiteral("favorites.state"),
        protocol::Json{{"paths", protocol::Json::array({protocol::encode_raw_path(
                                     QFile::encodeName(file).toStdString())})}},
        [&asked](core::Result<protocol::Json> answer) {
            asked = answer && answer->at("favorites").front().get<bool>();
        });
    QTRY_VERIFY(asked);
    // Asked again for the same track, after another: still a favourite.
    state.entry = QStringLiteral("e2");
    state.path = music + QStringLiteral("/other.flac");
    window.refreshFavorite(state);
    QTRY_COMPARE(heart->text(), QStringLiteral("♡"));
    state.entry = QStringLiteral("e1");
    state.path = file;
    window.refreshFavorite(state);
    QTRY_COMPARE(heart->text(), QStringLiteral("♥"));
    // And out again.
    action->trigger();
    QTRY_COMPARE(heart->text(), QStringLiteral("♡"));
    // Nothing playing: no heart.
    state.status = QStringLiteral("stopped");
    window.refreshFavorite(state);
    QVERIFY(heart->isHidden());
}

void BenchMainWindowTest::Playback::muteRestoresLocalVolumeAcrossBrowsing() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(!window.list_tabs_.empty());
    QTRY_VERIFY(window.playingOnEngine());
    auto* local = window.list_tabs_.front()->view;
    window.tabs_->setCurrentWidget(local);
    // The engine's volume: what the slider shows is what the engine reports.
    const auto engine_volume = [&window] { return window.localPlayback()->state().volume_percent; };
    window.volume_->setValue(37);
    QTRY_COMPARE(engine_volume(), 37);
    QTest::mouseClick(window.mute_button_, Qt::LeftButton);
    QTRY_COMPARE(engine_volume(), 0);
    QVERIFY(window.mute_button_->isChecked());
    QVERIFY(window.mute_button_->isEnabled());
    QTest::mouseClick(window.mute_button_, Qt::LeftButton);
    QTRY_COMPARE(engine_volume(), 37);
    QVERIFY(!window.mute_button_->isChecked());
    window.volume_->setValue(0);
    QTRY_COMPARE(engine_volume(), 0);
    QVERIFY(window.mute_button_->isChecked());
    window.volume_->setValue(21);
    QTRY_COMPARE(engine_volume(), 21);
    QVERIFY(!window.mute_button_->isChecked());
}

// ADR-0228: an output agent registered with the engine is offered in the
// speaker menu, and choosing it plays there. A real melody-agent process, on
// the engine's socket.
void BenchMainWindowTest::Playback::theDeviceMenuChoosesAnOutputAgent() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto* menu = window.findChild<QMenu*>(QStringLiteral("bench-device-menu"));
    QVERIFY(menu != nullptr);
    const auto output_action = [menu](const QString& id) {
        return menu->findChild<QAction*>(QStringLiteral("action-output-%1").arg(id));
    };
    // Nothing to choose between yet, so no choice is offered.
    QVERIFY(output_action(QStringLiteral("agent:bedside")) == nullptr);

    QProcess agent;
    agent.setProgram(QStringLiteral(MELODY_AGENT_BINARY));
    agent.setArguments({QStringLiteral("--server"), engine_.socket(), QStringLiteral("--name"),
                        QStringLiteral("bedside")});
    agent.setProcessChannelMode(QProcess::MergedChannels);
    agent.start();
    QVERIFY(agent.waitForStarted());
    const auto stop_agent = qScopeGuard([&agent] {
        agent.terminate();
        if (!agent.waitForFinished(5'000)) {
            agent.kill();
            agent.waitForFinished();
        }
    });
    QTRY_VERIFY_WITH_TIMEOUT(output_action(QStringLiteral("agent:bedside")) != nullptr ||
                                 agent.state() != QProcess::Running,
                             10'000);
    if (output_action(QStringLiteral("agent:bedside")) == nullptr) {
        QSKIP("melody-agent has no audio output here");
    }
    auto* bedside = output_action(QStringLiteral("agent:bedside"));
    QVERIFY(bedside->isEnabled() && !bedside->isChecked());
    QVERIFY(!bedside->text().contains(QStringLiteral("offline")));

    bedside->trigger();
    QTRY_COMPARE(window.property("trackknife-player-output").toString(),
                 QStringLiteral("agent:bedside"));
    QTRY_VERIFY(output_action(QStringLiteral("agent:bedside")) != nullptr &&
                output_action(QStringLiteral("agent:bedside"))->isChecked());
    auto* button = window.findChild<QToolButton*>(QStringLiteral("bench-device"));
    QVERIFY(button != nullptr);
    // Music in another room is named where it can be seen.
    QCOMPARE(button->text(), QStringLiteral("bedside"));
    // Named, the pill ends in a chevron: it opens a choice.
    auto* chevron = button->findChild<QLabel*>(QStringLiteral("bench-device-chevron"));
    QVERIFY(chevron != nullptr && chevron->isVisible());
    // And the menu says which list is which: speakers, then the chosen
    // speaker's devices -- in any style, not only those that draw sections.
    {
        QStringList headings;
        for (const auto* heading :
             menu->findChildren<QLabel*>(QStringLiteral("bench-device-menu-heading"))) {
            headings << heading->text();
        }
        QVERIFY2(headings.contains(QStringLiteral("Speakers")), qPrintable(headings.join(u'|')));
        QVERIFY2(headings.contains(QStringLiteral("Sound device on bedside")),
                 qPrintable(headings.join(u'|')));
    }

    // Gone, it stays chosen and says so.
    agent.terminate();
    QVERIFY(agent.waitForFinished(5'000));
    QTRY_VERIFY_WITH_TIMEOUT(output_action(QStringLiteral("agent:bedside")) != nullptr &&
                                 output_action(QStringLiteral("agent:bedside"))
                                     ->text()
                                     .contains(QStringLiteral("offline")),
                             10'000);
    QVERIFY(output_action(QStringLiteral("agent:bedside"))->isChecked());

    // And this computer again, if the engine has audio of its own here.
    if (auto* local = output_action(QStringLiteral("local")); local != nullptr) {
        // Read before choosing it: choosing rebuilds the menu.
        const auto own_name = local->text();
        local->trigger();
        QTRY_COMPARE(window.property("trackknife-player-output").toString(),
                     QStringLiteral("local"));
        // With an agent to choose, where it plays stays named -- by the
        // engine's name for its machine, as other engines list it too.
        QVERIFY(!own_name.isEmpty() && own_name != QStringLiteral("This computer"));
        QCOMPARE(button->text(), own_name);
    }
}

// Regression for the runaway auto-advance: the player's "ended" state names a
// finished *file*, not a finished list, and persists for several transport
// ticks while the next source loads. Each finished track must advance the
// list exactly one row, and the last row must stay ended without wrapping
// (ADR-0023).
void BenchMainWindowTest::Playback::localPlaybackModesPersistAndStayLocal() {
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_VERIFY(tabs->count() >= 1);
        tabs->setCurrentIndex(0);
        auto* repeat = window.findChild<QAction*>(QStringLiteral("action-local-repeat"));
        auto* random = window.findChild<QAction*>(QStringLiteral("action-local-random"));
        auto* single = window.findChild<QAction*>(QStringLiteral("action-local-single"));
        auto* consume = window.findChild<QAction*>(QStringLiteral("action-local-consume"));
        auto* automatic =
            window.findChild<QAction*>(QStringLiteral("action-local-replaygain-auto"));
        auto* rg = window.findChild<QToolButton*>(QStringLiteral("bench-local-replaygain"));
        QVERIFY(repeat && random && single && consume && automatic && rg);
        for (const auto* name : {"bench-local-album-random"}) {
            auto* album_mode = window.findChild<QToolButton*>(QString::fromLatin1(name));
            QVERIFY(album_mode);
            QCOMPARE(album_mode->toolButtonStyle(), Qt::ToolButtonIconOnly);
            QVERIFY(!album_mode->icon().isNull());
            QVERIFY(album_mode->defaultAction()->isCheckable());
            QCOMPARE(album_mode->defaultAction()->text(), QStringLiteral("Album shuffle"));
        }
        QVERIFY(rg->isVisible());
        QVERIFY(!repeat->isChecked());
        repeat->trigger();
        automatic->trigger();
        QTRY_COMPARE(window.property("trackknife-player-replaygain").toInt(), 2);
        random->trigger();
        QTRY_COMPARE(window.property("trackknife-player-replaygain").toInt(), 1);
        QCOMPARE(rg->text(), QStringLiteral("ReplayGain: Automatic"));
        // Icons, not letters, with one-shot marked on the icon itself.
        QVERIFY(!single->icon().isNull() && !consume->icon().isNull());
        const auto plain_single = single->icon().pixmap(22, 22).toImage();
        single->trigger();
        single->trigger();
        QCOMPARE(single->iconText(), QStringLiteral("1×"));
        QVERIFY(single->icon().pixmap(22, 22).toImage() != plain_single);
        consume->trigger();
        consume->trigger();
        QCOMPARE(consume->iconText(), QStringLiteral("C×"));
        QVERIFY(rg->isVisible());
        QVERIFY(repeat->isChecked());
        QVERIFY(random->isChecked());
        QVERIFY(automatic->isChecked());
        // ADR-0138: the preamp dialog is reachable from the ReplayGain menu.
        QVERIFY(window.findChild<QAction*>(QStringLiteral("action-local-replaygain-preamp")) !=
                nullptr);
    }
    BenchMainWindow restored;
    restored.show();
    auto* repeat = restored.findChild<QAction*>(QStringLiteral("action-local-repeat"));
    auto* random = restored.findChild<QAction*>(QStringLiteral("action-local-random"));
    auto* single = restored.findChild<QAction*>(QStringLiteral("action-local-single"));
    auto* consume = restored.findChild<QAction*>(QStringLiteral("action-local-consume"));
    QVERIFY(repeat && random && single && consume);
    QVERIFY(repeat->isChecked());
    QVERIFY(random->isChecked());
    QCOMPARE(single->iconText(), QStringLiteral("1×"));
    QCOMPARE(consume->iconText(), QStringLiteral("C×"));
    QTRY_COMPARE(restored.property("trackknife-player-replaygain").toInt(), 1);
    // ADR-0325: preamps changed in Settings reach the playing engine; a
    // window starting sends none of its own.
    {
        QSettings settings;
        settings.setValue(QStringLiteral("playback/rg-preamp-with"), 4.5);
        settings.setValue(QStringLiteral("playback/rg-preamp-without"), -3.0);
    }
    restored.workspace_.reloadPlaybackPreferences();
    QTRY_COMPARE(restored.property("trackknife-player-rg-preamp-with").toDouble(), 4.5);
    QTRY_COMPARE(restored.property("trackknife-player-rg-preamp-without").toDouble(), -3.0);
}

void BenchMainWindowTest::Playback::localPlaybackModesAdvance_data() {
    QTest::addColumn<bool>("repeat");
    QTest::addColumn<bool>("random");
    QTest::addColumn<int>("single");
    QTest::addColumn<int>("consume");
    QTest::addColumn<int>("expected_count");
    QTest::addColumn<bool>("ended");
    QTest::addColumn<int>("track_ms");
    QTest::newRow("single") << false << false << 1 << 0 << 3 << true << 700;
    QTest::newRow("single-once") << false << false << 2 << 0 << 3 << true << 700;
    QTest::newRow("repeat-single") << true << false << 1 << 0 << 3 << false << 700;
    QTest::newRow("repeat-list") << true << false << 0 << 0 << 3 << false << 700;
    QTest::newRow("consume-duplicates") << false << false << 0 << 1 << 0 << true << 700;
    QTest::newRow("consume-once") << false << false << 0 << 2 << 2 << true << 700;
    QTest::newRow("random-cycle") << false << true << 0 << 0 << 3 << true << 700;
    QTest::newRow("repeat-consume-single") << true << false << 1 << 1 << 2 << true << 700;
    QTest::newRow("consume-gapless-duplicates") << false << false << 0 << 1 << 0 << true << 1200;
    QTest::newRow("repeat-single-gapless") << true << false << 1 << 0 << 3 << false << 1200;
}

void BenchMainWindowTest::Playback::localPlaybackModesAdvance() {
    MELODY_REQUIRE_REALTIME_AUDIO();
    QFETCH(bool, repeat);
    QFETCH(bool, random);
    QFETCH(int, single);
    QFETCH(int, consume);
    QFETCH(int, expected_count);
    QFETCH(bool, ended);
    QFETCH(int, track_ms);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto first = media.filePath(QStringLiteral("a.wav"));
    const auto last = media.filePath(QStringLiteral("b.wav"));
    write_wave(first, wave_sample_rate * static_cast<std::uint32_t>(track_ms) / 1000U);
    write_wave(last, wave_sample_rate * static_cast<std::uint32_t>(track_ms) / 1000U);
    BenchMainWindow window;
    window.show();
    window.openLocalPaths({QFile::encodeName(first).toStdString(),
                           QFile::encodeName(first).toStdString(),
                           QFile::encodeName(last).toStdString()});
    QTableView* view = nullptr;
    const auto find_view = [&] {
        for (auto* candidate : window.findChildren<QTableView*>()) {
            if (qobject_cast<LocalListModel*>(candidate->model()) &&
                candidate->model()->rowCount() == 3) {
                view = candidate;
                return true;
            }
        }
        return false;
    };
    QTRY_VERIFY(find_view());
    if (view == nullptr)
        QFAIL("local list view not found");
    auto* model = qobject_cast<LocalListModel*>(view->model());
    const auto trigger = [&](const QString& name, const int count) {
        auto* action = window.findChild<QAction*>(QStringLiteral("action-local-%1").arg(name));
        QVERIFY(action != nullptr);
        for (int i = 0; i < count; ++i) {
            action->trigger();
        }
    };
    trigger(QStringLiteral("repeat"), repeat ? 1 : 0);
    trigger(QStringLiteral("random"), random ? 1 : 0);
    trigger(QStringLiteral("single"), single);
    trigger(QStringLiteral("consume"), consume);
    view->selectionModel()->setCurrentIndex(
        model->index(0, 1), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTest::keyClick(view, Qt::Key_Return);
    // ADR-0226: the engine plays, so its reported status is what is watched.
    const auto status = [&window] {
        return window.property("trackknife-engine-playback").toString();
    };
    QTRY_VERIFY(status() == QStringLiteral("playing") || status() == QStringLiteral("loading"));
    QTest::qWait(track_ms * 3 + 600);
    QTRY_COMPARE(model->rowCount(), expected_count);
    if (ended) {
        QTRY_COMPARE(status(), QStringLiteral("stopped"));
    } else {
        QVERIFY(status() != QStringLiteral("stopped"));
    }
    if (single != 0 && consume == 0) {
        QVERIFY(model->index(0, 0).data(ui::track_current_role).toBool());
        QVERIFY(!model->index(1, 0).data(ui::track_current_role).toBool());
    }
    if (single == 2) {
        QVERIFY(!window.findChild<QAction*>(QStringLiteral("action-local-single"))->isChecked());
    }
    if (consume == 2) {
        QVERIFY(!window.findChild<QAction*>(QStringLiteral("action-local-consume"))->isChecked());
    }
    QVERIFY(QFile::exists(first));
    QVERIFY(QFile::exists(last));
}

void BenchMainWindowTest::Playback::localTrackRatingsPersistByContentIdentity() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("rated.wav"));
    write_wave(path, wave_sample_rate / 2U);
    BenchMainWindow window;
    window.show();
    window.openLocalPaths({QFile::encodeName(path).toStdString()});
    QTableView* view = nullptr;
    const auto find_view = [&] {
        for (auto* candidate : window.findChildren<QTableView*>()) {
            if (qobject_cast<LocalListModel*>(candidate->model()) &&
                candidate->model()->rowCount() == 1) {
                view = candidate;
                return true;
            }
        }
        return false;
    };
    QTRY_VERIFY(find_view());
    if (view == nullptr) {
        QFAIL("the list's view is found");
    }
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    // The probe projection derives the Melody-compatible content identity.
    QTRY_VERIFY(!model->rows().front().rating_hash.empty());
    QCOMPARE(model->headerData(local_rating_column, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Rating"));
    const auto track_hash = model->rows().front().rating_hash;
    const auto album_hash = model->rows().front().album_rating_hash;
    QVERIFY(!album_hash.empty());

    auto* panel = window.findChild<LocalLibraryPanel*>();
    QVERIFY(panel != nullptr);
    auto* track_menu = window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"));
    QVERIFY(track_menu != nullptr);
    view->selectionModel()->select(model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY(QMetaObject::invokeMethod(
        view, "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, view->visualRect(model->index(0, local_title_column)).center())));
    auto* rate_menu = window.findChild<QMenu*>(QStringLiteral("bench-local-rate-menu"));
    auto* album_rate_menu = window.findChild<QMenu*>(QStringLiteral("bench-local-album-rate-menu"));
    QVERIFY(rate_menu != nullptr);
    QVERIFY(album_rate_menu != nullptr);
    QVERIFY(rate_menu->isEnabled());
    QVERIFY(album_rate_menu->isEnabled());
    auto* four_stars = window.findChild<QAction*>(QStringLiteral("action-local-rate-8"));
    auto* album_three = window.findChild<QAction*>(QStringLiteral("action-local-album-rate-6"));
    QVERIFY(four_stars != nullptr);
    QVERIFY(album_three != nullptr);
    four_stars->trigger();
    album_three->trigger();
    track_menu->close();
    QCOMPARE(model->rows().front().rating, 8U);
    QCOMPARE(model->index(0, local_rating_column).data().toString(), QStringLiteral("★★★★"));

    // Both identities reach the store. It is written by the workspace, not
    // through the panel's queue, so the panel is asked until it has them.
    std::optional<std::vector<unsigned>> stored;
    const auto ask = [&] {
        panel->requestRatings({track_hash, album_hash}, [&stored](std::vector<unsigned> values) {
            stored = std::move(values);
        });
    };
    ask();
    QTRY_VERIFY_WITH_TIMEOUT(
        (stored.has_value() && *stored == std::vector<unsigned>{8U, 6U}) || (ask(), false), 10'000);
    // The store notification reloads rows, filling the album rating painted
    // over the group's cover artwork.
    QTRY_COMPARE(model->rows().front().album_rating, 6U);
    QCOMPARE(model->index(0, local_artwork_column).data(ui::track_album_rating_role).toUInt(), 6U);

    // Unrating deletes the stored value and clears the stars immediately.
    QVERIFY(QMetaObject::invokeMethod(
        view, "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, view->visualRect(model->index(0, local_title_column)).center())));
    auto* unrate = window.findChild<QAction*>(QStringLiteral("action-local-rate-0"));
    QVERIFY(unrate != nullptr);
    unrate->trigger();
    track_menu->close();
    QCOMPARE(model->rows().front().rating, 0U);
    QCOMPARE(model->index(0, local_rating_column).data().toString(), QString{});
    // As above, the store is asked until the workspace's write reached it.
    stored.reset();
    const auto ask_track = [&] {
        panel->requestRatings(
            {track_hash}, [&stored](std::vector<unsigned> values) { stored = std::move(values); });
    };
    ask_track();
    QTRY_VERIFY_WITH_TIMEOUT((stored.has_value() && *stored == std::vector<unsigned>{0U}) ||
                                 (ask_track(), false),
                             10'000);
}

void BenchMainWindowTest::Playback::autoAdvancesOncePerFinishedTrack() {
    MELODY_REQUIRE_REALTIME_AUDIO();
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const std::array names{QStringLiteral("a.wav"), QStringLiteral("b.wav"),
                           QStringLiteral("c.wav")};
    std::vector<std::string> raw_paths;
    for (const auto& name : names) {
        const auto path = media.filePath(name);
        write_wave(path, wave_sample_rate * 2U);
        const auto encoded = QFile::encodeName(path);
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(raw_paths);

    QTableView* view = nullptr;
    const QDeadlineTimer view_deadline{5'000};
    while (view == nullptr && !view_deadline.hasExpired()) {
        const auto views = window.findChildren<QTableView*>();
        for (auto* candidate : views) {
            if (qobject_cast<LocalListModel*>(candidate->model()) != nullptr &&
                candidate->model()->rowCount() == 3) {
                view = candidate;
                break;
            }
        }
        if (view == nullptr) {
            QTest::qWait(50);
        }
    }
    QVERIFY(view != nullptr);
    if (view == nullptr) {
        return;
    }
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(std::ranges::all_of(model->rows(),
                                                 [](const LocalTrackRow& row) {
                                                     return row.probed &&
                                                            row.source_revision.has_value();
                                                 }),
                             5'000);
    const auto current = [model](const int row) {
        return model->index(row, 0).data(ui::track_current_role).toBool();
    };

    view->selectionModel()->setCurrentIndex(
        model->index(0, 1), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTest::keyClick(view, Qt::Key_Return);

    // Without a live PipeWire server the load fails and no row ever becomes
    // current; that environment cannot exercise progression.
    const auto deadline = QDeadlineTimer{3'000};
    while (!current(0) && !current(1) && !deadline.hasExpired()) {
        QTest::qWait(50);
    }
    if (!current(0) && !current(1)) {
        QSKIP("live PipeWire playback unavailable");
    }

    // History replay keeps the playing occurrence and the future gapless queue valid.
    const QPersistentModelIndex playing_before_edit{model->index(0, 0)};
    model->removeRowIndexes({2});
    QVERIFY(model->undo());
    model->reorderRows({0, 2}, 3);
    QCOMPARE(playing_before_edit.row(), 1);
    QVERIFY(model->undo());
    QCOMPARE(playing_before_edit.row(), 0);
    QVERIFY(model->redo());
    QVERIFY(model->undo());
    QVERIFY(current(0));
    QVERIFY(model->applyPermutation({2, 1, 0}, QStringLiteral("Reverse list")));
    QCOMPARE(playing_before_edit.row(), 2);
    QVERIFY(model->undo());
    QCOMPARE(playing_before_edit.row(), 0);
    QVERIFY(current(0));

    // Editing another album must not discard the playing occurrence or the
    // already queued successor. Exercise the same publication notification
    // used by tag and artwork commits while the first track is playing.
    const auto& tagged = model->rows()[2];
    QVERIFY(tagged.source_revision.has_value());
    const auto refreshed =
        model->applyCommittedMetadata(tagged.raw_path, tagged.metadata, *tagged.source_revision);
    QVERIFY(refreshed.has_value());
    QCOMPARE(*refreshed, 1U);

    // Each two-second track advances exactly one row, and transitions are
    // gapless: the engine never reports "stopped" between tracks -- that
    // may only come once the final track finishes.
    const auto stopped = [&window] {
        return window.localPlayback() != nullptr &&
               window.localPlayback()->state().status == QStringLiteral("stopped");
    };
    // The row is marked as the play is asked; the engine says so a moment later.
    QTRY_VERIFY_WITH_TIMEOUT(window.localPlayback() != nullptr &&
                                 window.localPlayback()->state().status ==
                                     QStringLiteral("playing"),
                             5'000);
    bool stopped_between_tracks = false;
    const auto follow_until = [&](const int row) {
        const QDeadlineTimer row_deadline{5'000};
        while (!current(row) && !row_deadline.hasExpired()) {
            stopped_between_tracks = stopped_between_tracks || stopped();
            QTest::qWait(50);
        }
    };
    follow_until(1);
    QVERIFY(current(1));
    QTest::qWait(300);
    QVERIFY(current(1));
    QVERIFY(!current(2));
    follow_until(2);
    QVERIFY(current(2));
    QVERIFY(!stopped_between_tracks);

    // The end of the list stays ended: no wrap-around back to the first row.
    QTRY_VERIFY_WITH_TIMEOUT(stopped(), 5'000);
    QTest::qWait(300);
    QVERIFY(current(2));
    QVERIFY(!current(0));
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::Playback)

#include "window_playback_test.moc"
