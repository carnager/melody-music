// SPDX-License-Identifier: GPL-3.0-only

// The window test: file work: renames, moves, preparation and recovery.

#include "window_test.hpp"

#include "trackknife/core/engine_state.hpp"
#include "workspace/artwork_session.hpp"
#include "workspace/sources.hpp"

#include <QGroupBox>
#include <QMessageBox>
#include <sqlite3.h>

namespace trackknife::bench {

class BenchMainWindowTest::FileWork final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void theLibraryMenuMovesTheLibrary();
    void theEngineFolderDialogBrowsesAnEnginesMachine();
    void metadataRenameMovesTheFileAndItsRow();
    void aFileAlreadyWhereRenamingPutsItGetsItsCover();
    void aMovedAlbumGetsItsFolderCoverWhereItMoved_data();
    void aMovedAlbumGetsItsFolderCoverWhereItMoved();
    void aCoverWrittenWithAMoveShowsInItsTab();
    void applyReadsAgainAFileChangedElsewhere();
    void listRowsFollowChangedFiles();
    void fileWorkWithoutAnEngineSaysWhy();
    void preparationSidePanelEditsReusableOutputProfiles();
    void pathOnlyPreparationUsesActualTagsAndAppliesReviewedPlan();
    void moveDestinationsArePerEngine();
    void actionChoicesAreRemembered();
    void actionsLinksOpenTheirOwnSettings();
    void namingLayoutsAreMelodys();
    void combinedTagAndRenameReviewReachesPreparationApply_data();
    void combinedTagAndRenameReviewReachesPreparationApply();
    void applyMovesIntoAChosenFolder();
    void metadataStartupPresentsReconciliation();
    void filePublicationStartupPresentsReconciliation();
    void combinedPublicationStartupRecoversMetadataAndPath();
};

