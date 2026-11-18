// SPDX-License-Identifier: GPL-3.0-only

// The window test: Up Next.

#include "window_test.hpp"

#include "trackknife/core/engine_state.hpp"
#include "uicommon/library_tree_view.hpp"

#include <QPainter>
#include <QStandardItemModel>

namespace trackknife::bench {

class BenchMainWindowTest::UpNext final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void libraryDragsIntoUpNextWithCovers();
    void upNextPreservesNormalPlayback_data();
    void upNextPreservesNormalPlayback();
    void upNextEditingAndPersistence();
    void upNextPanelAnimationAndSettings();
    void upNextMultiSelectionEdits();
    void deleteInUpNextTakesItsTrackNotTheLists();
};

void BenchMainWindowTest::UpNext::libraryDragsIntoUpNextWithCovers() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_ && window.up_next_restored_);
    {
        auto catalogue = window.localCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    window.localLibrary()->refreshLibrary();
    auto* tree = window.localLibrary()->findChild<QTreeView*>();
    QVERIFY(tree != nullptr);
    QTRY_VERIFY(tree->model()->rowCount() > 0 && tree->model()
                                                     ->index(0, 0)
                                                     .data(Qt::DisplayRole)
                                                     .toString()
                                                     .contains(QStringLiteral("Trackknife")));

    // Dragged from the library onto Up Next: queued with its tags.
    window.findChild<QAction*>(QStringLiteral("action-show-up-next"))->trigger();
    QTRY_VERIFY(window.up_next_view_->isVisible());
    std::unique_ptr<QMimeData> mime{tree->model()->mimeData({tree->model()->index(0, 0)})};
    QVERIFY(mime != nullptr);
    auto* viewport = window.up_next_view_->viewport();
    QDragEnterEvent enter{dragPosition(QPoint{10, 10}), Qt::CopyAction, mime.get(), Qt::LeftButton,
                          Qt::NoModifier};
    QApplication::sendEvent(viewport, &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop{QPointF{10, 10}, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(viewport, &drop);
    QVERIFY(drop.isAccepted());
    QTRY_COMPARE(window.playback_.requests.pending().size(), std::size_t{1});
    const auto queued = window.playback_.requests.pending().front().source;
    QCOMPARE(queued.title, std::string{"Fixture Tone"});

    // And onto the header's Up Next button: queued at the end.
    {
        auto* button = window.up_next_button_;
        QDragEnterEvent over{dragPosition(QPoint{5, 5}), Qt::CopyAction, mime.get(), Qt::LeftButton,
                             Qt::NoModifier};
        QApplication::sendEvent(button, &over);
        QVERIFY(over.isAccepted());
        QDropEvent onto{QPointF{5, 5}, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier};
        QApplication::sendEvent(button, &onto);
        QVERIFY(onto.isAccepted());
        QTRY_COMPARE(window.playback_.requests.pending().size(), std::size_t{2});
    }

    // No tab holds its album, yet Up Next and the header find its cover.
    const auto key = LocalListModel::groupKeyOf(queued);
    for (const auto& tab : window.list_tabs_) {
        tab->model->setArtwork(key, {});
    }
    window.covers_.forget(key);
    window.up_next_local_model_->setArtwork(key, {});
    window.refreshUpNext();
    QTRY_VERIFY(window.up_next_local_model_->hasArtwork(key));
    window.refreshHeaderCover(QString::fromStdString(queued.entry_id.to_string()));
    QTRY_COMPARE(window.header_cover_key_, key);
}

void BenchMainWindowTest::UpNext::upNextMultiSelectionEdits() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.up_next_restored_ && !window.list_tabs_.empty());
    window.tabs_->setCurrentWidget(window.list_tabs_.front()->view);
    LocalTrackRow row;
    row.raw_path = "/tmp/request.flac";
    row.title = "Duplicate request";
    window.enqueueLocalRequests({row, row, row, row, row});
    const auto original = window.up_next_display_ids_;
    QCOMPARE(original.size(), std::size_t{5});
    // The header's pill counts what waits.
    auto* count = window.findChild<QLabel*>(QStringLiteral("bench-up-next-count"));
    QVERIFY(count != nullptr && !count->isHidden());
    QCOMPARE(count->text(), QStringLiteral("5"));
    auto* view = window.up_next_view_;
    QCOMPARE(view->selectionMode(), QAbstractItemView::ExtendedSelection);
    QCOMPARE(view->dragDropMode(), QAbstractItemView::DragDrop);
    QVERIFY(view->dragEnabled());
    QVERIFY(view->acceptDrops());
    QVERIFY(view->viewport()->acceptDrops());
    QVERIFY(view->showDropIndicator());
    QVERIFY(!view->dragDropOverwriteMode());
    QCOMPARE(view->defaultDropAction(), Qt::MoveAction);
    QVERIFY(view->model()->index(0, 0).flags().testFlag(Qt::ItemIsDragEnabled));
    const auto select = [&](std::initializer_list<int> rows) {
        view->selectionModel()->clearSelection();
        for (const auto i : rows)
            view->selectionModel()->select(view->model()->index(i, 0),
                                           QItemSelectionModel::Select | QItemSelectionModel::Rows);
    };
    select({1, 3});
    window.editUpNextSelection(4, 5);
    QCOMPARE(window.up_next_display_ids_,
             (std::vector<std::uint64_t>{original[0], original[2], original[4], original[1],
                                         original[3]}));
    QCOMPARE(view->selectionModel()->selectedRows().size(), 2);
    window.editUpNextSelection(2);
    QCOMPARE(window.up_next_display_ids_,
             (std::vector<std::uint64_t>{original[0], original[2], original[1], original[3],
                                         original[4]}));
    const auto moved = window.up_next_display_ids_;
    window.editUpNextSelection(1);
    QCOMPARE(window.playback_.requests.pending().size(), std::size_t{3});
    QVERIFY(window.playback_.requests.undo());
    window.refreshUpNext();
    QCOMPARE(window.up_next_display_ids_, moved);
    const auto revision = window.playback_.requests.revision();
    QVERIFY(!window.playback_.requests.retain({original[0], original[0]}));
    QCOMPARE(window.playback_.requests.revision(), revision);
    QCOMPARE(window.up_next_display_ids_, moved);
}

