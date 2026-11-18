// SPDX-License-Identifier: GPL-3.0-only

// The window test: engines elsewhere: their tabs, passwords and libraries.

#include "window_test.hpp"

#include "trackknife/core/engine_state.hpp"
#include "uicommon/library_tree_view.hpp"
#include "workspace/artwork_session.hpp"
#include "workspace/sources.hpp"

#include <QGroupBox>
#include <QMessageBox>
#include <QPainter>
#include <QStandardItemModel>
#include <sqlite3.h>

namespace trackknife::bench {

class BenchMainWindowTest::RemoteEngines final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void settingsListEnginesFoundOnTheNetwork();
    void tabsAreGroupedByEngine();
    void theRemotesListsTakeItsIdOnceItSaysIt();
    void remoteEnginesAreKeptAsAList();
    void twoEnginesElsewhereSideBySide();
    void settingsListTheEnginesElsewhere();
    void anEnginesOwnPasswordIsSaidAndCanGo();
    void enginesAddedOrRemovedApplyAtOnce();
    void aPasswordGivenLaterReachesTheEngine();
    void anotherEngineAtTheSameAddressLeavesTheOldOnesLists();
    void thisComputersTabsAreMarkedWhenTheLibraryIsElsewhere();
    void thisComputersFilesDroppedOnARemoteTab();
    void aRemoteEnginePlaysItsOwnTabs();
    void theWindowFollowsAnEngineStartedElsewhere();
    void theRemoteTabTakesTheNameItsEngineAnnounces();
    void anUnreachableRemoteDoesNotHoldTheWindow();
    void aRemoteTabGetsTagsAndCoversFromItsEngine();
    void aRemoteTabRatesOnItsEngine();
    void thisComputersFilesMoveIntoTheLibrary();
    void replacingARemoteTabFromItsLibraryPlays();
    void locateFindsARemoteTracksAlbumInTheRemoteLibrary();
    void remoteUpNextKeepsItsIdentityAcrossARestart();
    void aRestoredRemoteTabGetsItsCovers();
};

void BenchMainWindowTest::RemoteEngines::settingsListEnginesFoundOnTheNetwork() {
    // An engine announcing itself -- under this test's own service name, set
    // by the test environment, so nothing real is listed or disturbed.
    auto announcer = discovery::Announcer::start(discovery::Advertisement{
        .instance = "lounge", .port = 6603, .txt = {{"id", "lounge-id"}, {"auth", "1"}}});
    if (!announcer) {
        QSKIP("no multicast DNS here");
    }
    BenchMainWindow window;
    window.show();
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::engine);
    auto* menu = dialog->findChild<QMenu*>(QStringLiteral("bench-settings-found-engines-menu"));
    QVERIFY(menu != nullptr);
    const auto lounge = [menu]() -> QAction* {
        for (auto* action : menu->actions()) {
            if (action->text().startsWith(QStringLiteral("lounge — "))) {
                return action;
            }
        }
        return nullptr;
    };
    QTRY_VERIFY_WITH_TIMEOUT(lounge() != nullptr, 8'000);
    QVERIFY(lounge()->text().contains(QStringLiteral(":6603")));
    QVERIFY(lounge()->text().endsWith(QStringLiteral("· password")));
    // Chosen, not typed: its address fills the field.
    lounge()->trigger();
    auto* field = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-engine-socket"));
    QVERIFY(field->text().endsWith(QStringLiteral(":6603")));
    dialog->reject();
}

// ADR-0233: this computer's lists first, then the remote engine's -- kept
// that way whatever order tabs are opened or dragged in.
void BenchMainWindowTest::RemoteEngines::tabsAreGroupedByEngine() {
    QTemporaryDir remote_state;
    QVERIFY(remote_state.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    auto* remote_tab = window.remoteQueueTab();
    QVERIFY(remote_tab != nullptr);
    auto* local_tab =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Added later",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          false);
    auto* bar = window.tabs_->tabBar();
    const auto remote_at = [&] { return window.tabs_->indexOf(remote_tab->view); };
    const auto local_at = [&] { return window.tabs_->indexOf(local_tab->view); };
    QVERIFY2(local_at() < remote_at(), "a local list opens before the remote's group");
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(bar->grab().save(directory + QStringLiteral("/tabs-grouped-by-engine.png")));
    }
    // Dragged into the other group: back in its own.
    bar->moveTab(remote_at(), 0);
    QVERIFY2(local_at() < remote_at(), "the groups stay together");
    for (int index = 0; index < remote_at(); ++index) {
        QVERIFY(engineOfView(window.tabs_->widget(index)).isLocal());
    }
}

// ADR-0234: an engine elsewhere not reached yet is known by its address.
// Reached, it says who it is: its lists take its id, which is remembered,
// and a restart finds them under it without asking again.
void BenchMainWindowTest::RemoteEngines::theRemotesListsTakeItsIdOnceItSaysIt() {
    QTemporaryDir remote_state;
    QVERIFY(remote_state.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    QFile id_file{remote_state.path() + QStringLiteral("/engine-id")};
    QVERIFY(id_file.open(QIODevice::ReadOnly));
    const auto id = QString::fromUtf8(id_file.readAll()).trimmed();
    QVERIFY(!id.isEmpty());
    std::string list_id;
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
        auto* tab = window.remoteQueueTab();
        QVERIFY(tab != nullptr);
        list_id = tab->document.id.to_string();
        QTRY_COMPARE(QString::fromStdString(tab->document.engine), id);
        QCOMPARE(window.remoteEngine()->key.text(), id);
        QCOMPARE(engineOfView(tab->view).text(), id);
        QCOMPARE(window.remoteLibrary()->engine().text(), id);
        QCOMPARE(testing::rememberedRemoteId(), id);
        window.persistNow(true);
        window.close();
    }
    // Kept under the id, not the placeholder (ADR-0259: as the window keeps
    // its tabs).
    {
        const auto kept = TabStore::loadState();
        const auto found = std::ranges::find(
            kept.tabs, list_id, [](const TabStore::Tab& tab) { return tab.id.to_string(); });
        QVERIFY(found != kept.tabs.end());
        QCOMPARE(QString::fromStdString(found->engine), id);
    }
    // Again: known by its id from the start, and its tab with it.
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QVERIFY(window.remoteEngine() != nullptr);
    QCOMPARE(window.remoteEngine()->key.text(), id);
    auto* tab = window.tabForDocument(QString::fromStdString(list_id));
    QVERIFY(tab != nullptr);
    QCOMPARE(QString::fromStdString(tab->document.engine), id);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    QCOMPARE(window.remoteQueueTab(), tab);
}

// The address now leads to another engine than the one whose id was kept --
// a NAS replaced, say. The link takes the new id; the lists of the engine it
// led to before stay that engine's rather than move to one without their files.
void BenchMainWindowTest::RemoteEngines::anotherEngineAtTheSameAddressLeavesTheOldOnesLists() {
    QTemporaryDir remote_state;
    QVERIFY(remote_state.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto before = QString::fromStdString(core::StableId::random().to_string());
    rememberEngineId(remote.socket(), before);
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    // A list of the engine that was there before, open as the window starts.
    auto* old_tab =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Of the old NAS",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {},
                                                    .engine = before.toStdString()},
                          false);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    QTRY_VERIFY(window.remoteEngine()->key.text() != before);
    QCOMPARE(QString::fromStdString(old_tab->document.engine), before);
    QCOMPARE(testing::rememberedRemoteId(), window.remoteEngine()->key.text());
    // The remote's own tab is its own, not the old engine's list.
    QVERIFY(window.remoteQueueTab() != old_tab);
}

// ADR-0234: several engines elsewhere, kept as a list in order, each with
// its own password or this computer's, and the id each gave.
void BenchMainWindowTest::RemoteEngines::remoteEnginesAreKeptAsAList() {
    testing::configureRemoteEngine(QStringLiteral("192.0.2.7:6603"));
    auto engines = loadRemoteEngines();
    QCOMPARE(engines.size(), std::size_t{1});
    QCOMPARE(engines.front().address, QStringLiteral("192.0.2.7:6603"));

    engines.push_back({.address = QStringLiteral("198.51.100.3:6603"),
                       .password = QStringLiteral("other"),
                       .music_folder = QStringLiteral("/srv/music"),
                       .reachable_at = QStringLiteral("/media/galactica"),
                       .id = {}});
    saveRemoteEngines(engines);
    QCOMPARE(loadRemoteEngines(), engines);
    QCOMPARE(loadRemoteEngines().back().effectivePassword(), QStringLiteral("other"));

    rememberEngineId(QStringLiteral("198.51.100.3:6603"), QStringLiteral("def"));
    QCOMPARE(loadRemoteEngines().back().id, QStringLiteral("def"));

    saveRemoteEngines({});
    QVERIFY(loadRemoteEngines().empty());
}