// Rename ticked, the file already named as the layout names it: nothing
// moves, so its folder cover, tags and all are written -- not refused as for
// files moving away from their cover (ADR-0184).
void BenchMainWindowTest::FileWork::aFileAlreadyWhereRenamingPutsItGetsItsCover() {
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_embed_key), true);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_folder_image_key), true);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_folder_image_name_key),
                         QStringLiteral("cover.jpg"));
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto staging = media.filePath(QStringLiteral("staging.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), staging));
    const auto read = metadata::read_local_metadata(QFile::encodeName(staging).toStdString());
    QVERIFY(read.has_value());
    const auto titles = read->document.effective_values("title");
    QVERIFY(!titles.empty());
    // Named as the layout names it.
    const auto source_path =
        media.filePath(QString::fromStdString(titles.front()) + QStringLiteral(".flac"));
    QVERIFY(QFile::rename(staging, source_path));
    const auto raw_path = QFile::encodeName(source_path).toStdString();
    const auto cover_file = media.filePath(QStringLiteral("picked.png"));
    QImage picked{40, 40, QImage::Format_RGB32};
    picked.fill(Qt::darkYellow);
    QVERIFY(picked.save(cover_file, "PNG"));

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    bool layout_saved = false;
    window.persistence_->saveOutputLayoutProfile(
        persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile =
                operations::OutputLayoutProfile{
                    .schema_version = 1U,
                    .name = "By title",
                    .dialect = {},
                    .relative_directory_expression = {},
                    .basename_expression = "%title%",
                    .sanitization_policy = {"linux", 1U},
                },
        },
        [&layout_saved](const QString& error) { layout_saved = error.isEmpty(); });
    QTRY_VERIFY(layout_saved);
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr && properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows().front().probed, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties_action->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    properties_action->trigger();
    auto* properties = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* rename_files =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-rename-files"));
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    auto* artwork = properties->findChild<ArtworkSession*>();
    QVERIFY(aggregate_model && rename_files && apply && status && artwork);
    QTRY_VERIFY(rename_files->isEnabled());
    const auto album_row = aggregate_model->fieldRow(QStringLiteral("album"));
    QVERIFY(album_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*album_row, 2),
                                     QStringLiteral("Kept in place"), Qt::EditRole));
    // A cover staged: as Fetch cover… or Add image… stage one.
    artwork->setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(artwork->canAdd(), 10'000);
    artwork->add(cover_file, 0);
    QTRY_VERIFY_WITH_TIMEOUT(artwork->hasPendingChanges(), 10'000);
    rename_files->setChecked(true);
    QTRY_VERIFY(apply->isEnabled());
    QTest::mouseClick(apply, Qt::LeftButton);
    // Not refused: the folder cover is offered for review, as always.
    QDialog* review = nullptr;
    QTRY_VERIFY2_WITH_TIMEOUT((review = properties->findChild<QDialog*>(
                                   QStringLiteral("bench-folder-cover-review"))) != nullptr,
                              qPrintable(status->text()), 15'000);
    review->findChild<QDialogButtonBox*>(QStringLiteral("bench-folder-cover-review-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    // The cover beside the file, the tag in it, the name kept.
    const auto cover = media.filePath(QStringLiteral("cover.jpg"));
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(cover), qPrintable(status->text()), 15'000);
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            const auto after = metadata::read_local_metadata(raw_path);
            return after && after->document.effective_values("album") ==
                                std::vector<std::string>{"Kept in place"};
        }(),
        10'000);
    QVERIFY(QFile::exists(source_path));
    QVERIFY(!status->text().contains(QStringLiteral("Save folder covers")));
    QPointer guard{properties};
    QVERIFY(window_test::closeTaggerAfterWrite(guard));
}

// ADR-0318: an album moved with a folder cover staged -- its old cover
// travels along and is replaced by the new one in the folder it moved to,
// the old folder gone; undone, the old cover is back where it was.
void BenchMainWindowTest::FileWork::aMovedAlbumGetsItsFolderCoverWhereItMoved_data() {
    QTest::addColumn<bool>("undone");
    QTest::newRow("undone") << true;
    QTest::newRow("kept") << false;
}

void BenchMainWindowTest::FileWork::aMovedAlbumGetsItsFolderCoverWhereItMoved() {
    QFETCH(bool, undone);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_embed_key), true);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_folder_image_key), true);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_folder_image_name_key),
                         QStringLiteral("cover.jpg"));
    QTemporaryDir media;
    QVERIFY(media.isValid());
    QVERIFY(QDir{media.path()}.mkpath(QStringLiteral("Old")));
    QVERIFY(QDir{media.path()}.mkpath(QStringLiteral("Sorted")));
    const auto staging = media.filePath(QStringLiteral("Old/staging.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), staging));
    const auto read = metadata::read_local_metadata(QFile::encodeName(staging).toStdString());
    QVERIFY(read.has_value());
    const auto title = QString::fromStdString(read->document.effective_values("title").front());
    const auto source_path =
        media.filePath(QStringLiteral("Old/") + title + QStringLiteral(".flac"));
    QVERIFY(QFile::rename(staging, source_path));
    const auto raw_path = QFile::encodeName(source_path).toStdString();
    QImage old_cover{30, 30, QImage::Format_RGB32};
    old_cover.fill(Qt::red);
    QVERIFY(old_cover.save(media.filePath(QStringLiteral("Old/cover.jpg")), "JPEG"));
    const auto picked_file = media.filePath(QStringLiteral("picked.png"));
    QImage picked{40, 40, QImage::Format_RGB32};
    picked.fill(Qt::yellow);
    QVERIFY(picked.save(picked_file, "PNG"));
    const auto colour_of = [](const QString& path) { return QImage{path}.pixelColor(10, 10); };

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    bool layout_saved = false;
    window.persistence_->saveOutputLayoutProfile(
        persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile =
                operations::OutputLayoutProfile{
                    .schema_version = 1U,
                    .name = "Moved",
                    .dialect = {},
                    .relative_directory_expression = "Album",
                    .basename_expression = "%title%",
                    .sanitization_policy = {"linux", 1U},
                },
        },
        [&layout_saved](const QString& error) { layout_saved = error.isEmpty(); });
    QTRY_VERIFY(layout_saved);
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr && properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows().front().probed, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties_action->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    properties_action->trigger();
    auto* properties = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);
    TaggerSession* session = nullptr;
    ArtworkSession* artwork = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((session = properties->findChild<TaggerSession*>()) != nullptr &&
                                 (artwork = properties->findChild<ArtworkSession*>()) != nullptr,
                             10'000);
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(apply && status);
    QTRY_VERIFY_WITH_TIMEOUT(session->renameAvailable(), 10'000);
    session->chooseMoveFolder(
        QFile::encodeName(media.filePath(QStringLiteral("Sorted"))).toStdString());
    QTRY_VERIFY_WITH_TIMEOUT(session->moveAvailable(), 10'000);
    session->chooseMove(true);
    artwork->setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(artwork->canAdd(), 10'000);
    artwork->add(picked_file, 0);
    QTRY_VERIFY_WITH_TIMEOUT(artwork->hasPendingChanges(), 10'000);
    QTRY_VERIFY(apply->isEnabled());
    QTest::mouseClick(apply, Qt::LeftButton);
    QDialog* review = nullptr;
    QTRY_VERIFY2_WITH_TIMEOUT((review = properties->findChild<QDialog*>(
                                   QStringLiteral("bench-folder-cover-review"))) != nullptr,
                              qPrintable(status->text()), 15'000);
    review->findChild<QDialogButtonBox*>(QStringLiteral("bench-folder-cover-review-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    const auto moved =
        media.filePath(QStringLiteral("Sorted/Album/") + title + QStringLiteral(".flac"));
    const auto cover = media.filePath(QStringLiteral("Sorted/Album/cover.jpg"));
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(moved), qPrintable(status->text()), 15'000);
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(cover) && colour_of(cover).green() > 200 &&
                                  colour_of(cover).red() > 200,
                              qPrintable(status->text()), 10'000);
    // Nothing of the album left where it was: the file it moved from is kept
    // hidden, to undo the move while the tagger is open.
    QVERIFY(QDir{media.filePath(QStringLiteral("Old"))}
                .entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                .isEmpty());
    if (!undone) {
        // Closed, the kept file let go of, and the folder with it.
        QPointer kept_guard{properties};
        QVERIFY(window_test::closeTaggerAfterWrite(kept_guard));
        QTRY_VERIFY_WITH_TIMEOUT(!QDir{media.filePath(QStringLiteral("Old"))}.exists(), 10'000);
        return;
    }
    // Undone: the album back, with its own cover.
    QTRY_VERIFY_WITH_TIMEOUT(session->canUndoWrite(), 10'000);
    session->undoWrite();
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(source_path), qPrintable(status->text()), 15'000);
    const auto old_back = media.filePath(QStringLiteral("Old/cover.jpg"));
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(old_back) && colour_of(old_back).red() > 200 &&
                                  colour_of(old_back).green() < 60,
                              qPrintable(status->text()), 10'000);
    QPointer guard{properties};
    QVERIFY(window_test::closeTaggerAfterWrite(guard));
}

// A file shown without a cover -- it had none -- that is given one as it is
// moved shows it in its tab: the miss cached before does not outlive the
// write.
void BenchMainWindowTest::FileWork::aCoverWrittenWithAMoveShowsInItsTab() {
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_embed_key), true);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_folder_image_key), false);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    QVERIFY(QDir{media.path()}.mkpath(QStringLiteral("Old")));
    QVERIFY(QDir{media.path()}.mkpath(QStringLiteral("Sorted")));
    const auto staging = media.filePath(QStringLiteral("Old/staging.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), staging));
    const auto read = metadata::read_local_metadata(QFile::encodeName(staging).toStdString());
    QVERIFY(read.has_value());
    const auto title = QString::fromStdString(read->document.effective_values("title").front());
    const auto source_path =
        media.filePath(QStringLiteral("Old/") + title + QStringLiteral(".flac"));
    QVERIFY(QFile::rename(staging, source_path));
    const auto picked_file = media.filePath(QStringLiteral("picked.png"));
    QImage picked{40, 40, QImage::Format_RGB32};
    picked.fill(Qt::yellow);
    QVERIFY(picked.save(picked_file, "PNG"));

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    bool layout_saved = false;
    window.persistence_->saveOutputLayoutProfile(
        persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile =
                operations::OutputLayoutProfile{
                    .schema_version = 1U,
                    .name = "Moved",
                    .dialect = {},
                    .relative_directory_expression = "Album",
                    .basename_expression = "%title%",
                    .sanitization_policy = {"linux", 1U},
                },
        },
        [&layout_saved](const QString& error) { layout_saved = error.isEmpty(); });
    QTRY_VERIFY(layout_saved);
    window.openLocalPaths({QFile::encodeName(source_path).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr && properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows().front().probed, 5'000);
    // Looked for, and none: the miss is cached.
    const auto key = list_model->groupKey(0);
    QTRY_VERIFY_WITH_TIMEOUT(window.workspace_.artwork_cache_.contains(key), 10'000);
    QVERIFY(!list_model->hasArtwork(key));

    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties_action->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    properties_action->trigger();
    auto* properties = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);
    TaggerSession* session = nullptr;
    ArtworkSession* artwork = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((session = properties->findChild<TaggerSession*>()) != nullptr &&
                                 (artwork = properties->findChild<ArtworkSession*>()) != nullptr,
                             10'000);
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(apply && status);
    QTRY_VERIFY_WITH_TIMEOUT(session->renameAvailable(), 10'000);
    session->chooseMoveFolder(
        QFile::encodeName(media.filePath(QStringLiteral("Sorted"))).toStdString());
    QTRY_VERIFY_WITH_TIMEOUT(session->moveAvailable(), 10'000);
    session->chooseMove(true);
    artwork->setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(artwork->canAdd(), 10'000);
    artwork->add(picked_file, 0);
    QTRY_VERIFY_WITH_TIMEOUT(artwork->hasPendingChanges(), 10'000);
    QTRY_VERIFY(apply->isEnabled());
    QTest::mouseClick(apply, Qt::LeftButton);
    const auto moved =
        media.filePath(QStringLiteral("Sorted/Album/") + title + QStringLiteral(".flac"));
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(moved), qPrintable(status->text()), 15'000);
    // The tab shows the cover written with the move.
    QTRY_VERIFY2_WITH_TIMEOUT(list_model->hasArtwork(list_model->groupKey(0)),
                              qPrintable(status->text()), 10'000);
    QPointer guard{properties};
    QVERIFY(window_test::closeTaggerAfterWrite(guard));
}

void BenchMainWindowTest::FileWork::metadataRenameMovesTheFileAndItsRow() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto source_path = media.filePath(QStringLiteral("rename-me.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), source_path));
    const auto encoded = QFile::encodeName(source_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    bool layout_saved = false;
    window.persistence_->saveOutputLayoutProfile(
        persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile =
                operations::OutputLayoutProfile{
                    .schema_version = 1U,
                    .name = "By title",
                    .dialect = {},
                    .relative_directory_expression = {},
                    .basename_expression = "%title%",
                    .sanitization_policy = {"linux", 1U},
                },
        },
        [&layout_saved](const QString& error) { layout_saved = error.isEmpty(); });
    QTRY_VERIFY(layout_saved);
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr);
    QVERIFY(properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    QTRY_VERIFY(!window.property("trackknife-metadata-operation-running").toBool());
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows().front().probed, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties_action->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    properties_action->trigger();

    auto* properties = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);
    QCOMPARE(properties->property("trackknife-file-work").toString(), QStringLiteral("engine"));
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* rename_files =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-rename-files"));
    auto* preview =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(rename_files != nullptr);
    QVERIFY(preview != nullptr);
    QTRY_VERIFY(rename_files->isEnabled());
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*title_row, 2),
                                     QStringLiteral("Renamed by title"), Qt::EditRole));
    rename_files->setChecked(true);
    QTRY_VERIFY(preview->isEnabled());
    QTest::mouseClick(preview, Qt::LeftButton);

    // The row follows the file to its new name, which the new title made.
    const auto target = media.filePath(QStringLiteral("Renamed by title.flac"));
    const auto target_raw = QFile::encodeName(target).toStdString();
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rows().front().raw_path, target_raw, 10'000);
    // ADR-0278: the row reads its new tags from the file a moment after.
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rows().front().title, std::string{"Renamed by title"},
                              5'000);
    QVERIFY(!QFile::exists(source_path));
    const auto reread = metadata::read_local_metadata(target_raw);
    QVERIFY(reread.has_value());
    QCOMPARE(reread->document.effective_values("title"),
             (std::vector<std::string>{"Renamed by title"}));
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) == nullptr);
    QVERIFY(window_test::closeTaggerAfterWrite(window));
    QTRY_VERIFY_WITH_TIMEOUT(!window.property("trackknife-metadata-operation-running").toBool(),
                             5'000);
}

// ADR-0268: a file changed by another tagger after the editor read it is no
// dead end. Apply reads it again, the draft kept, for another look; the
// next Apply writes it.
void BenchMainWindowTest::FileWork::applyReadsAgainAFileChangedElsewhere() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("changed-elsewhere.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr && properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows().front().probed, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties_action->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    properties_action->trigger();

    auto* properties = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(aggregate_model != nullptr && status != nullptr && apply != nullptr);
    // Read and ready: the status says so, or has moved on to the draft.
    QTRY_VERIFY2_WITH_TIMEOUT(status->text().contains(QStringLiteral("Tags read")) ||
                                  status->text().contains(QStringLiteral("No pending edits")),
                              qPrintable(status->text()), 10'000);
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*title_row, 2),
                                     QStringLiteral("Edited here"), Qt::EditRole));

    // Another tagger writes the file meanwhile: it is no longer the one read.
    const auto before = metadata::read_local_metadata(raw_path);
    QVERIFY(before.has_value());
    {
        QFile file{path};
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(-3'600),
                                 QFileDevice::FileModificationTime));
    }

    QTRY_VERIFY(apply->isEnabled());
    QTest::mouseClick(apply, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("Changed since read")), 10'000);
    QVERIFY(status->text().contains(QStringLiteral("1 draft kept")));
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) == nullptr);
    QTRY_COMPARE(aggregate_model->index(*title_row, 2).data(Qt::DisplayRole).toString(),
                 QStringLiteral("Edited here"));
    const auto untouched = metadata::read_local_metadata(raw_path);
    QVERIFY(untouched.has_value());
    QCOMPARE(untouched->document.effective_values("title"),
             before->document.effective_values("title"));

    // Looked at again, applied again: written.
    QTRY_VERIFY(apply->isEnabled());
    QTest::mouseClick(apply, Qt::LeftButton);
    QVERIFY(window_test::closeTaggerAfterWrite(window));
    const auto written = metadata::read_local_metadata(raw_path);
    QVERIFY(written.has_value());
    QCOMPARE(written->document.effective_values("title"),
             (std::vector<std::string>{"Edited here"}));
    QTRY_VERIFY_WITH_TIMEOUT(!window.property("trackknife-metadata-operation-running").toBool(),
                             5'000);
}

// ADR-0268: the rows a change names -- at its paths, or under a folder of
// them -- are described again, a CUE track by its sheet and not; and a row
// described again after a rename, naming the file as written, still follows
// the file to its new name.
void BenchMainWindowTest::FileWork::listRowsFollowChangedFiles() {
    LocalListModel model;
    const auto row = [](const char* path) {
        LocalTrackRow track;
        track.raw_path = path;
        track.probed = true;
        return track;
    };
    auto cue = row("/music/A/live.flac");
    cue.segment = formats::SampleRange{.start_sample = 0, .end_sample = 100};
    model.replaceRows({row("/music/A/01.flac"), row("/music/A/02.flac"), row("/music/B/01.flac"),
                       row("/music/AB/01.flac"), cue});
    const auto ids = [&model](const std::vector<core::StableId>& entries) {
        std::vector<std::string> paths;
        for (const auto& entry : entries) {
            for (const auto& track : model.rows()) {
                if (track.entry_id == entry) {
                    paths.push_back(track.raw_path);
                }
            }
        }
        return paths;
    };
    QCOMPARE(ids(model.entriesAt({"/music/A/02.flac"}, false)),
             (std::vector<std::string>{"/music/A/02.flac"}));
    QCOMPARE(ids(model.entriesAt({"/music/A"}, false)),
             (std::vector<std::string>{"/music/A/01.flac", "/music/A/02.flac"}));
    QCOMPARE(model.entriesAt({}, true).size(), std::size_t{4U});

    const core::LocalSourceRevision before{.device = 1, .inode = 10};
    const core::LocalSourceRevision written{.device = 1, .inode = 11};
    auto described = row("/music/old.flac");
    described.source_revision = written;
    model.replaceRows({described});
    const auto moved =
        model.applyCommittedRelocation("/music/old.flac", "/music/new.flac", before, written);
    QVERIFY(moved && *moved == 1U);
    QCOMPARE(model.rows().front().raw_path, std::string{"/music/new.flac"});
    auto other = row("/music/old.flac");
    other.source_revision = core::LocalSourceRevision{.device = 1, .inode = 99};
    model.replaceRows({other});
    QVERIFY(!model.applyCommittedRelocation("/music/old.flac", "/music/new.flac", before, written));
}

// ADR-0237: files are written only by the engine holding them. Without one
// that does file work, the tools say so rather than open to an Apply that
// cannot happen.
void BenchMainWindowTest::FileWork::fileWorkWithoutAnEngineSaysWhy() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("unwritable.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    BenchMainWindow window;
    window.show();
    window.openLocalPaths({QFile::encodeName(path).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    window.localEngine().does_file_work = false;

    auto* properties = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(properties != nullptr);
    QTRY_VERIFY(properties->isEnabled());
    properties->trigger();
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties")) == nullptr);
    QCOMPARE(window.statusBar()->currentMessage(),
             QStringLiteral("Editing tags is done by the engine on this computer, which is not "
                            "available right now"));

    auto* replaygain = window.findChild<QAction*>(QStringLiteral("action-replaygain-dialog"));
    QVERIFY(replaygain != nullptr);
    replaygain->trigger();
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("bench-replaygain-dialog")) == nullptr);
    QVERIFY(window.statusBar()->currentMessage().startsWith(
        QStringLiteral("Measuring ReplayGain is done by the engine on this computer")));
}

void BenchMainWindowTest::FileWork::preparationSidePanelEditsReusableOutputProfiles() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("profiles.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    const auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = raw_path,
                .source_revision = read->source_revision,
                .baseline = read->document,
            },
        .track_label = QStringLiteral("Profile fixture"),
    };

    std::vector<persistence::SavedOutputLayoutProfile> layouts{
        persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile =
                operations::OutputLayoutProfile{
                    .schema_version = 1U,
                    .name = "Albums",
                    .dialect = {},
                    .relative_directory_expression = "%album artist%/%album%",
                    .basename_expression = "%tracknumber% - %title%",
                    .sanitization_policy = {"linux", 1U},
                },
        },
    };
    std::vector<persistence::SavedDestinationProfile> destinations{
        persistence::SavedDestinationProfile{
            .id = core::StableId::random(),
            .profile =
                operations::DestinationProfile{
                    .schema_version = 1U,
                    .name = "Library",
                    .root_raw_path = QFile::encodeName(media.path()).toStdString(),
                    .containment_policy = {"lexical-beneath-root", 1U},
                },
        },
    };
    OutputProfileStore output_store{
        .load =
            [&layouts, &destinations](OutputProfileStore::LoadCompletion completion) {
                completion(layouts, destinations, {});
            },
        .save_layout =
            [&layouts](persistence::SavedOutputLayoutProfile saved,
                       OutputProfileStore::Completion completion) {
                const auto found = std::ranges::find(layouts, saved.id,
                                                     &persistence::SavedOutputLayoutProfile::id);
                if (found == layouts.end()) {
                    layouts.push_back(std::move(saved));
                } else {
                    *found = std::move(saved);
                }
                completion({});
            },
        .remove_layout =
            [&layouts](core::StableId id, OutputProfileStore::Completion completion) {
                std::erase_if(layouts, [id](const auto& saved) { return saved.id == id; });
                completion({});
            },
        .save_destination =
            [&destinations](persistence::SavedDestinationProfile saved,
                            OutputProfileStore::Completion completion) {
                const auto found = std::ranges::find(destinations, saved.id,
                                                     &persistence::SavedDestinationProfile::id);
                if (found == destinations.end()) {
                    destinations.push_back(std::move(saved));
                } else {
                    *found = std::move(saved);
                }
                completion({});
            },
        .remove_destination =
            [&destinations](core::StableId id, OutputProfileStore::Completion completion) {
                std::erase_if(destinations, [id](const auto& saved) { return saved.id == id; });
                completion({});
            },
        .destinations_on = {},
        .destinations_key = {},
        .places = {},
    };

    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, {}, output_store);
    properties->show();

    QWidget* side_panel = nullptr;
    QTRY_VERIFY((side_panel = properties->findChild<QWidget*>(
                     QStringLiteral("bench-metadata-side-panel"))) != nullptr);
    // ADR-0183: the apply options live in the third sections tab and the
    // footer carries the plan summary label.
    QTabWidget* sections = nullptr;
    QTRY_VERIFY((sections = properties->findChild<QTabWidget*>(
                     QStringLiteral("bench-metadata-sections"))) != nullptr);
    QTRY_COMPARE(sections->count(), 2);
    QVERIFY(properties->findChild<QLabel*>(QStringLiteral("bench-metadata-apply-summary")) !=
            nullptr);

    // ADR-0238: the footer Actions button opens a popover with every
    // apply option in view, driving the hidden state controls.
    auto* actions_button =
        properties->findChild<QToolButton*>(QStringLiteral("bench-metadata-actions"));
    QVERIFY(actions_button != nullptr);
    const auto open_actions = [&]() -> QFrame* {
        QTest::mouseClick(actions_button, Qt::LeftButton);
        QFrame* popover = nullptr;
        [&] {
            QTRY_VERIFY((popover = properties->findChild<QFrame*>(
                             QStringLiteral("bench-metadata-actions-popover"))) != nullptr &&
                        popover->isVisible());
        }();
        return popover;
    };
    auto* popover = open_actions();
    QVERIFY(popover != nullptr);
    auto* save_tags_choice =
        popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-save-tags"));
    auto* rename_choice =
        popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-rename-files"));
    auto* manage_layouts_link =
        popover->findChild<QLabel*>(QStringLiteral("bench-actions-manage-layouts"));
    QVERIFY(save_tags_choice != nullptr && rename_choice != nullptr &&
            manage_layouts_link != nullptr);
    QVERIFY(save_tags_choice->isChecked());
    QVERIFY(!rename_choice->isEnabled());
    QSignalSpy settings_requests{properties, &MetadataPropertiesDialog::openSettingsRequested};
    emit manage_layouts_link->linkActivated(QStringLiteral("#"));
    QCOMPARE(settings_requests.count(), 1);
    QTRY_VERIFY(!popover->isVisible() || properties->findChild<QFrame*>(QStringLiteral(
                                             "bench-metadata-actions-popover")) != popover);
    popover = open_actions();
    save_tags_choice = popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-save-tags"));
    save_tags_choice->click();
    auto* save_tags =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-save-tags"));
    QVERIFY(save_tags != nullptr);
    QVERIFY(!save_tags->isChecked());
    save_tags_choice->click();
    QVERIFY(save_tags->isChecked());
    popover->close();
    auto* rename_files =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-rename-files"));
    auto* move_files =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-move-files"));
    auto* replaygain = properties->findChild<QPushButton*>(QStringLiteral("bench-replaygain-scan"));
    QVERIFY(save_tags != nullptr);
    QVERIFY(rename_files != nullptr);
    QVERIFY(move_files != nullptr);
    QVERIFY(replaygain != nullptr);
    QVERIFY(save_tags->isChecked());
    QVERIFY(save_tags->isEnabled());
    QVERIFY(!rename_files->isEnabled());
    QVERIFY(!move_files->isEnabled());
    QTRY_VERIFY(replaygain->isEnabled());
    QTableView* fields = nullptr;
    auto* preview =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    auto* preparation_status =
        properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    QVERIFY(preview != nullptr);
    QVERIFY(preparation_status != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    QVERIFY(aggregate_model != nullptr);
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*title_row, 2),
                                     QStringLiteral("Naming context"), Qt::EditRole));
    QTRY_VERIFY(preview->isEnabled());
    save_tags->setChecked(false);
    QVERIFY(!preview->isEnabled());
    QVERIFY(preparation_status->text().contains(QStringLiteral("Save tags is off")));
    save_tags->setChecked(true);
    QTRY_VERIFY(preview->isEnabled());

    auto* layout_combo =
        properties->findChild<QComboBox*>(QStringLiteral("bench-output-layout-profile"));
    auto* destination_combo =
        properties->findChild<QComboBox*>(QStringLiteral("bench-destination-profile"));
    QVERIFY(layout_combo != nullptr);
    QVERIFY(destination_combo != nullptr);
    QCOMPARE(layout_combo->count(), 1);
    QCOMPARE(layout_combo->currentText(), QStringLiteral("Albums"));
    // The saved one, then Choose folder… (ADR-0262).
    QCOMPARE(destination_combo->count(), 2);
    QCOMPARE(destination_combo->currentText(), QStringLiteral("Library"));

    // ADR-0185: the Edit buttons ask for the Settings screen instead of
    // opening local managers.
    auto* layout_manage =
        properties->findChild<QPushButton*>(QStringLiteral("bench-output-layout-manage"));
    QVERIFY(layout_manage != nullptr);
    QSignalSpy manage_requests{properties, &MetadataPropertiesDialog::openSettingsRequested};
    QTest::mouseClick(layout_manage, Qt::LeftButton);
    QCOMPARE(manage_requests.count(), 1);

    // The Settings Naming page owns profile CRUD through the same store.
    SettingsDialog settings{nullptr, output_store};
    settings.showPage(SettingsDialog::Page::naming);
    settings.show();
    auto* layout_list =
        settings.findChild<QTreeWidget*>(QStringLiteral("bench-output-layout-list"));
    auto* layout_remove =
        settings.findChild<QPushButton*>(QStringLiteral("bench-output-layout-remove"));
    QVERIFY(layout_remove != nullptr);
    auto* layout_name = settings.findChild<QLineEdit*>(QStringLiteral("bench-output-layout-name"));
    auto* layout_directory =
        settings.findChild<QLineEdit*>(QStringLiteral("bench-output-layout-directory-expression"));
    auto* layout_basename =
        settings.findChild<QLineEdit*>(QStringLiteral("bench-output-layout-basename-expression"));
    auto* layout_sanitization =
        settings.findChild<QComboBox*>(QStringLiteral("bench-output-layout-sanitization"));
    auto* layout_new = settings.findChild<QPushButton*>(QStringLiteral("bench-output-layout-new"));
    auto* layout_save =
        settings.findChild<QPushButton*>(QStringLiteral("bench-output-layout-save"));
    auto* destination_list =
        settings.findChild<QTreeWidget*>(QStringLiteral("bench-destination-list"));
    auto* destination_name =
        settings.findChild<QLineEdit*>(QStringLiteral("bench-destination-name"));
    auto* destination_root =
        settings.findChild<QLineEdit*>(QStringLiteral("bench-destination-root"));
    auto* destination_new =
        settings.findChild<QPushButton*>(QStringLiteral("bench-destination-new"));
    auto* destination_save =
        settings.findChild<QPushButton*>(QStringLiteral("bench-destination-save"));
    QVERIFY(layout_list != nullptr && layout_name != nullptr && layout_directory != nullptr &&
            layout_basename != nullptr && layout_sanitization != nullptr && layout_new != nullptr &&
            layout_save != nullptr && destination_list != nullptr && destination_name != nullptr &&
            destination_root != nullptr && destination_new != nullptr &&
            destination_save != nullptr);
    // ADR-0264: the layouts as a list, name and whole pattern, the selected
    // one edited below.
    QTRY_COMPARE(layout_list->topLevelItemCount(), 1);
    QCOMPARE(layout_list->topLevelItem(0)->text(0), QStringLiteral("Albums"));
    QVERIFY(
        layout_list->topLevelItem(0)->text(1).endsWith(QStringLiteral("%tracknumber% - %title%")));
    QCOMPARE(layout_list->currentItem(), layout_list->topLevelItem(0));
    QVERIFY(layout_remove->isEnabled());
    QCOMPARE(layout_name->text(), QStringLiteral("Albums"));
    QCOMPARE(layout_basename->text(), QStringLiteral("%tracknumber% - %title%"));
    QCOMPARE(layout_sanitization->currentData().toString(), QStringLiteral("linux"));
    // Only this computer's: listed without a heading.
    QTRY_COMPARE(destination_list->topLevelItemCount(), 1);
    QCOMPARE(destination_list->topLevelItem(0)->text(0), QStringLiteral("Library"));

    // ADR-0264: Naming layouts, Move destinations, ReplayGain, Undo -- one
    // row of tabs.
    auto* profile_sections =
        settings.findChild<QTabWidget*>(QStringLiteral("bench-settings-file-operations"));
    QVERIFY(profile_sections);
    QCOMPARE(profile_sections->count(), 4);
    QVERIFY(layout_directory->width() > 450);
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(settings.grab().save(directory + QStringLiteral("/settings-naming.png")));
    }
    QSignalSpy profile_changes{&settings, &SettingsDialog::outputProfilesChanged};
    QTest::mouseClick(layout_new, Qt::LeftButton);
    // A new one: nothing selected, nothing to remove.
    QVERIFY(layout_list->currentItem() == nullptr);
    QVERIFY(!layout_remove->isEnabled());
    layout_name->setText(QStringLiteral("Artist folders"));
    layout_directory->setText(QStringLiteral("%artist%"));
    layout_basename->setText(QStringLiteral("%title%"));
    layout_sanitization->setCurrentIndex(layout_sanitization->findData(QStringLiteral("portable")));
    QTRY_VERIFY(layout_save->isEnabled());
    QTest::mouseClick(layout_save, Qt::LeftButton);
    QTRY_COMPARE(layouts.size(), 2U);
    QCOMPARE(profile_changes.count(), 1);
    QCOMPARE(layouts.back().profile.sanitization_policy,
             (operations::PolicyVersion{"portable", 1U}));

    QTRY_COMPARE(layout_list->topLevelItemCount(), 2);
    // Saved: listed, and still the one selected.
    QCOMPARE(layout_list->currentItem()->text(0), QStringLiteral("Artist folders"));
    QCOMPARE(layout_list->currentItem()->text(1), QStringLiteral("%artist%/%title%"));
    QVERIFY(layout_remove->isEnabled());
    layout_list->setCurrentItem(layout_list->topLevelItem(0));
    QCOMPARE(layout_name->text(), layout_list->currentItem()->text(0));
    QCOMPARE(layout_basename->text(), QStringLiteral("%tracknumber% - %title%"));
    layout_list->setCurrentItem(layout_list->topLevelItem(1));
    QCOMPARE(layout_name->text(), QStringLiteral("Artist folders"));
    QCOMPARE(layout_basename->text(), QStringLiteral("%title%"));
    // Remove removes the one selected.
    QTest::mouseClick(layout_remove, Qt::LeftButton);
    QTRY_COMPARE(layouts.size(), 1U);
    QCOMPARE(layout_list->topLevelItemCount(), 1);
    QCOMPARE(layout_list->topLevelItem(0)->text(0), QStringLiteral("Albums"));
    // The first left is selected in its place.
    QCOMPARE(layout_list->currentItem(), layout_list->topLevelItem(0));
    QCOMPARE(layout_name->text(), QStringLiteral("Albums"));
    profile_sections->setCurrentIndex(1);
    QCOMPARE(destination_name->text(), QStringLiteral("Library"));
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(settings.grab().save(directory + QStringLiteral("/settings-destinations.png")));
    }
    QTest::mouseClick(destination_new, Qt::LeftButton);
    destination_name->setText(QStringLiteral("Archive"));
    // The root comes from the browse dialog in real use; the raw path is
    // what enables Save, so drive the widget's browse-backed state through
    // typing is no longer supported — use the store round trip instead.
    QVERIFY(!destination_save->isEnabled());

    // The editor's selectors refresh from the shared store on request.
    properties->reloadOutputProfiles();
    QTRY_COMPARE(layout_combo->count(), 1);

    delete properties;
}

