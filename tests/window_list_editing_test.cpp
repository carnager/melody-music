// SPDX-License-Identifier: GPL-3.0-only

// The window test: editing lists: reordering, undo and history.

#include "window_test.hpp"

namespace trackknife::bench {

class BenchMainWindowTest::ListEditing final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void currentTabHistoryPreservesOccurrences();
    void localReorderPreservesVisibleRowGeometry();
    void localListUndoRestoresOccurrencesAndFreshMetadata();
    void localListHistoryBranchesAndBounds();
    void crossTabMoveUndoIsOneTransaction();
    void localListUndoActionsRespectAuthorityAndTextEditing();
    void localListOrderingActionsRespectAuthorityAndPersist();
    void portablePlaylistImportsPreserveAuthorityAndPersist();
    void noncontiguousLocalReorderPreservesOccurrences();
};

void BenchMainWindowTest::ListEditing::currentTabHistoryPreservesOccurrences() {
    // The engine's database, which is the application's (ADR-0226): history
    // written there is what the engine answers with.
    const auto database =
        std::filesystem::path{
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toStdString()} /
        "lists.sqlite";
    auto repository = persistence::ListRepository::open(database);
    QVERIFY(repository);
    LocalTrackRow played, unplayed;
    played.raw_path = std::string{"/not-mounted/raw-"} + char(0xff) + ".flac";
    played.title = "Played";
    played.source_revision = core::LocalSourceRevision{.device = 1, .inode = 2};
    unplayed.raw_path = "/not-mounted/unplayed.flac";
    unplayed.title = "Unplayed";
    unplayed.source_revision = core::LocalSourceRevision{.device = 1, .inode = 3};
    persistence::ListItem source;
    source.source_reference = played.raw_path;
    source.source_revision = played.source_revision;
    QVERIFY(repository->record_local_listen(source, core::StableId::random(), 1000));
    CatalogueSource catalogues{database};
    SearchDialog dialog(
        catalogues, EngineKey::local(), [&]() -> std::optional<SearchDialog::TabSnapshot> {
            return SearchDialog::TabSnapshot{QStringLiteral("Current"), {played, unplayed, played}};
        });
    dialog.show();
    dialog.findChild<QComboBox*>(QStringLiteral("bench-search-scope"))->setCurrentIndex(1);
    dialog.findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"))->setChecked(true);
    auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* results = dialog.findChild<QListWidget*>(QStringLiteral("bench-search-results"));
    input->setText(QStringLiteral("HISTORY(playcount) EQUAL 1"));
    QTRY_COMPARE(results->count(), 2);
    input->setText(QStringLiteral("ALL SORT HISTORY(lastplayed)"));
    QTRY_COMPARE(results->count(), 3);
    QVERIFY(results->item(0)->text().contains(QStringLiteral("Unplayed")));
    QVERIFY(results->item(1)->text().contains(QStringLiteral("Played")));
}

void BenchMainWindowTest::ListEditing::localReorderPreservesVisibleRowGeometry() {
    LocalListModel model;
    const auto make_row = [](std::string path) {
        LocalTrackRow result;
        result.raw_path = std::move(path);
        result.title = result.raw_path;
        result.artist = "Artist";
        result.album = "Album";
        return result;
    };
    model.replaceRows(
        {make_row("/music/one.flac"), make_row("/music/two.flac"), make_row("/music/three.flac")});
    QSignalSpy reset{&model, &QAbstractItemModel::modelReset};
    QSignalSpy moved{&model, &QAbstractItemModel::rowsMoved};
    ui::QueueTableView view{nullptr};
    view.setModel(&model);
    view.setItemDelegate(new ui::QueueItemDelegate(&view));
    view.verticalHeader()->setDefaultSectionSize(22);
    view.verticalHeader()->setMinimumSectionSize(18);
    view.setAlbumGroupingEnabled(true);
    view.resize(640, 360);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    model.reorderRows({0}, 3);
    QCoreApplication::processEvents();

    QCOMPARE(reset.count(), 0);
    QCOMPARE(moved.count(), 1);
    QCOMPARE(model.rows()[0].raw_path, std::string{"/music/two.flac"});
    QCOMPARE(model.rows()[1].raw_path, std::string{"/music/three.flac"});
    QCOMPARE(model.rows()[2].raw_path, std::string{"/music/one.flac"});
    QCOMPARE(view.verticalHeader()->count(), model.rowCount());
    for (int row_index = 0; row_index < model.rowCount(); ++row_index) {
        QVERIFY(!view.isRowHidden(row_index));
        QVERIFY(view.rowHeight(row_index) >= 18);
        QVERIFY(!view.visualRect(model.index(row_index, local_title_column)).isEmpty());
    }
}

