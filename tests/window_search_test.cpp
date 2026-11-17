// SPDX-License-Identifier: GPL-3.0-only

// The window test: searching: the search dialog, quick find and the command palette.

#include "window_test.hpp"

#include "trackknife/core/engine_state.hpp"

namespace trackknife::bench {

class BenchMainWindowTest::Search final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void quickAlbumFindsByWordsAndPutsItAway();
    void quickAlbumShiftEnterReplacesAndPlays();
    void quickTrackFindsATrackByItsTitle();
    void commandPaletteFindsAndRunsRegisteredActions();
    void commandPaletteTracksAvailabilityAndLifetime();
    void commandPaletteOffersEveryMenuEntry();
    void commandPaletteHidesWhatAGreyedMenuHolds();
    void everyCommandTakesAKeyAndCtrlLSearchesTheLibrary();
    void savedSearchesCanBeManagedAndReopened();
    void searchDialogFiltersTabAndOpensResults();
    void searchResultTabLoadsCovers();
    void searchDialogProbesMissingTechnicalsOnDemand();
    void searchPresetsAreGroupedAndPrompt();
    void quickAlbumSearchesTheLibraryChosen();
    void aKeptSearchOpensAtOnce();
};

void BenchMainWindowTest::Search::commandPaletteFindsAndRunsRegisteredActions() {
    BenchMainWindow window;
    window.show();

    auto* command = window.findChild<QAction*>(QStringLiteral("action-command-palette"));
    QVERIFY(command != nullptr);
    QCOMPARE(command->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+P")));
    // Actions in no menu and not the window's own -- transient choices --
    // are not commands. Named so no real speaker matches: the menus' are
    // offered (ADR-0345), and this computer's is named after it.
    QAction device(QStringLiteral("stray-device-choice"), &window);
    device.setObjectName(QStringLiteral("action-output-123"));
    QAction rating(QStringLiteral("1"), &window);
    rating.setObjectName(QStringLiteral("action-local-rate-1"));
    command->trigger();

    auto* palette = window.findChild<QDialog*>(QStringLiteral("command-palette"));
    QVERIFY(palette != nullptr);
    auto* filter = palette->findChild<QLineEdit*>(QStringLiteral("command-filter"));
    auto* results = palette->findChild<QListWidget*>(QStringLiteral("command-results"));
    auto* run = palette->findChild<QPushButton*>(QStringLiteral("command-run"));
    QVERIFY(filter != nullptr);
    QVERIFY(results != nullptr);
    QVERIFY(run != nullptr);
    filter->setText(QStringLiteral("stray-device-choice"));
    QCOMPARE(results->count(), 0);
    filter->setText(QStringLiteral("queue-rate"));
    QCOMPARE(results->count(), 0);
    QVERIFY(palette->findChild<QKeySequenceEdit*>() == nullptr);
    command->trigger();
    QCOMPARE(window.findChildren<ui::CommandPalette*>().size(), 1);

    filter->setText(QStringLiteral("Settings"));
    QCOMPARE(results->count(), 1);
    QVERIFY(results->currentItem()->text().startsWith(QStringLiteral("Settings")));
    run->click();
    QTRY_VERIFY(window.findChild<SettingsDialog*>() != nullptr);
    QVERIFY(!palette->isVisible());
}

void BenchMainWindowTest::Search::commandPaletteTracksAvailabilityAndLifetime() {
    QAction first{QStringLiteral("Open files")};
    first.setObjectName(QStringLiteral("action-open-files"));
    auto second = std::make_unique<QAction>(QStringLiteral("Open folder"));
    second->setObjectName(QStringLiteral("action-open-folder"));
    second->setEnabled(false);
    QSignalSpy triggered{&first, &QAction::triggered};
    ui::CommandPalette palette({&first, &first, second.get()});
    palette.show();
    auto* filter = palette.findChild<QLineEdit*>(QStringLiteral("command-filter"));
    auto* results = palette.findChild<QListWidget*>(QStringLiteral("command-results"));
    auto* run = palette.findChild<QPushButton*>(QStringLiteral("command-run"));
    QCOMPARE(results->count(), 1);
    second->setEnabled(true);
    QCOMPARE(results->count(), 2);
    QTest::keyClick(filter, Qt::Key_Down);
    QCOMPARE(results->currentRow(), 1);
    QVERIFY(run->isEnabled());
    second->setEnabled(false);
    QCOMPARE(results->count(), 1);
    QCOMPARE(results->currentRow(), 0);
    second->setEnabled(true);
    QCOMPARE(results->count(), 2);
    second.reset();
    QCOMPARE(results->count(), 1);
    filter->setText(QStringLiteral("files open"));
    QCOMPARE(results->count(), 1);
    first.setEnabled(false);
    QCOMPARE(results->count(), 0);
    QVERIFY(!run->isEnabled());
    QTest::keyClick(filter, Qt::Key_Return);
    QCOMPARE(triggered.count(), 0);
    first.setEnabled(true);
    QCOMPARE(results->count(), 1);
    first.setVisible(false);
    QCOMPARE(results->count(), 0);
    QVERIFY(!run->isEnabled());
    first.setVisible(true);
    QTest::keyClick(filter, Qt::Key_Return);
    QCOMPARE(triggered.count(), 1);
    QVERIFY(!palette.isVisible());
}

// ADR-0345: the palette offers what the menus hold, submenus included, and
// the window's own keys -- nothing kept in a list of its own to forget.
void BenchMainWindowTest::Search::commandPaletteOffersEveryMenuEntry() {
    BenchMainWindow window;
    window.show();
    const auto commands = ui::windowCommands(window);
    const auto offered = [&commands](const QAction* action) {
        return std::ranges::any_of(commands, [action](const ui::PaletteCommand& command) {
            return command.action == action;
        });
    };
    // Everything the old list named is still there.
    for (const auto* id : workspace_command_ids) {
        auto* action = window.findChild<QAction*>(QString::fromLatin1(id));
        QVERIFY2(action != nullptr, id);
        QVERIFY2(offered(action), id);
    }
    // And each command once.
    QSet<const QAction*> once;
    for (const auto& command : commands) {
        QVERIFY(!once.contains(command.action.data()));
        once.insert(command.action.data());
    }

    window.findChild<QAction*>(QStringLiteral("action-command-palette"))->trigger();
    auto* palette = window.findChild<QDialog*>(QStringLiteral("command-palette"));
    QVERIFY(palette != nullptr);
    auto* filter = palette->findChild<QLineEdit*>(QStringLiteral("command-filter"));
    auto* results = palette->findChild<QListWidget*>(QStringLiteral("command-results"));
    const auto shown = [results] {
        QStringList texts;
        for (int row = 0; row < results->count(); ++row) {
            texts.append(results->item(row)->text());
        }
        return texts;
    };
    // The modes, only in the Playback menu and on the status bar before.
    filter->setText(QStringLiteral("repeat"));
    QVERIFY(shown().contains(QStringLiteral("Repeat")));
    // A submenu's entry is named under it; the menu bar's menu is searched
    // but not shown.
    filter->setText(QStringLiteral("playback output refresh"));
    QCOMPARE(shown(), QStringList{QStringLiteral("Output › Refresh audio devices")});
    filter->setText(QStringLiteral("buffer resilient"));
    QCOMPARE(shown().size(), 1);
    QVERIFY(shown().front().startsWith(QStringLiteral("Playback buffer › Resilient")));
    // The palette does not offer itself.
    filter->setText(QStringLiteral("Commands"));
    QCOMPARE(results->count(), 0);
    // No engine elsewhere, no choice of where the library is.
    filter->setText(QStringLiteral("library this computer"));
    QCOMPARE(results->count(), 0);
}

void BenchMainWindowTest::Search::commandPaletteHidesWhatAGreyedMenuHolds() {
    QMainWindow window;
    auto* menu = window.menuBar()->addMenu(QStringLiteral("&Edit"));
    auto* sort = menu->addMenu(QStringLiteral("Sort list"));
    auto* by_artist = sort->addAction(QStringLiteral("By artist"));
    menu->addSeparator();
    auto* heading = new QWidgetAction(menu);
    heading->setDefaultWidget(new QLabel(QStringLiteral("Heading")));
    menu->addAction(heading);
    auto* key_only = new QAction(QStringLiteral("Focus search"), &window);
    window.addAction(key_only);
    QSignalSpy triggered{by_artist, &QAction::triggered};

    const auto commands = ui::windowCommands(window);
    QCOMPARE(commands.size(), 2);
    QCOMPARE(commands.front().action.data(), by_artist);
    QCOMPARE(commands.front().path,
             (QStringList{QStringLiteral("Edit"), QStringLiteral("Sort list")}));
    QCOMPARE(commands.back().action.data(), key_only);

    ui::CommandPalette palette(commands);
    palette.show();
    auto* filter = palette.findChild<QLineEdit*>(QStringLiteral("command-filter"));
    auto* results = palette.findChild<QListWidget*>(QStringLiteral("command-results"));
    filter->setText(QStringLiteral("sort"));
    QCOMPARE(results->count(), 1);
    QCOMPARE(results->item(0)->text(), QStringLiteral("Sort list › By artist"));
    sort->setEnabled(false);
    QCOMPARE(results->count(), 0);
    sort->setEnabled(true);
    QCOMPARE(results->count(), 1);
    QTest::keyClick(filter, Qt::Key_Return);
    QCOMPARE(triggered.count(), 1);
}

void BenchMainWindowTest::Search::quickTrackFindsATrackByItsTitle() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    {
        auto catalogue = window.localCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr && EngineKey::of(tab->document).isLocal());
    tab->model->replaceRows({}, true);
    auto* action = window.findChild<QAction*>(QStringLiteral("action-quick-track"));
    QVERIFY(action != nullptr);
    QCOMPARE(action->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+T")));
    action->trigger();
    QuickPickPopup* popup = nullptr;
    for (auto* candidate : window.findChildren<QuickPickPopup*>()) {
        if (candidate->isVisible() &&
            candidate->objectName() == QStringLiteral("bench-quick-track")) {
            popup = candidate;
        }
    }
    QVERIFY(popup != nullptr);
    // Words of the title and the artist, in any order and case.
    QTest::keyClicks(popup->input(), QStringLiteral("TONE trackknife"));
    QTRY_COMPARE(popup->results()->count(), 1);
    QVERIFY(popup->results()->item(0)->text().startsWith(QStringLiteral("Fixture Tone — ")));
    QTest::keyClick(popup->input(), Qt::Key_Return);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    QCOMPARE(tab->model->rows().front().title, std::string{"Fixture Tone"});
}

void BenchMainWindowTest::Search::quickAlbumShiftEnterReplacesAndPlays() {
    MELODY_REQUIRE_REALTIME_AUDIO();
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music));
    write_wave(music + QStringLiteral("/long.wav"), wave_sample_rate * 60U);
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_ && window.playingOnEngine());
    {
        auto catalogue = window.localCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr && EngineKey::of(tab->document).isLocal());
    window.findChild<QAction*>(QStringLiteral("action-quick-album"))->trigger();
    QuickPickPopup* popup = nullptr;
    for (auto* candidate : window.findChildren<QuickPickPopup*>()) {
        if (candidate->isVisible()) {
            popup = candidate;
        }
    }
    QVERIFY(popup != nullptr);
    QTest::keyClicks(popup->input(), QStringLiteral("unknown"));
    QTRY_COMPARE(popup->results()->count(), 1);
    QTest::keyClick(popup->input(), Qt::Key_Return, Qt::ShiftModifier);
    // Return puts the popup away before the click's Shift release.
    releaseHeldModifiers(&window);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(window.localPlayback()->state().status, QStringLiteral("playing"),
                              10'000);
    window.localPlayback()->stop();
}