// ADR-0234: two engines elsewhere at once -- a desktop's library and a
// NAS's -- each by its own id, with its own library tab and its own lists,
// grouped after this computer's in the order Settings have them.
void BenchMainWindowTest::RemoteEngines::twoEnginesElsewhereSideBySide() {
    QTemporaryDir first_state;
    QTemporaryDir second_state;
    QVERIFY(first_state.isValid() && second_state.isValid());
    testing::TestEngine first;
    testing::TestEngine second;
    QVERIFY2(first.start(first_state.path().toStdString(), true), first.log().constData());
    QVERIFY2(second.start(second_state.path().toStdString(), true), second.log().constData());
    const auto id_of = [](const QTemporaryDir& state) {
        QFile file{state.path() + QStringLiteral("/engine-id")};
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed()
                                              : QString{};
    };
    const auto first_id = id_of(first_state);
    const auto second_id = id_of(second_state);
    QVERIFY(!first_id.isEmpty() && !second_id.isEmpty() && first_id != second_id);
    saveRemoteEngines({{.address = first.socket(),
                        .password = {},
                        .music_folder = {},
                        .reachable_at = {},
                        .id = {}},
                       {.address = second.socket(),
                        .password = {},
                        .music_folder = {},
                        .reachable_at = {},
                        .id = {}}});

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QCOMPARE(window.engines_.size(), std::size_t{3});
    QTRY_VERIFY(window.engines_[1]->playback->active() && window.engines_[2]->playback->active());
    QTRY_COMPARE(window.engines_[1]->key.text(), first_id);
    QTRY_COMPARE(window.engines_[2]->key.text(), second_id);
    // Each remembered, by its address.
    const auto kept = loadRemoteEngines();
    QCOMPARE(kept.size(), std::size_t{2});
    QCOMPARE(kept[0].id, first_id);
    QCOMPARE(kept[1].id, second_id);

    // A library tab each, found by the engine's key.
    QStringList keys;
    for (int index = 0; index < window.local_source_tabs_->count(); ++index) {
        keys << window.local_source_tabs_->tabData(index).toString();
    }
    QVERIFY2(keys.contains(first_id) && keys.contains(second_id), qPrintable(keys.join(u',')));
    QVERIFY(window.engines_[1]->library != window.engines_[2]->library);
    QCOMPARE(window.engines_[2]->library->engine().text(), second_id);

    // Each its own tab, of its own engine, after this computer's.
    auto* first_tab = window.engineTab(*window.engines_[1]);
    auto* second_tab = window.engineTab(*window.engines_[2]);
    QVERIFY(first_tab != nullptr && second_tab != nullptr && first_tab != second_tab);
    QCOMPARE(QString::fromStdString(first_tab->document.engine), first_id);
    QCOMPARE(QString::fromStdString(second_tab->document.engine), second_id);
    const auto at = [&window](const BenchMainWindow::ListTab* tab) {
        return window.tabs_->indexOf(tab->view);
    };
    QVERIFY(at(window.list_tabs_.front().get()) < at(first_tab));
    QVERIFY(at(first_tab) < at(second_tab));
    QCOMPARE(window.playbackOf(EngineKey::of(second_tab->document)), window.engines_[2]->playback);
    saveRemoteEngines({});
}

// ADR-0234: Settings list the engines elsewhere; the form below edits the
// one chosen, Add and Remove change the list, and what is saved is those
// with an address, in order.
void BenchMainWindowTest::RemoteEngines::settingsListTheEnginesElsewhere() {
    saveRemoteEngines({{.address = QStringLiteral("192.0.2.10:6603"),
                        .password = {},
                        .music_folder = QStringLiteral("/mnt/nas/Music"),
                        .reachable_at = {},
                        .id = QStringLiteral("nas-id")}});
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::engine);
    QVERIFY(dialog != nullptr);
    auto* list = dialog->findChild<QListWidget*>(QStringLiteral("bench-settings-engines"));
    auto* address = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-engine-socket"));
    auto* folder = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-remote-folder"));
    auto* reachable = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-remote-mount"));
    auto* add = dialog->findChild<QPushButton*>(QStringLiteral("bench-settings-engine-add"));
    auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("bench-settings-engine-remove"));
    QVERIFY(list && address && folder && reachable && add && remove);
    // ADR-0286: this computer first, then the engines elsewhere, each with
    // its "holds the library" choice.
    const auto row = [list](const int index) {
        auto* holder = qobject_cast<QRadioButton*>(list->itemWidget(list->item(index)));
        return holder != nullptr ? holder->text() : QString{};
    };
    QCOMPARE(list->count(), 2);
    QVERIFY(row(0).startsWith(QStringLiteral("This computer")));
    QVERIFY(row(1).startsWith(QStringLiteral("192.0.2.10:6603")));
    QCOMPARE(address->text(), QStringLiteral("192.0.2.10:6603"));
    QCOMPARE(folder->text(), QStringLiteral("/mnt/nas/Music"));

    // One more, filled in.
    add->click();
    QCOMPARE(list->count(), 3);
    QVERIFY(address->text().isEmpty());
    address->setText(QStringLiteral("192.0.2.20:6603"));
    folder->setText(QStringLiteral("/srv/music"));
    reachable->setText(QStringLiteral("/media/desktop"));
    QVERIFY(row(2).startsWith(QStringLiteral("192.0.2.20:6603")));
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/settings-engines.png")));
    }
    // Back to the first: its fields, untouched.
    list->setCurrentRow(1);
    QCOMPARE(address->text(), QStringLiteral("192.0.2.10:6603"));
    QVERIFY(reachable->text().isEmpty());
    // This computer chosen: nothing of an engine elsewhere to edit or remove.
    list->setCurrentRow(0);
    QVERIFY(!remove->isEnabled());
    QVERIFY(!address->isVisibleTo(dialog));
    list->setCurrentRow(2);
    QVERIFY(remove->isEnabled());
    // A third, left empty, and removed again.
    add->click();
    QCOMPARE(list->count(), 4);
    remove->click();
    QCOMPARE(list->count(), 3);
    // The second engine elsewhere holds the library; its folders are shown.
    auto* holder = qobject_cast<QRadioButton*>(list->itemWidget(list->item(2)));
    QVERIFY(holder != nullptr);
    holder->click();
    const auto folders_title = [dialog] {
        for (auto* box : dialog->findChildren<QGroupBox*>()) {
            if (box->title().startsWith(QStringLiteral("Music folders of "))) {
                return box->title();
            }
        }
        return QString{};
    };
    QCOMPARE(folders_title(), QStringLiteral("Music folders of 192.0.2.20:6603"));
    QVERIFY(row(2).endsWith(QStringLiteral("holds the library")));

    QPointer<SettingsDialog> lifetime = dialog;
    dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    QTRY_VERIFY(lifetime.isNull());
    const auto saved = loadRemoteEngines();
    QCOMPARE(saved.size(), std::size_t{2});
    QCOMPARE(saved[0].address, QStringLiteral("192.0.2.10:6603"));
    QCOMPARE(saved[0].id, QStringLiteral("nas-id"));
    QCOMPARE(saved[1].address, QStringLiteral("192.0.2.20:6603"));
    QCOMPARE(saved[1].music_folder, QStringLiteral("/srv/music"));
    QCOMPARE(saved[1].reachable_at, QStringLiteral("/media/desktop"));
    QVERIFY(saved[1].id.isEmpty());
    QCOMPARE(QSettings{}.value(QLatin1String(SettingsDialog::library_engine_key)).toString(),
             QStringLiteral("192.0.2.20:6603"));
    QCOMPARE(libraryEngine(), QStringLiteral("192.0.2.20:6603"));
    QVERIFY(!localLibraryShown());
    QSettings{}.remove(QLatin1String(SettingsDialog::library_engine_key));
    saveRemoteEngines({});
}