void BenchMainWindowTest::ListEditing::noncontiguousLocalReorderPreservesOccurrences() {
    LocalListModel model;
    const auto make_row = [](std::string path) {
        LocalTrackRow result;
        result.raw_path = std::move(path);
        result.title = result.raw_path;
        return result;
    };
    model.replaceRows({make_row("/music/one.flac"), make_row("/music/two.flac"),
                       make_row("/music/three.flac"), make_row("/music/four.flac")});
    std::vector<std::string> rows_seen_before_move;
    connect(&model, &QAbstractItemModel::layoutAboutToBeChanged, &model, [&] {
        for (const auto& row : model.rows()) {
            rows_seen_before_move.push_back(row.raw_path);
        }
    });
    QSignalSpy reset{&model, &QAbstractItemModel::modelReset};
    QAbstractItemModelTester tester{&model, QAbstractItemModelTester::FailureReportingMode::QtTest};
    const QPersistentModelIndex playing{model.index(2, local_title_column)};
    model.setCurrentSource(model.source(2), 2);

    model.reorderRows({0, 2}, 4);

    QCOMPARE(reset.count(), 0);
    QCOMPARE(rows_seen_before_move,
             (std::vector<std::string>{"/music/one.flac", "/music/two.flac", "/music/three.flac",
                                       "/music/four.flac"}));
    QCOMPARE(model.rows()[0].raw_path, std::string{"/music/two.flac"});
    QCOMPARE(model.rows()[1].raw_path, std::string{"/music/four.flac"});
    QCOMPARE(model.rows()[2].raw_path, std::string{"/music/one.flac"});
    QCOMPARE(model.rows()[3].raw_path, std::string{"/music/three.flac"});
    QCOMPARE(playing.row(), 3);
    QVERIFY(model.index(3, 0).data(ui::track_current_role).toBool());
    QVERIFY(model.undo());
    QCOMPARE(playing.row(), 2);
    QCOMPARE(model.rows()[2].raw_path, std::string{"/music/three.flac"});
    QVERIFY(model.redo());
    QCOMPARE(playing.row(), 3);
    QCOMPARE(reset.count(), 0);
}

void BenchMainWindowTest::ListEditing::localListUndoRestoresOccurrencesAndFreshMetadata() {
    LocalTrackRow whole;
    whole.raw_path = std::string{"/music/raw-"} + static_cast<char>(0xff) + ".flac";
    whole.title = "Original";
    whole.probed = true;
    const core::LocalSourceRevision before{.device = 1,
                                           .inode = 2,
                                           .size = 3,
                                           .modification_time_seconds = 4,
                                           .modification_time_nanoseconds = 5};
    auto published = before;
    published.inode = 6;
    auto relocated = published;
    relocated.inode = 7;
    whole.source_revision = before;
    auto logical = whole;
    logical.logical_reference = "cue-track-2";
    logical.segment = formats::SampleRange{.start_sample = 44100, .end_sample = 88200};
    metadata::MetadataField overlay;
    overlay.canonical_name = "title";
    overlay.native_name = "TITLE";
    overlay.values = {"Logical title"};
    overlay.provenance = metadata::FieldProvenance::sidecar;
    logical.metadata.fields.push_back(overlay);
    LocalTrackRow playing = whole;
    playing.raw_path = "/music/playing.flac";
    LocalListModel model;
    model.replaceRows({whole, logical, playing, whole});
    QAbstractItemModelTester tester{&model, QAbstractItemModelTester::FailureReportingMode::QtTest};
    const QPersistentModelIndex playback{model.index(2, 0)};
    model.setCurrentSource(model.source(2), 2);
    model.removeRowIndexes({3, 1, 0, 1, -1, 99});
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(playback.row(), 0);
    QVERIFY(model.canUndo());
    auto embedded = overlay;
    embedded.values = {"Published title"};
    embedded.provenance = metadata::FieldProvenance::embedded;
    metadata::MetadataDocument document;
    document.fields.push_back(embedded);
    // All instances of this physical file are detached, but history must still follow commits.
    const auto metadata_update = model.applyCommittedMetadata(whole.raw_path, document, published);
    QVERIFY(metadata_update.has_value());
    QCOMPARE(*metadata_update, 0U);
    const auto path_update = model.applyCommittedRelocation(whole.raw_path, "/archive/renamed.flac",
                                                            published, relocated);
    QVERIFY(path_update.has_value());
    QCOMPARE(*path_update, 0U);
    QVERIFY(model.undo());
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(playback.row(), 2);
    QVERIFY(model.index(2, 0).data(ui::track_current_role).toBool());
    QCOMPARE(model.rows()[0].raw_path, std::string{"/archive/renamed.flac"});
    QCOMPARE(model.rows()[0].title, std::string{"Published title"});
    QCOMPARE(model.rows()[0].source_revision, std::optional{relocated});
    QCOMPARE(model.rows()[1].title, std::string{"Logical title"});
    QCOMPARE(model.rows()[1].segment, logical.segment);
    QCOMPARE(model.rows()[1].logical_reference, logical.logical_reference);
    QCOMPARE(model.rows()[3], model.rows()[0]);
    QVERIFY(model.redo());
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(playback.row(), 0);
    QVERIFY(model.undo());
    QCOMPARE(model.rows()[0].raw_path, std::string{"/archive/renamed.flac"});
}