void BenchMainWindowTest::Search::quickAlbumFindsByWordsAndPutsItAway() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_ && window.up_next_restored_);
    persistence::LibraryEntry album;
    {
        auto catalogue = window.localCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
        persistence::LibraryQuery albums;
        albums.kind = persistence::LibraryEntryKind::album;
        const auto page = catalogue->query(albums);
        QVERIFY(page && page->entries.size() == 1U && !page->entries.front().date.empty());
        album = page->entries.front();
    }
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr && EngineKey::of(tab->document).isLocal());
    tab->model->replaceRows({}, true);
    // A word of the album and its year, in any case.
    const auto words =
        QString::fromStdString(album.album).section(QLatin1Char(' '), 0, 0).toUpper() +
        QStringLiteral(" ") + QString::fromStdString(album.date);

    const auto open = [&window]() -> QuickPickPopup* {
        window.findChild<QAction*>(QStringLiteral("action-quick-album"))->trigger();
        QuickPickPopup* popup = nullptr;
        for (auto* candidate : window.findChildren<QuickPickPopup*>()) {
            if (candidate->isVisible()) {
                popup = candidate;
            }
        }
        return popup;
    };
    auto* popup = open();
    QVERIFY(popup != nullptr);
    QCOMPARE(window.findChild<QAction*>(QStringLiteral("action-quick-album"))->shortcut(),
             QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    QCOMPARE(popup->findChild<QLabel*>(QStringLiteral("bench-quick-album-scope"))->text(),
             QStringLiteral("This computer"));
    QTest::keyClicks(popup->input(), words);
    QTRY_COMPARE(popup->results()->count(), 1);
    // Enter: added to the list in front.
    const QPointer<QuickPickPopup> chosen{popup};
    QTest::keyClick(popup->input(), Qt::Key_Return);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    // Done once chosen: it closes itself.
    QTRY_VERIFY(chosen.isNull() || !chosen->isVisible());

    // Ctrl+Shift+Enter: onto the end of Up Next instead.
    const auto waiting = window.playback_.requests.pending().size();
    popup = open();
    QVERIFY(popup != nullptr);
    QTest::keyClicks(popup->input(), words);
    QTRY_COMPARE(popup->results()->count(), 1);
    QTest::keyClick(popup->input(), Qt::Key_Return, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(window.playback_.requests.pending().size(), waiting + 1U);
    QCOMPARE(tab->model->rowCount(), 1);

    // Another year finds nothing, and says so.
    popup = open();
    QVERIFY(popup != nullptr);
    QTest::keyClicks(popup->input(),
                     QString::fromStdString(album.album).section(QLatin1Char(' '), 0, 0) +
                         QStringLiteral(" 1066"));
    QTRY_COMPARE(popup->findChild<QLabel*>(QStringLiteral("bench-quick-album-status"))->text(),
                 QStringLiteral("No albums match."));
    QTest::keyClick(popup->input(), Qt::Key_Escape);
}

// Ctrl+L was documented, and refused to anything else as "reserved for
// library search", but nothing was bound to it. And only actions with a key
// by default could be given one: Convert, ReplayGain and the rest could not.
void BenchMainWindowTest::Search::everyCommandTakesAKeyAndCtrlLSearchesTheLibrary() {
    QSettings{}.remove(QStringLiteral("shortcuts"));
    BenchMainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QTRY_VERIFY(window.lists_restored_);

    // From Playlists, which has no search, to the library's.
    window.local_source_tabs_->setCurrentIndex(window.playlists_tab_);
    auto* search = window.findChild<QAction*>(QStringLiteral("action-focus-library-search"));
    QVERIFY(search != nullptr);
    QCOMPARE(search->shortcut(), QKeySequence(QStringLiteral("Ctrl+L")));
    search->trigger();
    QTRY_VERIFY(QApplication::focusWidget() != nullptr);
    QCOMPARE(QApplication::focusWidget()->objectName(), QStringLiteral("local-library-search"));
    QVERIFY(window.local_source_tabs_->currentIndex() != window.playlists_tab_);

    auto* convert = window.findChild<QAction*>(QStringLiteral("action-convert-files"));
    QVERIFY(convert != nullptr && convert->shortcut().isEmpty());
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::shortcuts);
    auto* edit =
        dialog->findChild<QKeySequenceEdit*>(QStringLiteral("shortcut-action-convert-files"));
    QVERIFY(edit != nullptr);
    edit->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Alt+C")));
    dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    QCOMPARE(convert->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+C")));
    QSettings{}.remove(QStringLiteral("shortcuts"));
}

// ADR-0140: Enter opens the kept search's tab at once, saying the tracks are
// being collected, and fills that tab when they come; nothing found, it says
// so there.
void BenchMainWindowTest::Search::aKeptSearchOpensAtOnce() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* browser = window.findChild<LibraryBrowser*>();
    QVERIFY(browser != nullptr);
    const auto tabs_before = window.tabs_->count();

    emit browser->searchStarted(QStringLiteral("ALL"));
    QCOMPARE(window.tabs_->count(), tabs_before + 1);
    auto* view = qobject_cast<QTableView*>(window.tabs_->currentWidget());
    QVERIFY(view != nullptr);
    auto* tab = window.tabForDocument(view->property("bench-document-id").toString());
    QVERIFY(tab != nullptr && tab->model->rowCount() == 0);
    QCOMPARE(window.tabs_->tabText(window.tabs_->currentIndex()), QStringLiteral("Search: ALL"));
    QVERIFY(static_cast<ui::QueueTableView*>(view)->emptyTitle().contains(QStringLiteral("ALL")));

    LocalTrackRow row;
    row.raw_path = "/music/found.flac";
    row.title = "Found";
    emit browser->searchCommitted(QStringLiteral("ALL"), {row});
    QCOMPARE(window.tabs_->count(), tabs_before + 1);
    QCOMPARE(tab->model->rowCount(), 1);
    QCOMPARE(static_cast<ui::QueueTableView*>(view)->emptyTitle(),
             window.emptyListTitle(EngineKey::of(tab->document)));

    emit browser->searchStarted(QStringLiteral("nothing like it"));
    auto* empty = window.tabForDocument(qobject_cast<QTableView*>(window.tabs_->currentWidget())
                                            ->property("bench-document-id")
                                            .toString());
    QVERIFY(empty != nullptr && empty != tab);
    emit browser->searchFailed(QStringLiteral("nothing like it"),
                               QStringLiteral("Nothing matches"));
    QCOMPARE(static_cast<ui::QueueTableView*>(empty->view)->emptyTitle(),
             QStringLiteral("Nothing matches"));
    QCOMPARE(empty->model->rowCount(), 0);
}