void BenchMainWindowTest::FileWork::pathOnlyPreparationUsesActualTagsAndAppliesReviewedPlan() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("before.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    const auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const auto actual_title = read->document.first_effective_value("title");
    QVERIFY(actual_title.has_value());
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = raw_path,
                .source_revision = read->source_revision,
                .baseline = read->document,
            },
        .track_label = QStringLiteral("Path fixture"),
    };
    const std::vector layouts{persistence::SavedOutputLayoutProfile{
        .id = core::StableId::random(),
        .profile =
            operations::OutputLayoutProfile{
                .schema_version = 1U,
                .name = "Draft title",
                .dialect = {},
                .relative_directory_expression = {},
                .basename_expression = "%title%",
                .sanitization_policy = {"linux", 1U},
            },
    }};
    OutputProfileStore output_store{
        .load = [layouts](
                    OutputProfileStore::LoadCompletion completion) { completion(layouts, {}, {}); },
        .save_layout = {},
        .remove_layout = {},
        .save_destination = {},
        .remove_destination = {},
        .destinations_on = {},
        .destinations_key = {},
        .places = {},
    };
    const std::vector automatic_chains{persistence::SavedMetadataTransformationChain{
        .id = core::StableId::random(),
        .chain =
            metadata::MetadataTransformationChain{
                .schema_version = 1U,
                .name = "Synthetic path title",
                .actions = {metadata::MetadataFormatValueAction{
                    .target_field = "title", .dialect = {}, .source = "Synthetic path title"}},
            },
        .automatic = true,
    }};
    MetadataTransformationStore transformation_store{
        .load =
            [automatic_chains](MetadataTransformationStore::LoadCompletion completion) {
                completion(automatic_chains, {});
            },
        .save = {},
        .remove = {},
    };
    std::optional<operations::FilePublicationApplyResult> observed;
    std::string reviewed_target;
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, transformation_store, output_store,
        [&reviewed_target] {
            return FilePublicationPlanApplier{
                [&reviewed_target](const operations::PreparationPlan& plan,
                                   const operations::FilePublicationApplyProgressCallback& progress,
                                   const core::CancellationToken&)
                    -> core::Result<operations::FilePublicationApplyResult> {
                    if (!plan.path_preflight || plan.path_preflight->sources.size() != 1U) {
                        return std::unexpected(core::Error{
                            .code = core::ErrorCode::invariant,
                            .message = "Expected one reviewed path",
                            .context = {},
                        });
                    }
                    const auto& preflight = *plan.path_preflight;
                    const auto& checked_source = preflight.sources.front();
                    reviewed_target = checked_source.planned.target_raw_path;
                    if (progress) {
                        progress(operations::FilePublicationApplyProgress{
                            .source_index = 0U,
                            .source_raw_path = checked_source.planned.source_raw_path,
                            .target_raw_path = checked_source.planned.target_raw_path,
                            .publication = checked_source.publication,
                            .state = operations::FilePublicationApplySourceState::committed,
                            .completed_sources = 1U,
                            .total_sources = 1U,
                            .issue = std::nullopt,
                        });
                    }
                    auto commit = operations::FilePublicationCommitResult{
                        .journal_id = core::StableId::random(),
                        .source_raw_path = checked_source.planned.source_raw_path,
                        .target_raw_path = checked_source.planned.target_raw_path,
                        .source_revision = checked_source.planned.source_revision,
                        .target_revision = checked_source.observed_revision,
                        .occurrence_indexes = checked_source.planned.item_indexes,
                        // As on an NFS share: done, but ownership not kept.
                        .notes = {"File ownership was not preserved: the destination "
                                  "filesystem refused the change"},
                    };
                    return operations::FilePublicationApplyResult{
                        .sources = {operations::FilePublicationApplySourceResult{
                            .source_index = 0U,
                            .source_raw_path = checked_source.planned.source_raw_path,
                            .target_raw_path = checked_source.planned.target_raw_path,
                            .publication = checked_source.publication,
                            .state = operations::FilePublicationApplySourceState::committed,
                            .commit = std::move(commit),
                            .metadata_commit = std::nullopt,
                            .folder_image_commit = std::nullopt,
                            .published_metadata = std::nullopt,
                            .issue = std::nullopt,
                        }},
                        .cancellation_requested = false,
                    };
                }};
        },
        [&observed](const operations::FilePublicationApplyResult& result) { observed = result; });
    QSignalSpy status{properties, &MetadataPropertiesDialog::statusMessage};
    properties->show();
    // Success auto-closes the WA_DeleteOnClose dialog; only a pointer
    // guarded from the start may observe that.
    const QPointer<MetadataPropertiesDialog> closed_guard{properties};

    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* save_tags =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-save-tags"));
    auto* rename_files =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-rename-files"));
    auto* preview =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(save_tags != nullptr);
    QVERIFY(rename_files != nullptr);
    QVERIFY(preview != nullptr);
    QTRY_VERIFY(rename_files->isEnabled());
    QListWidget* automatic_list = nullptr;
    QTRY_VERIFY((automatic_list = properties->findChild<QListWidget*>(
                     QStringLiteral("bench-metadata-transformation-list"))) != nullptr);
    QTRY_COMPARE(automatic_list->count(), 1);
    QCOMPARE(automatic_list->item(0)->checkState(), Qt::Checked);
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*title_row, 2),
                                     QStringLiteral("Draft path title"), Qt::EditRole));
    save_tags->setChecked(false);
    rename_files->setChecked(true);
    QTRY_VERIFY(preview->isEnabled());
    QTest::mouseClick(preview, Qt::LeftButton);

    // Direct apply: the checked plan runs immediately. The applier receives the
    // preflighted path derived strictly from the file's actual tags — never the
    // unsaved draft or the automatic script output.
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 5'000);
    QCOMPARE(observed->committed_source_count(), 1U);
    const auto expected_target =
        media.filePath(QString::fromStdString(*actual_title) + QStringLiteral(".flac"));
    QVERIFY(!expected_target.endsWith(QStringLiteral("/Draft path title.flac")));
    QVERIFY(!expected_target.endsWith(QStringLiteral("/Synthetic path title.flac")));
    QCOMPARE(QString::fromStdString(reviewed_target), expected_target);
    // A note is a line in the status bar, not a window to close first.
    QVERIFY(window_test::closeTaggerAfterWrite(closed_guard));
    QCOMPARE(status.count(), 1);
    QVERIFY(
        status.front().front().toString().contains(QStringLiteral("ownership was not preserved")));
}