void BenchMainWindowTest::ListEditing::localListHistoryBranchesAndBounds() {
    LocalTrackRow first;
    first.raw_path = "/music/a.flac";
    LocalTrackRow second;
    second.raw_path = "/music/b.flac";
    LocalTrackRow third;
    third.raw_path = "/music/c.flac";
    LocalListModel model;
    model.replaceRows({first, second, third});
    model.removeRowIndexes({1});
    model.reorderRows({0}, 2);
    QVERIFY(model.undo());
    QVERIFY(model.undo());
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{first, second, third}));
    QVERIFY(model.redo());
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{first, third}));
    model.reorderRows({0}, 0); // A no-op preserves the redo chain.
    QVERIFY(model.canRedo());
    model.removeRowIndexes({0});
    QVERIFY(!model.canRedo());
    QVERIFY(model.undo());
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{first, third}));
    model.appendRows({second});
    QCOMPARE(model.undoLabel(), QStringLiteral("Add tracks"));
    QVERIFY(model.canUndo());
    QVERIFY(!model.canRedo());
    QVERIFY(model.undo());
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{first, third}));
    QVERIFY(model.redo());
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{first, third, second}));
    model.replaceRows({first, second}, true);
    QCOMPARE(model.undoLabel(), QStringLiteral("Replace list contents"));
    QVERIFY(model.undo());
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{first, third, second}));
    QVERIFY(model.redo());
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{first, second}));
    model.removeRowIndexes({0});
    model.removeRowIndexes({0}, false); // Consume and cross-tab moves do not enter local history.
    QVERIFY(!model.canUndo());
    model.replaceRows({first, second});
    QSignalSpy discarded{&model, &LocalListModel::historyDiscarded};
    for (int edit = 0; edit < 101; ++edit)
        model.reorderRows({0}, 2);
    QCOMPARE(discarded.size(), 1);
    int undone = 0;
    while (model.undo())
        ++undone;
    QCOMPARE(undone, 100);
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{second, first}));
    while (model.redo()) {
    }
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{second, first}));
}

void BenchMainWindowTest::ListEditing::crossTabMoveUndoIsOneTransaction() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    tabs->setCurrentIndex(0);
    auto* source_view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* source = qobject_cast<LocalListModel*>(source_view->model());
    QVERIFY(source != nullptr);
    LocalTrackRow first;
    first.raw_path = "/music/first.flac";
    LocalTrackRow second;
    second.raw_path = "/music/second.flac";
    source->replaceRows({first, second});
    window.duplicateCurrentTab();
    QCOMPARE(tabs->count(), 2);
    auto* target_view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* target = qobject_cast<LocalListModel*>(target_view->model());
    QVERIFY(target != nullptr && target != source);
    QCOMPARE(target->rowCount(), 2);

    QVERIFY(window.transferRows(source_view, {0},
                                target_view->property("bench-document-id").toString(), true, -1));
    QCOMPARE(source->rowCount(), 1);
    QCOMPARE(target->rowCount(), 3);
    QVERIFY(window.canReplayCrossTabMove(true));
    window.replayListEdit(true);
    QCOMPARE(source->rows(), (std::vector<LocalTrackRow>{first, second}));
    QCOMPARE(target->rows(), (std::vector<LocalTrackRow>{first, second}));
    QVERIFY(window.canReplayCrossTabMove(false));
    window.replayListEdit(false);
    QCOMPARE(source->rows(), (std::vector<LocalTrackRow>{second}));
    QCOMPARE(target->rows(), (std::vector<LocalTrackRow>{first, second, first}));
}