// ADR-0294: Up Next under the lists, its height kept; the side panels slide
// in, unless that is turned off.
void BenchMainWindowTest::UpNext::upNextPanelAnimationAndSettings() {
    QSettings{}.setValue(QStringLiteral("appearance/panel-animations"), false);
    QSettings{}.setValue(QStringLiteral("up-next/visible"), false);
    QSettings{}.setValue(QStringLiteral("up-next/height"), 240);
    QSettings{}.setValue(QStringLiteral("lyrics/visible"), false);
    QSettings{}.setValue(QStringLiteral("lyrics/width"), 380);
    BenchMainWindow window;
    window.resize(1320, 760);
    window.show();
    QTRY_VERIFY(window.up_next_restored_);

    // Up Next: a section under the lists, not a side panel.
    auto* section = window.up_next_section_;
    auto* split = window.findChild<QSplitter*>(QStringLiteral("bench-lists-split"));
    QVERIFY(split != nullptr);
    QCOMPARE(split->orientation(), Qt::Vertical);
    QCOMPARE(split->widget(0), window.tabs_);
    QCOMPARE(split->widget(1), section);
    QVERIFY(qobject_cast<QDockWidget*>(section) == nullptr);
    auto* up_next = window.findChild<QAction*>(QStringLiteral("action-show-up-next"));
    QVERIFY(!section->isVisible());
    up_next->trigger();
    QTRY_VERIFY(section->isVisible() && window.up_next_view_->isVisible());
    QTRY_VERIFY(qAbs(split->sizes().value(1) - 240) <= 2);
    QVERIFY(section->geometry().top() >= window.tabs_->geometry().bottom());
    split->setSizes({split->height() - 300, 300});
    emit split->splitterMoved(split->height() - 300, 1);
    QTRY_COMPARE(QSettings{}.value(QStringLiteral("up-next/height")).toInt(), 300);
    up_next->trigger();
    QTRY_VERIFY(!section->isVisible());
    QVERIFY(!QSettings{}.value(QStringLiteral("up-next/visible")).toBool());
    up_next->trigger();
    QTRY_VERIFY(section->isVisible());
    QTRY_VERIFY(qAbs(split->sizes().value(1) - 300) <= 2);
    up_next->trigger();
    QTRY_VERIFY(!section->isVisible());
    // With panel animations on, it slides up to that height and back down.
    QSettings{}.setValue(QStringLiteral("appearance/panel-animations"), true);
    auto* slide = split->findChild<QVariantAnimation*>();
    QVERIFY(slide != nullptr);
    up_next->trigger();
    QTRY_COMPARE(slide->state(), QAbstractAnimation::Running);
    QVERIFY(section->isVisible() && split->sizes().value(1) < 300);
    QTRY_COMPARE(slide->state(), QAbstractAnimation::Stopped);
    QTRY_VERIFY(qAbs(split->sizes().value(1) - 300) <= 2);
    up_next->trigger();
    QTRY_COMPARE(slide->state(), QAbstractAnimation::Running);
    QTRY_VERIFY(!section->isVisible());
    QSettings{}.setValue(QStringLiteral("appearance/panel-animations"), false);

    // A side panel slides in, as wide as it was left.
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-lyrics"));
    auto* animation = dock->findChild<QVariantAnimation*>();
    QVERIFY(animation);
    auto* toggle = window.findChild<QAction*>(QStringLiteral("action-show-lyrics"));
    QSettings{}.setValue(QStringLiteral("appearance/panel-animations"), true);
    toggle->trigger();
    QTRY_COMPARE(animation->state(), QAbstractAnimation::Running);
    QTest::qWait(40);
    toggle->trigger();
    QVERIFY(!toggle->isChecked());
    QTest::qWait(30);
    toggle->trigger();
    QTRY_COMPARE(animation->state(), QAbstractAnimation::Stopped);
    QVERIFY(dock->isVisible());
    QVERIFY(toggle->isChecked());
    QVERIFY(qAbs(dock->width() - 380) <= 2);
    window.resizeDocks({dock}, {460}, Qt::Horizontal);
    QTRY_COMPARE(QSettings{}.value(QStringLiteral("lyrics/width")).toInt(), 460);
    toggle->trigger();
    QTRY_VERIFY(!dock->isVisible());
    QCOMPARE(QSettings{}.value(QStringLiteral("lyrics/width")).toInt(), 460);
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::general);
    auto* checkbox =
        dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-panel-animations"));
    QVERIFY(checkbox && checkbox->isChecked());
    checkbox->setChecked(false);
    dialog->reject();
    QVERIFY(QSettings{}.value(QStringLiteral("appearance/panel-animations")).toBool());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog = window.showSettingsDialog(SettingsDialog::Page::general);
    checkbox = dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-panel-animations"));
    checkbox->setChecked(false);
    auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"));
    QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QVERIFY(!QSettings{}.value(QStringLiteral("appearance/panel-animations")).toBool());
    toggle->trigger();
    QVERIFY(dock->isVisible());
    QCOMPARE(animation->state(), QAbstractAnimation::Stopped);
    toggle->trigger();
    QVERIFY(!dock->isVisible());

    // The artists' initials in the library: grey, or -- set so -- on tiles
    // of their names' colours, the library redrawn once Settings is saved.
    auto* library = window.findChild<LocalLibraryPanel*>();
    QVERIFY(library != nullptr && !library->colouredInitials());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog = window.showSettingsDialog(SettingsDialog::Page::general);
    auto* coloured =
        dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-coloured-initials"));
    QVERIFY(coloured && !coloured->isChecked());
    coloured->setChecked(true);
    buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"));
    QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QVERIFY(QSettings{}.value(QStringLiteral("appearance/library-coloured-initials")).toBool());
    QVERIFY(library->colouredInitials());
    // Drawn so: an artist's tile is a colour, not a grey.
    {
        ui::LibraryTreeView view;
        QStandardItemModel model;
        model.appendRow(new QStandardItem(QStringLiteral("Royal Thunder")));
        view.setModel(&model);
        ui::LibraryTreeDelegate delegate{&view, {}, [](const QModelIndex&) {
                                             return ui::LibraryTreeDelegate::Presentation{
                                                 .track = false,
                                                 .album = false,
                                                 .root = true,
                                                 .artist = true,
                                                 .secondary = {},
                                                 .count = {},
                                                 .album_rating = 0U};
                                         }};
        const auto tile_saturation = [&view, &model, &delegate](const bool on) {
            delegate.setColouredInitials(on);
            QImage image{300, 40, QImage::Format_ARGB32};
            image.fill(Qt::white);
            QPainter painter{&image};
            QStyleOptionViewItem option;
            option.initFrom(&view);
            option.rect = image.rect();
            option.text = QStringLiteral("Royal Thunder");
            delegate.paint(&painter, option, model.index(0, 0));
            painter.end();
            // A corner of the tile, clear of its letters.
            return image.pixelColor(QPoint{7, 10}).hslSaturationF();
        };
        QVERIFY(tile_saturation(false) < 0.1F);
        QVERIFY(tile_saturation(true) > 0.25F);
    }
    dialog = window.showSettingsDialog(SettingsDialog::Page::general);
    dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-coloured-initials"))
        ->setChecked(false);
    buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"));
    QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QVERIFY(!library->colouredInitials());
    QSettings{}.remove(QStringLiteral("lyrics"));
    QSettings{}.remove(QStringLiteral("up-next"));
}