// ADR-0345: Workspace › Library engine chooses the engine holding the library, as
// Settings → Engines does -- through the palette too, as a laptop does
// leaving home: the server's library there, its own transcodes away.
void BenchMainWindowTest::FileWork::theLibraryMenuMovesTheLibrary() {
    const auto server = QStringLiteral("192.0.2.10:6603");
    saveRemoteEngines({{.address = server,
                        .password = {},
                        .music_folder = {},
                        .reachable_at = {},
                        .id = {},
                        .stream_kbps = -1,
                        .name = QStringLiteral("gemenon")}});
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_key),
                         QLatin1String(SettingsDialog::library_engine_local));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* sources = window.findChild<QTabBar*>(QStringLiteral("bench-local-source-tabs"));
    QVERIFY(sources != nullptr);
    QVERIFY(sources->isTabVisible(0));

    auto* menu = window.findChild<QMenu*>(QStringLiteral("bench-library-engine-menu"));
    QVERIFY(menu != nullptr);
    window.rebuildLibraryMenu();
    QVERIFY(menu->menuAction()->isVisible());
    QCOMPARE(menu->actions().size(), 2);
    QCOMPARE(menu->actions().front()->text(), QStringLiteral("This computer"));
    QVERIFY(menu->actions().front()->isChecked());
    QCOMPARE(menu->actions().back()->text(), QStringLiteral("gemenon"));

    window.findChild<QAction*>(QStringLiteral("action-command-palette"))->trigger();
    auto* palette = window.findChild<QDialog*>(QStringLiteral("command-palette"));
    QVERIFY(palette != nullptr);
    auto* filter = palette->findChild<QLineEdit*>(QStringLiteral("command-filter"));
    auto* results = palette->findChild<QListWidget*>(QStringLiteral("command-results"));
    filter->setText(QStringLiteral("library gemenon"));
    QCOMPARE(results->count(), 1);
    QCOMPARE(results->item(0)->text(), QStringLiteral("Library engine › gemenon"));
    QTest::keyClick(filter, Qt::Key_Return);
    QCOMPARE(libraryEngine(), server);
    // This computer's library is no library now: its tab goes.
    QVERIFY(!sources->isTabVisible(0));

    // Home again -- away, rather: this computer's own.
    window.rebuildLibraryMenu();
    QVERIFY(menu->actions().back()->isChecked());
    menu->actions().front()->trigger();
    QCOMPARE(libraryEngine(), QLatin1String(SettingsDialog::library_engine_local));
    QVERIFY(sources->isTabVisible(0));
    saveRemoteEngines({});
    QSettings{}.remove(QLatin1String(SettingsDialog::library_engine_key));
}

