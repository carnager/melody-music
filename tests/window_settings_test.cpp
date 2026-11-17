// SPDX-License-Identifier: GPL-3.0-only

// The window test: the settings dialog, shortcuts, backup and restore.

#include "window_test.hpp"

#include "bench/file_scope_view.hpp"
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

class BenchMainWindowTest::Settings final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void shortcutSettingsValidateSaveAndCancel();
    void sharingSettingsAreHandedToTheEngineOnce();
    void aRestoreTakesBackOnlyWhatIsTicked();
    void settingsSayAndChooseThePlaylistFolder();
    void thePlaylistsTabOpensAPlaylistFile();
    void theSettingsDialogKeepsItsSize();
    void settingsGroupWhatTheyAreAbout();
    void lastFmIsHandedToTheEngine();
    void lastFmSettingsAndTrackActions();
    void upnpSettingRoundTrip();
    void playbackSettingsApplyLiveAndCancel();
    void ratingsInTagsIsAnEngineOption();
    void settingsLeftOffReadAsOff();
};

// Settings by what they are about: writing files (rename and move,
// ReplayGain, undo) as tabs of one page, ratings on their own, and the music
// folders of each engine on Library.
void BenchMainWindowTest::Settings::settingsGroupWhatTheyAreAbout() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::replaygain);
    QVERIFY(dialog != nullptr);
    auto* pages = dialog->findChild<QListWidget*>(QStringLiteral("bench-settings-pages"));
    QStringList titles;
    for (int row = 0; row < pages->count(); ++row) {
        titles << pages->item(row)->text();
    }
    QCOMPARE(titles.mid(0, 6),
             (QStringList{QStringLiteral("General"), QStringLiteral("Playback"),
                          QStringLiteral("Engines"), QStringLiteral("File operations"),
                          QStringLiteral("Ratings"), QStringLiteral("Covers")}));
    QCOMPARE(titles, SettingsSession::pageTitles().mid(0, titles.size()));
    // A tab of File operations opens that page at the tab.
    auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("bench-settings-file-operations"));
    QVERIFY(tabs != nullptr);
    QCOMPARE(pages->currentItem()->text(), QStringLiteral("File operations"));
    QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("ReplayGain"));
    QVERIFY(tabs->currentWidget()->isAncestorOf(
        dialog->findChild<QWidget*>(QStringLiteral("bench-replaygain-sidecar-only"))));
    dialog->showPage(SettingsDialog::Page::undo);
    QCOMPARE(tabs->currentWidget()->objectName(), QStringLiteral("bench-settings-undo"));
    dialog->showPage(SettingsDialog::Page::ratings);
    QCOMPARE(pages->currentItem()->text(), QStringLiteral("Ratings"));
    dialog->showPage(SettingsDialog::Page::naming);
    QCOMPARE(tabs->currentIndex(), 0);
    QCOMPARE(pages->currentItem()->text(), QStringLiteral("File operations"));
    auto* stack = dialog->findChild<QStackedWidget*>();
    QVERIFY(stack != nullptr);
    const auto page_of = [stack](const char* name) {
        auto* widget = stack->findChild<QWidget*>(QLatin1String(name));
        for (int index = 0; widget != nullptr && index < stack->count(); ++index) {
            if (stack->widget(index)->isAncestorOf(widget)) {
                return index;
            }
        }
        return -1;
    };
    QCOMPARE(page_of("bench-settings-ratings-in-tags"),
             static_cast<int>(SettingsDialog::Page::ratings));
    // ADR-0286: Engines -- this computer alone, holding the library, its
    // music folders on the same page.
    QCOMPARE(page_of("bench-settings-library-folders"),
             static_cast<int>(SettingsDialog::Page::engine));
    auto* engines = dialog->findChild<QListWidget*>(QStringLiteral("bench-settings-engines"));
    QVERIFY(engines != nullptr);
    QCOMPARE(engines->count(), 1);
    auto* holder =
        dialog->findChild<QRadioButton*>(QStringLiteral("bench-settings-library-holder-0"));
    QVERIFY(holder != nullptr && holder->isChecked());
    QVERIFY(holder->text().startsWith(QStringLiteral("This computer")));
    // ADR-0322: whether this computer's engine watches its library's
    // folders, as the engine itself says -- on, its default.
    auto* watch = dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-library-watch-box"));
    QVERIFY(watch != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(watch->isEnabled(), 10'000);
    QVERIFY(watch->isChecked());
    dialog->close();
}

void BenchMainWindowTest::Settings::shortcutSettingsValidateSaveAndCancel() {
    QSettings{}.remove(QStringLiteral("shortcuts"));
    BenchMainWindow window;
    window.show();
    auto* jump = window.findChild<QAction*>(QStringLiteral("action-jump-to-playing"));
    QVERIFY(jump);
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::shortcuts);
    auto* edit =
        dialog->findChild<QKeySequenceEdit*>(QStringLiteral("shortcut-action-jump-to-playing"));
    QVERIFY(edit);
    edit->setKeySequence(QKeySequence(QStringLiteral("Ctrl+O")));
    auto* save = dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
                     ->button(QDialogButtonBox::Save);
    save->click();
    QVERIFY(dialog->isVisible());
    QVERIFY(!dialog->findChild<QLabel*>(QStringLiteral("shortcut-conflict"))->text().isEmpty());
    QCOMPARE(jump->shortcut(), QKeySequence(QStringLiteral("Ctrl+J")));
    edit->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Alt+J")));
    save->click();
    QCOMPARE(jump->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+J")));
    QCOMPARE(QSettings{}.value(QStringLiteral("shortcuts/action-jump-to-playing")).toString(),
             QStringLiteral("Ctrl+Alt+J"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog = window.showSettingsDialog(SettingsDialog::Page::shortcuts);
    dialog->findChild<QPushButton*>(QStringLiteral("shortcut-restore-defaults"))->click();
    dialog->reject();
    QCOMPARE(jump->shortcut(),
             QKeySequence(QStringLiteral("Ctrl+Alt+J"))); // Cancel keeps live binding.
    {
        BenchMainWindow reopened;
        QCOMPARE(reopened.findChild<QAction*>(QStringLiteral("action-jump-to-playing"))->shortcut(),
                 QKeySequence(QStringLiteral("Ctrl+Alt+J")));
    }
    QSettings{}.remove(QStringLiteral("shortcuts"));
}

// ADR-0286: how this computer's engine is shared was kept in Settings and
// given as flags at every start; it is the engine's melodyd.conf now, handed
// over the first time the window meets the engine, and not again.
void BenchMainWindowTest::Settings::sharingSettingsAreHandedToTheEngineOnce() {
    engine_.stop();
    QSettings settings;
    settings.remove(QLatin1String(SettingsDialog::library_local_engine_socket_key));
    settings.remove(QLatin1String(SettingsDialog::library_engine_socket_key));
    settings.remove(QLatin1String(SettingsDialog::engine_settings_in_config_key));
    settings.setValue(QLatin1String(SettingsDialog::engine_music_root_key),
                      QStringLiteral("/srv/handed-over"));
    settings.sync();
    const auto program = qgetenv("MELODY_ENGINE");
    qputenv("MELODY_ENGINE", MELODY_ENGINE_BINARY);
    allowLocalEngine(true);
    const auto engine = localEngine();
    QVERIFY(engine.has_value());
    // A test's folder: started here, with its own melodyd.conf.
    QVERIFY(!engine->standard);
    const auto restore = qScopeGuard([&] {
        static_cast<void>(stopLocalEngine(*engine));
        allowLocalEngine(false);
        qputenv("MELODY_ENGINE", program);
        QFile::remove(QString::fromStdString((engine->state / "melodyd.conf").string()));
    });
    const auto config = [&engine] {
        QFile file{QString::fromStdString((engine->state / "melodyd.conf").string())};
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
    };
    pid_t handed_to = 0;
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QTRY_VERIFY_WITH_TIMEOUT(
            settings.value(QLatin1String(SettingsDialog::engine_settings_in_config_key), false)
                .toBool(),
            20'000);
        QVERIFY(config().contains(QStringLiteral("\nmusic-root = /srv/handed-over\n")));
        // Restarted to use it, and answering again.
        QTRY_VERIFY_WITH_TIMEOUT(
            window.localPlayback() != nullptr && window.localPlayback()->active(), 15'000);
        const auto holders = lockHolders(engine->state / "engine.lock");
        QCOMPARE(holders.size(), std::size_t{1U});
        handed_to = holders.front();
    }
    // Once: the next window finds it handed over, and leaves the engine be.
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
        QTest::qWait(1'000);
        const auto holders = lockHolders(engine->state / "engine.lock");
        QVERIFY(holders.size() == 1U && holders.front() == handed_to);
    }
    settings.remove(QLatin1String(SettingsDialog::engine_music_root_key));
}

// ADR-0296: Settings › Engines says where the engine's playlists are --
// Playlists in its music folder -- and a style chosen there is the engine's.
void BenchMainWindowTest::Settings::settingsSayAndChooseThePlaylistFolder() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    QVERIFY(window.localCatalogue()->open()->add_root(QFile::encodeName(music).toStdString()));
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::engine);
    QPointer<QDialog> guard{dialog};
    QRadioButton* standard = nullptr;
    QTRY_VERIFY((standard = dialog->findChild<QRadioButton*>(
                     QStringLiteral("bench-playlist-folder-default"))) != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(standard->isChecked() && standard->isEnabled(), 10'000);
    QVERIFY2(standard->text().endsWith(music + QStringLiteral("/Playlists")),
             qPrintable(standard->text()));
    auto* style = dialog->findChild<QComboBox*>(QStringLiteral("bench-playlist-folder-style"));
    QVERIFY(style != nullptr);
    QCOMPARE(style->currentData().toString(), QStringLiteral("root"));
    const auto absolute = style->findData(QStringLiteral("absolute"));
    style->setCurrentIndex(absolute);
    emit style->activated(absolute);
    QTRY_VERIFY_WITH_TIMEOUT(!style->isEnabled(), 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(style->isEnabled(), 10'000);
    // The engine's now: asked again, it says so.
    auto* again = new PlaylistFolderSession(window.localEngine().file_work, {}, &window);
    again->load();
    QTRY_VERIFY_WITH_TIMEOUT(again->loaded() && again->style() == "absolute", 10'000);
    // A folder typed -- on the engine's machine -- is taken as one chosen.
    QTemporaryDir elsewhere;
    QVERIFY(elsewhere.isValid());
    auto* path = dialog->findChild<QLineEdit*>(QStringLiteral("bench-playlist-folder-path"));
    QVERIFY(path != nullptr && !path->isReadOnly());
    path->setText(elsewhere.path());
    emit path->textEdited(path->text());
    emit path->returnPressed();
    auto* chosen = dialog->findChild<QRadioButton*>(QStringLiteral("bench-playlist-folder-chosen"));
    QTRY_VERIFY_WITH_TIMEOUT(chosen->isChecked() && chosen->isEnabled(), 10'000);
    // One that is not a folder there is refused, and stays to be mended.
    path->setText(elsewhere.filePath(QStringLiteral("not-there")));
    emit path->textEdited(path->text());
    emit path->returnPressed();
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-playlist-folder-status"));
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("Not changed")), 10'000);
    QCOMPARE(path->text(), elsewhere.filePath(QStringLiteral("not-there")));
    if (guard) {
        dialog->reject();
    }
}

// ADR-0296, ADR-0298: the library's Playlists tab, its engine's playlists --
// a file someone dropped in the playlist folder too -- one opened as a tab by
// choosing it.
void BenchMainWindowTest::Settings::thePlaylistsTabOpensAPlaylistFile() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music + QStringLiteral("/Playlists")));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    // As MPD writes it: the track named under the music folder.
    {
        QFile mix{music + QStringLiteral("/Playlists/mix.m3u")};
        QVERIFY(mix.open(QIODevice::WriteOnly));
        mix.write("art.flac\n");
    }
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    QVERIFY(window.localCatalogue()->open()->add_root(QFile::encodeName(music).toStdString()));

    // The library's Playlists tab: its engine's playlists.
    auto* sources = window.local_source_tabs_;
    auto* playlists = window.findChild<ListsPanel*>(QStringLiteral("bench-library-playlists"));
    QVERIFY(playlists != nullptr);
    QTRY_VERIFY(window.localLibrary() != nullptr);
    sources->setCurrentIndex(0);
    QTRY_VERIFY(!playlists->isVisibleTo(&window));
    sources->setCurrentIndex(window.playlists_tab_);
    QTRY_VERIFY(playlists->isVisibleTo(&window));
    // The engine reads its folder every few seconds; the list then shows.
    QTreeWidgetItem* found = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((found = [playlists]() -> QTreeWidgetItem* {
                                 const auto items =
                                     playlists->findItems(QStringLiteral("mix"),
                                                          Qt::MatchStartsWith | Qt::MatchRecursive);
                                 return items.isEmpty() ? nullptr : items.front();
                             }()) != nullptr,
                             15'000);
    const auto before = window.list_tabs_.size();
    playlists->setCurrentItem(found);
    QTRY_COMPARE(window.list_tabs_.size(), before + 1);
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    QCOMPARE(QString::fromStdString(tab->model->rows().front().raw_path),
             music + QStringLiteral("/art.flac"));
    // The engine's own only, flat.
    QCOMPARE(playlists->topLevelItemCount(), 1);
    QCOMPARE(playlists->topLevelItem(0)->text(0), QStringLiteral("mix"));
}

// Settings opens at the size it was last left at.
void BenchMainWindowTest::Settings::theSettingsDialogKeepsItsSize() {
    BenchMainWindow window;
    window.show();
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::general);
    // Within the offscreen screen, which restoreGeometry() keeps it to.
    const QSize size{713, 517};
    QVERIFY(dialog->size() != size);
    dialog->resize(size);
    dialog->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog = window.showSettingsDialog(SettingsDialog::Page::general);
    QCOMPARE(dialog->size(), size);
    dialog->reject();
}

void BenchMainWindowTest::Settings::lastFmSettingsAndTrackActions() {
    BenchMainWindow window;
    window.show();
    // The service talks to an endpoint that accepts and never answers, so no
    // request leaves this machine and no real reply races the ones fed below.
    QTcpServer silent;
    QVERIFY(silent.listen(QHostAddress::LocalHost));
    QTemporaryDir state;
    QVERIFY(state.isValid());
    delete window.lastfm_;
    window.lastfm_ =
        new LastFmService(state.filePath(QStringLiteral("lastfm.json")), &window,
                          QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(silent.serverPort())));
    const auto reply = [&window](const QString& op, const QByteArray& json) {
        emit window.lastfm_->completed(op, QJsonDocument::fromJson(json).object(), {});
    };
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::lastfm);
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("lastfm-status"));
    auto* secret = dialog->findChild<QLineEdit*>(QStringLiteral("lastfm-account-secret"));
    QVERIFY(status && secret);
    QCOMPARE(secret->echoMode(), QLineEdit::Password);
    QTRY_VERIFY(status->text().contains(QStringLiteral("Not connected")));
    auto* key = dialog->findChild<QLineEdit*>(QStringLiteral("lastfm-account-key"));
    auto* begin = dialog->findChild<QPushButton*>(QStringLiteral("lastfm-authorize"));
    auto* cancel = dialog->findChild<QPushButton*>(QStringLiteral("lastfm-cancel"));
    auto* poll = dialog->findChild<QTimer*>(QStringLiteral("lastfm-auth-poll"));
    auto* deadline = dialog->findChild<QTimer*>(QStringLiteral("lastfm-auth-deadline"));
    QVERIFY(key && begin && cancel && poll && deadline);
    // ADR-0350: Last.fm dynamic playlists use the account an engine is
    // given; there is no key of the window's own to share.
    QVERIFY(!dialog->findChild<QCheckBox*>(QStringLiteral("lastfm-reuse-key")));
    QVERIFY(!dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-lastfm-key")));
    QVERIFY(!dialog->findChild<QPushButton*>(QStringLiteral("lastfm-finish")));
    QVERIFY(cancel->isHidden());
    key->setText(QStringLiteral("invalid"));
    begin->click();
    QVERIFY(status->text().contains(QStringLiteral("32-character")));
    key->setText(QString(32, 'a'));
    secret->setText(QString(32, 'b'));
    begin->click();
    QVERIFY(!QSettings{}.contains(QStringLiteral("lastfm/api-key")));
    reply(QStringLiteral("status"),
          QByteArray(R"({"credentials_saved":true,"authorization_pending":true})"));
    QVERIFY(dialog->findChild<QWidget*>(QStringLiteral("lastfm-credentials"))->isHidden());
    QVERIFY(!poll->isActive());
    QVERIFY(begin->property("credentials-saved").toBool());
    // Feed deterministic replies without opening a real browser or contacting Last.fm.
    auto* page = dialog->findChild<QWidget*>(QStringLiteral("lastfm-settings"));
    auto* session = page->findChild<LastFmSettingsSession*>();
    QVERIFY(session != nullptr);
    session->setWaiting(true);
    reply(QStringLiteral("begin"),
          QByteArray(R"({"credentials_saved":true,"authorization_pending":true})"));
    QVERIFY(poll->isActive());
    QVERIFY(status->text().contains(QStringLiteral("Waiting for browser approval")));
    reply(QStringLiteral("finish"),
          QByteArray(R"({"credentials_saved":true,"authorization_pending":true})"));
    QVERIFY(poll->isActive());
    reply(QStringLiteral("finish"),
          QByteArray(
              R"({"credentials_saved":true,"connected":true,"user":"listener","enabled":true})"));
    QVERIFY(!poll->isActive());
    QVERIFY(!session->waiting());
    QVERIFY(dialog->findChild<QCheckBox*>(QStringLiteral("lastfm-enabled"))->isChecked());
    session->setWaiting(true);
    poll->start();
    cancel->click();
    QVERIFY(!poll->isActive());
    QVERIFY(status->text().contains(QStringLiteral("Stopped waiting")));
    // A late reply after cancellation must not restart polling.
    reply(QStringLiteral("finish"), QByteArray(R"({"authorization_pending":true})"));
    QVERIFY(!poll->isActive());
    session->setWaiting(true);
    poll->start();
    QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection);
    QVERIFY(!poll->isActive());
    QVERIFY(status->text().contains(QStringLiteral("timed out")));
    QVERIFY(cancel->isHidden());
    dialog->close();
    LocalListModel model;
    LocalTrackRow track;
    track.artist = "Artist";
    track.title = "Title";
    track.raw_path = "/test.flac";
    model.appendRows({track});
    QTableView view;
    view.setModel(&model);
    view.selectRow(0);
    QMenu menu;
    window.addLastFmActions(&menu, &view);
    QCOMPARE(menu.actions().size(), 1);
    auto* submenu = menu.actions().first()->menu();
    QVERIFY(submenu && submenu->isEnabled());
    QCOMPARE(submenu->actions()[1]->text(), QStringLiteral("Love track"));
    QCOMPARE(submenu->actions()[2]->text(), QStringLiteral("Unlove track"));
}

// ADR-0220: the account signed in here is handed to an engine, which then
// scrobbles what it plays itself -- and this window stops crediting it, or
// every listen would count twice.
void BenchMainWindowTest::Settings::lastFmIsHandedToTheEngine() {
    {
        const auto path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                          QStringLiteral("/lastfm-v1.json");
        QDir{}.mkpath(QFileInfo{path}.absolutePath());
        QFile file{path};
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument{QJsonObject{{"version", 1},
                                             {"key", QString(32, QLatin1Char('k'))},
                                             {"secret", QString(32, QLatin1Char('s'))},
                                             {"session", "session-key"},
                                             {"user", "listener"},
                                             {"enabled", true}}}
                       .toJson());
    }
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    QVERIFY(!window.localPlayback()->scrobblesItself());

    QWidget host;
    auto* page = window.buildLastFmSettings(&host);
    auto* use = page->findChild<QPushButton*>(QStringLiteral("lastfm-engine-local-use"));
    auto* state = page->findChild<QLabel*>(QStringLiteral("lastfm-engine-local-state"));
    QVERIFY(use != nullptr && state != nullptr);
    QTRY_COMPARE(state->text(), QStringLiteral("Not scrobbling"));
    use->click();
    QTRY_COMPARE(state->text(), QStringLiteral("Scrobbling as listener"));
    QTRY_VERIFY(window.localPlayback()->scrobblesItself());
    // The engine has this account now: handing it over again would do nothing.
    QTRY_COMPARE(use->text(), QStringLiteral("In use"));
    QVERIFY(!use->isEnabled());
    // And a fresh look at the page says the same.
    {
        QWidget again_host;
        auto* again = window.buildLastFmSettings(&again_host);
        auto* again_use = again->findChild<QPushButton*>(QStringLiteral("lastfm-engine-local-use"));
        QTRY_COMPARE(again_use->text(), QStringLiteral("In use"));
    }

    // The window no longer credits what that engine plays.
    EnginePlayback::State playing;
    playing.status = QStringLiteral("playing");
    playing.position_ms = 1'000;
    playing.duration_ms = 200'000;
    window.lastfm_sample_time_ = -1'000'000;
    window.sampleLastFmFromEngine(playing);
    QCOMPARE(window.findChild<trackknife::bench::Workspace*>()
                 ->property("trackknife-lastfm-sample")
                 .toString(),
             QStringLiteral("engine"));
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                  QStringLiteral("/lastfm-v1.json"));
}