// ADR-0286: quick album, from a tab of this computer's, searches the one
// library, on the engine holding it -- not this computer.
void BenchMainWindowTest::Search::quickAlbumSearchesTheLibraryChosen() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_show_local_key), false);
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    std::string album;
    {
        auto catalogue = window.remoteCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
        persistence::LibraryQuery albums;
        albums.kind = persistence::LibraryEntryKind::album;
        const auto page = catalogue->query(albums);
        QVERIFY(page && page->entries.size() == 1U);
        album = page->entries.front().album;
    }
    for (auto& candidate : window.list_tabs_) {
        if (EngineKey::of(candidate->document).isLocal()) {
            window.tabs_->setCurrentWidget(candidate->view);
            break;
        }
    }
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr && EngineKey::of(tab->document).isLocal());

    window.findChild<QAction*>(QStringLiteral("action-quick-album"))->trigger();
    QuickPickPopup* popup = nullptr;
    for (auto* candidate : window.findChildren<QuickPickPopup*>()) {
        if (candidate->isVisible()) {
            popup = candidate;
        }
    }
    QVERIFY(popup != nullptr);
    QCOMPARE(popup->findChild<QLabel*>(QStringLiteral("bench-quick-album-scope"))->text(),
             window.remoteCatalogue()->name());
    QVERIFY(popup->findChild<QLabel*>(QStringLiteral("bench-quick-album-scope"))->text() !=
            QStringLiteral("This computer"));
    QTest::keyClicks(popup->input(), QString::fromStdString(album).section(QLatin1Char(' '), 0, 0));
    QTRY_COMPARE(popup->results()->count(), 1);
    QTest::keyClick(popup->input(), Qt::Key_Escape);
    QSettings{}.remove(QLatin1String(SettingsDialog::library_show_local_key));
}