// ADR-0286: one password for every engine. One of its own kept from before
// still works, is said under Advanced, and can go.
void BenchMainWindowTest::RemoteEngines::anEnginesOwnPasswordIsSaidAndCanGo() {
    QSettings{}.setValue(QLatin1String(SettingsDialog::engine_password_key),
                         QStringLiteral("shared"));
    saveRemoteEngines({{.address = QStringLiteral("192.0.2.10:6603"),
                        .password = QStringLiteral("its own"),
                        .music_folder = {},
                        .reachable_at = {},
                        .id = {}}});
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::engine);
    QVERIFY(dialog != nullptr);
    auto* list = dialog->findChild<QListWidget*>(QStringLiteral("bench-settings-engines"));
    auto* own = dialog->findChild<QWidget*>(QStringLiteral("bench-settings-engine-own-password"));
    auto* drop =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-settings-engine-own-password-drop"));
    QVERIFY(list && own && drop);
    list->setCurrentRow(1);
    QVERIFY(own->isVisibleTo(own->parentWidget()));
    drop->click();
    QVERIFY(!own->isVisibleTo(own->parentWidget()));
    QPointer<SettingsDialog> lifetime = dialog;
    dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    QTRY_VERIFY(lifetime.isNull());
    const auto saved = loadRemoteEngines();
    QCOMPARE(saved.size(), std::size_t{1});
    QVERIFY(saved[0].password.isEmpty());
    QCOMPARE(saved[0].effectivePassword(), QStringLiteral("shared"));
    QSettings{}.remove(QLatin1String(SettingsDialog::engine_password_key));
    saveRemoteEngines({});
}

// Engines added in Settings are connected, and those removed let go, at
// once -- no restart. A removed engine's lists stay, as an engine's that is
// not reached.
void BenchMainWindowTest::RemoteEngines::enginesAddedOrRemovedApplyAtOnce() {
    QTemporaryDir first_state;
    QTemporaryDir second_state;
    QVERIFY(first_state.isValid() && second_state.isValid());
    testing::TestEngine first;
    testing::TestEngine second;
    QVERIFY2(first.start(first_state.path().toStdString(), true), first.log().constData());
    QVERIFY2(second.start(second_state.path().toStdString(), true), second.log().constData());
    const RemoteEngineSetting first_setting{.address = first.socket(),
                                            .password = {},
                                            .music_folder = {},
                                            .reachable_at = {},
                                            .id = {}};
    const RemoteEngineSetting second_setting{.address = second.socket(),
                                             .password = {},
                                             .music_folder = {},
                                             .reachable_at = {},
                                             .id = {}};
    saveRemoteEngines({first_setting});
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QCOMPARE(window.engines_.size(), std::size_t{2});
    QTRY_VERIFY(window.engines_[1]->playback->active());
    const auto sidebar_keys = [&window] {
        QStringList keys;
        for (int index = 0; index < window.local_source_tabs_->count(); ++index) {
            keys << window.local_source_tabs_->tabData(index).toString();
        }
        return keys;
    };

    // Added: connected, with its library tab.
    saveRemoteEngines({first_setting, second_setting});
    window.syncRemoteEngines();
    QCOMPARE(window.engines_.size(), std::size_t{3});
    QTRY_VERIFY(window.engines_[2]->playback->active());
    QTRY_VERIFY(sidebar_keys().contains(window.engines_[2]->key.text()));
    const auto first_key = window.engines_[1]->key;
    auto* first_tab = window.engineTab(*window.engines_[1]);
    QVERIFY(first_tab != nullptr);
    const auto first_list = QString::fromStdString(first_tab->document.id.to_string());

    // Removed: let go -- its tab in the sidebar too -- its list kept.
    saveRemoteEngines({second_setting});
    window.syncRemoteEngines();
    QCOMPARE(window.engines_.size(), std::size_t{2});
    QVERIFY(window.link(first_key) == nullptr);
    QVERIFY(!sidebar_keys().contains(first_key.text()));
    QVERIFY(window.tabForDocument(first_list) != nullptr);
    QVERIFY(window.playbackOf(first_key) == nullptr);
    // The one left still works.
    QVERIFY(window.engines_[1]->playback->active());
    QCOMPARE(window.engines_[1]->setting.address, second.socket());
    saveRemoteEngines({});
}

// An engine on the network added without its password, and "the password
// above" filled in afterwards: pressing OK again connects with it, rather
// than keeping the connection made with none until Trackknife restarts.
void BenchMainWindowTest::RemoteEngines::aPasswordGivenLaterReachesTheEngine() {
    QTemporaryDir state;
    QTemporaryDir music;
    QVERIFY(state.isValid() && music.isValid());
    const auto port = [] {
        QTcpServer probe;
        probe.listen(QHostAddress::LocalHost, 0);
        return probe.serverPort();
    }();
    const auto password = QStringLiteral("correct horse");
    QProcess engine;
    engine.setProgram(QStringLiteral(MELODY_ENGINE_BINARY));
    engine.setArguments({QStringLiteral("--socket"), state.filePath(QStringLiteral("melodyd.sock")),
                         QStringLiteral("--state"), state.path(), QStringLiteral("--listen"),
                         QStringLiteral("127.0.0.1:%1").arg(port), QStringLiteral("--password"),
                         password});
    engine.setProcessChannelMode(QProcess::MergedChannels);
    engine.start();
    QVERIFY(engine.waitForStarted());
    const auto address = QStringLiteral("127.0.0.1:%1").arg(port);
    QTRY_VERIFY_WITH_TIMEOUT(
        protocol::Client::connect(
            *protocol::Endpoint::parse(address.toStdString(), password.toStdString()))
            .has_value(),
        10'000);
    saveRemoteEngines({});
    QSettings{}.remove(QLatin1String(SettingsDialog::engine_password_key));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    const RemoteEngineSetting setting{
        .address = address, .password = {}, .music_folder = {}, .reachable_at = {}, .id = {}};
    saveRemoteEngines({setting});
    window.syncRemoteEngines();
    QCOMPARE(window.engines_.size(), std::size_t{2});
    // Refused: no password yet.
    QVERIFY(!window.engines_[1]->catalogue->open()->roots().has_value());
    QSettings{}.setValue(QLatin1String(SettingsDialog::engine_password_key), password);
    window.syncRemoteEngines();
    QCOMPARE(window.engines_.size(), std::size_t{2});
    auto& link = *window.engines_[1];
    const auto folder = QFile::encodeName(music.path()).toStdString();
    link.library->addRoot(folder);
    const auto roots = [&link] {
        std::vector<std::string> found;
        if (auto listed = link.catalogue->open()->roots()) {
            for (const auto& root : *listed) {
                found.push_back(root.raw_path);
            }
        }
        return found;
    };
    QTRY_VERIFY2_WITH_TIMEOUT(
        std::ranges::contains(roots(), folder),
        qPrintable(
            link.library->findChild<QLabel*>(QStringLiteral("local-library-status"))->text()),
        5'000);
    saveRemoteEngines({});
    QSettings{}.remove(QLatin1String(SettingsDialog::engine_password_key));
    engine.terminate();
    QVERIFY(engine.waitForFinished(5'000));
}

// ADR-0286: the library's tabs are the usual ones; this computer's are
// marked -- a computer icon, a tooltip saying where its files are -- and kept
// after them. The sidebar calls this computer's folders by their name.
void BenchMainWindowTest::RemoteEngines::thisComputersTabsAreMarkedWhenTheLibraryIsElsewhere() {
    // The library on an engine elsewhere, reached or not.
    saveRemoteEngines({{.address = QStringLiteral("192.0.2.10:6603"),
                        .password = {},
                        .music_folder = {},
                        .reachable_at = {},
                        .id = {},
                        .stream_kbps = -1,
                        .name = QStringLiteral("gemenon")}});
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_key),
                         QStringLiteral("192.0.2.10:6603"));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QVERIFY(!window.list_tabs_.empty());
    auto& local = *window.list_tabs_.front();
    QVERIFY(EngineKey::of(local.document).isLocal());
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    const auto index = tabs->indexOf(local.view);
    QVERIFY(!tabs->tabIcon(index).isNull());
    QVERIFY(tabs->tabToolTip(index).contains(QStringLiteral("files on ")));
    QVERIFY(tabs->tabToolTip(index).contains(QStringLiteral("played here")));
    auto* sources = window.findChild<QTabBar*>(QStringLiteral("bench-local-source-tabs"));
    QVERIFY(sources != nullptr);
    // This computer's library is no library: its tab is not shown.
    QCOMPARE(sources->tabText(0), QStringLiteral("Library"));
    QVERIFY(!sources->isTabVisible(0));

    // The library here again: nothing to mark.
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_key),
                         QLatin1String(SettingsDialog::library_engine_local));
    window.refreshTabChrome(local);
    QVERIFY(tabs->tabIcon(tabs->indexOf(local.view)).isNull());
    QVERIFY(!tabs->tabToolTip(tabs->indexOf(local.view)).contains(QStringLiteral("files on ")));
    saveRemoteEngines({});
}