// ADR-0237: a move destination is a folder on one engine's machine; the
// manager shows whose, browses that machine's folders, and saves there.
void BenchMainWindowTest::FileWork::moveDestinationsArePerEngine() {
    const auto destination = [](std::string name, std::string root) {
        return persistence::SavedDestinationProfile{
            .id = core::StableId::random(),
            .profile =
                operations::DestinationProfile{.schema_version = 1U,
                                               .name = std::move(name),
                                               .root_raw_path = std::move(root),
                                               .containment_policy = {"lexical-beneath-root", 1U}}};
    };
    std::vector here{destination("Library here", "/mnt/nas/music/library")};
    std::vector there{destination("Library there", "/srv/music/library")};
    std::vector<std::string> listed_paths;
    const auto place = [](QString key, QString name,
                          std::vector<persistence::SavedDestinationProfile>& held) {
        return DestinationPlace{
            .key = std::move(key),
            .name = std::move(name),
            .load = [&held](DestinationPlace::LoadCompletion done) { done(held, {}); },
            .save =
                [&held](persistence::SavedDestinationProfile saved,
                        DestinationPlace::Completion done) {
                    std::erase_if(held, [&saved](const auto& old) { return old.id == saved.id; });
                    held.push_back(std::move(saved));
                    done({});
                },
            .remove =
                [&held](core::StableId id, DestinationPlace::Completion done) {
                    std::erase_if(held, [id](const auto& old) { return old.id == id; });
                    done({});
                },
            .folders = {},
            .copyable = {},
        };
    };
    auto local = place(QStringLiteral("local"), QStringLiteral("this computer"), here);
    auto remote = place(QStringLiteral("gemenon-id"), QStringLiteral("gemenon"), there);
    remote.folders = [&listed_paths](std::string path, EngineFolderDialog::ListingCompletion done) {
        listed_paths.push_back(path);
        const auto at = path.empty() ? std::string{"/srv/music"} : path;
        done(EngineFolderDialog::Listing{.path = at,
                                         .parent = std::string{"/srv"},
                                         .folders = {"incoming", "sorted"},
                                         .linked = {},
                                         .home = {},
                                         .roots = {}});
    };
    remote.copyable = [] {
        return std::vector{
            persistence::SavedDestinationProfile{
                .id = core::StableId::random(),
                .profile =
                    operations::DestinationProfile{
                        .schema_version = 1U,
                        .name = "Library here",
                        .root_raw_path = "/srv/music/sorted",
                        .containment_policy = {"lexical-beneath-root", 1U}}},
        };
    };
    const OutputProfileStore store{
        .load = [&here](OutputProfileStore::LoadCompletion done) { done({}, here, {}); },
        .save_layout = {},
        .remove_layout = {},
        .save_destination = local.save,
        .remove_destination = local.remove,
        .destinations_on = {},
        .destinations_key = {},
        .places = {local, remote},
    };
    SettingsDialog settings{nullptr, store};
    settings.showDestinationsOf(QStringLiteral("gemenon-id"));
    settings.show();
    auto* list = settings.findChild<QTreeWidget*>(QStringLiteral("bench-destination-list"));
    auto* copy = settings.findChild<QPushButton*>(QStringLiteral("bench-destination-copy"));
    auto* browse = settings.findChild<QPushButton*>(QStringLiteral("bench-destination-browse"));
    auto* name = settings.findChild<QLineEdit*>(QStringLiteral("bench-destination-name"));
    auto* root = settings.findChild<QLineEdit*>(QStringLiteral("bench-destination-root"));
    auto* create = settings.findChild<QPushButton*>(QStringLiteral("bench-destination-new"));
    auto* save = settings.findChild<QPushButton*>(QStringLiteral("bench-destination-save"));
    auto* remove = settings.findChild<QPushButton*>(QStringLiteral("bench-destination-remove"));
    auto* sections =
        settings.findChild<QTabWidget*>(QStringLiteral("bench-settings-file-operations"));
    QVERIFY(list && copy && browse && name && root && create && save && remove && sections);
    // ADR-0264: every engine's destinations in one list, under its name.
    QCOMPARE(sections->currentIndex(), 1);
    QTRY_COMPARE(list->topLevelItemCount(), 2);
    QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("This computer"));
    QCOMPARE(list->topLevelItem(1)->text(0), QStringLiteral("Gemenon"));
    QTRY_COMPARE(list->topLevelItem(0)->childCount(), 1);
    QTRY_COMPARE(list->topLevelItem(1)->childCount(), 1);
    QCOMPARE(list->topLevelItem(1)->child(0)->text(1), QStringLiteral("/srv/music/library"));
    // Opened for an engine: its first destination selected.
    QVERIFY(list->currentItem() != nullptr);
    QCOMPARE(list->currentItem()->text(0), QStringLiteral("Library there"));
    QCOMPARE(list->currentItem()->parent(), list->topLevelItem(1));
    QCOMPARE(name->text(), QStringLiteral("Library there"));
    QVERIFY(remove->isEnabled());
    // A heading is not a destination.
    QVERIFY(!(list->topLevelItem(1)->flags() & Qt::ItemIsSelectable));

    // This computer's that lie under the engine's mount are offered.
    QTRY_VERIFY(copy->isVisible());
    QCOMPARE(copy->text(), QStringLiteral("Copy 1 from this computer"));
    QTest::mouseClick(copy, Qt::LeftButton);
    QTRY_COMPARE(there.size(), 2U);
    QVERIFY(std::ranges::any_of(there, [](const auto& saved) {
        return saved.profile.root_raw_path == "/srv/music/sorted";
    }));
    QTRY_VERIFY(!copy->isVisible());
    QCOMPARE(here.size(), 1U);

    // A new one, on the engine asked for, its folder chosen on that machine.
    QVERIFY(create->menu() != nullptr);
    QCOMPARE(create->menu()->actions().size(), 2);
    QCOMPARE(create->menu()->actions().at(1)->text(), QStringLiteral("On gemenon"));
    create->menu()->actions().at(1)->trigger();
    QVERIFY(list->currentItem() == nullptr);
    QVERIFY(!remove->isEnabled());
    name->setText(QStringLiteral("Incoming there"));
    QTest::mouseClick(browse, Qt::LeftButton);
    EngineFolderDialog* chooser = nullptr;
    QTRY_VERIFY((chooser = settings.findChild<EngineFolderDialog*>()) != nullptr);
    QCOMPARE(chooser->windowTitle(), QStringLiteral("Choose a folder on gemenon"));
    auto* folders = chooser->findChild<QListWidget*>(QStringLiteral("bench-engine-folder-list"));
    QVERIFY(folders != nullptr);
    QTRY_COMPARE(folders->count(), 2);
    folders->setCurrentRow(0);
    chooser->findChild<QPushButton*>(QStringLiteral("bench-engine-folder-choose"))->click();
    QCOMPARE(root->text(), QStringLiteral("/srv/music/incoming"));
    QTRY_VERIFY(save->isEnabled());
    QTest::mouseClick(save, Qt::LeftButton);
    QTRY_COMPARE(there.size(), 3U);
    QCOMPARE(here.size(), 1U);

    QTRY_COMPARE(list->topLevelItem(1)->childCount(), 3);

    // This computer's are its own: chosen in the list, edited and removed
    // on this computer.
    QCOMPARE(list->topLevelItem(0)->childCount(), 1);
    list->setCurrentItem(list->topLevelItem(0)->child(0));
    QCOMPARE(name->text(), QStringLiteral("Library here"));
    QTRY_VERIFY(remove->isEnabled());
    QTest::mouseClick(remove, Qt::LeftButton);
    QTRY_COMPARE(here.size(), 0U);
    QCOMPARE(there.size(), 3U);
    QTRY_COMPARE(list->topLevelItem(0)->childCount(), 1); // "None yet"
    QVERIFY(!list->topLevelItem(0)->child(0)->data(0, Qt::UserRole).isValid());

    // Properties for tracks on that engine says whose destinations it offers.
    auto* properties = new MetadataPropertiesDialog(
        0U, [](std::size_t) -> std::optional<MetadataPropertiesSource> { return std::nullopt; }, {},
        {}, {}, {},
        OutputProfileStore{
            .load = [](OutputProfileStore::LoadCompletion done) { done({}, {}, {}); },
            .save_layout = {},
            .remove_layout = {},
            .save_destination = {},
            .remove_destination = {},
            .destinations_on = QStringLiteral("gemenon"),
            .destinations_key = QStringLiteral("gemenon-id"),
            .places = {}});
    auto* offered = properties->findChild<QComboBox*>(QStringLiteral("bench-destination-profile"));
    QVERIFY(offered != nullptr);
    QCOMPARE(offered->placeholderText(), QStringLiteral("None saved on gemenon yet"));
    QCOMPARE(offered->toolTip(), QStringLiteral("Move destinations on gemenon"));
    delete properties;
}