void BenchMainWindowTest::ListEditing::localListUndoActionsRespectAuthorityAndTextEditing() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model);
    auto* undo = window.findChild<QAction*>(QStringLiteral("action-undo-list-edit"));
    auto* redo = window.findChild<QAction*>(QStringLiteral("action-redo-list-edit"));
    QVERIFY(undo && redo);
    LocalTrackRow row;
    row.raw_path = "/unavailable/track.flac";
    row.probed = true;
    model->replaceRows({row, row, row});
    view->setCurrentIndex(model->index(1, 0));
    view->setFocus();
    QTest::keyClick(view, Qt::Key_Delete);
    QCOMPARE(model->rowCount(), 2);
    QVERIFY(undo->isEnabled());
    QVERIFY(undo->text().contains(QStringLiteral("Remove tracks")));
    QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
    releaseHeldModifiers(view);
    QCOMPARE(model->rowCount(), 3);
    QCOMPARE(view->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(view->selectionModel()->selectedRows().front().row(), 1);
    QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    releaseHeldModifiers(view);
    QCOMPARE(model->rowCount(), 2);
    // Undo follows the list on screen: another list has its own (empty)
    // history, and undoing there never reaches back into this one.
    auto* other =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Other",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    QVERIFY(other != nullptr);
    QCOMPARE(tabs->currentWidget(), other->view);
    QVERIFY(!undo->isEnabled());
    QVERIFY(!redo->isEnabled());
    undo->trigger();
    QCOMPARE(model->rowCount(), 2);
    tabs->setCurrentWidget(view);
    QVERIFY(undo->isEnabled());
    auto* selector = window.findChild<QTabBar*>(QStringLiteral("bench-local-source-tabs"));
    QVERIFY(selector);
    selector->setCurrentIndex(1);
    auto* search = window.findChild<QLineEdit*>(QStringLiteral("local-library-search"));
    QVERIFY(search);
    search->setText(QStringLiteral("ab"));
    search->insert(QStringLiteral("c"));
    search->setFocus();
    QTest::keyClick(search, Qt::Key_Z, Qt::ControlModifier);
    releaseHeldModifiers(search);
    QCOMPARE(search->text(), QStringLiteral("ab"));
    QCOMPARE(model->rowCount(), 2);
    view->setFocus();
    undo->trigger();
    QCOMPARE(model->rowCount(), 3);
    QVERIFY(tabs->tabText(tabs->indexOf(view)).endsWith(QStringLiteral(" *")));
}

void BenchMainWindowTest::ListEditing::localListOrderingActionsRespectAuthorityAndPersist() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* model = qobject_cast<LocalListModel*>(view->model());
    auto* sort = window.findChild<QAction*>(QStringLiteral("action-sort-list-title"));
    auto* reverse = window.findChild<QAction*>(QStringLiteral("action-reverse-list"));
    auto* deduplicate = window.findChild<QAction*>(QStringLiteral("action-deduplicate-list"));
    auto* sort_menu = window.findChild<QMenu*>(QStringLiteral("bench-sort-list-menu"));
    auto* bar = window.findChild<LocalListEditBar*>();
    QVERIFY(model && sort && reverse && deduplicate && sort_menu && bar);
    QVERIFY(!reverse->isEnabled());
    LocalTrackRow first;
    first.raw_path = "/unavailable/b.flac";
    first.title = "B";
    first.probed = true;
    auto second = first;
    second.raw_path = "/unavailable/a.flac";
    second.title = "A";
    model->replaceRows({first, second, second});
    QVERIFY(reverse->isEnabled());
    QSignalSpy edits{bar, &LocalListEditBar::edited};
    sort->trigger();
    QTRY_COMPARE(edits.size(), 1);
    QCOMPARE(model->rows().front().title, std::string{"A"});
    QVERIFY(tabs->tabText(tabs->indexOf(view)).endsWith(QStringLiteral(" *")));
    reverse->trigger();
    QTRY_COMPARE(edits.size(), 2);
    QCOMPARE(model->rows().front().title, std::string{"B"});
    deduplicate->trigger();
    QTRY_COMPARE(edits.size(), 3);
    QCOMPARE(model->rowCount(), 2);
    window.findChild<QAction*>(QStringLiteral("action-undo-list-edit"))->trigger();
    QCOMPARE(model->rowCount(), 3);
    // Ordering actions follow the list on screen, and an empty one offers none.
    auto* other =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Other",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    QVERIFY(other != nullptr);
    QCOMPARE(tabs->currentWidget(), other->view);
    QVERIFY(!sort_menu->isEnabled());
    QVERIFY(!reverse->isEnabled());
    QVERIFY(!deduplicate->isEnabled());
    QVERIFY(bar->isHidden());
    // Even a directly invoked action cannot use a stale local destination.
    reverse->trigger();
    QCOMPARE(model->rowCount(), 3);
    // Closed again, so the restart below restores only the list under test.
    window.closeTabAt(tabs->indexOf(other->view));
    tabs->setCurrentWidget(view);
    QVERIFY(reverse->isEnabled());
    // Custom-expression editing retains native text undo, separate from list undo.
    window.findChild<QAction*>(QStringLiteral("action-sort-list-custom"))->trigger();
    auto* expression = bar->findChild<QLineEdit*>();
    expression->setText(QStringLiteral("%title%"));
    expression->setCursorPosition(static_cast<int>(expression->text().size()));
    expression->insert(QStringLiteral("x"));
    QTest::keyClick(expression, Qt::Key_Z, Qt::ControlModifier);
    releaseHeldModifiers(expression);
    QCOMPARE(expression->text(), QStringLiteral("%title%"));
    QCOMPARE(model->rowCount(), 3);
    // Closing cancels a pending edit before the durable workspace flush.
    reverse->trigger();
    window.close();
    BenchMainWindow reopened;
    reopened.show();
    auto* restored_tabs = reopened.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(restored_tabs->count(), 1);
    auto* restored_view = qobject_cast<QTableView*>(restored_tabs->currentWidget());
    auto* restored_model = qobject_cast<LocalListModel*>(restored_view->model());
    QVERIFY(restored_model);
    QTRY_COMPARE(restored_model->rowCount(), 3);
    QCOMPARE(restored_model->rows()[0].raw_path, first.raw_path);
    QCOMPARE(restored_model->rows()[1].raw_path, second.raw_path);
    QCOMPARE(restored_model->rows()[2].raw_path, second.raw_path);
    QVERIFY(!restored_model->canUndo());
}