// ADR-0153: the standalone search dialog filters the current tab with
// tkq over row metadata and technicals, and keeps results as tabs.
void BenchMainWindowTest::Search::savedSearchesCanBeManagedAndReopened() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto database =
        std::filesystem::path{directory.filePath(QStringLiteral("searches.sqlite3")).toStdString()};
    LocalTrackRow first{};
    first.title = "Jazz";
    first.raw_path = "/music/first.flac";
    LocalTrackRow second{};
    second.title = "Rock";
    second.raw_path = "/music/second.flac";
    for (auto* row : {&first, &second}) {
        row->metadata.fields.push_back({.canonical_name = "title",
                                        .native_name = "TITLE",
                                        .values = {row->title},
                                        .qualifier = {},
                                        .provenance = metadata::FieldProvenance::embedded});
    }
    std::vector<LocalTrackRow> current{first, second};
    const auto access = [&current]() -> std::optional<SearchDialog::TabSnapshot> {
        return SearchDialog::TabSnapshot{QStringLiteral("Current"), current};
    };
    const auto name_dialog = [](const QString& value) {
        QTimer::singleShot(0, [value] {
            auto* prompt = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            if (prompt) {
                prompt->setTextValue(value);
                prompt->accept();
            }
        });
    };
    {
        CatalogueSource catalogues{database};
        SearchDialog dialog{catalogues, EngineKey::local(), access, {}};
        dialog.show();
        auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
        auto* scope = dialog.findChild<QComboBox*>(QStringLiteral("bench-search-scope"));
        auto* mode = dialog.findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
        auto* saved = dialog.findChild<QComboBox*>(QStringLiteral("bench-search-saved"));
        auto* save = dialog.findChild<QPushButton*>(QStringLiteral("bench-search-save"));
        auto* update = dialog.findChild<QPushButton*>(QStringLiteral("bench-search-update"));
        auto* rename = dialog.findChild<QPushButton*>(QStringLiteral("bench-search-rename"));
        auto* results = dialog.findChild<QListWidget*>(QStringLiteral("bench-search-results"));
        QVERIFY(input && scope && mode && saved && save && update && rename && results);
        scope->setCurrentIndex(1);
        mode->setChecked(true);
        input->setText(QStringLiteral("title IS Jazz"));
        QTRY_VERIFY(save->isEnabled());
        QTRY_COMPARE(results->count(), 1);
        name_dialog(QStringLiteral("My jazz"));
        save->click();
        QTRY_COMPARE(saved->count(), 2);
        QCOMPARE(saved->currentText(), QStringLiteral("My jazz"));
        QVERIFY(!update->isEnabled());
        mode->setChecked(false);
        input->setText(QStringLiteral("Rock"));
        QTRY_VERIFY(update->isEnabled());
        update->click();
        QTRY_VERIFY(saved->isEnabled());
        QVERIFY(!update->isEnabled());
        name_dialog(QStringLiteral("My rock"));
        rename->click();
        QTRY_COMPARE(saved->currentText(), QStringLiteral("My rock"));
        QTRY_COMPARE(results->count(), 1);
        const auto screenshot_dir = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        if (!screenshot_dir.isEmpty()) {
            QVERIFY(dialog.grab().save(screenshot_dir + QStringLiteral("/saved-searches.png")));
        }
        // Invalid queries cannot replace a valid saved definition.
        mode->setChecked(true);
        input->setText(QStringLiteral("AND ("));
        update->click();
        auto repository = persistence::ListRepository::open(database);
        QVERIFY(repository.has_value());
        auto definitions = repository->load_saved_searches();
        QVERIFY(definitions && definitions->size() == 1);
        QCOMPARE(definitions->front().expression, std::string{"Rock"});
        QCOMPARE(definitions->front().dialect, std::string{"words-1"});
        QCOMPARE(definitions->front().scope, persistence::SavedSearchScope::current_tab);
    }
    // A new dialog reruns the saved definition against the current tab, not old hits.
    current.push_back(second);
    CatalogueSource reopened_catalogues{database};
    SearchDialog reopened{reopened_catalogues, EngineKey::local(), access, {}};
    reopened.show();
    auto* saved = reopened.findChild<QComboBox*>(QStringLiteral("bench-search-saved"));
    auto* input = reopened.findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* scope = reopened.findChild<QComboBox*>(QStringLiteral("bench-search-scope"));
    auto* mode = reopened.findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
    auto* results = reopened.findChild<QListWidget*>(QStringLiteral("bench-search-results"));
    auto* open = reopened.findChild<QPushButton*>(QStringLiteral("bench-search-open-tab"));
    auto* remove = reopened.findChild<QPushButton*>(QStringLiteral("bench-search-delete"));
    QVERIFY(saved && input && scope && mode && results && open && remove);
    QTRY_COMPARE(saved->count(), 2);
    saved->setCurrentIndex(1);
    QVERIFY(QMetaObject::invokeMethod(saved, "activated", Q_ARG(int, 1)));
    QCOMPARE(input->text(), QStringLiteral("Rock"));
    QCOMPARE(scope->currentIndex(), 1);
    QVERIFY(!mode->isChecked());
    QTRY_COMPARE(results->count(), 2);
    // Switching scope invalidates the old payload immediately.
    scope->setCurrentIndex(0);
    QVERIFY(!open->isEnabled());
    QCOMPARE(results->count(), 0);
    QTRY_VERIFY(reopened.findChild<QLabel*>(QStringLiteral("bench-search-status"))
                    ->text()
                    .startsWith(QStringLiteral("0 matches")));
    QTimer::singleShot(0, [] {
        if (auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            prompt->button(QMessageBox::Yes)->click();
        }
    });
    remove->click();
    QTRY_COMPARE(saved->count(), 1);
    auto repository = persistence::ListRepository::open(database);
    QVERIFY(repository && repository->load_saved_searches()->empty());
    mode->setChecked(false);
}