// ADR-0237: naming layouts are global -- made here, and a copy in every
// engine, so whatever moves files through one can name them. This
// computer's engine shares the workspace: what it holds is what was saved,
// and a copy sent on connecting never lands on top of a newer one.
// ADR-0238: what the Actions popover was set to is how the next Properties
// window starts: toggles, layout, and each engine's destination.
void BenchMainWindowTest::FileWork::actionChoicesAreRemembered() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("remembered.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto raw_path = QFile::encodeName(path).toStdString();
    const auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const MetadataPropertiesSource source{
        .source = metadata::StagedMetadataSource{.raw_path = raw_path,
                                                 .source_revision = read->source_revision,
                                                 .baseline = read->document},
        .track_label = QStringLiteral("Remembered"),
    };
    const auto layout = [](std::string name) {
        return persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile = operations::OutputLayoutProfile{.schema_version = 1U,
                                                       .name = std::move(name),
                                                       .dialect = {},
                                                       .relative_directory_expression = {},
                                                       .basename_expression = "%title%",
                                                       .sanitization_policy = {"linux", 1U}}};
    };
    const auto destination = [](std::string name) {
        return persistence::SavedDestinationProfile{
            .id = core::StableId::random(),
            .profile =
                operations::DestinationProfile{.schema_version = 1U,
                                               .name = std::move(name),
                                               .root_raw_path = "/srv/music",
                                               .containment_policy = {"lexical-beneath-root", 1U}}};
    };
    const std::vector layouts{layout("Plain"), layout("Albumartist/Date Album")};
    const std::vector destinations{destination("Incoming"), destination("Library")};
    const auto store_for = [&](const QString& engine) {
        return OutputProfileStore{
            .load =
                [&](OutputProfileStore::LoadCompletion done) { done(layouts, destinations, {}); },
            .save_layout = {},
            .remove_layout = {},
            .save_destination = {},
            .remove_destination = {},
            .destinations_on = {},
            .destinations_key = engine,
            .places = {}};
    };
    const auto open = [&](const QString& engine) {
        auto* properties = new MetadataPropertiesDialog(
            1U,
            [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
                return index == 0U ? std::optional{source} : std::nullopt;
            },
            {}, {}, {}, {}, store_for(engine),
            [] {
                return FilePublicationPlanApplier{
                    [](const operations::PreparationPlan&,
                       const operations::FilePublicationApplyProgressCallback&,
                       const core::CancellationToken&)
                        -> core::Result<operations::FilePublicationApplyResult> {
                        return std::unexpected(core::Error{.code = core::ErrorCode::cancelled,
                                                           .message = "not here",
                                                           .context = {}});
                    }};
            },
            {});
        properties->show();
        return properties;
    };
    const auto hidden = [](MetadataPropertiesDialog* properties, const char* name) {
        return properties->findChild<QCheckBox*>(QString::fromLatin1(name));
    };
    const auto popover_of = [](MetadataPropertiesDialog* properties) -> QFrame* {
        QTest::mouseClick(
            properties->findChild<QToolButton*>(QStringLiteral("bench-metadata-actions")),
            Qt::LeftButton);
        return properties->findChild<QFrame*>(QStringLiteral("bench-metadata-actions-popover"));
    };

    auto* first = open(QStringLiteral("gemenon-id"));
    auto* layout_combo =
        first->findChild<QComboBox*>(QStringLiteral("bench-output-layout-profile"));
    QTRY_COMPARE(layout_combo->count(), 2);
    auto* popover = popover_of(first);
    QVERIFY(popover != nullptr);
    popover->findChild<QComboBox*>(QStringLiteral("bench-actions-layout"))->setCurrentIndex(1);
    emit popover->findChild<QComboBox*>(QStringLiteral("bench-actions-layout"))->activated(1);
    popover->findChild<QComboBox*>(QStringLiteral("bench-actions-destination"))->setCurrentIndex(1);
    emit popover->findChild<QComboBox*>(QStringLiteral("bench-actions-destination"))->activated(1);
    QTRY_VERIFY(
        popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-rename-files"))->isEnabled());
    popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-rename-files"))->click();
    popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-move-files"))->click();
    QVERIFY(hidden(first, "bench-preparation-rename-files")->isChecked());
    QVERIFY(hidden(first, "bench-preparation-move-files")->isChecked());
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(popover->grab().save(directory + QStringLiteral("/actions-popover.png")));
    }
    popover->close();
    delete first;

    // The next window starts as this one was left.
    auto* second = open(QStringLiteral("gemenon-id"));
    layout_combo = second->findChild<QComboBox*>(QStringLiteral("bench-output-layout-profile"));
    auto* destination_combo =
        second->findChild<QComboBox*>(QStringLiteral("bench-destination-profile"));
    QTRY_COMPARE(layout_combo->currentText(), QStringLiteral("Albumartist/Date Album"));
    QCOMPARE(destination_combo->currentText(), QStringLiteral("Library"));
    QTRY_VERIFY(hidden(second, "bench-preparation-rename-files")->isChecked());
    QTRY_VERIFY(hidden(second, "bench-preparation-move-files")->isChecked());
    // Unticked is remembered too.
    popover = popover_of(second);
    popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-move-files"))->click();
    QVERIFY(!hidden(second, "bench-preparation-move-files")->isChecked());
    popover->close();
    delete second;

    // Another engine's destination is its own: nothing chosen there yet.
    auto* elsewhere = open(QStringLiteral("other-engine"));
    destination_combo =
        elsewhere->findChild<QComboBox*>(QStringLiteral("bench-destination-profile"));
    QTRY_COMPARE(destination_combo->count(), 3);
    QCOMPARE(destination_combo->currentText(), QStringLiteral("Incoming"));
    QTRY_VERIFY(hidden(elsewhere, "bench-preparation-rename-files")->isChecked());
    QVERIFY(!hidden(elsewhere, "bench-preparation-move-files")->isChecked());
    delete elsewhere;
    // Left as other tests expect to find it.
    QSettings{}.remove(QStringLiteral("properties/actions"));
}

// Each Manage link opens what it names: the naming layouts, or the move
// destinations of the tracks' engine -- also when Settings is already open.
void BenchMainWindowTest::FileWork::actionsLinksOpenTheirOwnSettings() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("linked.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    BenchMainWindow window;
    window.show();
    window.openLocalPaths({QFile::encodeName(path).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    window.findChild<QAction*>(QStringLiteral("action-track-properties"))->trigger();
    auto* properties = window.findChild<MetadataPropertiesDialog*>();
    QVERIFY(properties != nullptr);
    const auto follow = [&](const char* link) {
        QTest::mouseClick(
            properties->findChild<QToolButton*>(QStringLiteral("bench-metadata-actions")),
            Qt::LeftButton);
        QFrame* popover = nullptr;
        QTRY_VERIFY((popover = properties->findChild<QFrame*>(
                         QStringLiteral("bench-metadata-actions-popover"))) != nullptr);
        auto* label = popover->findChild<QLabel*>(QString::fromLatin1(link));
        QVERIFY(label != nullptr);
        emit label->linkActivated(QStringLiteral("#"));
    };
    const auto shown_tab = [&window] {
        auto* settings = window.findChild<SettingsDialog*>();
        auto* sections =
            settings
                ? settings->findChild<QTabWidget*>(QStringLiteral("bench-settings-file-operations"))
                : nullptr;
        return sections != nullptr && settings->isVisible() ? sections->currentIndex() : -1;
    };
    follow("bench-actions-manage-destinations");
    QTRY_COMPARE(shown_tab(), 1);
    QVERIFY(window.findChild<SettingsDialog*>()->findChild<QTreeWidget*>(
                QStringLiteral("bench-destination-list")) != nullptr);
    // Settings still open: the other link switches it to the layouts.
    follow("bench-actions-manage-layouts");
    QTRY_COMPARE(shown_tab(), 0);
    follow("bench-actions-manage-destinations");
    QTRY_COMPARE(shown_tab(), 1);
    window.findChild<SettingsDialog*>()->reject();
}

// ADR-0339: naming layouts are Melody's -- kept in its presets, which
// melody-cli shares on this computer -- and no engine is handed a copy.
void BenchMainWindowTest::FileWork::namingLayoutsAreMelodys() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    const auto kept = [] {
        std::vector<std::string> found;
        auto presets = persistence::ListRepository::open(core::presets_database_path());
        if (auto layouts = presets ? presets->load_output_layout_profiles()
                                   : decltype(presets->load_output_layout_profiles()){}) {
            for (const auto& layout : *layouts) {
                found.push_back(layout.profile.name);
            }
        }
        return found;
    };
    const auto id = core::StableId::random();
    bool saved = false;
    window.buildOutputProfileStore().save_layout(
        persistence::SavedOutputLayoutProfile{
            .id = id,
            .profile = operations::OutputLayoutProfile{.schema_version = 1U,
                                                       .name = "Mine",
                                                       .dialect = {},
                                                       .relative_directory_expression = "%artist%",
                                                       .basename_expression = "%title%",
                                                       .sanitization_policy = {"linux", 1U}},
        },
        [&saved](const QString& error) { saved = error.isEmpty(); });
    QTRY_VERIFY(saved);
    QTRY_VERIFY_WITH_TIMEOUT(std::ranges::contains(kept(), std::string{"Mine"}), 5'000);
    // The engine keeps none: there is nothing to ask it for.
    auto client = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(client.has_value());
    QVERIFY(!(*client)->call("layouts.list").has_value());
    (*client)->close();
    bool removed = false;
    window.buildOutputProfileStore().remove_layout(
        id, [&removed](const QString& error) { removed = error.isEmpty(); });
    QTRY_VERIFY(removed);
    QTRY_VERIFY_WITH_TIMEOUT(!std::ranges::contains(kept(), std::string{"Mine"}), 5'000);
}

void BenchMainWindowTest::FileWork::combinedTagAndRenameReviewReachesPreparationApply_data() {
    // ADR-0237 stage 5: the rename checked and published by the engine holding
    // the file, or reaching this process's applier -- the same window, the
    // same plan.
    QTest::addColumn<bool>("through_engine");
    QTest::newRow("engine") << true;
    QTest::newRow("this-process") << false;
}

void BenchMainWindowTest::FileWork::combinedTagAndRenameReviewReachesPreparationApply() {
    QFETCH(bool, through_engine);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("combined-ui-before.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    const auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = raw_path,
                .source_revision = read->source_revision,
                .baseline = read->document,
            },
        .track_label = QStringLiteral("Combined UI fixture"),
    };
    const std::vector layouts{persistence::SavedOutputLayoutProfile{
        .id = core::StableId::random(),
        .profile =
            operations::OutputLayoutProfile{
                .schema_version = 1U,
                .name = "Final title",
                .dialect = {},
                .relative_directory_expression = {},
                .basename_expression = "%title%",
                .sanitization_policy = {"linux", 1U},
            },
    }};
    const OutputProfileStore output_store{
        .load = [layouts](
                    OutputProfileStore::LoadCompletion completion) { completion(layouts, {}, {}); },
        .save_layout = {},
        .remove_layout = {},
        .save_destination = {},
        .remove_destination = {},
        .destinations_on = {},
        .destinations_key = {},
        .places = {},
    };
    const MetadataTransformationStore transformation_store{
        .load = [](MetadataTransformationStore::LoadCompletion completion) { completion({}, {}); },
        .save = {},
        .remove = {},
    };
    bool combined_applied = false;
    std::string reviewed_target;
    std::optional<operations::FilePublicationApplyResult> published;
    const auto work = through_engine
                          ? std::make_shared<engine::RemoteFileWork>(protocol::Endpoint{
                                .socket = QFile::encodeName(engine_.socket()).toStdString(),
                                .host = {},
                                .port = 0,
                                .token = {}})
                          : nullptr;
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, transformation_store, output_store,
        [&combined_applied, &reviewed_target, &published, work] {
            return FilePublicationPlanApplier{
                [&combined_applied, &reviewed_target, &published,
                 work](const operations::PreparationPlan& plan,
                       const operations::FilePublicationApplyProgressCallback& progress,
                       const core::CancellationToken& cancellation)
                    -> core::Result<operations::FilePublicationApplyResult> {
                    if (!plan.ready() || !plan.metadata || !plan.path_preflight ||
                        plan.metadata->sources.size() != 1U ||
                        plan.path_preflight->sources.size() != 1U) {
                        return std::unexpected(core::Error{
                            .code = core::ErrorCode::invariant,
                            .message = "Expected one ready combined preparation source",
                            .context = {},
                        });
                    }
                    const auto& checked = plan.path_preflight->sources.front();
                    reviewed_target = checked.planned.target_raw_path;
                    if (work) {
                        auto applied = work->publish(plan, progress, cancellation);
                        if (applied) {
                            published = *applied;
                        }
                        combined_applied = true;
                        return applied;
                    }
                    combined_applied = true;
                    if (progress) {
                        progress(operations::FilePublicationApplyProgress{
                            .source_index = 0U,
                            .source_raw_path = checked.planned.source_raw_path,
                            .target_raw_path = checked.planned.target_raw_path,
                            .publication = checked.publication,
                            .state = operations::FilePublicationApplySourceState::committed,
                            .completed_sources = 1U,
                            .total_sources = 1U,
                            .issue = std::nullopt,
                        });
                    }
                    auto commit = operations::FilePublicationCommitResult{
                        .journal_id = core::StableId::random(),
                        .content =
                            operations::FilePublicationContentKind::prepared_destination_artifact,
                        .source_raw_path = checked.planned.source_raw_path,
                        .target_raw_path = checked.planned.target_raw_path,
                        .source_revision = checked.planned.source_revision,
                        .target_revision = checked.observed_revision,
                        .occurrence_indexes = checked.planned.item_indexes,
                        .notes = {},
                    };
                    return operations::FilePublicationApplyResult{
                        .sources = {operations::FilePublicationApplySourceResult{
                            .source_index = 0U,
                            .source_raw_path = checked.planned.source_raw_path,
                            .target_raw_path = checked.planned.target_raw_path,
                            .publication = checked.publication,
                            .state = operations::FilePublicationApplySourceState::committed,
                            .commit = std::move(commit),
                            .metadata_commit = std::nullopt,
                            .folder_image_commit = std::nullopt,
                            .published_metadata = std::nullopt,
                            .issue = std::nullopt,
                        }},
                        .cancellation_requested = false,
                    };
                }};
        },
        {}, nullptr, {}, {}, work ? engineFileWorkTools(work) : FileWorkTools{});
    properties->show();
    // Success auto-closes the WA_DeleteOnClose dialog; only a pointer
    // guarded from the start may observe that.
    const QPointer<MetadataPropertiesDialog> closed_guard{properties};

    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* save_tags =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-save-tags"));
    auto* rename_files =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-rename-files"));
    auto* preview =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(save_tags != nullptr);
    QVERIFY(rename_files != nullptr);
    QVERIFY(preview != nullptr);
    QTRY_VERIFY(rename_files->isEnabled());
    QVERIFY(save_tags->isChecked());
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*title_row, 2),
                                     QStringLiteral("Combined UI title"), Qt::EditRole));
    rename_files->setChecked(true);
    QTRY_VERIFY(preview->isEnabled());
    QTest::mouseClick(preview, Qt::LeftButton);

    // Direct apply: the combined tag-and-rename plan reaches the applier with
    // the draft title driving the new path, without any review dialog.
    QTRY_VERIFY_WITH_TIMEOUT(combined_applied, 5'000);
    QVERIFY(QString::fromStdString(reviewed_target)
                .endsWith(QStringLiteral("/Combined UI title.flac")));
    QVERIFY(window_test::closeTaggerAfterWrite(closed_guard));
    if (through_engine) {
        // Written once, at its new place, by the engine.
        QVERIFY(published && published->committed_source_count() == 1U);
        QVERIFY(!QFile::exists(path));
        const auto written = metadata::read_local_metadata(reviewed_target);
        QVERIFY(written.has_value());
        QCOMPARE(written->document.first_effective_value("title"),
                 std::optional<std::string>{"Combined UI title"});
    }
}

