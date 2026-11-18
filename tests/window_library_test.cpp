// SPDX-License-Identifier: GPL-3.0-only

// The window test: the library's rows: discs, segments, missing files, the status bar.

#include "window_test.hpp"

#include "bench/file_scope_view.hpp"
#include "trackknife/core/engine_state.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "workspace/artwork_session.hpp"
#include "workspace/sources.hpp"
#include "workspace/startup.hpp"
#include "workspace/tab_store.hpp"

#include <QCheckBox>
#include <QGroupBox>
#include <QMessageBox>
#include <sqlite3.h>

namespace trackknife::bench {

class BenchMainWindowTest::Library final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void discsOfAnAlbumAreNamed();
    void recentlyAddedComesFirstWhereAskedFor();
    void statusBarSummarizesTrackSelection();
    void cachedRowsAreReadEvenWithARevision();
    void listRowsWhoseFilesAreGoneAreGreyed();
    void emptyListsSayHowToFillThem();
    void librarySettingsManageFoldersWithoutScanning();
    void folderDiscoveryAdmitsWave64();
    void contextMenusTargetSelectionsAndLists();
    void cueSheetsExpandIntoPersistentSegmentRows();
    void containerChaptersExpandIntoPersistentSegmentRows();
    void codecNativeSubsongsExpandAndPersistDecoderSelections();
};

// OK Computer with its bonus disc read as one album numbered 1 to 12 and
// then 1 to 15 again. Each disc's first row now names it -- but only in an
// album of more than one: a single disc says nothing.
void BenchMainWindowTest::Library::discsOfAnAlbumAreNamed() {
    const auto track = [](const char* album, const char* disc, const char* subtitle,
                          const char* title) {
        LocalTrackRow row;
        row.raw_path = std::string{"/music/"} + album + "/" + disc + "/" + title + ".flac";
        row.title = title;
        row.artist = "Radiohead";
        row.album = album;
        row.date = "1997";
        const auto field = [&row](const char* name, const char* value) {
            if (*value != 0) {
                row.metadata.fields.push_back(
                    metadata::MetadataField{.canonical_name = name,
                                            .native_name = QByteArray{name}.toUpper().toStdString(),
                                            .values = {value},
                                            .qualifier = {},
                                            .provenance = metadata::FieldProvenance::embedded});
            }
        };
        field("discnumber", disc);
        field("discsubtitle", subtitle);
        row.probed = true;
        return row;
    };
    LocalListModel model;
    model.replaceRows({
        track("OK Computer", "1/2", "", "Airbag"),
        track("OK Computer", "1/2", "", "Paranoid Android"),
        track("OK Computer", "02/2", "B-sides", "Polyethylene"),
        track("OK Computer", "02/2", "B-sides", "Pearly"),
        track("The Bends", "1", "", "Planet Telex"),
        track("The Bends", "1", "", "The Bends"),
    });
    const auto disc = [&model](const int row) {
        return model.index(row, 0).data(ui::track_disc_start_role).toString();
    };
    QCOMPARE(disc(0), QStringLiteral("Disc 1"));
    QCOMPARE(disc(1), QString{});
    QCOMPARE(disc(2), QStringLiteral("Disc 2 · B-sides"));
    QCOMPARE(disc(3), QString{});
    QCOMPARE(disc(4), QString{});
    QCOMPARE(disc(5), QString{});
}