void BenchMainWindowTest::ListEditing::portablePlaylistImportsPreserveAuthorityAndPersist() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto playlist = media.filePath(QStringLiteral("Offline mix.m3u8"));
    QFile file{playlist};
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray contents{"#EXTM3U\n#EXTINF:12.345,Offline title\ngone.flac\ngone.flac\n"};
    QCOMPARE(file.write(contents), contents.size());
    file.close();
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* local_view = qobject_cast<QTableView*>(tabs->currentWidget());
        auto* local_model = qobject_cast<LocalListModel*>(local_view->model());
        QVERIFY(local_model);
        auto* import_action = window.findChild<QAction*>(QStringLiteral("action-import-m3u8"));
        auto* export_action = window.findChild<QAction*>(QStringLiteral("action-export-m3u8"));
        QVERIFY(import_action->isEnabled());
        auto* bar = window.findChild<PlaylistTransferBar*>();
        QSignalSpy completed{bar, &PlaylistTransferBar::completed};
        window.importM3u8Path(QFile::encodeName(playlist).toStdString());
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(completed[0][0].toBool());
        QCOMPARE(tabs->count(), 2);
        QVERIFY(export_action->isEnabled());
        // Imported into a list of its own, not into the one that was showing.
        QCOMPARE(local_model->rowCount(), 0);
        auto* imported_view = qobject_cast<QTableView*>(tabs->currentWidget());
        auto* imported_model = qobject_cast<LocalListModel*>(imported_view->model());
        QVERIFY(imported_model);
        QCOMPARE(imported_model->rowCount(), 2);
        QCOMPARE(imported_model->rows()[0].title, std::string{"Offline title"});
        QCOMPARE(imported_model->rows()[0].raw_path, imported_model->rows()[1].raw_path);
        window.close();
    }
    BenchMainWindow reopened;
    reopened.show();
    auto* tabs = reopened.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 2);
    LocalListModel* restored = nullptr;
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->tabText(i) == QStringLiteral("Offline mix")) {
            auto* view = qobject_cast<QTableView*>(tabs->widget(i));
            restored = view ? qobject_cast<LocalListModel*>(view->model()) : nullptr;
        }
    }
    QVERIFY(restored);
    QCOMPARE(restored->rowCount(), 2);
    QCOMPARE(restored->rows()[0].title, std::string{"Offline title"});
    QCOMPARE(restored->rows()[0].duration_ms, std::optional<std::int64_t>{12345});
    QCOMPARE(restored->rows()[0].raw_path, restored->rows()[1].raw_path);
    QVERIFY(restored->rows()[0].probed);
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::ListEditing)

#include "window_list_editing_test.moc"