void BenchMainWindowTest::UpNext::upNextEditingAndPersistence() {
    audio::RequestQueue<std::string> queue;
    QVERIFY(queue.insert({"A", "A"}, 0));
    const auto first = queue.pending()[0];
    const auto second = queue.pending()[1];
    QVERIFY(first.id != second.id);
    QVERIFY(queue.insert({"X"}, 0));
    QCOMPARE(queue.pending()[0].source, std::string{"X"});
    QVERIFY(queue.canUndo());
    QVERIFY(queue.undo());
    QCOMPARE(queue.pending().size(), 2U);
    QVERIFY(queue.move(second.id, 0));
    QCOMPARE(queue.pending()[0].id, second.id);
    queue.started(second);
    QVERIFY(!queue.canUndo());
    queue.clear();
    QVERIFY(queue.active());
    QVERIFY(queue.pending().empty());
    queue.finished();
    QVERIFY(!queue.active());
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.up_next_restored_);
    window.tabs_->setCurrentIndex(0);
    LocalTrackRow row;
    row.raw_path = std::string{"/tmp/raw-"} + char(0xff) + ".flac";
    row.title = "Request";
    window.enqueueLocalRequests({row, row});
    QCOMPARE(window.playback_.requests.pending().size(), 2U);
    QVERIFY(!window.up_next_view_->albumGroupingEnabled());
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        window.findChild<QAction*>(QStringLiteral("action-show-up-next"))->setChecked(true);
        window.resize(1320, 760);
        QTest::qWait(250);
        QVERIFY(window.grab().save(directory + QStringLiteral("/up-next.png")));
    }
    window.editUpNext(1, 0);
    QCOMPARE(window.playback_.requests.pending().size(), 1U);
    bool saved = false;
    window.persistence_->loadUiState(QStringLiteral("playback/up-next/v1"),
                                     [&](QByteArray payload, QString error) {
                                         QVERIFY(error.isEmpty());
                                         QVERIFY(payload.contains("Request"));
                                         saved = true;
                                     });
    QTRY_VERIFY(saved);
    window.playback_.requests.clear();
    window.up_next_restored_ = false;
    window.restoreUpNext();
    QTRY_VERIFY(window.up_next_restored_);
    QCOMPARE(window.playback_.requests.pending().size(), 1U);
    QCOMPARE(window.playback_.requests.pending()[0].source.raw_path, row.raw_path);
}