// ADR-0257/0259: a row the engine described carries the library's tags and
// the file's revision; the tagger still reads the file, as it reads any row
// whose tags are the library's. A row read from the file is not read again.
void BenchMainWindowTest::Library::cachedRowsAreReadEvenWithARevision() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr);
    persistence::LibraryTrackSnapshot snapshot;
    snapshot.raw_path = "/music/Deaf Radio/01.flac";
    snapshot.facts.fields["musicbrainzalbumid"].push_back({"731084e3", {}});
    auto described = cached_library_row(std::move(snapshot));
    described.source_revision = core::LocalSourceRevision{.device = 1, .inode = 2};
    LocalTrackRow read;
    read.raw_path = "/music/Quicksand/01.flac";
    read.probed = true;
    read.source_revision = core::LocalSourceRevision{.device = 1, .inode = 3};
    read.metadata.fields.push_back(
        metadata::MetadataField{.canonical_name = "musicbrainzalbumid",
                                .native_name = "MUSICBRAINZ_ALBUMID",
                                .values = {"dd034d22"},
                                .qualifier = {},
                                .provenance = metadata::FieldProvenance::embedded});
    LocalTrackRow unread = described;
    unread.raw_path = "/music/Other/01.flac";
    unread.source_revision.reset();
    tab->model->replaceRows({described, read, unread});
    const auto reader =
        window.selectionSourceReader(tab->model, {QPersistentModelIndex{tab->model->index(0, 0)},
                                                  QPersistentModelIndex{tab->model->index(1, 0)},
                                                  QPersistentModelIndex{tab->model->index(2, 0)}});
    const auto first = reader(0U);
    const auto second = reader(1U);
    const auto third = reader(2U);
    QVERIFY(first && second && third);
    // ADR-0268: a row read from the file is read again too -- the file may
    // have changed since, through another tagger -- so every one is read.
    QVERIFY(first->source.needs_metadata_capture && !first->source.source_revision);
    QVERIFY(second->source.needs_metadata_capture && !second->source.source_revision);
    QVERIFY(third->source.needs_metadata_capture && !third->source.source_revision);
}