// ADR-0262: Move into a folder that is no saved destination, the layout's
// folders made inside it; offered again after.
void BenchMainWindowTest::FileWork::applyMovesIntoAChosenFolder() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto work = std::make_shared<engine::RemoteFileWork>(
        protocol::Endpoint{.socket = QFile::encodeName(engine_.socket()).toStdString(),
                           .host = {},
                           .port = 0,
                           .token = {}});
    const auto path = media.filePath(QStringLiteral("in/one.flac"));
    QVERIFY(QDir{}.mkpath(media.filePath(QStringLiteral("in"))));
    QVERIFY(QDir{}.mkpath(media.filePath(QStringLiteral("chosen"))));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), path));
    const auto raw_path = QFile::encodeName(path).toStdString();
    const auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const MetadataPropertiesSource source{
        .source = metadata::StagedMetadataSource{.raw_path = raw_path,
                                                 .source_revision = read->source_revision,
                                                 .baseline = read->document},
        .track_label = {},
    };
    const std::vector layouts{persistence::SavedOutputLayoutProfile{
        .id = core::StableId::random(),
        .profile = operations::OutputLayoutProfile{.schema_version = 1U,
                                                   .name = "Artist folders",
                                                   .dialect = {},
                                                   .relative_directory_expression = "%artist%",
                                                   .basename_expression = "%title%",
                                                   .sanitization_policy = {"linux", 1U}}}};
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {},
        TaggerServices{
            .plan_applier_factory = {},
            .apply_observer = {},
            .transformation_store =
                MetadataTransformationStore{.load = [](MetadataTransformationStore::LoadCompletion
                                                           completion) { completion({}, {}); },
                                            .save = {},
                                            .remove = {}},
            .output_profile_store =
                OutputProfileStore{.load =
                                       [layouts](OutputProfileStore::LoadCompletion completion) {
                                           completion(layouts, {}, {});
                                       },
                                   .save_layout = {},
                                   .remove_layout = {},
                                   .save_destination = {},
                                   .remove_destination = {},
                                   .destinations_on = {},
                                   .destinations_key = QStringLiteral("chosen-folder-test"),
                                   .places = {}},
            .file_plan_applier_factory =
                [work] {
                    return FilePublicationPlanApplier{
                        [work](const operations::PreparationPlan& plan,
                               const operations::FilePublicationApplyProgressCallback& progress,
                               const core::CancellationToken& cancellation) {
                            return work->publish(plan, progress, cancellation, true);
                        }};
                },
            .file_apply_observer = {},
            .layout_store = {},
            .musicbrainz = {},
            .tools = engineFileWorkTools(work),
            .library_elsewhere = {},
            .undo =
                [work](std::vector<operations::UndoRequest> requests,
                       const core::CancellationToken& cancellation) {
                    return work->undo(requests, {}, cancellation);
                },
            .release_undo =
                [work](std::vector<operations::UndoRequest> requests) {
                    static_cast<void>(work->release_undo_copies(requests));
                },
        });
    const QPointer<MetadataPropertiesDialog> guard{properties};
    properties->setProperty("trackknife-move-folder",
                            QFile::encodeName(media.filePath(QStringLiteral("chosen"))));
    properties->show();
    auto* tagger = properties->findChild<TaggerSession*>();
    auto* destinations =
        properties->findChild<QComboBox*>(QStringLiteral("bench-destination-profile"));
    QTRY_COMPARE(destinations->count(), 1);
    QCOMPARE(destinations->itemText(0), QStringLiteral("Choose folder…"));
    destinations->setCurrentIndex(0);
    QTRY_COMPARE(destinations->count(), 2);
    QCOMPARE(destinations->itemText(0), media.filePath(QStringLiteral("chosen")));
    QCOMPARE(tagger->destinationIndex(), 0);
    tagger->chooseRename(true);
    tagger->chooseMove(true);
    QTRY_VERIFY(tagger->moveFiles() && tagger->canApply());
    properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"))->click();
    QVERIFY(window_test::closeTaggerAfterWrite(guard));
    const auto moved =
        media.filePath(QStringLiteral("chosen/Trackknife Project/Fixture Tone.flac"));
    QVERIFY2(QFile::exists(moved), qPrintable(moved));
    QVERIFY(!QFile::exists(path));
    QSettings{}.remove(QStringLiteral("properties/actions"));
}

// The folder chooser for an engine elsewhere: where to start -- home, its
// music folders, the root -- the path in steps and typed, hidden folders
// when asked, links said, Backspace going out, and a folder made there.
void BenchMainWindowTest::FileWork::theEngineFolderDialogBrowsesAnEnginesMachine() {
    std::vector<std::string> asked;
    std::vector<std::string> made;
    EngineFolderLister lister;
    lister.list = [&asked, &made](std::string path, EngineFolderListingCompletion done) {
        asked.push_back(path);
        const auto at = path.empty() ? std::string{"/home/me"} : path;
        std::vector<std::string> folders{".mpd", "Music", "linked"};
        for (const auto& name : made) {
            folders.push_back(name);
        }
        done(EngineFolderListing{.path = at,
                                 .parent = at == "/" ? std::optional<std::string>{}
                                                     : std::optional<std::string>{"/home"},
                                 .folders = folders,
                                 .linked = {"linked"},
                                 .home = "/home/me",
                                 .roots = {"/srv/music"}});
    };
    lister.make = [&made](std::string path, std::string name, EngineFolderMadeCompletion done) {
        made.push_back(name);
        done(path + "/" + name);
    };
    EngineFolderDialog dialog{QStringLiteral("gemenon"), lister, {}, nullptr};
    dialog.setAttribute(Qt::WA_DeleteOnClose, false);
    dialog.show();
    auto* folders = dialog.findChild<QListWidget*>(QStringLiteral("bench-engine-folder-list"));
    auto* places = dialog.findChild<QListWidget*>(QStringLiteral("bench-engine-folder-places"));
    QVERIFY(folders != nullptr && places != nullptr);
    // Hidden ones left out until asked for.
    QTRY_COMPARE(folders->count(), 2);
    QCOMPARE(folders->item(0)->text(), QStringLiteral("Music"));
    QCOMPARE(folders->item(1)->toolTip(), QStringLiteral("A link to a folder"));
    dialog.findChild<QCheckBox*>(QStringLiteral("bench-engine-folder-hidden"))->setChecked(true);
    QTRY_COMPARE(folders->count(), 3);
    QCOMPARE(folders->item(0)->text(), QStringLiteral(".mpd"));
    // Places: home, the music folders, the root.
    QCOMPARE(places->count(), 3);
    QCOMPARE(places->item(1)->text(), QStringLiteral("/srv/music"));
    emit places->itemClicked(places->item(1));
    QTRY_COMPARE(asked.back(), std::string{"/srv/music"});
    // The steps of the path, each going back there.
    QList<QToolButton*> crumbs;
    QTRY_COMPARE(
        (crumbs = dialog.findChildren<QToolButton*>(QStringLiteral("bench-engine-folder-crumb")))
            .size(),
        3);
    crumbs.at(1)->click();
    QTRY_COMPARE(asked.back(), std::string{"/srv"});
    // A path typed, Enter going there.
    auto* path = dialog.findChild<QLineEdit*>(QStringLiteral("bench-engine-folder-path"));
    path->setText(QStringLiteral("/var/lib/mpd"));
    emit path->returnPressed();
    QTRY_COMPARE(asked.back(), std::string{"/var/lib/mpd"});
    // Backspace in the folders goes out.
    folders->setFocus();
    QTest::keyClick(folders, Qt::Key_Backspace);
    QTRY_COMPARE(asked.back(), std::string{"/home"});
    // A folder made there, and gone into.
    auto* make = dialog.findChild<QPushButton*>(QStringLiteral("bench-engine-folder-make"));
    QVERIFY(make != nullptr && make->isVisible());
    QTimer::singleShot(0, [] {
        for (auto* question : QApplication::topLevelWidgets()) {
            if (auto* input = qobject_cast<QInputDialog*>(question); input && input->isVisible()) {
                input->setTextValue(QStringLiteral("Playlists"));
                input->accept();
            }
        }
    });
    make->click();
    QTRY_COMPARE(made, std::vector<std::string>{"Playlists"});
    QTRY_COMPARE(asked.back(), std::string{"/home/Playlists"});
    // Choose: the folder shown, none selected.
    QByteArray chosen;
    connect(&dialog, &EngineFolderDialog::folderChosen, &dialog,
            [&chosen](const QByteArray& raw) { chosen = raw; });
    folders->clearSelection();
    folders->setCurrentItem(nullptr);
    dialog.findChild<QPushButton*>(QStringLiteral("bench-engine-folder-choose"))->click();
    QCOMPARE(chosen, QByteArray{"/home/Playlists"});
}