void BenchMainWindowTest::UpNext::upNextPreservesNormalPlayback_data() {
    QTest::addColumn<bool>("consume");
    QTest::newRow("normal") << false;
    QTest::newRow("consume") << true;
}

void BenchMainWindowTest::UpNext::upNextPreservesNormalPlayback() {
    MELODY_REQUIRE_REALTIME_AUDIO();
    QFETCH(bool, consume);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto a = media.filePath(QStringLiteral("a.wav"));
    const auto b = media.filePath(QStringLiteral("b.wav"));
    const auto x = media.filePath(QStringLiteral("x.wav"));
    write_wave(a, wave_sample_rate * 2U);
    write_wave(b, wave_sample_rate * 2U);
    write_wave(x, wave_sample_rate);
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.up_next_restored_);
    window.openLocalPaths({QFile::encodeName(a).toStdString(), QFile::encodeName(b).toStdString()});
    QTRY_VERIFY(window.currentListTab() != nullptr &&
                window.currentListTab()->model->rowCount() == 2);
    QTRY_VERIFY(window.playingOnEngine());
    window.playback_.modes.consume = consume ? audio::ModeState::on : audio::ModeState::off;
    window.applyLocalPlaybackModes();
    auto* tab = window.currentListTab();
    window.playRow(*tab, 0);
    QTRY_COMPARE(window.property("trackknife-engine-playback").toString(),
                 QStringLiteral("playing"));
    LocalTrackRow request;
    request.raw_path = QFile::encodeName(b).toStdString();
    request.title = "X";
    window.enqueueLocalRequests({request, request});
    const auto first = window.playback_.requests.pending()[0].id;
    const auto second = window.playback_.requests.pending()[1].id;
    QTRY_VERIFY_WITH_TIMEOUT(window.playback_.requests.active() &&
                                 window.playback_.requests.active()->id == first,
                             5000);
    // The header names what Up Next is playing by its tags, though the entry
    // is in no open list -- not by its file name.
    auto* now_playing = window.findChild<QLabel*>(QStringLiteral("bench-now-playing"));
    QVERIFY(now_playing != nullptr);
    QTRY_COMPARE(now_playing->text(), QStringLiteral("X"));
    // And it is credited as what it is.
    QTRY_VERIFY(window.findChild<trackknife::bench::Workspace*>()
                    ->property("trackknife-lastfm-sample")
                    .toString()
                    .contains(QStringLiteral("|X|")));
    QCOMPARE(tab->model->rowCount(), consume ? 1 : 2);
    QTRY_VERIFY_WITH_TIMEOUT(window.playback_.requests.active() &&
                                 window.playback_.requests.active()->id == second,
                             5000);
    QTRY_VERIFY_WITH_TIMEOUT(!window.playback_.requests.active() &&
                                 row_of(window.tabForDocument(window.playback_.anchors.document),
                                        window.playback_.anchors.current) == (consume ? 0 : 1),
                             4000);
    QCOMPARE(tab->model->rowCount(), consume ? 1 : 2);
    QVERIFY(window.playback_.requests.pending().empty());
}