// ADR-0227: this computer's engine and a remote one, side by side. Local tabs
// play here and remote tabs there, never both at once; the remote library
// fills remote tabs from its own index; and nothing mixes the two.
// Another client -- a picker, a script -- starts the remote engine while the
// window follows this computer's: the window follows the music there, and
// the remote's tab shows what plays, rather than the list it had.
// A remote that is away when the window opens has its tab made anyway, named
// by its address; when it comes back and says what it is called, the tab
// takes that name. It used to keep the address -- or an old name -- for good.
void BenchMainWindowTest::RemoteEngines::theRemoteTabTakesTheNameItsEngineAnnounces() {
    QTemporaryDir remote_state;
    QVERIFY(remote_state.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    remote.stop();

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QVERIFY(window.remoteCatalogue() != nullptr);
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);
    const auto address = window.remoteCatalogue()->addressName();
    QCOMPARE(displayText(tab->document.name), address);

    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    QTRY_VERIFY_WITH_TIMEOUT(window.remotePlayback()->active(), 10'000);
    QTRY_VERIFY(displayText(tab->document.name) != address);
    QCOMPARE(displayText(tab->document.name), window.remoteCatalogue()->name());
}

// A remote engine over TCP whose host is off -- the NAS -- held a connect for
// about two minutes of SYN retries, on the window's thread, at startup and
// again every few seconds after. It is reached from a worker now.
void BenchMainWindowTest::RemoteEngines::anUnreachableRemoteDoesNotHoldTheWindow() {
    // TEST-NET-1 (RFC 5737): routed nowhere, so a connect waits rather than
    // being refused.
    testing::configureRemoteEngine(QStringLiteral("192.0.2.1:6603"), QStringLiteral("a password"));

    QElapsedTimer opening;
    opening.start();
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QVERIFY2(opening.elapsed() < 2'000, QByteArray::number(opening.elapsed()).constData());
    QVERIFY(window.remotePlayback() != nullptr);
    QVERIFY(!window.remotePlayback()->active());

    // And across the reconnect timer's next attempt, the event loop keeps
    // turning.
    QElapsedTimer waiting;
    waiting.start();
    qint64 longest = 0;
    qint64 last = 0;
    while (waiting.elapsed() < 4'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(20);
        longest = std::max(longest, waiting.elapsed() - last);
        last = waiting.elapsed();
    }
    QVERIFY2(longest < 1'000, QByteArray::number(longest).constData());
    saveRemoteEngines({});
}

void BenchMainWindowTest::RemoteEngines::theWindowFollowsAnEngineStartedElsewhere() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    std::vector<std::string> paths;
    for (const auto* name : {"old.wav", "one.wav", "two.wav", "three.wav"}) {
        const auto path = media.filePath(QString::fromLatin1(name));
        write_wave(path, wave_sample_rate * 60U);
        paths.push_back(QFile::encodeName(path).toStdString());
    }

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    QVERIFY(window.transport_ == window.localPlayback());

    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = remote.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    const auto queue = [&](const std::vector<std::size_t>& which) {
        std::vector<std::string> ids;
        auto entries = protocol::Json::array();
        for (const auto index : which) {
            ids.push_back(core::StableId::random().to_string());
            entries.push_back(protocol::Json{{"entry", ids.back()},
                                             {"path", protocol::encode_raw_path(paths[index])},
                                             {"title", "Track " + std::to_string(index)},
                                             {"group", protocol::Json{{"album_artist", "Band"},
                                                                      {"artist", "Band"},
                                                                      {"album", "Record"},
                                                                      {"date", "1992"}}}});
        }
        static_cast<void>((*other)->call("playback.replace_queue",
                                         protocol::Json{{"entries", std::move(entries)}}));
        return ids;
    };
    // The server's tab already shows a list: an album played there before.
    static_cast<void>(queue({0}));
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);

    // A picker replaces it with an album and plays its second track -- both
    // reaching the window in one report.
    const auto album = queue({1, 2, 3});
    QVERIFY((*other)->call("playback.play", protocol::Json{{"entry", album[1]}}).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    QTRY_VERIFY_WITH_TIMEOUT(window.transport_ == window.remotePlayback(), 10'000);
    const auto id = [](const std::string& text) { return *core::StableId::parse(text); };
    QTRY_VERIFY(tab->model->rowOfEntry(id(album[0]), -1) >= 0 &&
                tab->model->rowOfEntry(id(album[2]), -1) >= 0);
    QCOMPARE(tab->model->rowCount(), 3);
    QCOMPARE(window.playback_.anchors.document, tab->document.id);
    // Its row is marked as the one playing, and stays so -- not the row
    // where the list's old track was, and not only once the next one plays.
    const auto marked = [&](const std::string& entry) {
        const auto row = tab->model->rowOfEntry(id(entry), -1);
        return row >= 0 && tab->model->index(row, 0).data(ui::track_current_role).toBool();
    };
    QTRY_VERIFY2(marked(album[1]), "the playing row is not marked");
    QVERIFY(!marked(album[0]) && !marked(album[2]));
    // Past two of the engine's state reports.
    QTest::qWait(1'100);
    QVERIFY2(marked(album[1]), "the playing row lost its mark");
    // The tags the other client gave are the engine's still: the window,
    // following, neither showed filenames nor sent its rows back in their
    // place -- which left a phone showing no tags for what it had queued.
    const auto held = (*other)->call("playback.queue");
    QVERIFY(held.has_value());
    QCOMPARE(held->at("entries").size(), std::size_t{3});
    for (const auto& entry : held->at("entries")) {
        QVERIFY2(entry.value("title", std::string{}).starts_with("Track "), entry.dump().c_str());
        QCOMPARE(entry.at("group").value("album", std::string{}), std::string{"Record"});
    }
    const auto row = tab->model->rowOfEntry(id(album[0]), -1);
    QCOMPARE(tab->model->rows()[static_cast<std::size_t>(row)].album, std::string{"Record"});
    static_cast<void>((*other)->call("playback.stop"));
    (*other)->close();
}

void BenchMainWindowTest::RemoteEngines::aRemoteEnginePlaysItsOwnTabs() {
    MELODY_REQUIRE_REALTIME_AUDIO();
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto here = media.filePath(QStringLiteral("here.wav"));
    // Long enough that only the switch, not the end of the track, stops it.
    write_wave(here, wave_sample_rate * 60U);
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    // A minute long: "playing" has to outlast what the test does while it
    // plays, or whether the test sees it is a race.
    const auto there = music + QStringLiteral("/there.wav");
    write_wave(there, wave_sample_rate * 60U);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    auto* remote_tab = window.remoteQueueTab();
    QVERIFY(remote_tab != nullptr && !EngineKey::of(remote_tab->document).isLocal());
    auto* sources = window.findChild<QTabBar*>(QStringLiteral("bench-local-source-tabs"));
    QVERIFY(sources != nullptr);
    // The remote's library tab, before Playlists, which stays last.
    QCOMPARE(window.playlists_tab_, sources->count() - 1);
    const auto remote_at = window.playlists_tab_ - 1;
    QCOMPARE(sources->tabData(remote_at).toString(), window.remoteEngine()->key.text());
    // Someone whose music is all on the remote can hide this computer's
    // library; its tab goes, and the remote's stays in front.
    {
        const auto library_tab = 0;
        QVERIFY(sources->isTabVisible(library_tab));
        sources->setCurrentIndex(library_tab);
        testing::holdLibraryOnTheRemote();
        window.applyLocalLibraryVisibility();
        QVERIFY(!sources->isTabVisible(library_tab));
        QCOMPARE(sources->tabData(sources->currentIndex()).toString(),
                 window.remoteEngine()->key.text());
        // Gone from the bar too: the remote's tab is where a click finds it.
        const auto remote_rect = sources->tabRect(sources->currentIndex());
        QVERIFY(remote_rect.isValid());
        QCOMPARE(sources->tabAt(remote_rect.center()), sources->currentIndex());
        testing::holdLibraryHere();
        window.applyLocalLibraryVisibility();
        QVERIFY(sources->isTabVisible(library_tab));
    }
    // "Library", the engine named in its tooltip by what it calls itself --
    // here its machine's name, as it was started without one -- rather than
    // by its socket or address.
    QCOMPARE(sources->tabText(remote_at), QStringLiteral("Library"));
    QTRY_VERIFY(sources->tabToolTip(remote_at).startsWith(
        QStringLiteral("The library on %1").arg(QSysInfo::machineHostName())));
    // And the library says so too, not the address.
    QTRY_COMPARE(window.remoteCatalogue()->describe(),
                 QStringLiteral("Library: %1").arg(QSysInfo::machineHostName()));

    // A folder is added to the remote's library by its path there: this
    // computer's file dialog would offer this computer's folders.
    {
        QWidget host;
        auto* folders = window.remoteLibrary()->createFoldersWidget(&host);
        auto* add = folders->findChild<QPushButton*>(QStringLiteral("local-library-folder-add"));
        QVERIFY(add != nullptr);
        QTimer::singleShot(0, [music] {
            auto* prompt = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            if (prompt != nullptr) {
                prompt->setTextValue(music);
                prompt->accept();
            }
        });
        add->click();
        QTRY_VERIFY([&] {
            const auto roots = window.remoteCatalogue()->open()->roots();
            return roots && roots->size() == 1U &&
                   roots->front().raw_path == QFile::encodeName(music).toStdString();
        }());
    }
    // The remote library fills the remote tab, from the remote's index.
    {
        persistence::LibraryScanProgress progress;
        QVERIFY(window.remoteCatalogue()->open()->scan({}, progress).has_value());
    }
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = window.remoteCatalogue()->open()->query(albums);
    QVERIFY(page && !page->entries.empty());
    window.openLocalPaths({QFile::encodeName(here).toStdString()});
    QTRY_VERIFY(window.currentListTab() != nullptr &&
                EngineKey::of(window.currentListTab()->document).isLocal() &&
                window.currentListTab()->model->rowCount() == 1);
    auto* local_tab = window.currentListTab();
    emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                           LocalLibraryAction::append);
    QTRY_COMPARE(remote_tab->model->rowCount(), 1);
    QCOMPARE(local_tab->model->rowCount(), 1);
    QVERIFY(!remote_tab->model->rows().front().title.empty());

    // Local plays here; the remote tab plays there and stops this one.
    window.playRow(*local_tab, 0);
    QTRY_COMPARE(window.localPlayback()->state().status, QStringLiteral("playing"));
    window.playRow(*remote_tab, 0);
    QTRY_COMPARE(window.remotePlayback()->state().status, QStringLiteral("playing"));
    window.refreshUpNext();
    QVERIFY2(window.up_next_status_->text().startsWith(QSysInfo::machineHostName() +
                                                       QStringLiteral(" · ")),
             qPrintable(window.up_next_status_->text()));
    // Adding to the remote tab while it plays: the rows stay as they were
    // added -- tagged, with their own identities -- rather than being traded
    // for the engine's bare paths.
    {
        emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                               LocalLibraryAction::append);
        QTRY_COMPARE(remote_tab->model->rowCount(), 2);
        const auto added = remote_tab->model->rows();
        for (int wait = 0; wait < 40; ++wait) {
            QTest::qWait(50);
            const auto& now = remote_tab->model->rows();
            QCOMPARE(static_cast<int>(now.size()), 2);
            for (std::size_t index = 0; index < now.size(); ++index) {
                QCOMPARE(now[index].entry_id, added[index].entry_id);
                QVERIFY2(now[index].probed && now[index].title == added[index].title,
                         now[index].title.c_str());
            }
        }
        remote_tab->model->removeRowIndexes({1});
        window.markTabDirty(*remote_tab);
        QTRY_VERIFY(!window.remotePlayback()->settling());
        QTRY_COMPARE(window.remotePlayback()->state().queue_size, std::size_t{1});
    }
    // And while the engine is slow to answer -- a remote busy scanning, as
    // gemenon was. An edit is on its way when an older state arrives, one
    // whose queue lacks the row just added. That is no reason to take the
    // engine's queue over: doing so blocked the window on the busy engine,
    // dropped the new row, and brought it back as a bare path once the
    // engine caught up.
    {
        auto extra = remote_tab->model->rows().front();
        extra.entry_id = core::StableId::random();
        const auto pid = static_cast<pid_t>(remote.processId());
        QVERIFY(pid > 0);
        QVERIFY(::kill(pid, SIGSTOP) == 0);
        const auto resume = qScopeGuard([pid] { ::kill(pid, SIGCONT); });
        remote_tab->model->appendRows({extra});
        window.markTabDirty(*remote_tab);
        QVERIFY(window.remotePlayback()->settling());
        // A state from before the edit, as far as the window can tell.
        window.engine_queue_revision_ = 0;
        QElapsedTimer waited;
        waited.start();
        emit window.remotePlayback()->changed();
        QVERIFY2(waited.elapsed() < 1'000, "the window does not wait on a busy engine");
        QCOMPARE(remote_tab->model->rowCount(), 2);
        QCOMPARE(remote_tab->model->rows().back().entry_id, extra.entry_id);
        ::kill(pid, SIGCONT);
        QTRY_VERIFY(!window.remotePlayback()->settling());
        QTest::qWait(300);
        QCOMPARE(remote_tab->model->rowCount(), 2);
        QCOMPARE(remote_tab->model->rows().back().entry_id, extra.entry_id);
        QVERIFY(remote_tab->model->rows().back().probed);
        remote_tab->model->removeRowIndexes({1});
        window.markTabDirty(*remote_tab);
    }
    QTRY_COMPARE(window.localPlayback()->state().status, QStringLiteral("stopped"));
    QVERIFY(window.transport_ == window.remotePlayback());
    // And back.
    window.playRow(*local_tab, 0);
    QTRY_COMPARE(window.localPlayback()->state().status, QStringLiteral("playing"));
    QTRY_COMPARE(window.remotePlayback()->state().status, QStringLiteral("stopped"));

    // Up Next is named by the engine it belongs to.
    {
        auto* status = window.up_next_status_;
        QVERIFY(status != nullptr);
        window.refreshUpNext();
        QVERIFY2(status->text().startsWith(QStringLiteral("This computer · ")),
                 qPrintable(status->text()));
    }
    // Up Next holds one engine's asks.
    window.enqueueLocalRequests({remote_tab->model->rows().front()}, -1,
                                window.remoteEngine()->key);
    QCOMPARE(window.playback_.requests.pending().size(), 1U);
    window.enqueueLocalRequests({local_tab->model->rows().front()}, -1, EngineKey::local());
    QCOMPARE(window.playback_.requests.pending().size(), 1U);
    window.playback_.requests.clear();

    // The binding survives a restart.
    window.persistNow(false);
    QVERIFY(window.close());
    BenchMainWindow restored;
    QTRY_VERIFY(restored.lists_restored_);
    const auto remote_tabs = std::ranges::count_if(restored.list_tabs_, [](const auto& tab) {
        return !EngineKey::of(tab->document).isLocal();
    });
    QCOMPARE(remote_tabs, 1);
}

void BenchMainWindowTest::RemoteEngines::aRemoteTabRatesOnItsEngine() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    {
        auto catalogue = window.remoteCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = window.remoteCatalogue()->open()->query(albums);
    QVERIFY(page && page->entries.size() == 1U);
    emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                           LocalLibraryAction::append);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    QTRY_VERIFY(!tab->model->rows().front().album_rating_hash.empty());
    const auto track_hash = tab->model->rows().front().rating_hash;
    const auto album_hash = tab->model->rows().front().album_rating_hash;

    window.tabs_->setCurrentWidget(tab->view);
    tab->view->selectionModel()->select(
        tab->model->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY(QMetaObject::invokeMethod(
        tab->view, "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, tab->view->visualRect(tab->model->index(0, local_title_column)).center())));
    auto* track_stars = window.findChild<QAction*>(QStringLiteral("action-local-rate-6"));
    auto* album_stars = window.findChild<QAction*>(QStringLiteral("action-local-album-rate-8"));
    QVERIFY(track_stars != nullptr && album_stars != nullptr);
    track_stars->trigger();
    album_stars->trigger();
    window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"))->close();

    // Stored where the track lives, so the remote's own queries see it...
    const auto remote_ratings = [&window, &track_hash, &album_hash] {
        auto found = window.remoteCatalogue()->open()->ratings({track_hash, album_hash});
        return found ? *found : std::vector<unsigned>{};
    };
    QTRY_COMPARE(remote_ratings(), (std::vector<unsigned>{6U, 8U}));
    // ...and read back from there onto the tab's cover.
    QTRY_COMPARE(tab->model->rows().front().album_rating, 8U);
    // Not on this computer's engine, which does not have the track.
    const auto here = window.localCatalogue()->open()->ratings({track_hash, album_hash});
    QVERIFY(here.has_value());
    QCOMPARE(*here, (std::vector<unsigned>{0U, 0U}));
}

// ADR-0286: with the library on another engine, the tagger's Move offers
// its folders for this computer's files: moved there through the mount --
// here the same path -- and taken into that engine's library.
void BenchMainWindowTest::RemoteEngines::thisComputersFilesMoveIntoTheLibrary() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    const auto here = media.filePath(QStringLiteral("here.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), here));
    const auto raw = [](const QString& path) {
        const auto encoded = QFile::encodeName(path);
        return std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    };
    // The remote holds the library.
    testing::holdLibraryOnTheRemote();

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    QVERIFY(window.remoteCatalogue()->open()->add_root(raw(music)).has_value());
    const auto* library = window.workspace_.libraryElsewhere();
    QVERIFY(library != nullptr && library->browser);
    library->browser->reload();
    QTRY_VERIFY(!library->browser->rootPaths().empty());
    bool layout_saved = false;
    window.persistence_->saveOutputLayoutProfile(
        persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile =
                operations::OutputLayoutProfile{
                    .schema_version = 1U,
                    .name = "Added",
                    .dialect = {},
                    .relative_directory_expression = "Added",
                    .basename_expression = "moved",
                    .sanitization_policy = {"linux", 1U},
                },
        },
        [&layout_saved](const QString& error) { layout_saved = error.isEmpty(); });
    QTRY_VERIFY(layout_saved);

    window.openLocalPaths({raw(here)});
    Workspace::ListTab* tab = nullptr;
    QTRY_VERIFY((tab = [&window]() -> Workspace::ListTab* {
                    for (auto& candidate : window.list_tabs_) {
                        if (EngineKey::of(candidate->document).isLocal() &&
                            candidate->model->rowCount() == 1) {
                            return candidate.get();
                        }
                    }
                    return nullptr;
                }()) != nullptr);
    window.tabs_->setCurrentWidget(tab->view);
    QTRY_VERIFY_WITH_TIMEOUT(tab->model->rows().front().probed, 5'000);
    tab->view->selectionModel()->select(
        tab->model->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QTRY_VERIFY(properties_action->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    properties_action->trigger();
    auto* properties = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);
    auto* tagger = properties->findChild<TaggerSession*>();
    QVERIFY(tagger != nullptr);
    QTRY_VERIFY(tagger->layoutsAvailable());
    const auto layouts = tagger->layouts();
    const auto layout =
        std::ranges::find(layouts, QStringLiteral("Added"), &TaggerSession::Choice::name);
    QVERIFY(layout != layouts.end());
    tagger->selectLayout(static_cast<int>(std::distance(layouts.begin(), layout)));

    // The library's folder is offered, by the engine holding it.
    const auto choices = tagger->destinations();
    const auto offered =
        std::ranges::find_if(choices, [&music](const TaggerSession::Choice& choice) {
            return choice.name.startsWith(QStringLiteral("Library on ")) &&
                   choice.name.endsWith(music);
        });
    QVERIFY2(offered != choices.end(),
             qPrintable(choices.empty() ? QString{} : choices.back().name));
    tagger->selectDestination(static_cast<int>(std::distance(choices.begin(), offered)));
    tagger->chooseRename(true);
    tagger->chooseMove(true);
    QVERIFY2(tagger->moveAvailable(), qPrintable(tagger->moveTooltip()));
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QTRY_VERIFY(apply->isEnabled());
    QTest::mouseClick(apply, Qt::LeftButton);

    // Moved into the library's folder, and the library has it.
    const auto target = music + QStringLiteral("/Added/moved.flac");
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(target), 10'000);
    QVERIFY(!QFile::exists(here));
    const auto in_library = [&window, &raw, &target] {
        persistence::LibraryQuery lookup;
        lookup.kind = persistence::LibraryEntryKind::track;
        lookup.raw_path = raw(target);
        lookup.limit = 1;
        const auto found = window.remoteCatalogue()->open()->query(lookup);
        return found && found->entries.size() == 1U;
    };
    QTRY_VERIFY_WITH_TIMEOUT(in_library(), 10'000);
    testing::holdLibraryHere();
}

void BenchMainWindowTest::RemoteEngines::remoteUpNextKeepsItsIdentityAcrossARestart() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    write_wave(music + QStringLiteral("/first.wav"), wave_sample_rate * 60U);
    write_wave(music + QStringLiteral("/asked.wav"), wave_sample_rate * 60U);
    const auto engine_requests = [&remote] {
        auto client = protocol::Client::connect(protocol::Endpoint{
            .socket = remote.socket().toStdString(), .host = {}, .port = 0, .token = {}});
        std::vector<std::string> ids;
        if (!client) {
            return ids;
        }
        if (auto answer = (*client)->call("playback.requests")) {
            for (const auto& id : answer->value("entries", protocol::Json::array())) {
                ids.push_back(id.get<std::string>());
            }
        }
        (*client)->close();
        return ids;
    };

    std::string asked_id;
    std::string asked_title;
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_ && window.up_next_restored_);
        QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
        {
            auto catalogue = window.remoteCatalogue()->open();
            QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
            persistence::LibraryScanProgress progress;
            QVERIFY(catalogue->scan({}, progress).has_value());
        }
        auto* tab = window.remoteQueueTab();
        QVERIFY(tab != nullptr);
        persistence::LibraryQuery tracks;
        tracks.kind = persistence::LibraryEntryKind::track;
        const auto page = window.remoteCatalogue()->open()->query(tracks);
        QVERIFY(page && page->entries.size() == 2U);
        emit window.remoteLibrary()->browser().actionRequested({page->entries[0]},
                                                               LocalLibraryAction::replace);
        QTRY_COMPARE(window.remotePlayback()->state().status, QStringLiteral("playing"));
        window.remotePlayback()->setVolume(0);
        std::vector<LocalTrackRow> asked;
        window.remoteLibrary()->resolveEntryRows(
            {page->entries[1]},
            [&asked](std::vector<LocalTrackRow> rows) { asked = std::move(rows); });
        QTRY_COMPARE(asked.size(), std::size_t{1});
        asked_title = asked.front().title;
        window.enqueueLocalRequests(asked, -1, window.remoteEngine()->key);
        // An ask is an occurrence of its own, with an identity of its own.
        QCOMPARE(window.playback_.requests.pending().size(), std::size_t{1});
        asked_id = window.playback_.requests.pending().front().source.entry_id.to_string();
        // The engine holds it by the window's identity.
        QTRY_VERIFY(engine_requests() == std::vector<std::string>{asked_id});
        window.persistUpNext();
        QTest::qWait(300);
    }

    // Trackknife restarted: the same Up Next, on the same engine, by the same
    // identity -- so the engine and the window still agree what it is.
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_ && window.up_next_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    QCOMPARE(window.playback_.requests.pending().size(), std::size_t{1});
    QCOMPARE(window.playback_.requests.pending().front().source.entry_id.to_string(), asked_id);
    QVERIFY(window.up_next_engine_ == window.remoteEngine()->key);
    QVERIFY(engine_requests() == std::vector<std::string>{asked_id});
    // Played, it is named in the header by its title, and leaves the waiting list.
    QTRY_VERIFY(window.transport_ == window.remotePlayback());
    window.remotePlayback()->next();
    QTRY_VERIFY(window.playback_.requests.pending().empty());
    QTRY_COMPARE(window.now_playing_->text(), QString::fromStdString(asked_title));
    // The title bar -- what a taskbar shows -- names it too, and lets go of
    // it when nothing plays.
    QVERIFY2(window.windowTitle().contains(QString::fromStdString(asked_title)) &&
                 window.windowTitle().endsWith(QStringLiteral("Melody")),
             qPrintable(window.windowTitle()));
    window.remotePlayback()->stop();
    QTRY_COMPARE(window.windowTitle(), QStringLiteral("Melody"));
}

void BenchMainWindowTest::RemoteEngines::locateFindsARemoteTracksAlbumInTheRemoteLibrary() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    // All music on the remote: this computer's library hidden, as it is
    // for someone who keeps none here.
    testing::holdLibraryOnTheRemote();
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    {
        auto catalogue = window.remoteCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);
    window.tabs_->setCurrentWidget(tab->view);
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = window.remoteCatalogue()->open()->query(albums);
    QVERIFY(page && page->entries.size() == 1U);
    const auto album_name = QString::fromStdString(page->entries.front().album);
    const auto artist_name = QString::fromStdString(page->entries.front().artist);
    emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                           LocalLibraryAction::replace);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    window.remoteLibrary()->refreshLibrary();

    auto* tree = window.remoteLibrary()->findChild<QTreeView*>();
    for (const bool album : {true, false}) {
        const auto position =
            tab->view->visualRect(tab->model->index(0, local_title_column)).center();
        QVERIFY(QMetaObject::invokeMethod(tab->view, "customContextMenuRequested",
                                          Qt::DirectConnection, Q_ARG(QPoint, position)));
        auto* menu = window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"));
        auto* locate =
            menu->findChild<QAction*>(album ? QStringLiteral("action-local-locate-album")
                                            : QStringLiteral("action-local-locate-artist"));
        QVERIFY(locate != nullptr);
        QVERIFY2(locate->isEnabled(), "a remote tab's track is located in the remote's library");
        locate->trigger();
        menu->close();
        // The remote's library comes forward, on the album or its artist.
        QTRY_COMPARE(window.local_source_tabs_->tabData(window.local_source_tabs_->currentIndex())
                         .toString(),
                     window.remoteEngine()->key.text());
        QTRY_VERIFY(
            tree->currentIndex().data().toString().contains(album ? album_name : artist_name));
    }
    // The same from the menu bar, and so the command palette: the open
    // list's current track.
    const auto commands = ui::windowCommands(window);
    for (const bool album : {true, false}) {
        const auto name =
            album ? QStringLiteral("action-locate-album") : QStringLiteral("action-locate-artist");
        QVERIFY2(std::ranges::any_of(commands,
                                     [&name](const ui::PaletteCommand& command) {
                                         return command.action->objectName() == name;
                                     }),
                 "the command palette offers Locate");
        window.local_source_tabs_->setCurrentIndex(window.playlists_tab_);
        tab->view->selectionModel()->setCurrentIndex(tab->model->index(0, local_title_column),
                                                     QItemSelectionModel::ClearAndSelect |
                                                         QItemSelectionModel::Rows);
        window.refreshSelectionStatus();
        auto* locate = window.findChild<QAction*>(name);
        QVERIFY(locate != nullptr);
        QVERIFY(locate->isEnabled());
        locate->trigger();
        QTRY_COMPARE(window.local_source_tabs_->tabData(window.local_source_tabs_->currentIndex())
                         .toString(),
                     window.remoteEngine()->key.text());
        QTRY_VERIFY(
            tree->currentIndex().data().toString().contains(album ? album_name : artist_name));
    }
    window.remotePlayback()->stop();
    testing::holdLibraryHere();
}

void BenchMainWindowTest::RemoteEngines::replacingARemoteTabFromItsLibraryPlays() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    // Long enough to still be playing when looked at.
    write_wave(music + QStringLiteral("/long.wav"), wave_sample_rate * 60U);
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    {
        auto catalogue = window.remoteCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);
    window.tabs_->setCurrentWidget(tab->view);
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = window.remoteCatalogue()->open()->query(albums);
    QVERIFY(page && page->entries.size() == 1U);
    // "Replace list and play" -- from the menu, or Shift+Enter in the quick
    // album popup -- plays, on the remote as it does here.
    emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                           LocalLibraryAction::replace);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(window.remotePlayback()->state().status, QStringLiteral("playing"),
                              10'000);
    window.remotePlayback()->stop();
}

void BenchMainWindowTest::RemoteEngines::aRemoteTabGetsTagsAndCoversFromItsEngine() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    const auto art = music + QStringLiteral("/art.flac");
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), art));

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    {
        auto catalogue = window.remoteCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);
    const auto covered = [tab](const int row) {
        return row < tab->model->rowCount() && tab->model->hasArtwork(tab->model->groupKey(row));
    };

    // With the buttons: tagged from the start, and a cover from the engine.
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = window.remoteCatalogue()->open()->query(albums);
    QVERIFY(page && page->entries.size() == 1U);
    emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                           LocalLibraryAction::append);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    QCOMPARE(tab->model->rows().front().title, std::string{"Fixture Tone"});
    QTRY_VERIFY(covered(0));

    // Dropped: a bare path, filled in from the engine's index -- not looked
    // for on this computer, where without a mount it would not be.
    window.insertRemotePaths(*tab, {QFile::encodeName(art).toStdString()}, -1);
    QCOMPARE(tab->model->rowCount(), 2);
    QTRY_COMPARE(tab->model->rows()[1].title, std::string{"Fixture Tone"});
    QCOMPARE(tab->model->rows()[1].artist, std::string{"Trackknife Project"});
    QTRY_VERIFY(covered(1));

    // A path the engine does not know keeps its file name, and is asked about
    // once rather than on every look at the tab.
    window.insertRemotePaths(*tab, {"/nowhere/on/either/machine.flac"}, -1);
    QTRY_VERIFY(tab->model->rows()[2].probed);
    QCOMPARE(tab->model->rows()[2].title, std::string{"machine.flac"});

    // Rows moving between the remote's tab and this computer's are translated
    // to where this computer sees the file -- here the same path, as with the
    // NAS mounted at the same place on both -- and what is not reachable here
    // is left out, with a word why.
    auto* local =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Here",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {},
                                                    .engine = {}},
                          true);
    const auto local_id = QString::fromStdString(local->document.id.to_string());
    QVERIFY(window.transferRows(tab->view, {0, 2}, local_id, false, -1));
    QCOMPARE(local->model->rowCount(), 1);
    QCOMPARE(local->model->rows().front().raw_path, QFile::encodeName(art).toStdString());
    QCOMPARE(local->model->rows().front().title, std::string{"Fixture Tone"});
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("1 of 2")));

    // Mounted elsewhere here: the path is translated, not kept.
    const auto mounted = media.filePath(QStringLiteral("mounted-here"));
    QVERIFY(QFile::link(music, mounted));
    const auto mounted_at = [](const QString& folder, const QString& here) {
        auto engines = loadRemoteEngines();
        engines.front().music_folder = folder;
        engines.front().reachable_at = here;
        saveRemoteEngines(engines);
    };
    mounted_at(music, mounted);
    QVERIFY(window.transferRows(tab->view, {0}, local_id, false, -1));
    QCOMPARE(local->model->rows().back().raw_path,
             QFile::encodeName(mounted + QStringLiteral("/art.flac")).toStdString());

    // And back: a file of this computer's goes to the remote only when the
    // remote's library has it.
    const auto remote_id = QString::fromStdString(tab->document.id.to_string());
    const auto remote_rows = tab->model->rowCount();
    QVERIFY(window.transferRows(local->view, {1}, remote_id, false, -1));
    QCOMPARE(tab->model->rowCount(), remote_rows + 1);
    QCOMPARE(tab->model->rows().back().raw_path, QFile::encodeName(art).toStdString());
    const auto download = media.filePath(QStringLiteral("download.wav"));
    write_wave(download, wave_sample_rate);
    auto stray = local->model->rows().front();
    stray.raw_path = QFile::encodeName(download).toStdString();
    stray.entry_id = core::StableId::random();
    local->model->appendRows({stray});
    QVERIFY(
        !window.transferRows(local->view, {local->model->rowCount() - 1}, remote_id, false, -1));
    QCOMPARE(tab->model->rowCount(), remote_rows + 1);
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("not in")));

    mounted_at({}, {});

    // ADR-0286: the search dialog searches the library on the engine holding
    // it -- here the remote -- as you type, with artists, albums and tracks
    // apart, and what it finds goes to a tab of it.
    testing::holdLibraryOnTheRemote();
    window.tabs_->setCurrentWidget(tab->view);
    window.openSearchDialog();
    auto* dialog = window.search_dialog_.data();
    QVERIFY(dialog != nullptr);
    auto* scope = dialog->findChild<QComboBox*>(QStringLiteral("bench-search-scope"));
    auto* query_mode = dialog->findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
    auto* input = dialog->findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* results = dialog->findChild<QListWidget*>(QStringLiteral("bench-search-results"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-search-status"));
    QVERIFY(scope && query_mode && input && results && status);
    QCOMPARE(scope->currentData().toString(), window.remoteEngine()->key.text());
    // The library and the current tab, nothing else.
    QCOMPARE(scope->count(), 2);
    QCOMPARE(scope->itemText(0), QStringLiteral("Library"));
    QTRY_COMPARE(dialog->focusWidget(), static_cast<QWidget*>(input));
    // Opened from a tab of this computer's files: still the library's.
    window.tabs_->setCurrentWidget(local->view);
    window.openSearchDialog();
    QCOMPARE(scope->currentData().toString(), window.remoteEngine()->key.text());
    window.tabs_->setCurrentWidget(tab->view);
    window.openSearchDialog();
    QCOMPARE(scope->currentData().toString(), window.remoteEngine()->key.text());
    query_mode->setChecked(false);
    input->setText(QStringLiteral("fixture"));
    QTRY_VERIFY(status->text().contains(QStringLiteral("1 album")));
    QVERIFY(status->text().contains(QStringLiteral("1 track")));
    QListWidgetItem* album = nullptr;
    QStringList headings;
    for (int row = 0; row < results->count(); ++row) {
        auto* item = results->item(row);
        if (item->flags() == Qt::NoItemFlags) {
            headings << item->text();
        } else if (album == nullptr && item->text().contains(QStringLiteral("Trackbench"))) {
            album = item;
        }
    }
    // The artist too: an artist matches by what it has, as in the library.
    QCOMPARE(headings, (QStringList{QStringLiteral("Artists (1)"), QStringLiteral("Albums (1)"),
                                    QStringLiteral("Tracks (1)")}));
    QVERIFY(album != nullptr);
    const auto before = tab->model->rowCount();
    results->clearSelection();
    album->setSelected(true);
    emit results->itemActivated(album);
    QTRY_COMPARE(tab->model->rowCount(), before + 1);
    QCOMPARE(tab->model->rows().back().title, std::string{"Fixture Tone"});
    dialog->close();
    testing::holdLibraryHere();

    // Files opened from this computer go into a local list, never the remote
    // tab that happens to be on screen.
    {
        window.tabs_->setCurrentWidget(tab->view);
        const auto rows_before = tab->model->rowCount();
        const auto opened = media.filePath(QStringLiteral("opened-here.wav"));
        write_wave(opened, wave_sample_rate);
        window.openLocalPaths({QFile::encodeName(opened).toStdString()});
        QTRY_VERIFY(
            window.currentListTab() != nullptr &&
            EngineKey::of(window.currentListTab()->document).isLocal() &&
            std::ranges::any_of(window.currentListTab()->model->rows(), [&opened](const auto& row) {
                return row.raw_path == QFile::encodeName(opened).toStdString();
            }));
        QCOMPARE(tab->model->rowCount(), rows_before);
    }
}

// Lists are restored before the remote engine is connected, so a restored
// remote tab once asked for its covers when there was no one to ask, and
// never again: they stayed missing until another album was added.
void BenchMainWindowTest::RemoteEngines::aRestoredRemoteTabGetsItsCovers() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    const auto covered = [](BenchMainWindow::ListTab* tab) {
        return tab != nullptr && tab->model->rowCount() > 0 &&
               tab->model->hasArtwork(tab->model->groupKey(0));
    };
    const auto remote_tab = [](BenchMainWindow& window) -> BenchMainWindow::ListTab* {
        for (const auto& tab : window.list_tabs_) {
            if (!EngineKey::of(tab->document).isLocal() && tab->model->rowCount() > 0) {
                return tab.get();
            }
        }
        return nullptr;
    };
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
        auto catalogue = window.remoteCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
        persistence::LibraryQuery albums;
        albums.kind = persistence::LibraryEntryKind::album;
        const auto page = catalogue->query(albums);
        QVERIFY(page && page->entries.size() == 1U);
        emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                               LocalLibraryAction::append);
        QTRY_VERIFY(covered(remote_tab(window)));
        window.persistNow(false);
        QVERIFY(window.close());
    }
    BenchMainWindow restored;
    restored.show();
    QTRY_VERIFY(restored.lists_restored_);
    QTRY_VERIFY(remote_tab(restored) != nullptr);
    QTRY_VERIFY(covered(remote_tab(restored)));
}