void BenchMainWindowTest::Search::searchDialogFiltersTabAndOpensResults() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto flac_path = media.filePath(QStringLiteral("tone.flac"));
    const auto opus_path = media.filePath(QStringLiteral("tone.opus"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), flac_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("loudness-tone-opus.b64"), opus_path));

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(
        {QFile::encodeName(flac_path).toStdString(), QFile::encodeName(opus_path).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 2);
    QTRY_VERIFY(model->rows().front().probed && model->rows().back().probed);

    auto* action = window.findChild<QAction*>(QStringLiteral("action-search-dialog"));
    QVERIFY(action != nullptr);
    action->trigger();
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<QDialog*>(QStringLiteral("bench-search-dialog"))) !=
                nullptr);
    auto* scope = dialog->findChild<QComboBox*>(QStringLiteral("bench-search-scope"));
    auto* input = dialog->findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* mode = dialog->findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
    auto* results = dialog->findChild<QListWidget*>(QStringLiteral("bench-search-results"));
    auto* open_button = dialog->findChild<QPushButton*>(QStringLiteral("bench-search-open-tab"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-search-status"));
    QVERIFY(scope && input && mode && results && open_button && status);

    scope->setCurrentIndex(1);
    mode->setChecked(true);
    input->setText(QStringLiteral("codec IS flac"));
    QTRY_COMPARE(results->count(), 1);
    input->setText(QStringLiteral("codec IS opus"));
    QTRY_VERIFY(results->count() == 1 && status->text().startsWith(QStringLiteral("1 match")));

    // Word search matches row metadata without tkq structure.
    mode->setChecked(false);
    input->setText(QStringLiteral("fixture"));
    QTRY_VERIFY(results->count() >= 1);

    // The full result set becomes an ordinary scratch tab with the rows.
    mode->setChecked(true);
    input->setText(QStringLiteral("codec IS opus"));
    QTRY_COMPARE(results->count(), 1);
    const auto tabs_before = tabs->count();
    QTRY_VERIFY(open_button->isEnabled());
    open_button->click();
    QTRY_COMPARE(tabs->count(), tabs_before + 1);
    auto* opened = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(opened != nullptr);
    auto* opened_model = qobject_cast<LocalListModel*>(opened->model());
    QVERIFY(opened_model != nullptr);
    QTRY_COMPARE(opened_model->rowCount(), 1);
    QCOMPARE(opened_model->rows().front().raw_path,
             std::string{QFile::encodeName(opus_path).constData()});

    mode->setChecked(false);
    dialog->close();
}

// A tab opened from the search dialog must show album covers like any other
// track tab: the rows carry the same files, so the shared artwork queue has
// to reach the new tab's model.
void BenchMainWindowTest::Search::searchResultTabLoadsCovers() {
    QTemporaryDir media;
    QTemporaryDir other;
    QVERIFY(media.isValid() && other.isValid());
    const auto flac_path = media.filePath(QStringLiteral("tone.flac"));
    const auto opus_path = other.filePath(QStringLiteral("tone.opus"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), flac_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("loudness-tone-opus.b64"), opus_path));
    {
        QImage cover{32, 32, QImage::Format_RGB32};
        cover.fill(Qt::darkGreen);
        QVERIFY(cover.save(media.filePath(QStringLiteral("cover.png")), "PNG"));
    }

    BenchMainWindow window;
    window.show();
    // A local tab from a different folder keeps the window in local context
    // without priming the shared artwork cache for the indexed album.
    window.openLocalPaths({QFile::encodeName(opus_path).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 1);
    QTRY_VERIFY(model->rows().front().probed);
    const auto artwork_of = [](LocalListModel* target) {
        return target->data(target->index(0, 0), static_cast<int>(ui::track_album_artwork_role))
            .value<QImage>();
    };

    {
        // The engine's library, in its own database (ADR-0339: not Melody's).
        auto library =
            persistence::LocalLibrary::open(core::engine_state_directory() / "lists.sqlite");
        QVERIFY(library.has_value());
        const auto root = QFile::encodeName(media.path());
        QVERIFY(
            library->add_root(std::string{root.constData(), static_cast<std::size_t>(root.size())})
                .has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(library->scan({}, progress).has_value());
    }

    auto* action = window.findChild<QAction*>(QStringLiteral("action-search-dialog"));
    QVERIFY(action != nullptr);
    action->trigger();
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<QDialog*>(QStringLiteral("bench-search-dialog"))) !=
                nullptr);
    auto* scope = dialog->findChild<QComboBox*>(QStringLiteral("bench-search-scope"));
    auto* input = dialog->findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* mode = dialog->findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
    auto* results = dialog->findChild<QListWidget*>(QStringLiteral("bench-search-results"));
    auto* open_button = dialog->findChild<QPushButton*>(QStringLiteral("bench-search-open-tab"));
    QVERIFY(scope && input && mode && results && open_button);

    // Scope 0 is the library database: the rows never passed through a tab,
    // so their covers must be loaded for the opened result tab.
    scope->setCurrentIndex(0);
    mode->setChecked(true);
    input->setText(QStringLiteral("codec IS flac"));
    QTRY_COMPARE(results->count(), 1);
    const auto tabs_before = tabs->count();
    QTRY_VERIFY(open_button->isEnabled());
    open_button->click();
    QTRY_COMPARE(tabs->count(), tabs_before + 1);
    auto* opened = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(opened != nullptr);
    auto* opened_model = qobject_cast<LocalListModel*>(opened->model());
    QVERIFY(opened_model != nullptr);
    QTRY_COMPARE(opened_model->rowCount(), 1);
    QTRY_VERIFY(!artwork_of(opened_model).isNull());

    mode->setChecked(false);
    dialog->close();
}