// ADR-0268: a list's rows whose files are not there are said to be, asked
// of its engine; a file that comes back, once the engine says so, is not.
void BenchMainWindowTest::Library::listRowsWhoseFilesAreGoneAreGreyed() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto here = media.filePath(QStringLiteral("here.wav"));
    const auto gone = media.filePath(QStringLiteral("gone.wav"));
    write_wave(here, wave_sample_rate / 10U);
    const auto here_raw = QFile::encodeName(here).toStdString();
    const auto gone_raw = QFile::encodeName(gone).toStdString();
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    persistence::ListDocument document{.id = core::StableId::random(),
                                       .kind = persistence::ListKind::scratch,
                                       .name = "Gone and here",
                                       .pinned = false,
                                       .dirty = false,
                                       .items = {},
                                       .engine = {}};
    for (const auto& path : {here_raw, gone_raw}) {
        persistence::ListItem item;
        item.source_reference = path;
        document.items.push_back(item);
    }
    auto* tab = window.addListTab(document, true);
    QVERIFY(tab != nullptr);
    const auto missing = [tab](const int row) {
        return tab->model->index(row, ui::track_title_column).data(ui::track_missing_role).toBool();
    };
    QTRY_VERIFY_WITH_TIMEOUT(missing(1), 10'000);
    QVERIFY(!missing(0));
    QVERIFY(tab->model->index(1, ui::track_title_column)
                .data(Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("not there")));
    // Back: once its engine says the path changed, the row is as any other.
    write_wave(gone, wave_sample_rate / 10U);
    window.workspace_.adoptLibraryChange(EngineKey::local(), {gone_raw}, {}, false);
    QTRY_VERIFY_WITH_TIMEOUT(!missing(1), 10'000);
}

void BenchMainWindowTest::Library::recentlyAddedComesFirstWhereAskedFor() {
    QSettings{}.remove(QStringLiteral("library/newest-first"));
    QSettings{}.remove(QStringLiteral("library/view"));
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music + QStringLiteral("/old")));
    // One album there from the start, dated by its file: years ago.
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/old/old.flac")));
    {
        QFile old{music + QStringLiteral("/old/old.flac")};
        QVERIFY(old.open(QIODevice::ReadWrite));
        QVERIFY(old.setFileTime(QDateTime::fromSecsSinceEpoch(1'000'000'000),
                                QFileDevice::FileModificationTime));
    }
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto catalogue = window.localCatalogue()->open();
    QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
    persistence::LibraryScanProgress progress;
    QVERIFY(catalogue->scan({}, progress).has_value());
    // And one that arrives later.
    QVERIFY(QDir{}.mkpath(music + QStringLiteral("/new")));
    write_wave(music + QStringLiteral("/new/new.wav"), wave_sample_rate);
    QVERIFY(catalogue->scan({}, progress).has_value());

    // The quick album popup, before anything is typed: newest first.
    window.findChild<QAction*>(QStringLiteral("action-quick-album"))->trigger();
    QuickPickPopup* popup = nullptr;
    for (auto* candidate : window.findChildren<QuickPickPopup*>()) {
        if (candidate->isVisible()) {
            popup = candidate;
        }
    }
    QVERIFY(popup != nullptr);
    QTRY_COMPARE(popup->results()->count(), 2);
    QVERIFY2(popup->results()->item(0)->text().contains(QStringLiteral("added today")),
             qPrintable(popup->results()->item(0)->text()));
    QVERIFY(popup->results()->item(1)->text().contains(QStringLiteral("years ago")));
    QVERIFY(popup->findChild<QLabel*>(QStringLiteral("bench-quick-album-status"))
                ->text()
                .startsWith(QStringLiteral("Recently added")));
    QTest::keyClick(popup->input(), Qt::Key_Escape);

    // ADR-0346: alone, as melody-pick shows it -- a window of its own, no
    // tab for Alt+Enter to open, and open until what was chosen is queued.
    {
        auto* alone =
            new QuickPickPopup(QuickPickKind::album,
                               std::shared_ptr<engine::Catalogue>{window.localCatalogue()->open()},
                               QStringLiteral("test"), nullptr, true);
        const QPointer<QuickPickPopup> held{alone};
        QVERIFY(alone->isWindow());
        QVERIFY(!alone->windowFlags().testFlag(Qt::Popup));
        QVERIFY(!alone->findChild<QLabel*>(QStringLiteral("bench-quick-album-keys"))
                     ->text()
                     .contains(QStringLiteral("new tab")));
        std::vector<LocalLibraryAction> asked;
        connect(alone, &QuickPickPopup::chosen, this,
                [&asked](const std::vector<persistence::LibraryEntry>& picked,
                         const LocalLibraryAction action) {
                    if (picked.size() == 1U) {
                        asked.push_back(action);
                    }
                });
        alone->showAlone();
        QTRY_COMPARE(alone->results()->count(), 2);
        QTest::keyClick(alone->input(), Qt::Key_Return, Qt::AltModifier);
        QVERIFY(asked.empty());
        QTest::keyClick(alone->input(), Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(asked, std::vector{LocalLibraryAction::replace});
        QVERIFY(alone->isVisible());
        alone->say(QStringLiteral("no engine"));
        QCOMPARE(alone->findChild<QLabel*>(QStringLiteral("bench-quick-album-status"))->text(),
                 QStringLiteral("no engine"));
        QTest::keyClick(alone->input(), Qt::Key_Escape);
        QTRY_VERIFY(held.isNull());
    }

    // The library panel's Recently added -- a view (ADR-0254): albums, the
    // new one first.
    window.localLibrary()->refreshLibrary();
    auto* views =
        window.localLibrary()->findChild<QComboBox*>(QStringLiteral("local-library-view"));
    QVERIFY(views != nullptr);
    const auto choose = [views](const QString& id) {
        const auto index = views->findData(id);
        QVERIFY(index >= 0);
        views->setCurrentIndex(index);
        emit views->activated(index);
    };
    choose(QStringLiteral("recently-added"));
    auto* tree = window.localLibrary()->findChild<QTreeView*>();
    QTRY_COMPARE(tree->model()->rowCount(), 2);
    const auto first = tree->model()->index(0, 0).data(library_entry_role);
    QVERIFY(first.isValid());
    QCOMPARE(first.value<persistence::LibraryEntry>().kind, persistence::LibraryEntryKind::album);
    QVERIFY(first.value<persistence::LibraryEntry>().added > tree->model()
                                                                 ->index(1, 0)
                                                                 .data(library_entry_role)
                                                                 .value<persistence::LibraryEntry>()
                                                                 .added);
    choose(QStringLiteral("artist-album"));
    QTRY_VERIFY(tree->model()->rowCount() >= 1 &&
                tree->model()
                        ->index(0, 0)
                        .data(library_entry_role)
                        .value<persistence::LibraryEntry>()
                        .kind == persistence::LibraryEntryKind::artist);
    QSettings{}.remove(QStringLiteral("library/newest-first"));
    QSettings{}.remove(QStringLiteral("library/view"));
}

void BenchMainWindowTest::Library::statusBarSummarizesTrackSelection() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto first_path = media.filePath(QStringLiteral("selection-first.wav"));
    const auto second_path = media.filePath(QStringLiteral("selection-second.wav"));
    write_wave(first_path, wave_sample_rate / 10U);
    write_wave(second_path, wave_sample_rate / 10U);
    const auto first_encoded = QFile::encodeName(first_path);
    const auto second_encoded = QFile::encodeName(second_path);
    const std::string first_raw{first_encoded.constData(),
                                static_cast<std::size_t>(first_encoded.size())};
    const std::string second_raw{second_encoded.constData(),
                                 static_cast<std::size_t>(second_encoded.size())};

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({first_raw, second_raw});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* status = window.findChild<QLabel*>(QStringLiteral("bench-selection-status"));
    QVERIFY(tabs != nullptr);
    QVERIFY(status != nullptr);
    QCOMPARE(status->accessibleName(), QStringLiteral("Selected track information"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 2);

    LocalTrackRow first_metadata{
        .raw_path = {},
        .logical_reference = std::nullopt,
        .selection = {},
        .segment = std::nullopt,
        .title = "First title",
        .artist = "First artist",
        .album = "First album",
        .album_artist = {},
        .date = "2026",
        .track_number = {},
        .duration_ms = 61'000,
        .metadata = {},
        .source_revision = std::nullopt,
    };
    LocalTrackRow second_metadata{
        .raw_path = {},
        .logical_reference = std::nullopt,
        .selection = {},
        .segment = std::nullopt,
        .title = "Second title",
        .artist = "Second artist",
        .album = "Second album",
        .album_artist = {},
        .date = "2025",
        .track_number = {},
        .duration_ms = 120'000,
        .metadata = {},
        .source_revision = std::nullopt,
    };
    QVERIFY(model->applyMetadata(first_raw, 0, std::move(first_metadata)));
    QVERIFY(model->applyMetadata(second_raw, 1, std::move(second_metadata)));

    view->selectionModel()->select(model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QCOMPARE(status->text(),
             QStringLiteral("First artist — First title · First album (2026) · 1:01"));
    QCOMPARE(status->toolTip(), QString::fromStdString(core::display_raw_path(first_raw)));

    view->selectionModel()->select(model->index(1, 0),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(status->text(), QStringLiteral("2 tracks selected · 3:01 total"));

    view->clearSelection();
    QCOMPARE(status->text(), QStringLiteral("No tracks selected"));
}

void BenchMainWindowTest::Library::emptyListsSayHowToFillThem() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_ && window.up_next_restored_);
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr);
    auto* view = static_cast<ui::QueueTableView*>(tab->view);
    QCOMPARE(view->emptyTitle(), QStringLiteral("This list is empty"));
    QCOMPARE(window.up_next_view_->emptyTitle(), QStringLiteral("Nothing waiting"));

    // Said in the middle of the list while it is empty, and gone once filled.
    const auto painted = [view] {
        const auto image = view->viewport()->grab().toImage();
        const auto ground = view->palette().color(QPalette::Base).lightness();
        const auto ink = view->palette().color(QPalette::Text).lightness();
        // Text shows as pixels well on the text's side of the background.
        const auto threshold = (ground + ink) / 2;
        for (int y = image.height() / 4; y < image.height() * 3 / 4; ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const auto lightness = image.pixelColor(x, y).lightness();
                if (ink > ground ? lightness > threshold : lightness < threshold) {
                    return true;
                }
            }
        }
        return false;
    };
    QVERIFY(painted());
    LocalTrackRow row;
    row.raw_path = "/tmp/filled.flac";
    row.title = "Filled";
    tab->model->appendRows({row});
    QCoreApplication::processEvents();
    QVERIFY(!painted());
}