// The library on an engine elsewhere, its music mounted here: files from a
// file manager dropped on its tab are its tracks where they are under its
// music folder; the rest, which only this computer reaches, go into a list
// of this computer's -- not left out.
void BenchMainWindowTest::RemoteEngines::thisComputersFilesDroppedOnARemoteTab() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music + QStringLiteral("/album")));
    QVERIFY(QDir{}.mkpath(media.filePath(QStringLiteral("downloads"))));
    const auto there = music + QStringLiteral("/album/one.wav");
    const auto here = media.filePath(QStringLiteral("downloads/two.wav"));
    write_wave(there, wave_sample_rate / 10U);
    write_wave(here, wave_sample_rate / 10U);
    testing::holdLibraryOnTheRemote();
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    QVERIFY(window.remoteCatalogue()->open()->add_root(QFile::encodeName(music).toStdString()));
    const auto* library = window.workspace_.libraryElsewhere();
    QVERIFY(library != nullptr && library->browser);
    library->browser->reload();
    QTRY_VERIFY(!library->browser->rootPaths().empty());
    auto* remote_tab = window.remoteQueueTab();
    QVERIFY(remote_tab != nullptr && !EngineKey::of(remote_tab->document).isLocal());
    window.tabs_->setCurrentWidget(remote_tab->view);
    const auto before = window.list_tabs_.size();
    const auto rows_before = remote_tab->model->rowCount();

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(there), QUrl::fromLocalFile(here)});
    auto* viewport = remote_tab->view->viewport();
    QDragEnterEvent enter{dragPosition(QPoint{10, 10}), Qt::CopyAction, &mime, Qt::LeftButton,
                          Qt::NoModifier};
    QApplication::sendEvent(viewport, &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop{QPointF{10, 10}, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(viewport, &drop);
    QVERIFY(drop.isAccepted());
    // The library's own, in its tab.
    QTRY_COMPARE(remote_tab->model->rowCount(), rows_before + 1);
    QCOMPARE(QString::fromStdString(remote_tab->model->rows().back().raw_path), there);
    // The other, in a list of this computer's.
    QTRY_COMPARE(window.list_tabs_.size(), before + 1U);
    auto* local = window.list_tabs_.back().get();
    QVERIFY(EngineKey::of(local->document).isLocal());
    QCOMPARE(local->document.name, std::string{"On this computer"});
    QTRY_COMPARE(local->model->rowCount(), 1);
    QCOMPARE(QString::fromStdString(local->model->rows().front().raw_path), here);
    QTRY_VERIFY(!window.discovery_running_);
    testing::holdLibraryHere();
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::RemoteEngines)

#include "window_remote_engines_test.moc"