// ADR-0237 stage 2: writing ratings into the files is an option in
// Settings, off by default, handed to the engines that keep the ratings.
void BenchMainWindowTest::Settings::ratingsInTagsIsAnEngineOption() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    auto client = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(client.has_value());
    const auto writes_tags = [&client] {
        auto answer = (*client)->call("ratings.tags");
        return answer && answer->value("write_tags", false);
    };
    QVERIFY(!writes_tags());
    const auto choose = [&window](const bool on) {
        window.findChild<QAction*>(QStringLiteral("action-settings"))->trigger();
        auto* dialog = window.findChild<SettingsDialog*>();
        QVERIFY(dialog != nullptr);
        auto* option =
            dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-ratings-in-tags"));
        QVERIFY(option != nullptr);
        option->setChecked(on);
        QPointer<SettingsDialog> lifetime = dialog;
        dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
            ->button(QDialogButtonBox::Save)
            ->click();
        QTRY_VERIFY(lifetime.isNull());
    };
    const auto scale = [&client] {
        auto answer = (*client)->call("ratings.tags");
        return answer ? answer->value("rating_scale", std::string{}) : std::string{};
    };
    QCOMPARE(scale(), std::string{"off"});
    choose(true);
    QVERIFY(QSettings{}.value(QLatin1String(SettingsDialog::ratings_in_tags_key)).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(writes_tags(), 5'000);
    choose(false);
    QTRY_VERIFY_WITH_TIMEOUT(!writes_tags(), 5'000);

    // Other players' plain RATING tags: read on the scale chosen.
    const auto choose_scale = [&window](const QString& value) {
        window.findChild<QAction*>(QStringLiteral("action-settings"))->trigger();
        auto* dialog = window.findChild<SettingsDialog*>();
        QVERIFY(dialog != nullptr);
        auto* combo =
            dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-rating-tag-scale"));
        QVERIFY(combo != nullptr);
        combo->setCurrentIndex(combo->findData(value));
        QPointer<SettingsDialog> lifetime = dialog;
        dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
            ->button(QDialogButtonBox::Save)
            ->click();
        QTRY_VERIFY(lifetime.isNull());
    };
    choose_scale(QStringLiteral("100"));
    QTRY_COMPARE_WITH_TIMEOUT(scale(), std::string{"100"}, 5'000);
    choose_scale(QStringLiteral("off"));
    QTRY_COMPARE_WITH_TIMEOUT(scale(), std::string{"off"}, 5'000);

    // ADR-0245: the backup tag, named in Settings, is the engine's; an
    // official name is written but warned about.
    const auto backup = [&client] {
        auto answer = (*client)->call("ratings.tags");
        return answer ? answer->value("backup_tag", std::string{"?"}) : std::string{"?"};
    };
    QCOMPARE(backup(), std::string{});
    {
        window.findChild<QAction*>(QStringLiteral("action-settings"))->trigger();
        auto* dialog = window.findChild<SettingsDialog*>();
        QVERIFY(dialog != nullptr);
        auto* writing =
            dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-ratings-in-tags"));
        auto* copy = dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-rating-backup"));
        auto* name =
            dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-rating-backup-tag"));
        auto* note =
            dialog->findChild<QLabel*>(QStringLiteral("bench-settings-rating-backup-note"));
        QVERIFY(writing && copy && name && note);
        QVERIFY(!copy->isEnabled());
        writing->setChecked(true);
        QVERIFY(copy->isEnabled());
        copy->setChecked(true);
        QCOMPARE(name->text(), QStringLiteral("MELODY_RATING"));
        QVERIFY(note->isHidden());
        name->setText(QStringLiteral("COMMENT"));
        QVERIFY(!note->isHidden() && note->text().contains(QStringLiteral("official")));
        QPointer<SettingsDialog> lifetime = dialog;
        dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
            ->button(QDialogButtonBox::Save)
            ->click();
        QTRY_VERIFY(lifetime.isNull());
    }
    QTRY_COMPARE_WITH_TIMEOUT(backup(), std::string{"COMMENT"}, 5'000);
    (*client)->close();
}

void BenchMainWindowTest::Settings::playbackSettingsApplyLiveAndCancel() {
    BenchMainWindow window;
    window.show();
    auto* action = window.findChild<QAction*>(QStringLiteral("action-settings"));
    QVERIFY(action);
    action->trigger();
    auto* dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    dialog->showPage(SettingsDialog::Page::playback);
    auto* profile = dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-buffer-profile"));
    auto* capacity = dialog->findChild<QSpinBox*>(QStringLiteral("bench-settings-buffer-capacity"));
    auto* threshold =
        dialog->findChild<QSpinBox*>(QStringLiteral("bench-settings-buffer-threshold"));
    auto* with_gain =
        dialog->findChild<QDoubleSpinBox*>(QStringLiteral("bench-settings-preamp-with"));
    auto* without_gain =
        dialog->findChild<QDoubleSpinBox*>(QStringLiteral("bench-settings-preamp-without"));
    auto* notifications =
        dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-notifications"));
    QVERIFY(profile && capacity && threshold && with_gain && without_gain && notifications);
    QCOMPARE(profile->currentData().toString(), QStringLiteral("balanced"));
    QCOMPARE(capacity->value(), 750);
    QVERIFY(!capacity->isEnabled());
    profile->setCurrentIndex(profile->findData(QStringLiteral("resilient")));
    QCOMPARE(capacity->value(), 2000);
    QCOMPARE(threshold->value(), 250);
    profile->setCurrentIndex(profile->findData(QStringLiteral("custom")));
    QVERIFY(capacity->isEnabled());
    capacity->setValue(100);
    QCOMPARE(threshold->maximum(), 100);
    QCOMPARE(threshold->value(), 100);
    capacity->setValue(1234);
    threshold->setValue(234);
    with_gain->setValue(-3.5);
    without_gain->setValue(-6.0);
    notifications->setChecked(true);
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/settings-playback.png")));
        dialog->showPage(SettingsDialog::Page::general);
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/settings-general.png")));
    }
    auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"));
    QVERIFY(buttons);
    QPointer<SettingsDialog> lifetime = dialog;
    buttons->button(QDialogButtonBox::Save)->click();
    QTRY_VERIFY(lifetime.isNull());
    QTRY_COMPARE(window.property("trackknife-player-buffer-capacity-ms").toLongLong(), 1234);
    QTRY_COMPARE(window.property("trackknife-player-rg-preamp-with").toDouble(), -3.5);
    QTRY_COMPARE(window.property("trackknife-player-rg-preamp-without").toDouble(), -6.0);
    auto* notification_action =
        window.findChild<QAction*>(QStringLiteral("action-desktop-notifications"));
    QVERIFY(notification_action && notification_action->isChecked());
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-buffer-custom"))->isChecked());
    QCOMPARE(QSettings{}.value(QStringLiteral("playback/buffer-start-threshold-ms")).toInt(), 234);

    action->trigger();
    dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    profile = dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-buffer-profile"));
    QCOMPARE(profile->currentData().toString(), QStringLiteral("custom"));
    with_gain = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("bench-settings-preamp-with"));
    QCOMPARE(with_gain->value(), -3.5);
    profile->setCurrentIndex(profile->findData(QStringLiteral("responsive")));
    with_gain->setValue(10);
    dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-notifications"))
        ->setChecked(false);
    lifetime = dialog;
    dialog->reject();
    QTRY_VERIFY(lifetime.isNull());
    QCOMPARE(QSettings{}.value(QStringLiteral("playback/buffer-profile")).toString(),
             QStringLiteral("custom"));
    QCOMPARE(QSettings{}.value(QStringLiteral("playback/rg-preamp-with")).toDouble(), -3.5);
    QVERIFY(notification_action->isChecked());
    QCOMPARE(window.property("trackknife-player-buffer-capacity-ms").toLongLong(), 1234);
    // Existing shortcuts and Settings read the same preference values.
    window.findChild<QAction*>(QStringLiteral("action-buffer-responsive"))->trigger();
    notification_action->setChecked(false);
    action->trigger();
    dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-buffer-profile"))
                 ->currentData()
                 .toString(),
             QStringLiteral("responsive"));
    QVERIFY(!dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-notifications"))
                 ->isChecked());
    dialog->reject();
}

// ADR-0235: the UPnP choice is saved and shown as saved when Settings opens
// again -- it was saved, and then shown off.
void BenchMainWindowTest::Settings::upnpSettingRoundTrip() {
#if MELODY_ENABLE_UPNP
    QSettings{}.remove(QStringLiteral("engine/upnp"));
    {
        SettingsDialog dialog;
        dialog.showPage(SettingsDialog::Page::engine);
        auto* upnp = dialog.findChild<QCheckBox*>(QStringLiteral("bench-settings-engine-upnp"));
        auto* buttons =
            dialog.findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"));
        QVERIFY(upnp != nullptr && buttons != nullptr);
        QVERIFY(upnp->isEnabled() && !upnp->isChecked());
        upnp->setChecked(true);
        buttons->button(QDialogButtonBox::Save)->click();
    }
    QVERIFY(QSettings{}.value(QStringLiteral("engine/upnp")).toBool());
    SettingsDialog reopened;
    QVERIFY(
        reopened.findChild<QCheckBox*>(QStringLiteral("bench-settings-engine-upnp"))->isChecked());
    reopened.reject();
    QSettings{}.remove(QStringLiteral("engine/upnp"));
#else
    QSKIP("built without UPnP");
#endif
}

// An INI file hands every value back as text, and the text "false" is true to
// anything that only asks whether a value is there: the Qt Quick window once
// showed every option left off as on. Settings come out as their type.
void BenchMainWindowTest::Settings::settingsLeftOffReadAsOff() {
    QSettings stored;
    stored.setValue(QLatin1String(SettingsKeys::ratings_in_tags_key), QStringLiteral("false"));
    stored.setValue(QLatin1String(SettingsKeys::rating_backup_key), QStringLiteral("false"));
    stored.setValue(QStringLiteral("playback/rg-preamp-with"), QStringLiteral("-3.5"));
    stored.setValue(QLatin1String(SettingsKeys::rating_tag_scale_key), QStringLiteral("10"));
    stored.sync();
    const SettingsSession session;
    const auto& values = session.values();
    const auto in_tags = values.value(QLatin1String(SettingsKeys::ratings_in_tags_key));
    QCOMPARE(in_tags.metaType(), QMetaType::fromType<bool>());
    QVERIFY(!in_tags.toBool());
    const auto backup = values.value(QLatin1String(SettingsKeys::rating_backup_key));
    QCOMPARE(backup.metaType(), QMetaType::fromType<bool>());
    QVERIFY(!backup.toBool());
    const auto preamp = values.value(QStringLiteral("playback/rg-preamp-with"));
    QCOMPARE(preamp.metaType(), QMetaType::fromType<double>());
    QCOMPARE(preamp.toDouble(), -3.5);
    QCOMPARE(values.value(QLatin1String(SettingsKeys::rating_tag_scale_key)).toString(),
             QStringLiteral("10"));
    stored.remove(QLatin1String(SettingsKeys::ratings_in_tags_key));
    stored.remove(QLatin1String(SettingsKeys::rating_backup_key));
    stored.remove(QStringLiteral("playback/rg-preamp-with"));
    stored.remove(QLatin1String(SettingsKeys::rating_tag_scale_key));
}

// A backup is Melody's presets, settings and tabs; a restore puts back the
// parts ticked and never the engine's database.
void BenchMainWindowTest::Settings::aRestoreTakesBackOnlyWhatIsTicked() {
    const auto engine_database = core::engine_state_directory() / "lists.sqlite";
    QTemporaryDir backups;
    QVERIFY(backups.isValid());
    const auto backup = backups.filePath(QStringLiteral("melody-workspace.sqlite"));
    QSettings{}.setValue(QStringLiteral("test/restore-probe"), QStringLiteral("backed up"));
    {
        BenchMainWindow window;
        window.show();
        auto* open_tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(open_tabs->count(), 1);
        window.workspace_.backupWorkspace(backup);
        QTRY_VERIFY(QFile::exists(backup + QStringLiteral(".settings.ini")));
        QVERIFY(QFile::exists(backup));
        QVERIFY(QDir{backup + QStringLiteral(".tabs")}.exists());

        // Every part it holds is offered and ticked; none ticked, nothing to do.
        window.chooseWorkspaceRestore(backup);
        QDialog* dialog = nullptr;
        QTRY_VERIFY((dialog = window.findChild<QDialog*>(
                         QStringLiteral("bench-restore-workspace"))) != nullptr);
        auto* presets = dialog->findChild<QCheckBox*>(QStringLiteral("restore-presets"));
        auto* settings = dialog->findChild<QCheckBox*>(QStringLiteral("restore-settings"));
        auto* tabs = dialog->findChild<QCheckBox*>(QStringLiteral("restore-tabs"));
        auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("restore-apply"));
        QVERIFY(presets->isChecked() && settings->isChecked() && tabs->isChecked());
        for (auto* box : {presets, settings, tabs}) {
            box->setChecked(false);
        }
        QVERIFY(!apply->isEnabled());
        presets->setChecked(true);
        settings->setChecked(true);
        apply->click();
    }

    // What has changed since: the presets, a setting, the open tabs.
    const auto live = core::presets_database_path();
    {
        auto repository = persistence::ListRepository::open(live);
        QVERIFY(repository.has_value());
        QVERIFY(repository
                    ->save_search(persistence::SavedSearch{.id = core::StableId::random(),
                                                           .name = "Changed since",
                                                           .expression = "artist IS x"})
                    .has_value());
    }
    const auto backed_up_tabs =
        QSettings{backup + QStringLiteral(".settings.ini"), QSettings::IniFormat}.value(
            QStringLiteral("values/tabs/v1"));
    {
        QSettings settings;
        settings.setValue(QStringLiteral("test/restore-probe"), QStringLiteral("changed"));
        settings.setValue(QStringLiteral("tabs/v1"), QStringLiteral("open now"));
    }
    const auto engine_before = std::filesystem::exists(engine_database)
                                   ? std::filesystem::last_write_time(engine_database)
                                   : std::filesystem::file_time_type{};

    const auto notice = applyPendingWorkspaceRestore();
    QVERIFY2(notice.startsWith(QStringLiteral("Restored presets, settings from")),
             qPrintable(notice));
    {
        auto restored = persistence::ListRepository::open(live);
        auto original = persistence::ListRepository::open(
            std::filesystem::path{QFile::encodeName(backup).toStdString()});
        QVERIFY(restored.has_value() && original.has_value());
        QCOMPARE(restored->load_saved_searches().value(), original->load_saved_searches().value());
    }
    QCOMPARE(QSettings{}.value(QStringLiteral("test/restore-probe")).toString(),
             QStringLiteral("backed up"));
    // The tabs were not ticked: those open now stay.
    QCOMPARE(QSettings{}.value(QStringLiteral("tabs/v1")).toString(), QStringLiteral("open now"));
    const auto kept =
        QDir{QFile::decodeName(QByteArray::fromStdString(core::melody_data_directory().native()))}
            .entryList({QStringLiteral("*-before-restore-*")});
    QCOMPARE(kept.size(), 2);
    // Nothing pending is applied twice.
    QVERIFY(applyPendingWorkspaceRestore().isEmpty());

    // Then the tabs alone.
    scheduleWorkspaceRestore({.from = backup, .tabs = true});
    QSettings{}.setValue(QStringLiteral("test/restore-probe"), QStringLiteral("changed again"));
    const auto tabs_notice = applyPendingWorkspaceRestore();
    QVERIFY2(tabs_notice.startsWith(QStringLiteral("Restored tabs from")), qPrintable(tabs_notice));
    QVERIFY(backed_up_tabs.isValid());
    QCOMPARE(QSettings{}.value(QStringLiteral("tabs/v1")), backed_up_tabs);
    QCOMPARE(QSettings{}.value(QStringLiteral("test/restore-probe")).toString(),
             QStringLiteral("changed again"));

    // The engine's database is the engine's.
    QCOMPARE(std::filesystem::exists(engine_database)
                 ? std::filesystem::last_write_time(engine_database)
                 : std::filesystem::file_time_type{},
             engine_before);
    QVERIFY(
        QDir{QFile::decodeName(QByteArray::fromStdString(core::engine_state_directory().native()))}
            .entryList({QStringLiteral("lists-before-restore-*")})
            .isEmpty());
    QSettings{}.remove(QStringLiteral("test/restore-probe"));
    QSettings{}.remove(QStringLiteral("tabs/v1"));
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::Settings)

#include "window_settings_test.moc"