void BenchMainWindowTest::Library::librarySettingsManageFoldersWithoutScanning() {
    QTemporaryDir music;
    QVERIFY(music.isValid());
    const auto path = music.filePath(QStringLiteral("keep.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    BenchMainWindow window;
    window.show();
    LocalLibraryPanel* panel = nullptr;
    QTRY_VERIFY((panel = window.findChild<LocalLibraryPanel*>()));
    const auto raw_root = QFile::encodeName(music.path()).toStdString();
    panel->addRoot(raw_root);
    auto* action = window.findChild<QAction*>(QStringLiteral("action-settings"));
    QVERIFY(action);
    action->trigger();
    auto* dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    dialog->showPage(SettingsDialog::Page::engine);
    auto* roots = dialog->findChild<QListWidget*>(QStringLiteral("local-library-roots"));
    QVERIFY(roots);
    QTRY_COMPARE(roots->count(), 1);
    QCOMPARE(roots->item(0)->data(Qt::UserRole).toByteArray().toStdString(), raw_root);
    QVERIFY(roots->item(0)->text().contains(QStringLiteral("not scanned")));
    QVERIFY(!panel->property("scanning").toBool());
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/settings-library.png")));
    }
    QPointer<SettingsDialog> lifetime = dialog;
    dialog->reject();
    QTRY_VERIFY(lifetime.isNull());
    auto* shortcut = panel->findChild<QToolButton*>(QStringLiteral("local-library-folders"));
    QVERIFY(shortcut);
    shortcut->click();
    dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    auto* pages = dialog->findChild<QListWidget*>(QStringLiteral("bench-settings-pages"));
    QCOMPARE(pages->currentItem()->text(), QStringLiteral("Engines"));
    roots = dialog->findChild<QListWidget*>(QStringLiteral("local-library-roots"));
    QTRY_COMPARE(roots->count(), 1);
    // The shortcut reuses the open Settings screen rather than making another manager.
    shortcut->click();
    QCOMPARE(window.findChildren<SettingsDialog*>().size(), 1);
    QCOMPARE(window.findChildren<QListWidget*>(QStringLiteral("local-library-roots")).size(), 1);
    roots->setCurrentRow(0);
    auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("local-library-folder-remove"));
    QVERIFY(remove && remove->isEnabled());
    remove->click();
    QTRY_COMPARE(roots->count(), 0);
    QVERIFY(QFile::exists(path));
    QVERIFY(!panel->property("scanning").toBool());
    lifetime = dialog;
    dialog->reject();
    QTRY_VERIFY(lifetime.isNull());
    shortcut->click();
    dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    roots = dialog->findChild<QListWidget*>(QStringLiteral("local-library-roots"));
    QTRY_COMPARE(roots->count(), 0);
    // Closing a manager with a pending root load must not leave stale widget callbacks.
    dialog->reject();
}