// Delete takes what is chosen where the keyboard is: in Up Next, its track,
// never the row chosen in the list behind it.
void BenchMainWindowTest::UpNext::deleteInUpNextTakesItsTrackNotTheLists() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model);
    LocalTrackRow row;
    row.raw_path = "/unavailable/track.flac";
    row.probed = true;
    model->replaceRows({row, row, row});
    // Nothing chosen in the list: Delete still works in Up Next.
    view->clearSelection();

    row.title = "Asked for";
    window.enqueueLocalRequests({row, row});
    QCOMPARE(window.up_next_display_ids_.size(), std::size_t{2});
    window.findChild<QAction*>(QStringLiteral("action-show-up-next"))->trigger();
    QTRY_VERIFY(window.up_next_view_->isVisible());
    window.up_next_view_->setCurrentIndex(window.up_next_view_->model()->index(0, 0));
    window.up_next_view_->setFocus();
    QTRY_VERIFY(window.up_next_view_->hasFocus());
    QTest::keyClick(window.up_next_view_, Qt::Key_Delete);
    QTRY_COMPARE(window.up_next_display_ids_.size(), std::size_t{1});
    QCOMPARE(model->rowCount(), 3);

    // In the list, the list's row.
    view->setCurrentIndex(model->index(1, 0));
    view->setFocus();
    QTest::keyClick(view, Qt::Key_Delete);
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(window.up_next_display_ids_.size(), std::size_t{1});
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::UpNext)

#include "window_up_next_test.moc"