void BenchMainWindowTest::FileWork::metadataStartupPresentsReconciliation() {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY(QDir().mkpath(base));
    const auto database_path = std::filesystem::path{
        QFile::encodeName(base + QStringLiteral("/lists.sqlite")).toStdString()};
    {
        auto journal = persistence::SqliteMetadataOperationJournal::open(database_path);
        QVERIFY(journal.has_value());
        const auto id = core::StableId::random();
        const core::LocalSourceRevision revision{.device = 1,
                                                 .inode = 2,
                                                 .size = 3,
                                                 .modification_time_seconds = 4,
                                                 .modification_time_nanoseconds = 5};
        const operations::MetadataOperationJournalRecord record{
            .id = id,
            .state = operations::MetadataOperationJournalState::planned,
            .source_raw_path = "/music/needs-attention.flac",
            .prepared_raw_path = "/music/.trackknife-prepared",
            .backup_raw_path = "/music/.trackknife-backup",
            .expected_revision = revision,
            .prepared_revision = std::nullopt,
            .published_revision = std::nullopt,
            .occurrence_indexes = {0U},
            .content_kind = operations::MetadataOperationContentKind::text_fields,
            .changes = {operations::MetadataOperationJournalChange{
                .field_index = 0U,
                .canonical_name = "title",
                .property_name = "TITLE",
                .original_present = true,
                .original_values = {"Old"},
                .kind = metadata::StagedMetadataPatchKind::replace_values,
                .planned_values = {"New"},
                .item_indexes = {0U},
                .exact_native_name = std::nullopt,
            }},
            .artwork = std::nullopt,
            .failure = std::nullopt,
        };
        QVERIFY(journal->create(record));
        QVERIFY(journal->transition(
            id, operations::MetadataOperationJournalTransition{
                    .expected_state = operations::MetadataOperationJournalState::planned,
                    .state = operations::MetadataOperationJournalState::needs_reconciliation,
                    .prepared_revision = std::nullopt,
                    .published_revision = std::nullopt,
                    .failure = core::Error{.code = core::ErrorCode::conflict,
                                           .message = "Ambiguous source identity",
                                           .context = {}},
                }));
    }

    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(!window.property("trackknife-metadata-operation-running").toBool());
        QTRY_COMPARE(window.property("trackknife-metadata-reconciliation-count").toULongLong(),
                     1ULL);
        QDialog* dialog = nullptr;
        QTRY_VERIFY((dialog = window.findChild<QDialog*>(
                         QStringLiteral("bench-preparation-feedback"))) != nullptr);
        auto* table =
            dialog->findChild<QTreeWidget*>(QStringLiteral("bench-preparation-feedback-table"));
        QVERIFY(table != nullptr);
        QCOMPARE(table->topLevelItemCount(), 1);
        QVERIFY(
            table->topLevelItem(0)->text(1).contains(QStringLiteral("Ambiguous source identity")));
        dialog->close();
        QTRY_VERIFY(window.findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) ==
                    nullptr);
    }

    // A presented incident is acknowledged and does not reopen on later starts.
    BenchMainWindow second;
    second.show();
    QTRY_VERIFY(!second.property("trackknife-metadata-operation-running").toBool());
    QTRY_COMPARE(second.property("trackknife-metadata-reconciliation-count").toULongLong(), 1ULL);
    QVERIFY(second.findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) == nullptr);
}

void BenchMainWindowTest::FileWork::filePublicationStartupPresentsReconciliation() {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY(QDir().mkpath(base));
    const auto database_path = std::filesystem::path{
        QFile::encodeName(base + QStringLiteral("/lists.sqlite")).toStdString()};
    {
        auto journal = persistence::SqliteFilePublicationJournal::open(database_path);
        QVERIFY(journal.has_value());
        const auto id = core::StableId::random();
        const core::LocalSourceRevision revision{.device = 1U,
                                                 .inode = 2U,
                                                 .size = 3U,
                                                 .modification_time_seconds = 4,
                                                 .modification_time_nanoseconds = 5};
        const operations::FilePublicationJournalRecord record{
            .id = id,
            .state = operations::FilePublicationJournalState::planned,
            .publication = operations::OutputPathPublicationKind::same_filesystem_rename,
            .source_raw_path = "/music/original.flac",
            .target_raw_path = "/music/renamed.flac",
            .prepared_raw_path = {},
            .expected_source_revision = revision,
            .prepared_revision = std::nullopt,
            .target_revision = std::nullopt,
            .occurrence_indexes = {0U},
            .planned_missing_directory_raw_paths = {},
            .reverses_journal_id = std::nullopt,
            .failure = std::nullopt,
        };
        QVERIFY(journal->create(record));
        QVERIFY(journal->transition(
            id, operations::FilePublicationJournalTransition{
                    .expected_state = operations::FilePublicationJournalState::planned,
                    .state = operations::FilePublicationJournalState::needs_reconciliation,
                    .prepared_revision = std::nullopt,
                    .target_revision = std::nullopt,
                    .failure = core::Error{.code = core::ErrorCode::conflict,
                                           .message = "Ambiguous rename topology",
                                           .context = {}},
                }));
    }

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(!window.property("trackknife-metadata-operation-running").toBool());
    QTRY_COMPARE(window.property("trackknife-metadata-reconciliation-count").toULongLong(), 0ULL);
    QTRY_COMPARE(window.property("trackknife-file-reconciliation-count").toULongLong(), 1ULL);
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<QDialog*>(
                     QStringLiteral("bench-preparation-feedback"))) != nullptr);
    auto* table =
        dialog->findChild<QTreeWidget*>(QStringLiteral("bench-preparation-feedback-table"));
    QVERIFY(table != nullptr);
    QCOMPARE(table->topLevelItemCount(), 1);
    QCOMPARE(table->topLevelItem(0)->text(0), QStringLiteral("/music/original.flac"));
    QVERIFY(table->topLevelItem(0)->text(1).contains(QStringLiteral("Ambiguous rename topology")));
    QVERIFY(table->topLevelItem(0)->text(1).contains(QStringLiteral("/music/renamed.flac")));
}

void BenchMainWindowTest::FileWork::combinedPublicationStartupRecoversMetadataAndPath() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto source_path = media.filePath(QStringLiteral("combined-recovery-source.flac"));
    const auto target_path = media.filePath(QStringLiteral("combined-recovery-target.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), source_path));
    const auto source_encoded = QFile::encodeName(source_path);
    const auto target_encoded = QFile::encodeName(target_path);
    const std::string source_raw{source_encoded.constData(),
                                 static_cast<std::size_t>(source_encoded.size())};
    const std::string target_raw{target_encoded.constData(),
                                 static_cast<std::size_t>(target_encoded.size())};
    const auto source_read = metadata::read_local_metadata(source_raw);
    QVERIFY(source_read.has_value());
    auto selection = metadata::StagedMetadataSelection::create({metadata::StagedMetadataSource{
        .raw_path = source_raw,
        .source_revision = source_read->source_revision,
        .baseline = source_read->document,
    }});
    QVERIFY(selection.has_value());
    const auto title = selection->field_index("title");
    QVERIFY(title.has_value());
    metadata::StagedMetadataPatchSet patches;
    QVERIFY(patches.replace_values(*selection, 0U, *title, {"Recovered combined title"}));
    const auto write_plan = metadata::revalidate_metadata_write_plan(*selection, patches);
    QVERIFY(write_plan && write_plan->ready() && write_plan->sources.size() == 1U);

    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY(QDir().mkpath(base));
    const auto database_path = std::filesystem::path{
        QFile::encodeName(base + QStringLiteral("/lists.sqlite")).toStdString()};
    const auto operation_id = core::StableId::random();
    // A tab kept by the window, of the file at its revision before the move.
    const auto tab_id = core::StableId::random();
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        persistence::ListItem item;
        item.source = persistence::ListSource::local;
        item.source_reference = source_raw;
        item.source_revision = source_read->source_revision;
        auto* tab =
            window.addListTab(persistence::ListDocument{.id = tab_id,
                                                        .kind = persistence::ListKind::scratch,
                                                        .name = "Combined recovery",
                                                        .pinned = false,
                                                        .dirty = false,
                                                        .items = {item},
                                                        .engine = {}},
                              true);
        QVERIFY(tab != nullptr);
        window.persistNow(true);
        QVERIFY(window.close());
    }
    {
        auto repository = persistence::ListRepository::open(database_path);
        QVERIFY(repository.has_value());
        const auto prepared =
            metadata::prepare_flac_metadata_write_copy(write_plan->sources.front(), target_raw);
        QVERIFY(prepared.has_value());
        auto journal = persistence::SqliteFilePublicationJournal::open(database_path);
        QVERIFY(journal.has_value());
        const auto prepared_raw =
            operations::file_publication_prepared_path(target_path.toStdString(), operation_id)
                .native();
        QVERIFY(journal->create(operations::FilePublicationJournalRecord{
            .id = operation_id,
            .state = operations::FilePublicationJournalState::planned,
            .publication = operations::OutputPathPublicationKind::same_filesystem_rename,
            .content = operations::FilePublicationContentKind::prepared_destination_artifact,
            .source_raw_path = source_raw,
            .target_raw_path = target_raw,
            .prepared_raw_path = prepared_raw,
            .expected_source_revision = source_read->source_revision,
            .prepared_revision = std::nullopt,
            .target_revision = std::nullopt,
            .occurrence_indexes = {0U},
            .planned_missing_directory_raw_paths = {},
            .reverses_journal_id = std::nullopt,
            .failure = std::nullopt,
        }));
        QVERIFY(journal->transition(
            operation_id, operations::FilePublicationJournalTransition{
                              .expected_state = operations::FilePublicationJournalState::planned,
                              .state = operations::FilePublicationJournalState::target_prepared,
                              .prepared_revision = prepared->prepared_revision,
                              .target_revision = std::nullopt,
                              .failure = std::nullopt,
                          }));
        QVERIFY(journal->transition(
            operation_id,
            operations::FilePublicationJournalTransition{
                .expected_state = operations::FilePublicationJournalState::target_prepared,
                .state = operations::FilePublicationJournalState::target_published,
                .prepared_revision = prepared->prepared_revision,
                .target_revision = prepared->prepared_revision,
                .failure = std::nullopt,
            }));
    }
    // ADR-0237: the engine recovers its journal when it starts, as after the
    // crash this stands for.
    engine_.stop();
    QVERIFY2(engine_.start(), engine_.log().constData());

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(!window.property("trackknife-metadata-operation-running").toBool(),
                             5'000);
    auto* tab = window.tabForDocument(QString::fromStdString(tab_id.to_string()));
    QVERIFY(tab != nullptr);
    auto* list_model = tab->model;
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE(list_model->rowCount(), 1);
    QTRY_COMPARE(list_model->rows().front().raw_path, target_raw);
    QTRY_COMPARE(list_model->rows().front().title, std::string{"Recovered combined title"});
    QVERIFY(!QFile::exists(source_path));
    QVERIFY(QFile::exists(target_path));

    // Successful recovery is silent: no attention window, no history surface.
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) == nullptr);
    QCOMPARE(window.property("trackknife-file-reconciliation-count").toULongLong(), 0ULL);
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::FileWork)

#include "window_file_work_test.moc"