void BenchMainWindowTest::Library::folderDiscoveryAdmitsWave64() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto wave64_path = media.filePath(QStringLiteral("accepted.W64"));
    const auto ignored_path = media.filePath(QStringLiteral("ignored.txt"));
    for (const auto& path : {wave64_path, ignored_path}) {
        QFile file{path};
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("fixture"), 7);
    }
    const auto directory_encoded = QFile::encodeName(media.path());
    const std::string directory_raw{directory_encoded.constData(),
                                    static_cast<std::size_t>(directory_encoded.size())};
    const auto wave64_encoded = QFile::encodeName(wave64_path);
    const std::string wave64_raw{wave64_encoded.constData(),
                                 static_cast<std::size_t>(wave64_encoded.size())};

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({directory_raw});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 1);
    QCOMPARE(model->rawPath(0), wave64_raw);
}

void BenchMainWindowTest::Library::contextMenusTargetSelectionsAndLists() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    std::vector<std::string> raw_paths;
    for (int index = 0; index < 3; ++index) {
        const auto path = media.filePath(QStringLiteral("context-%1.wav").arg(index));
        write_wave(path, wave_sample_rate / 10U);
        const auto encoded = QFile::encodeName(path);
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }

    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* duplicate = window.findChild<QAction*>(QStringLiteral("action-duplicate-tab"));
    auto* track_menu = window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"));
    QVERIFY(tabs != nullptr);
    QVERIFY(duplicate != nullptr);
    QVERIFY(track_menu != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    window.openLocalPaths(raw_paths);
    auto* source = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(source != nullptr);
    QTRY_COMPARE(source->model()->rowCount(), 3);

    duplicate->trigger();
    QCOMPARE(tabs->count(), 2);
    auto* destination = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(destination != nullptr);
    QCOMPARE(destination->model()->rowCount(), 3);
    tabs->setCurrentWidget(source);

    source->selectionModel()->select(source->model()->index(0, 0),
                                     QItemSelectionModel::ClearAndSelect |
                                         QItemSelectionModel::Rows);
    source->selectionModel()->select(source->model()->index(1, 0),
                                     QItemSelectionModel::Select | QItemSelectionModel::Rows);
    const auto selected_position = source->visualRect(source->model()->index(1, 0)).center();
    QVERIFY(QMetaObject::invokeMethod(source, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, selected_position)));
    QCOMPARE(source->selectionModel()->selectedRows().size(), 2);
    auto* copy_menu = track_menu->findChild<QMenu*>(QStringLiteral("bench-track-copy-menu"));
    auto* move_menu = track_menu->findChild<QMenu*>(QStringLiteral("bench-track-move-menu"));
    auto* play = window.findChild<QAction*>(QStringLiteral("action-play-selected-track"));
    auto* properties = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    auto* remove = window.findChild<QAction*>(QStringLiteral("action-remove-selected-tracks"));
    QVERIFY(copy_menu != nullptr);
    QVERIFY(move_menu != nullptr);
    QVERIFY(play != nullptr);
    QVERIFY(properties != nullptr);
    QVERIFY(remove != nullptr);
    auto* convert = window.findChild<QAction*>(QStringLiteral("action-convert-files"));
    QVERIFY(convert != nullptr);
    QVERIFY(play->isEnabled());
    QVERIFY(properties->isEnabled());
    QVERIFY(convert->isEnabled());
    QMenu* tools = nullptr;
    for (auto* action : track_menu->actions())
        if (action->text() == QStringLiteral("Tools"))
            tools = action->menu();
    QVERIFY(tools != nullptr);
    QVERIFY(tools->actions().contains(properties));
    QVERIFY(tools->actions().contains(convert));
    QVERIFY(!track_menu->actions().contains(properties));
    QVERIFY(!track_menu->actions().contains(convert));
    QVERIFY(remove->isEnabled());
    QCOMPARE(copy_menu->actions().size(), 3);
    QCOMPARE(move_menu->actions().size(), 3);
    copy_menu->actions().back()->trigger();
    QCOMPARE(destination->model()->rowCount(), 5);

    const auto unselected_position = source->visualRect(source->model()->index(2, 0)).center();
    QVERIFY(QMetaObject::invokeMethod(source, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, unselected_position)));
    QCOMPARE(source->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(source->selectionModel()->selectedRows().front().row(), 2);

    const auto album_header_position =
        QPoint{source->visualRect(source->model()->index(0, 0)).center().x(),
               source->visualRect(source->model()->index(0, 0)).top() + 5};
    QVERIFY(QMetaObject::invokeMethod(source, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, album_header_position)));
    QCOMPARE(source->selectionModel()->selectedRows().size(), 3);
    track_menu->close();
}