void BenchMainWindowTest::Search::searchDialogProbesMissingTechnicalsOnDemand() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto flac_path = media.filePath(QStringLiteral("tone.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), flac_path));
    const auto encoded = QFile::encodeName(flac_path);

    LocalTrackRow row;
    row.raw_path = std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    row.title = "Restored";
    row.metadata.fields.push_back({.canonical_name = "title",
                                   .native_name = "TITLE",
                                   .values = {"Restored"},
                                   .qualifier = {},
                                   .provenance = metadata::FieldProvenance::embedded});
    row.probed = true;

    std::vector<std::pair<std::string, LocalTrackTechnicals>> reported;
    CatalogueSource catalogues{std::filesystem::path{}};
    SearchDialog dialog{catalogues, EngineKey::local(),
                        [&row]() -> std::optional<SearchDialog::TabSnapshot> {
                            return SearchDialog::TabSnapshot{QStringLiteral("Fixture"), {row}};
                        },
                        [&reported](std::string path, LocalTrackTechnicals technicals) {
                            reported.emplace_back(std::move(path), technicals);
                        }};
    dialog.show();
    auto* scope = dialog.findChild<QComboBox*>(QStringLiteral("bench-search-scope"));
    auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* mode = dialog.findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
    auto* results = dialog.findChild<QListWidget*>(QStringLiteral("bench-search-results"));
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("bench-search-status"));
    QVERIFY(scope && input && mode && results && status);
    scope->setCurrentIndex(1);
    mode->setChecked(true);
    input->setText(QStringLiteral("samplerate GREATER 8000"));
    QTRY_COMPARE_WITH_TIMEOUT(results->count(), 1, 15'000);
    QVERIFY(status->text().contains(QStringLiteral("scanned 1 file")));
    QCOMPARE(reported.size(), 1U);
    QCOMPARE(reported.front().first, row.raw_path);
    QCOMPARE(reported.front().second.codec, std::string{"flac"});

    // Metadata-only queries never probe.
    reported.clear();
    input->setText(QStringLiteral("title HAS restored"));
    QTRY_COMPARE(results->count(), 1);
    QVERIFY(reported.empty());
    QVERIFY(!status->text().contains(QStringLiteral("scanned")));

    // ADR-0179: tab rows expose their loaded content-identity ratings to
    // rating predicates without touching the library store.
    input->setText(QStringLiteral("rating GREATER 7"));
    QTRY_COMPARE(results->count(), 0);
    row.rating = 8U;
    row.album_rating = 6U;
    input->setText(QStringLiteral("rating GREATER 7 AND albumrating EQUAL 6"));
    QTRY_COMPARE(results->count(), 1);
    input->setText(QStringLiteral("rating MISSING"));
    QTRY_COMPARE(results->count(), 0);
    mode->setChecked(false);
}

// The Server library scope translates tkq for the connected server,
// previews the result labels, refuses untranslatable queries with the
// translator's message, and opens results through the window callback.
void BenchMainWindowTest::Search::searchPresetsAreGroupedAndPrompt() {
    CatalogueSource catalogues{std::filesystem::path{}};
    SearchDialog dialog{catalogues, EngineKey::local(), {}, {}};
    dialog.show();
    auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* scope = dialog.findChild<QComboBox*>(QStringLiteral("bench-search-scope"));
    auto* mode = dialog.findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
    auto* menu = dialog.findChild<QMenu*>(QStringLiteral("bench-search-presets-menu"));
    QVERIFY(input && scope && mode && menu);
    QCOMPARE(scope->count(), 2); // Library database, current tab.
    QTRY_VERIFY(input->hasFocus());
    scope->setFocus();
    dialog.hide();
    dialog.show();
    dialog.activateWindow();
    // The offscreen platform need not reactivate a hidden native window;
    // its remembered focus target must still be the input, not the scope.
    QTRY_COMPARE(dialog.focusWidget(), input);
    const auto populate = [&] { QVERIFY(QMetaObject::invokeMethod(menu, "aboutToShow")); };
    populate();
    QCOMPARE(menu->actions().size(), 5);
    QCOMPARE(menu->actions().first()->text(), QStringLiteral("Explore"));
    auto* missing = menu->findChild<QAction*>(QStringLiteral("search-preset-missing-album"));
    QVERIFY(missing);
    missing->trigger();
    QCOMPARE(input->text(), QStringLiteral("album MISSING"));
    QVERIFY(mode->isChecked());
    auto* year = menu->findChild<QAction*>(QStringLiteral("search-preset-year"));
    QVERIFY(year);
    QTimer::singleShot(0, &dialog, [&] {
        auto* prompt = dialog.findChild<QInputDialog*>();
        QVERIFY(prompt);
        prompt->setIntValue(2001);
        prompt->accept();
    });
    year->trigger();
    QCOMPARE(input->text(), QStringLiteral("date EQUAL 2001"));
    QTimer::singleShot(0, &dialog, [&] {
        auto* prompt = dialog.findChild<QInputDialog*>();
        QVERIFY(prompt);
        prompt->reject();
    });
    year->trigger();
    QCOMPARE(input->text(), QStringLiteral("date EQUAL 2001"));
    // Every preset runs against the library itself, so none is withheld: the
    // gating only ever existed for what a remote server could translate.
    scope->setCurrentIndex(1);
    populate();
    QCOMPARE(menu->actions().size(), 5);
    QVERIFY(menu->findChild<QAction*>(QStringLiteral("search-preset-unplayed-albums")));
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::Search)

#include "window_search_test.moc"