void BenchMainWindowTest::Library::cueSheetsExpandIntoPersistentSegmentRows() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto wave_path = media.filePath(QStringLiteral("disc.wav"));
    const auto cue_path = media.filePath(QStringLiteral("album.cue"));
    write_wave(wave_path, wave_sample_rate * 2U);
    {
        std::ofstream cue{QFile::encodeName(cue_path).toStdString(), std::ios::binary};
        cue << "REM DATE 2026\n"
               "PERFORMER \"Cue Artist\"\n"
               "TITLE \"Cue Album\"\n"
               "FILE \"disc.wav\" WAVE\n"
               "TRACK 01 AUDIO\n"
               "TITLE \"First Cue Track\"\n"
               "INDEX 01 00:00:00\n"
               "TRACK 02 AUDIO\n"
               "TITLE \"Second Cue Track\"\n"
               "INDEX 01 00:01:00\n";
    }
    const auto folder_encoded = QFile::encodeName(media.path());
    const std::string raw_folder{folder_encoded.constData(),
                                 static_cast<std::size_t>(folder_encoded.size())};
    const auto canonical_wave =
        std::filesystem::canonical(
            std::filesystem::path{QFile::encodeName(wave_path).toStdString()})
            .native();

    {
        BenchMainWindow window;
        window.show();
        window.openLocalPaths({raw_folder});
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(view != nullptr);
        auto* model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(model != nullptr);
        // The referenced disc.wav is represented by its two cue tracks, not
        // by an additional whole-file row from the same folder scan.
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);
        QTRY_COMPARE(view->rowHeight(0), view->verticalHeader()->defaultSectionSize() +
                                             ui::QueueItemDelegate::album_header_height);
        QCOMPARE(view->rowHeight(1), view->verticalHeader()->defaultSectionSize());
        const auto& rows = model->rows();
        QCOMPARE(rows[0].raw_path, canonical_wave);
        QCOMPARE(rows[1].raw_path, canonical_wave);
        QVERIFY(rows[0].logical_reference.has_value());
        QVERIFY(rows[1].logical_reference.has_value());
        QVERIFY(rows[0].logical_reference != rows[1].logical_reference);
        QVERIFY(rows[0].segment.has_value());
        QVERIFY(rows[1].segment.has_value());
        QCOMPARE(rows[0].segment->start_sample, 0);
        QCOMPARE(rows[0].segment->end_sample, std::optional<std::int64_t>{44'100});
        QCOMPARE(rows[1].segment->start_sample, 44'100);
        QCOMPARE(rows[1].segment->end_sample, std::optional<std::int64_t>{88'200});
        QCOMPARE(rows[0].title, std::string{"First Cue Track"});
        QCOMPARE(rows[1].title, std::string{"Second Cue Track"});
        QCOMPARE(rows[0].artist, std::string{"Cue Artist"});
        QCOMPARE(rows[0].album, std::string{"Cue Album"});
        QCOMPARE(rows[0].date, std::string{"2026"});
        QCOMPARE(rows[0].duration_ms, std::optional<std::int64_t>{1'000});
        QCOMPARE(rows[0].metadata.first_effective_value("title"),
                 std::optional<std::string>{"First Cue Track"});
        QCOMPARE(rows[0].metadata.first_effective_value("album_artist"),
                 std::optional<std::string>{"Cue Artist"});
        QVERIFY(rows[0].source_revision.has_value());
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* model =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 2);
    auto* restored_view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(restored_view != nullptr);
    QTRY_COMPARE(restored_view->rowHeight(0),
                 restored_view->verticalHeader()->defaultSectionSize() +
                     ui::QueueItemDelegate::album_header_height);
    QVERIFY(model->rows()[0].segment.has_value());
    QCOMPARE(model->rows()[0].segment->end_sample, std::optional<std::int64_t>{44'100});
    QVERIFY(model->rows()[1].logical_reference.has_value());
    QCOMPARE(model->rows()[0].metadata.first_effective_value("title"),
             std::optional<std::string>{"First Cue Track"});
    QCOMPARE(model->rows()[1].title, std::string{"Second Cue Track"});
}

void BenchMainWindowTest::Library::containerChaptersExpandIntoPersistentSegmentRows() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto container_path = media.filePath(QStringLiteral("chaptered.mka"));
    QVERIFY(
        materialize_audio_fixture(QStringLiteral("container-chapters-mka.b64"), container_path));
    const auto encoded = QFile::encodeName(container_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    {
        BenchMainWindow window;
        window.show();
        window.openLocalPaths({raw_path});
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(view != nullptr);
        auto* model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(model != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);
        const auto& rows = model->rows();
        QCOMPARE(rows[0].raw_path, raw_path);
        QCOMPARE(rows[1].raw_path, raw_path);
        QVERIFY(rows[0].logical_reference.has_value());
        QVERIFY(rows[1].logical_reference.has_value());
        QVERIFY(rows[0].logical_reference != rows[1].logical_reference);
        QVERIFY(rows[0].segment.has_value());
        QVERIFY(rows[1].segment.has_value());
        QCOMPARE(rows[0].segment->start_sample, 0);
        QCOMPARE(rows[0].segment->end_sample, std::optional<std::int64_t>{4'800});
        QCOMPARE(rows[1].segment->start_sample, 4'800);
        QCOMPARE(rows[1].segment->end_sample, std::optional<std::int64_t>{9'600});
        QCOMPARE(rows[0].title, std::string{"First chapter"});
        QCOMPARE(rows[1].title, std::string{"Second chapter"});
        QCOMPARE(rows[0].artist, std::string{"First Artist"});
        QCOMPARE(rows[1].artist, std::string{"Album Artist"});
        QCOMPARE(rows[0].album, std::string{"Chapter Album"});
        QCOMPARE(rows[0].album_artist, std::string{"Album Artist"});
        QCOMPARE(rows[0].date, std::string{"2026"});
        QCOMPARE(rows[0].track_number, std::string{"1"});
        QCOMPARE(rows[1].track_number, std::string{"2"});
        QCOMPARE(rows[0].duration_ms, std::optional<std::int64_t>{100});
        QCOMPARE(rows[1].duration_ms, std::optional<std::int64_t>{100});
        QCOMPARE(rows[0].metadata.first_effective_value("artist"),
                 std::optional<std::string>{"First Artist"});
        QCOMPARE(rows[1].metadata.first_effective_value("artist"),
                 std::optional<std::string>{"Album Artist"});
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* model =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 2);
    QVERIFY(model->rows()[0].segment.has_value());
    QCOMPARE(model->rows()[0].segment->start_sample, 0);
    QCOMPARE(model->rows()[0].segment->end_sample, std::optional<std::int64_t>{4'800});
    QVERIFY(model->rows()[1].logical_reference.has_value());
    QCOMPARE(model->rows()[0].metadata.first_effective_value("title"),
             std::optional<std::string>{"First chapter"});
}

void BenchMainWindowTest::Library::codecNativeSubsongsExpandAndPersistDecoderSelections() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto module_path = media.filePath(QStringLiteral("two-songs.mod"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("two-subsongs-mod.b64"), module_path));
    const auto encoded_folder = QFile::encodeName(media.path());
    const std::string raw_folder{encoded_folder.constData(),
                                 static_cast<std::size_t>(encoded_folder.size())};
    const auto encoded_path = QFile::encodeName(module_path);
    const std::string raw_path{encoded_path.constData(),
                               static_cast<std::size_t>(encoded_path.size())};

    {
        BenchMainWindow window;
        window.show();
        // Folder intake proves tracker extensions participate in bounded
        // discovery rather than working only through explicit file opens.
        window.openLocalPaths({raw_folder});
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(view != nullptr);
        auto* model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(model != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);
        const auto& rows = model->rows();
        for (std::size_t index = 0U; index < rows.size(); ++index) {
            QCOMPARE(rows[index].raw_path, raw_path);
            QVERIFY(rows[index].logical_reference.has_value());
            QCOMPARE(rows[index].selection.stream_index, std::optional<int>{0});
            QCOMPARE(rows[index].selection.subsong_index,
                     std::optional<int>{static_cast<int>(index)});
            QVERIFY(rows[index].segment.has_value());
            QCOMPARE(rows[index].segment->start_sample, 0);
            QCOMPARE(rows[index].segment->end_sample, std::optional<std::int64_t>{28'800});
            QCOMPARE(rows[index].title, "Subsong " + std::to_string(index + 1U));
            QCOMPARE(rows[index].album, std::string{"Trackknife subsongs"});
            QCOMPARE(rows[index].duration_ms, std::optional<std::int64_t>{600});
            QCOMPARE(rows[index].metadata.first_effective_value("title"),
                     std::optional<std::string>{"Subsong " + std::to_string(index + 1U)});
        }
        QVERIFY(rows[0].logical_reference != rows[1].logical_reference);
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* model =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 2);
    QCOMPARE(model->rows()[0].selection.subsong_index, std::optional<int>{0});
    QCOMPARE(model->rows()[1].selection.subsong_index, std::optional<int>{1});
    QVERIFY(model->rows()[1].segment.has_value());
    QCOMPARE(model->rows()[1].segment->end_sample, std::optional<std::int64_t>{28'800});
    QCOMPARE(model->rows()[1].metadata.first_effective_value("title"),
             std::optional<std::string>{"Subsong 2"});
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::Library)

#include "window_library_test.moc"
