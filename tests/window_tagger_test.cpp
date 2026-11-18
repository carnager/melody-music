// SPDX-License-Identifier: GPL-3.0-only

// The window test: the tagger window: layouts, apply, undo and what it reads.

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

class BenchMainWindowTest::Tagger final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void committedMetadataRefreshesDuplicatesAndPreservesCueOverlay();
    void metadataReadyPlanAppliesAndRefreshesHistory();
    void aTaggerUndoesItsWriteUntilClosed();
    void metadataApplyCancellationPreservesDraftForFreshPreview();
    void metadataDialogLayoutsPersistAsynchronously();
    void taggerWindowStateRoundTrips();
    void metadataFieldLayoutsLoadFilterAndPersist();
    void settingsChooseWhereUndoCopiesAreKept();
    void propertiesShowTechnicalSummary();
    void theTaggerOpensOnCachedTagsAndReadsBehind();
    void propertiesFileListLivesInTheTaggerWindow();
    void fileListShowsTheFilesUnderTheirAlbums();
    void metadataRetrySkipsSavedFiles_data();
    void metadataRetrySkipsSavedFiles();
    void metadataServiceSettingsAndCompactPages();
    void richMetadataValuesAndIdentitiesSurviveListRestart();
};

// ADR-0266: where an engine keeps its undo copies, asked of it and changed
// there at once.
void BenchMainWindowTest::Tagger::settingsChooseWhereUndoCopiesAreKept() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::undo);
    QVERIFY(dialog != nullptr);
    auto* place = dialog->findChild<QWidget*>(QStringLiteral("bench-undo-location"));
    QVERIFY(place != nullptr);
    auto* own = place->findChild<QRadioButton*>(QStringLiteral("bench-undo-location-engine"));
    auto* beside = place->findChild<QRadioButton*>(QStringLiteral("bench-undo-location-beside"));
    auto* kept_in = place->findChild<QLabel*>(QStringLiteral("bench-undo-location-engine-folder"));
    auto* status = place->findChild<QLabel*>(QStringLiteral("bench-undo-location-status"));
    QVERIFY(own && beside && kept_in && status);
    // By default, the engine's own folder: said, with where it is.
    QTRY_VERIFY_WITH_TIMEOUT(own->isChecked() && own->isEnabled(), 10'000);
    QVERIFY(kept_in->text().endsWith(QStringLiteral("undo")));
    // Beside each file: the engine's at once.
    beside->click();
    QTRY_VERIFY_WITH_TIMEOUT(beside->isChecked() && beside->isEnabled(), 10'000);
    QCOMPARE(status->text(), QStringLiteral("Undo copies are kept there now"));
    auto asked = window.localEngine().file_work->backup_location();
    QVERIFY(asked.has_value());
    QCOMPARE(asked->place, std::string{"beside"});
    QVERIFY(asked->kept_in.empty());
    // And back.
    own->click();
    QTRY_VERIFY_WITH_TIMEOUT(own->isChecked() && own->isEnabled(), 10'000);
    asked = window.localEngine().file_work->backup_location();
    QVERIFY(asked && asked->place == "engine" && asked->kept_in.ends_with("undo"));
    dialog->close();
}

void BenchMainWindowTest::Tagger::committedMetadataRefreshesDuplicatesAndPreservesCueOverlay() {
    const std::string source{"/music/shared.flac"};
    const auto field = [](std::string canonical_name, std::string value,
                          const metadata::FieldProvenance provenance) {
        return metadata::MetadataField{
            .canonical_name = canonical_name,
            .native_name = canonical_name,
            .values = {std::move(value)},
            .qualifier = {},
            .provenance = provenance,
        };
    };
    LocalTrackRow whole{
        .raw_path = source,
        .logical_reference = std::nullopt,
        .selection = {},
        .segment = std::nullopt,
        .title = "Old",
        .artist = {},
        .album = "Old album",
        .album_artist = {},
        .date = {},
        .track_number = {},
        .duration_ms = 1'000,
        .metadata =
            metadata::MetadataDocument{
                .fields = {field("title", "Old", metadata::FieldProvenance::embedded),
                           field("album", "Old album", metadata::FieldProvenance::embedded)},
                .unsupported_native_objects = {}},
        .source_revision = std::nullopt,
        .probed = true,
    };
    auto cue = whole;
    cue.logical_reference = std::string{"cue-v1\0sheet\0track", 18U};
    cue.segment = formats::SampleRange{.start_sample = 0, .end_sample = 44'100};
    cue.title = "CUE title";
    cue.metadata.fields.push_back(field("title", "CUE title", metadata::FieldProvenance::sidecar));
    LocalTrackRow unrelated = whole;
    unrelated.raw_path = "/music/other.flac";
    unrelated.title = "Other";
    unrelated.metadata.fields.front().values = {"Other"};

    LocalListModel model;
    model.replaceRows({whole, cue, unrelated});
    const QPersistentModelIndex playing{model.index(2, 0)};
    const QPersistentModelIndex duplicate{model.index(1, 0)};
    QSignalSpy resets{&model, &QAbstractItemModel::modelReset};
    QSignalSpy changes{&model, &QAbstractItemModel::dataChanged};
    const metadata::MetadataDocument committed{
        .fields = {field("title", "New", metadata::FieldProvenance::embedded),
                   field("album", "New album", metadata::FieldProvenance::embedded)},
        .unsupported_native_objects = {},
    };
    const core::LocalSourceRevision revision{.device = 1,
                                             .inode = 2,
                                             .size = 3,
                                             .modification_time_seconds = 4,
                                             .modification_time_nanoseconds = 5};
    const auto refreshed = model.applyCommittedMetadata(source, committed, revision);
    QVERIFY(refreshed.has_value());
    QCOMPARE(*refreshed, 2U);
    QCOMPARE(model.rows()[0].title, std::string{"New"});
    QCOMPARE(model.rows()[0].album, std::string{"New album"});
    QCOMPARE(model.rows()[1].title, std::string{"CUE title"});
    QCOMPARE(model.rows()[1].album, std::string{"New album"});
    QCOMPARE(model.rows()[1].metadata.fields.back().provenance, metadata::FieldProvenance::sidecar);
    QCOMPARE(model.rows()[0].source_revision, std::optional{revision});
    QCOMPARE(model.rows()[2].title, std::string{"Other"});
    QVERIFY(playing.isValid());
    QVERIFY(duplicate.isValid());
    QCOMPARE(playing.row(), 2);
    QCOMPARE(duplicate.row(), 1);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(changes.count(), 2);

    model.setCurrentSource(model.source(1), 1);
    const core::LocalSourceRevision relocated_revision{.device = 6,
                                                       .inode = 7,
                                                       .size = 8,
                                                       .modification_time_seconds = 9,
                                                       .modification_time_nanoseconds = 10};
    const std::string relocated{"/archive/shared.flac"};
    const auto relocated_rows =
        model.applyCommittedRelocation(source, relocated, revision, relocated_revision);
    QVERIFY(relocated_rows.has_value());
    QCOMPARE(*relocated_rows, 2U);
    QCOMPARE(model.rows()[0].raw_path, relocated);
    QCOMPARE(model.rows()[1].raw_path, relocated);
    QCOMPARE(model.rows()[0].source_revision, std::optional{relocated_revision});
    QCOMPARE(model.rows()[1].logical_reference, cue.logical_reference);
    QCOMPARE(model.rows()[2].raw_path, std::string{"/music/other.flac"});
    QVERIFY(model.data(model.index(1, 0), ui::track_current_role).toBool());
    QVERIFY(playing.isValid());
    QVERIFY(duplicate.isValid());
    QCOMPARE(resets.count(), 0);

    auto legacy = cue;
    legacy.metadata.fields = {
        field("title", "Flattened CUE", metadata::FieldProvenance::cached_snapshot)};
    model.replaceRows({legacy});
    const auto rejected = model.applyCommittedMetadata(source, committed, revision);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, core::ErrorCode::conflict);
    QCOMPARE(model.rows()[0], legacy);
}

// ADR-0183 addendum: while a tag editor tab is active, its file list is
// hosted as a temporary Files page in the local sources sidebar; leaving
// or closing the editor returns the widget and the sidebar state.
void BenchMainWindowTest::Tagger::propertiesFileListLivesInTheTaggerWindow() {
    // ADR-0221: the tagger is a window and owns its file list. The list sits
    // beside the field table rather than above it, shows paths relative to the
    // selection's common folder, and keeps the checkbox selection that scopes
    // edits. None of that depends on the workspace sidebar any more.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto file = directory.filePath(QStringLiteral("hosted.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), file));
    const auto encoded = QFile::encodeName(file);

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(
        {std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())}});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    auto* source_tabs = window.findChild<QTabBar*>(QStringLiteral("bench-local-source-tabs"));
    QVERIFY(tabs != nullptr && properties_action != nullptr && source_tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* list_view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(list_view != nullptr);
    auto* list_model = qobject_cast<LocalListModel*>(list_view->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE(list_model->rowCount(), 1);
    QTRY_VERIFY(list_model->rows().front().probed);
    list_view->selectionModel()->select(
        list_model->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties_action->isEnabled());
    const auto sidebar_tabs_before = source_tabs->count();
    properties_action->trigger();
    auto* properties =
        window.findChild<MetadataPropertiesDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);

    // The workspace is untouched: no extra sidebar page, no extra tab.
    QCOMPARE(source_tabs->count(), sidebar_tabs_before);
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(tabs->currentWidget(), static_cast<QWidget*>(list_view));

    // The grid, and with it the file list, builds asynchronously.
    QTRY_VERIFY(properties->fileListView() != nullptr);
    auto* files_view = properties->fileListView();
    QVERIFY(properties->isAncestorOf(files_view));
    QTRY_VERIFY(files_view->isVisible());
    QCOMPARE(window.findChildren<QTableView*>(QStringLiteral("bench-metadata-files")).size(), 1);

    // Beside the fields, not above them, so the field table gets full height.
    auto* splitter = properties->findChild<QSplitter*>(QStringLiteral("bench-metadata-splitter"));
    QVERIFY(splitter != nullptr);
    QCOMPARE(splitter->orientation(), Qt::Horizontal);

    // Checkbox selection scopes the edit: click the indicator or press Space.
    auto* selection = files_view->selectionModel();
    QCOMPARE(selection->selectedRows().size(), 1);
    const auto cell = files_view->visualRect(files_view->model()->index(0, 0));
    QTest::mouseClick(files_view->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(cell.left() + 14, cell.center().y()));
    QCOMPARE(selection->selectedRows().size(), 0);
    QTest::keyClick(files_view, Qt::Key_Space);
    QCOMPARE(selection->selectedRows().size(), 1);
    if (const auto screenshots = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !screenshots.isEmpty()) {
        QVERIFY(
            properties->grab().save(screenshots + QStringLiteral("/tag-window-checkboxes.png")));
    }

    // The breadcrumb carries the common folder and belongs to the tagger.
    auto* crumb = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-files-dir"));
    QVERIFY(crumb != nullptr);
    QVERIFY(properties->isAncestorOf(crumb));
    QTRY_VERIFY(crumb->text().endsWith(QLatin1Char('/')));
    QVERIFY(!crumb->text().contains(QStringLiteral("hosted.flac")));

    // A second tagger stands alongside the first, each owning its own view and
    // an independent selection -- which is what windows buy over a single tab.
    selection->clearSelection();
    QTRY_VERIFY(properties_action->isEnabled());
    properties_action->trigger();
    MetadataPropertiesDialog* second = nullptr;
    for (auto* candidate : window.findChildren<MetadataPropertiesDialog*>(
             QStringLiteral("bench-metadata-properties"))) {
        if (candidate != properties) {
            second = candidate;
        }
    }
    QVERIFY(second != nullptr);
    QVERIFY(second->isWindow());
    QTRY_VERIFY(second->fileListView());
    auto* second_view = second->fileListView();
    QVERIFY(second->isAncestorOf(second_view));
    QCOMPARE(second_view->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(window.findChildren<QTableView*>(QStringLiteral("bench-metadata-files")).size(), 2);
    QCOMPARE(selection->selectedRows().size(), 0);

    // Closing one tears down only its own view; the other is unaffected.
    QPointer<QTableView> second_lifetime = second_view;
    QVERIFY(second->close());
    QTRY_VERIFY(second_lifetime.isNull());
    QVERIFY(properties->isAncestorOf(files_view));
    QCOMPARE(files_view->selectionModel(), selection);

    QPointer<QTableView> lifetime = files_view;
    QVERIFY(properties->close());
    QTRY_VERIFY(lifetime.isNull());
    QCOMPARE(source_tabs->count(), sidebar_tabs_before);
}

void BenchMainWindowTest::Tagger::metadataReadyPlanAppliesAndRefreshesHistory() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto source_path = media.filePath(QStringLiteral("apply-ready.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), source_path));
    const auto encoded = QFile::encodeName(source_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    BenchMainWindow window;
    window.show();
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
    // ADR-0221: a window, so the tab strip is untouched and the list the
    // selection came from stays current.
    QVERIFY(properties->isWindow());
    QCOMPARE(tabs->count(), 1);
    QVERIFY(qobject_cast<MetadataPropertiesDialog*>(tabs->currentWidget()) == nullptr);
    QVERIFY(properties->windowTitle().startsWith(QStringLiteral("Edit tags · 1 track")));
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* preview =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(preview != nullptr);
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    const auto title_draft = aggregate_model->index(*title_row, 2);
    QVERIFY(aggregate_model->setData(title_draft, QStringLiteral("Applied from ready preview"),
                                     Qt::EditRole));
    QTRY_VERIFY(preview->isEnabled());
    QTest::mouseClick(preview, Qt::LeftButton);

    // Direct apply: a ready plan runs immediately with inline progress; no
    // review or result dialog appears; the tagger stays open to undo it, and is closed.
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rows().front().title,
                              std::string{"Applied from ready preview"}, 5'000);
    const auto reread = metadata::read_local_metadata(raw_path);
    QVERIFY(reread.has_value());
    QCOMPARE(reread->document.effective_values("title"),
             (std::vector<std::string>{"Applied from ready preview"}));
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) == nullptr);
    QVERIFY(window_test::closeTaggerAfterWrite(window));
    QTRY_COMPARE(tabs->count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!window.property("trackknife-metadata-operation-running").toBool(),
                             5'000);
}

// ADR-0289: a write from the tagger is undone while it is open; closed, it
// lets go of what it kept, and the engine keeps nothing.
void BenchMainWindowTest::Tagger::aTaggerUndoesItsWriteUntilClosed() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto source_path = media.filePath(QStringLiteral("undo-write.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), source_path));
    const auto encoded = QFile::encodeName(source_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    const auto before = metadata::read_local_metadata(raw_path);
    QVERIFY(before.has_value());
    const auto original_title = before->document.effective_values("title");
    const auto database =
        std::filesystem::path{qEnvironmentVariable("MELODY_STATE_DIR").toStdString()} /
        "lists.sqlite";

    BenchMainWindow window;
    window.show();
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
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    QTRY_VERIFY(properties_action->isEnabled());
    properties_action->trigger();
    QPointer<QDialog> properties =
        window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(!properties.isNull());
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    auto* undo = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-undo-write"));
    QVERIFY(aggregate_model != nullptr && apply != nullptr && undo != nullptr);
    QVERIFY(!undo->isVisible());
    const auto write_title = [&](const QString& title) {
        const auto row = aggregate_model->fieldRow(QStringLiteral("title"));
        QVERIFY(row.has_value());
        QVERIFY(aggregate_model->setData(aggregate_model->index(*row, 2), title, Qt::EditRole));
        QTRY_VERIFY(apply->isEnabled());
        QTest::mouseClick(apply, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(undo->isVisible() && undo->isEnabled(), 10'000);
    };

    // Written, and kept to undo: the window stays.
    write_title(QStringLiteral("Written to be undone"));
    QVERIFY(!properties.isNull() && properties->isVisible());
    QCOMPARE(metadata::read_local_metadata(raw_path)->document.effective_values("title"),
             (std::vector<std::string>{"Written to be undone"}));
    QCOMPARE(engine::kept_undo_copies(database).size(), 1U);

    // Undone: the file as it was, the row too, nothing more to undo.
    QTest::mouseClick(undo, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!undo->isVisible(), 10'000);
    QCOMPARE(metadata::read_local_metadata(raw_path)->document.effective_values("title"),
             original_title);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rows().front().title,
                              original_title.empty() ? std::string{} : original_title.front(),
                              5'000);
    QVERIFY(engine::kept_undo_copies(database).empty());

    // Written again and closed: kept no longer.
    write_title(QStringLiteral("Written and kept"));
    QCOMPARE(engine::kept_undo_copies(database).size(), 1U);
    properties->close();
    QTRY_VERIFY(properties.isNull() || !properties->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(engine::kept_undo_copies(database).empty(), 10'000);
    QCOMPARE(metadata::read_local_metadata(raw_path)->document.effective_values("title"),
             (std::vector<std::string>{"Written and kept"}));
}

void BenchMainWindowTest::Tagger::metadataApplyCancellationPreservesDraftForFreshPreview() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    std::vector<MetadataPropertiesSource> sources;
    for (int index = 0; index < 3; ++index) {
        const auto path = media.filePath(QStringLiteral("cancel-%1.flac").arg(index));
        QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
        const auto encoded = QFile::encodeName(path);
        const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
        const auto read = metadata::read_local_metadata(raw_path);
        QVERIFY(read.has_value());
        sources.push_back(MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = raw_path,
                    .source_revision = read->source_revision,
                    .baseline = read->document,
                },
            .track_label = QStringLiteral("Cancel %1").arg(index),
        });
    }

    std::atomic_size_t admitted{0U};
    std::optional<operations::MetadataApplyResult> observed;
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {},
        [&admitted] {
            return MetadataWritePlanApplier{
                [&admitted](const metadata::MetadataWritePlan& plan,
                            const operations::MetadataApplyProgressCallback& progress,
                            const core::CancellationToken& cancellation) {
                    return operations::apply_metadata_write_plan(
                        plan,
                        [&admitted](const metadata::MetadataWritePlanSource& source,
                                    const core::CancellationToken& token)
                            -> core::Result<operations::MetadataCommitResult> {
                            admitted.fetch_add(1U, std::memory_order_relaxed);
                            while (!token.is_cancellation_requested()) {
                                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                            }
                            return std::unexpected(core::Error{
                                .code = core::ErrorCode::cancelled,
                                .message = "cancelled " + source.raw_path,
                                .context = {},
                            });
                        },
                        {}, {}, progress, cancellation,
                        operations::MetadataApplyOptions{.maximum_parallelism = 2U});
                }};
        },
        [&observed](const operations::MetadataApplyResult& result) { observed = result; });
    properties->show();

    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* preview =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(preview != nullptr);
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*title_row, 2),
                                     QStringLiteral("Cancelled batch title"), Qt::EditRole));
    QTRY_VERIFY(preview->isEnabled());
    QTest::mouseClick(preview, Qt::LeftButton);

    // The ready plan applies directly; progress and Stop live in the footer.
    auto* stop = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-stop"));
    auto* progress_bar =
        properties->findChild<QProgressBar*>(QStringLiteral("bench-metadata-apply-progress"));
    QVERIFY(stop != nullptr);
    QVERIFY(progress_bar != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(stop->isVisible(), 5'000);
    QVERIFY(progress_bar->isVisible());
    QTRY_COMPARE(admitted.load(std::memory_order_relaxed), 2U);
    QTest::mouseClick(stop, Qt::LeftButton);

    // A stopped run reports the untouched files in one compact dialog.
    QDialog* feedback = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((feedback = properties->findChild<QDialog*>(
                                  QStringLiteral("bench-preparation-feedback"))) != nullptr,
                             5'000);
    auto* feedback_table =
        feedback->findChild<QTreeWidget*>(QStringLiteral("bench-preparation-feedback-table"));
    auto* feedback_summary =
        feedback->findChild<QLabel*>(QStringLiteral("bench-preparation-feedback-summary"));
    auto* close =
        feedback->findChild<QPushButton*>(QStringLiteral("bench-preparation-feedback-close"));
    QVERIFY(feedback_table != nullptr);
    QVERIFY(feedback_summary != nullptr);
    QVERIFY(close != nullptr);
    QCOMPARE(feedback->windowTitle(), QStringLiteral("Save stopped"));
    QCOMPARE(feedback_table->topLevelItemCount(), 3);
    QVERIFY(feedback_summary->text().contains(QStringLiteral("0 saved")));
    QVERIFY(feedback_summary->text().contains(QStringLiteral("3 stopped")));
    QVERIFY(observed.has_value());
    QCOMPARE(observed->cancelled_source_count(), 3U);
    QTest::mouseClick(close, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) ==
                nullptr);
    QVERIFY(!stop->isVisible());
    QVERIFY(!progress_bar->isVisible());
    QTRY_VERIFY(preview->isEnabled());
    QTRY_COMPARE(
        aggregate_model->data(aggregate_model->index(*title_row, 2), Qt::EditRole).toString(),
        QStringLiteral("Cancelled batch title"));
    delete properties;
}

// The size a window of any toolkit restores: stored on close, handed back
// to the next tagger as it was.
void BenchMainWindowTest::Tagger::taggerWindowStateRoundTrips() {
    QHash<QString, QByteArray> saved_states;
    TaggerServices services;
    services.layout_store = MetadataDialogLayoutStore{
        .load =
            [this, &saved_states](QString key,
                                  MetadataDialogLayoutStore::LoadCompletion completion) {
                const auto state = saved_states.value(key);
                QTimer::singleShot(0, this, [completion = std::move(completion), state]() mutable {
                    completion(state, {});
                });
            },
        .save =
            [&saved_states](QString key, QByteArray value,
                            MetadataDialogLayoutStore::Completion completion) {
                saved_states.insert(std::move(key), std::move(value));
                if (completion) {
                    completion({});
                }
            },
    };
    const auto no_source = [](std::size_t) -> std::optional<MetadataPropertiesSource> {
        return std::nullopt;
    };
    {
        TaggerSession first(0U, no_source, {}, services);
        first.storeWindowState({{QStringLiteral("width"), 930},
                                {QStringLiteral("height"), 640},
                                {QStringLiteral("maximized"), true},
                                {QStringLiteral("listWidth"), 210}});
    }
    QVERIFY(saved_states.contains(QStringLiteral("workspace/metadata-properties-window-v1")));

    TaggerSession second(0U, no_source, {}, services);
    QVariantMap loaded;
    second.loadWindowState([&loaded](QVariantMap state) { loaded = std::move(state); });
    QTRY_VERIFY(!loaded.isEmpty());
    QCOMPARE(loaded.value(QStringLiteral("width")).toInt(), 930);
    QCOMPARE(loaded.value(QStringLiteral("height")).toInt(), 640);
    QCOMPARE(loaded.value(QStringLiteral("maximized")).toBool(), true);
    QCOMPARE(loaded.value(QStringLiteral("listWidth")).toInt(), 210);
}

void BenchMainWindowTest::Tagger::metadataDialogLayoutsPersistAsynchronously() {
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = "/music/layout-fixture.flac",
                .source_revision = std::nullopt,
                .baseline = metadata::MetadataDocument{},
            },
        .track_label = QStringLiteral("Layout fixture"),
    };
    QHash<QString, QByteArray> saved_states;
    const MetadataDialogLayoutStore layout_store{
        .load =
            [this, &saved_states](QString key,
                                  MetadataDialogLayoutStore::LoadCompletion completion) {
                const auto state = saved_states.value(key);
                QTimer::singleShot(0, this, [completion = std::move(completion), state]() mutable {
                    completion(state, {});
                });
            },
        .save =
            [&saved_states](QString key, QByteArray value,
                            MetadataDialogLayoutStore::Completion completion) {
                saved_states.insert(std::move(key), std::move(value));
                if (completion) {
                    completion({});
                }
            },
    };
    const auto create_properties = [&] {
        return new MetadataPropertiesDialog(
            1U,
            [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
                return index == 0U ? std::optional{source} : std::nullopt;
            },
            {}, {}, {}, {}, {}, {}, {}, nullptr, layout_store);
    };

    auto* first = create_properties();
    first->show();
    QSplitter* first_metadata = nullptr;
    QTRY_VERIFY((first_metadata = first->findChild<QSplitter*>(
                     QStringLiteral("bench-metadata-splitter"))) != nullptr);
    first->resize(930, 640);
    first_metadata->setSizes({125, 405});

    auto* transform = first->findChild<QPushButton*>(QStringLiteral("bench-metadata-transform"));
    QVERIFY(transform != nullptr);
    QTRY_VERIFY(transform->isEnabled());
    QTest::mouseClick(transform, Qt::LeftButton);
    QDialog* first_editor = nullptr;
    QTRY_VERIFY((first_editor = first->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-transformation"))) != nullptr);
    auto* first_editor_splitter = first_editor->findChild<QSplitter*>(
        QStringLiteral("bench-metadata-transformation-splitter"));
    QVERIFY(first_editor_splitter != nullptr);
    first_editor->resize(970, 720);
    first_editor_splitter->setSizes({365, 585});
    QVERIFY(first_editor->close());
    QTRY_VERIFY(first->findChild<QDialog*>(QStringLiteral("bench-metadata-transformation")) ==
                nullptr);
    QVERIFY(first->close());
    QTRY_COMPARE(saved_states.size(), 4);

    auto* second = create_properties();
    second->show();
    QSplitter* second_metadata = nullptr;
    QTRY_VERIFY((second_metadata = second->findChild<QSplitter*>(
                     QStringLiteral("bench-metadata-splitter"))) != nullptr);
    QTRY_COMPARE(second->height(), 640);
    QTRY_COMPARE(
        second_metadata->saveState(),
        saved_states.value(QStringLiteral("workspace/metadata-properties-metadata-splitter-v1")));

    transform = second->findChild<QPushButton*>(QStringLiteral("bench-metadata-transform"));
    QVERIFY(transform != nullptr);
    QTRY_VERIFY(transform->isEnabled());
    QTest::mouseClick(transform, Qt::LeftButton);
    QDialog* second_editor = nullptr;
    QTRY_VERIFY((second_editor = second->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-transformation"))) != nullptr);
    auto* second_editor_splitter = second_editor->findChild<QSplitter*>(
        QStringLiteral("bench-metadata-transformation-splitter"));
    QVERIFY(second_editor_splitter != nullptr);
    QTRY_COMPARE(second_editor->height(), 720);
    QTRY_COMPARE(
        second_editor_splitter->saveState(),
        saved_states.value(QStringLiteral("workspace/metadata-transformation-splitter-v1")));
    QVERIFY(second_editor->close());
    QVERIFY(second->close());
}

void BenchMainWindowTest::Tagger::metadataFieldLayoutsLoadFilterAndPersist() {
    const auto make_field = [](std::string name, std::string value) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = {std::move(value)},
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = "/music/field-layout.flac",
                .source_revision = std::nullopt,
                .baseline =
                    metadata::MetadataDocument{
                        .fields = {make_field("TITLE", "One"), make_field("ARTIST", "Artist"),
                                   make_field("ALBUM", "Album")},
                        .unsupported_native_objects = {},
                    },
            },
        .track_label = QStringLiteral("Field layout fixture"),
    };
    QHash<QString, QByteArray> states;
    states.insert(
        QStringLiteral("workspace/metadata-field-layouts-v1"),
        QByteArrayLiteral(
            R"({"schema":1,"active":"compact","layouts":[{"id":"compact","name":"Compact","fields":["title","artist"]}]})"));
    const MetadataDialogLayoutStore store{
        .load = [&states](QString key, auto completion) { completion(states.value(key), {}); },
        .save =
            [&states](QString key, QByteArray value, auto completion) {
                states.insert(std::move(key), std::move(value));
                if (completion) {
                    completion({});
                }
            },
    };
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, store);
    properties->show();
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* combo = properties->findChild<QComboBox*>(QStringLiteral("bench-metadata-field-layout"));
    QVERIFY(combo != nullptr);
    QVERIFY(combo->isHidden());
    auto* model = qobject_cast<MetadataAggregateModel*>(fields->model());
    QVERIFY(model != nullptr);
    const auto title = model->fieldRow(QStringLiteral("title"));
    const auto artist = model->fieldRow(QStringLiteral("artist"));
    const auto album = model->fieldRow(QStringLiteral("album"));
    QVERIFY(title && artist && album);
    QCOMPARE(*title, 0);
    QCOMPARE(*artist, 1);
    QVERIFY(!fields->isRowHidden(*title));
    QVERIFY(!fields->isRowHidden(*artist));
    QVERIFY(!fields->isRowHidden(*album));
    QVERIFY(!fields->isRowHidden(*album));
    QCOMPARE(
        states.value(QStringLiteral("workspace/metadata-field-layouts-v1")),
        QByteArrayLiteral(
            R"({"schema":1,"active":"compact","layouts":[{"id":"compact","name":"Compact","fields":["title","artist"]}]})"));
    delete properties;
}

// ADR-0152: the read-only technical summary under the file list probes
// the selected files in the background and aggregates codec, rate,
// depth, channels, bitrate, and duration; disagreement shows "mixed".
void BenchMainWindowTest::Tagger::propertiesShowTechnicalSummary() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto flac_path = media.filePath(QStringLiteral("tone.flac"));
    const auto opus_path = media.filePath(QStringLiteral("tone.opus"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), flac_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("loudness-tone-opus.b64"), opus_path));
    const auto make_source = [](const QString& path, const QString& label) {
        const auto encoded = QFile::encodeName(path);
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path =
                        std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())},
                    .source_revision = std::nullopt,
                    .baseline = metadata::MetadataDocument{},
                },
            .track_label = label,
        };
    };
    const std::vector sources{make_source(flac_path, QStringLiteral("Flac")),
                              make_source(opus_path, QStringLiteral("Opus"))};
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {});
    properties->show();

    QLabel* technical = nullptr;
    QTRY_VERIFY((technical = properties->findChild<QLabel*>(
                     QStringLiteral("bench-metadata-technical"))) != nullptr);
    // The whole selection mixes a 44.1 kHz FLAC with a 48 kHz Opus file.
    QTRY_VERIFY_WITH_TIMEOUT(!technical->text().isEmpty() &&
                                 !technical->text().contains(QStringLiteral("analyzing")),
                             15'000);
    QVERIFY(technical->text().contains(QStringLiteral("2 tracks")));
    QVERIFY(technical->text().contains(QStringLiteral("mixed codecs")));
    QVERIFY(technical->text().contains(QStringLiteral("mixed rates")));
    QVERIFY(technical->text().contains(QStringLiteral("total ")));

    // A single selection shows that file's concrete values.
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    files->selectionModel()->select(files->model()->index(0, 0),
                                    QItemSelectionModel::ClearAndSelect |
                                        QItemSelectionModel::Rows);
    QTRY_VERIFY_WITH_TIMEOUT(technical->text().contains(QStringLiteral("FLAC")) &&
                                 technical->text().contains(QStringLiteral("44100 Hz")),
                             15'000);
    QVERIFY(technical->text().contains(QStringLiteral("16 bit")));
    QVERIFY(!technical->text().contains(QStringLiteral("mixed")));
    delete properties;
}

void BenchMainWindowTest::Tagger::theTaggerOpensOnCachedTagsAndReadsBehind() {
    QTemporaryDir media;
    const auto path = media.filePath(QStringLiteral("behind.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto raw_path = QFile::encodeName(path).toStdString();
    auto native = metadata::read_local_metadata(raw_path);
    QVERIFY(native);
    auto cached = native->document;
    for (auto& field : cached.fields) {
        field.native_name = field.canonical_name;
        field.provenance = metadata::FieldProvenance::cached_snapshot;
        if (field.canonical_name == "title") {
            field.values = {"As cached"};
        }
    }
    const metadata::StagedMetadataSource source{.raw_path = raw_path,
                                                .source_revision = {},
                                                .baseline = cached,
                                                .needs_metadata_capture = true};
    // The reads wait until let go, as a slow disk far away would.
    std::promise<void> release;
    auto released = release.get_future().share();
    bool let_go = false;
    const auto go = [&] {
        if (!let_go) {
            let_go = true;
            release.set_value();
        }
    };
    TaggerServices services;
    const auto local = services.tools.access;
    services.tools.access.read_many = [local, released](const std::vector<std::string>& paths,
                                                        const core::CancellationToken& token)
        -> core::Result<std::vector<core::Result<metadata::LocalMetadataRead>>> {
        released.wait();
        std::vector<core::Result<metadata::LocalMetadataRead>> read;
        for (const auto& each : paths) {
            read.push_back(local.read(each, token));
        }
        return read;
    };
    MetadataPropertiesDialog properties{
        1,
        [source](std::size_t) -> std::optional<MetadataPropertiesSource> {
            return MetadataPropertiesSource{.source = source,
                                            .track_label = QStringLiteral("Behind")};
        },
        {},
        std::move(services)};
    // Let go before the dialog is, whatever fails: it waits for its reads.
    const auto always = qScopeGuard(go);
    properties.show();
    MetadataGridModel* grid = nullptr;
    QTRY_VERIFY((grid = properties.findChild<MetadataGridModel*>()) != nullptr);
    auto* session = properties.findChild<TaggerSession*>();
    QVERIFY(session != nullptr);
    // Open before anything is read: the cached tags, provisional.
    QVERIFY(!grid->selection().source(0).source_revision);
    const auto cached_title = grid->selection().field_index("title");
    QVERIFY(cached_title);
    QCOMPARE(grid->selection().cell(0, *cached_title)->values,
             std::vector<std::string>{"As cached"});
    QVERIFY(!session->canTransform());
    QVERIFY(!session->canApply());
    QTRY_VERIFY(session->status().contains(QStringLiteral("Reading tags")));
    // A value edit is open at once.
    const std::vector<std::size_t> first{0U};
    QVERIFY(grid->replaceFieldValues(first, *cached_title, {"Typed meanwhile"}));

    go();
    QTRY_VERIFY(grid->selection().source(0).source_revision);
    const auto title = grid->selection().field_index("title");
    QVERIFY(title);
    // The file's own title now, not the cached one.
    QCOMPARE(grid->selection().cell(0, *title)->values.front(),
             native->document.first_effective_value("title").value_or(std::string{}));
    const auto* kept = grid->patches().patch(0, *title);
    QVERIFY(kept != nullptr);
    QCOMPARE(kept->values, std::vector<std::string>{"Typed meanwhile"});
    QVERIFY(session->status().contains(QStringLiteral("Tags read")));
    QTRY_VERIFY(session->canTransform());
}

void BenchMainWindowTest::Tagger::metadataRetrySkipsSavedFiles_data() {
    QTest::addColumn<bool>("change_source");
    QTest::addColumn<bool>("stopped");
    QTest::newRow("failed-then-saved") << false << false;
    QTest::newRow("stopped-then-saved") << false << true;
    QTest::newRow("changed-file-blocked") << true << false;
}

void BenchMainWindowTest::Tagger::metadataRetrySkipsSavedFiles() {
    QFETCH(bool, change_source);
    QFETCH(bool, stopped);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    std::vector<MetadataPropertiesSource> sources;
    std::vector<std::string> paths;
    for (int index = 0; index < 2; ++index) {
        const auto path = media.filePath(QStringLiteral("retry-%1.flac").arg(index));
        QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), path));
        paths.push_back(QFile::encodeName(path).toStdString());
        const auto read = metadata::read_local_metadata(paths.back());
        QVERIFY(read.has_value());
        sources.push_back({.source = {.raw_path = paths.back(),
                                      .source_revision = read->source_revision,
                                      .baseline = read->document},
                           .track_label = path});
    }
    std::array<int, 2> attempts{};
    std::vector<operations::MetadataApplyResult> outcomes;
    const auto database = std::filesystem::path{
        QFile::encodeName(media.filePath(QStringLiteral("retry.sqlite3"))).toStdString()};
    auto* dialog = new MetadataPropertiesDialog(
        sources.size(),
        [sources](std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return sources.at(index);
        },
        {},
        [&, database] {
            return MetadataWritePlanApplier{
                [&, database](const metadata::MetadataWritePlan& plan,
                              const operations::MetadataApplyProgressCallback& progress,
                              const core::CancellationToken& cancellation)
                    -> core::Result<operations::MetadataApplyResult> {
                    auto journal = persistence::SqliteMetadataOperationJournal::open(database);
                    if (!journal) {
                        return std::unexpected(journal.error());
                    }
                    return operations::apply_metadata_write_plan(
                        plan,
                        [&](const metadata::MetadataWritePlanSource& source,
                            const core::CancellationToken& token)
                            -> core::Result<operations::MetadataCommitResult> {
                            const auto index = source.raw_path == paths[0] ? 0U : 1U;
                            ++attempts[index];
                            if (index == 1U && attempts[index] == 1) {
                                return std::unexpected(
                                    core::Error{.code = stopped ? core::ErrorCode::cancelled
                                                                : core::ErrorCode::io,
                                                .message = "injected temporary failure",
                                                .context = {}});
                            }
                            return operations::commit_metadata_source(
                                source, *journal,
                                [](const operations::MetadataCommitResult&) -> core::Result<void> {
                                    return {};
                                },
                                token);
                        },
                        {}, {}, progress, cancellation,
                        operations::MetadataApplyOptions{.maximum_parallelism = 1U});
                }};
        },
        [&](const operations::MetadataApplyResult& result) { outcomes.push_back(result); });
    dialog->setArtworkMutationServices([] { return ArtworkWritePlanApplier{}; }, {});
    dialog->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = dialog->fileListView()) != nullptr);
    auto* grid = qobject_cast<MetadataGridModel*>(files->model());
    QVERIFY(grid != nullptr);
    const auto title = grid->ensureField(QStringLiteral("TITLE"));
    QVERIFY(title.has_value());
    const std::vector<std::size_t> indexes{0U, 1U};
    QVERIFY(grid->replaceFieldValues(indexes, title->field_index, {"Retried title"}));
    auto* sections = dialog->findChild<QTabWidget*>(QStringLiteral("bench-metadata-sections"));
    QVERIFY(sections != nullptr);
    sections->setCurrentIndex(1);
    auto* covers = dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    QVERIFY(covers != nullptr);
    QTRY_COMPARE(covers->model()->rowCount(), 1); // ADR-0348: one cover, one row
    covers->selectAll();
    auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-remove"));
    QVERIFY(remove != nullptr);
    QTRY_VERIFY(remove->isEnabled());
    remove->click();
    auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(apply != nullptr);
    QTRY_VERIFY(apply->isEnabled());
    apply->click();
    QTRY_COMPARE(outcomes.size(), 1U);
    QCOMPARE(outcomes.front().committed_source_count(), 1U);
    QFile first{QString::fromStdString(paths[0])};
    QVERIFY(first.open(QIODevice::ReadOnly));
    const auto saved_bytes = first.readAll();
    first.close();
    if (change_source) {
        QFile changed{QString::fromStdString(paths[1])};
        QVERIFY(changed.open(QIODevice::Append));
        QCOMPARE(changed.write("external-change"), 15);
        changed.close();
    }
    QPushButton* retry = nullptr;
    QTRY_VERIFY((retry = dialog->findChild<QPushButton*>(
                     QStringLiteral("bench-preparation-retry"))) != nullptr);
    QPointer guard{dialog};
    retry->click();
    QTRY_COMPARE(outcomes.size(), 2U);
    QCOMPARE(attempts[0], 1);
    QCOMPARE(attempts[1], 2);
    QVERIFY(first.open(QIODevice::ReadOnly));
    QCOMPARE(first.readAll(), saved_bytes);
    if (change_source) {
        QCOMPARE(outcomes.back().failed_source_count(), 1U);
        QCOMPARE(outcomes.back().sources.front().issue->code, core::ErrorCode::conflict);
        QVERIFY(!guard.isNull());
        delete dialog;
    } else {
        QCOMPARE(outcomes.back().committed_source_count(), 1U);
        QVERIFY(window_test::closeTaggerAfterWrite(guard));
        const auto art = metadata::read_local_artwork_inventory(paths[1]);
        QVERIFY(art && art->items.empty());
        const auto tags = metadata::read_local_metadata(paths[1]);
        QVERIFY(tags.has_value());
        QCOMPARE(tags->document.first_effective_value("title"),
                 std::optional<std::string>{"Retried title"});
    }
}

void BenchMainWindowTest::Tagger::metadataServiceSettingsAndCompactPages() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.findChild<LocalLibraryPanel*>());
    auto* action = window.findChild<QAction*>(QStringLiteral("action-settings"));
    QVERIFY(action);
    action->trigger();
    auto* dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    dialog->showPage(SettingsDialog::Page::metadata_services);
    auto* key = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-acoustid-key"));
    auto* reveal = dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-acoustid-show"));
    QVERIFY(key && reveal);
    auto* link = dialog->findChild<QLabel*>(QStringLiteral("bench-settings-acoustid-link"));
    QVERIFY(link && link->openExternalLinks());
    QVERIFY(link->text().contains(QStringLiteral("https://acoustid.org/new-application")));
    // ADR-0220: which engine serves the library and owns playback, settable
    // rather than hand-edited -- "is an engine in use" was otherwise
    // unanswerable from the UI.
    auto* engine_socket =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-engine-socket"));
    QVERIFY(engine_socket);
    // The remote engine: none, until one is named. This computer's engine
    // (the fixture's) is not configured here (ADR-0227).
    QVERIFY(engine_socket->text().isEmpty());
    // Nowhere an engine runs: Save connects to it, and the key below is
    // handed to whatever answers there.
    const auto elsewhere = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) +
                           QStringLiteral("/no-engine-here.sock");
    QVERIFY(!QFile::exists(elsewhere));
    engine_socket->setText(elsewhere);
    // ADR-0286: one password for every engine -- none of its own to give.
    QVERIFY(dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-engine-token")) ==
            nullptr);

    QCOMPARE(key->echoMode(), QLineEdit::Password);
    key->setText(QStringLiteral("  test-client-key  "));
    reveal->setChecked(true);
    QCOMPARE(key->echoMode(), QLineEdit::Normal);
    reveal->setChecked(false);
    dialog->activateWindow();
    QTRY_COMPARE(QApplication::activeWindow(), static_cast<QWidget*>(dialog));
    key->setFocus();
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(key));
    QTest::keyClick(key, Qt::Key_Tab);
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(reveal));
    auto* note = dialog->findChild<QLabel*>(QStringLiteral("bench-settings-save-note"));
    // Pages that wait for Save say nothing about it; the button does.
    QVERIFY(note->isHidden());
    auto* pages = dialog->findChild<QListWidget*>(QStringLiteral("bench-settings-pages"));
    auto* stack = dialog->findChild<QStackedWidget*>(QStringLiteral("bench-settings-stack"));
    QVERIFY(pages && stack);
    dialog->resize(720, 480);
    QCoreApplication::processEvents();
    QVERIFY(dialog->width() <= 720);
    QVERIFY(dialog->height() <= 480);
    for (int page = 0; page < pages->count(); ++page) {
        pages->setCurrentRow(page);
        auto* scroll = qobject_cast<QScrollArea*>(stack->currentWidget());
        QVERIFY(scroll && scroll->widgetResizable());
    }
    dialog->showPage(SettingsDialog::Page::naming);
    QVERIFY(note->text().contains(QStringLiteral("immediately")));
    QVERIFY(!note->isHidden());
    // Every page is titled, and a number field is not stretched across it.
    QCOMPARE(dialog->findChildren<QLabel*>(QStringLiteral("bench-settings-page-title")).size(),
             pages->count());
    auto* capacity = dialog->findChild<QSpinBox*>(QStringLiteral("bench-settings-buffer-capacity"));
    QVERIFY(capacity != nullptr && capacity->maximumWidth() <= 260);
    dialog->showPage(SettingsDialog::Page::metadata_services);
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/settings-metadata-small.png")));
    }
    QPointer<SettingsDialog> lifetime = dialog;
    dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    QTRY_VERIFY(lifetime.isNull());
    QVERIFY(!loadRemoteEngines().empty());
    QCOMPARE(loadRemoteEngines().front().address, elsewhere);
    QCOMPARE(QSettings{}.value(QLatin1String(SettingsDialog::acoustid_client_key)).toString(),
             QStringLiteral("test-client-key"));
    action->trigger();
    dialog = window.findChild<SettingsDialog*>();
    key = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-acoustid-key"));
    QCOMPARE(key->text(), QStringLiteral("test-client-key"));
    QCOMPARE(key->echoMode(), QLineEdit::Password);
    key->clear();
    lifetime = dialog;
    dialog->reject();
    QTRY_VERIFY(lifetime.isNull());
    QCOMPARE(QSettings{}.value(QLatin1String(SettingsDialog::acoustid_client_key)).toString(),
             QStringLiteral("test-client-key"));
    action->trigger();
    dialog = window.findChild<SettingsDialog*>();
    dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-acoustid-key"))->clear();
    lifetime = dialog;
    dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    QTRY_VERIFY(lifetime.isNull());
    QVERIFY(
        QSettings{}.value(QLatin1String(SettingsDialog::acoustid_client_key)).toString().isEmpty());
    // The old preamp shortcut opens the shared Settings page instead of a separate editor.
    window.findChild<QAction*>(QStringLiteral("action-local-replaygain-preamp"))->trigger();
    dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    pages = dialog->findChild<QListWidget*>(QStringLiteral("bench-settings-pages"));
    QCOMPARE(pages->currentItem()->text(), QStringLiteral("Playback"));
    QVERIFY(!window.findChild<QDialog*>(QStringLiteral("bench-rg-preamp-dialog")));
    dialog->reject();
}

// ADR-0332: files of several albums are listed under their albums' titles;
// a title selects its album's files, with Ctrl adds them.
void BenchMainWindowTest::Tagger::fileListShowsTheFilesUnderTheirAlbums() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    std::vector<MetadataPropertiesSource> sources;
    for (const auto& [album, file] : {std::pair{std::string{"Alpha"}, std::string{"01.flac"}},
                                      std::pair{std::string{"Alpha"}, std::string{"02.flac"}},
                                      std::pair{std::string{"Beta"}, std::string{"01.flac"}}}) {
        QVERIFY(QDir{media.path()}.mkpath(QString::fromStdString(album)));
        const auto name =
            QString::fromStdString(album) + QLatin1Char('/') + QString::fromStdString(file);
        const auto path = media.filePath(name);
        QVERIFY(materialize_audio_fixture(album == "Alpha"
                                              ? QStringLiteral("art-tone-flac.b64")
                                              : QStringLiteral("rich-metadata-flac.b64"),
                                          path));
        const auto encoded = QFile::encodeName(path);
        const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
        auto read = metadata::read_local_metadata(raw_path);
        QVERIFY(read.has_value());
        auto document = read->document;
        sources.push_back(MetadataPropertiesSource{
            .source = metadata::StagedMetadataSource{.raw_path = raw_path,
                                                     .source_revision = read->source_revision,
                                                     .baseline = std::move(document)},
            .track_label = name,
        });
    }
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {});
    properties->show();
    FileScopeView* files = nullptr;
    QTRY_VERIFY((files = dynamic_cast<FileScopeView*>(properties->fileListView())) != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(files->model()->rowCount(), 3, 10'000);
    QTRY_COMPARE_WITH_TIMEOUT(files->albumHeaders().size(), std::size_t{2U}, 10'000);
    // Each album by its title and artist.
    QCOMPARE(files->albumHeaders().at(0),
             QStringLiteral("Trackbench Fixtures · Trackknife Project"));
    QVERIFY(files->albumHeaders().at(2).startsWith(QStringLiteral("Rich Metadata · ")));
    const auto selected = [files] {
        std::set<int> rows;
        for (const auto& index : files->selectionModel()->selectedRows()) {
            rows.insert(index.row());
        }
        return rows;
    };
    const auto title_of = [files](const int row) {
        return files->visualRect(files->model()->index(row, 0)).topLeft() + QPoint{20, 4};
    };
    QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, title_of(2));
    QCOMPARE(selected(), (std::set<int>{2}));
    QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::ControlModifier, title_of(0));
    QCOMPARE(selected(), (std::set<int>{0, 1, 2}));
    QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, title_of(0));
    QCOMPARE(selected(), (std::set<int>{0, 1}));
    // A file, below its album's title, is still itself.
    const auto second = files->visualRect(files->model()->index(1, 0));
    QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint{second.center().x(), second.center().y()});
    QCOMPARE(selected(), (std::set<int>{1}));
    delete properties;
}

void BenchMainWindowTest::Tagger::richMetadataValuesAndIdentitiesSurviveListRestart() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto fixture_path = media.filePath(QStringLiteral("rich-metadata.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), fixture_path));
    const auto encoded = QFile::encodeName(fixture_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    {
        BenchMainWindow window;
        window.show();
        window.openLocalPaths({raw_path});
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_COMPARE(tabs->count(), 1);
        auto* model = qobject_cast<LocalListModel*>(
            qobject_cast<QTableView*>(tabs->currentWidget())->model());
        QVERIFY(model != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, 5'000);
        QTRY_VERIFY_WITH_TIMEOUT(model->rows().front().probed, 5'000);
        const auto& row = model->rows().front();
        QCOMPARE(row.title, std::string{"Metadata Fixture"});
        QCOMPARE(row.artist, std::string{"First Artist"});
        QCOMPARE(row.album, std::string{"Rich Metadata"});
        QCOMPARE(row.album_artist, std::string{"Album Credit"});
        QCOMPARE(row.metadata.effective_values("artist"),
                 (std::vector<std::string>{"First Artist", "Second Artist"}));
        QCOMPARE(row.metadata.effective_values("custom_field"),
                 (std::vector<std::string>{"first custom value", "second custom value"}));
        const auto identity = metadata::project_musicbrainz(row.metadata);
        QCOMPARE(identity.recording_ids,
                 (std::vector<std::string>{"11111111-1111-1111-1111-111111111111"}));
        QCOMPARE(identity.artist_ids,
                 (std::vector<std::string>{"55555555-5555-5555-5555-555555555555",
                                           "66666666-6666-6666-6666-666666666666"}));
        QVERIFY(row.source_revision.has_value());
        QCOMPARE(row.source_revision->size, 2'308U);

        // Simulate a workspace saved by the pre-ADR-0066 merge: FFmpeg's
        // generic `track` projection was cached beside TagLib's authoritative
        // native TRACKNUMBER field. Restart must discard only the redundant
        // stream projection and retain the embedded field.
        auto legacy_rows = model->rows();
        legacy_rows.front().metadata.fields.push_back(metadata::MetadataField{
            .canonical_name = "tracknumber",
            .native_name = "TRACKNUMBER",
            .values = {"3"},
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        });
        legacy_rows.front().metadata.fields.push_back(metadata::MetadataField{
            .canonical_name = "track",
            .native_name = "track",
            .values = {"3"},
            .qualifier = {},
            .provenance = metadata::FieldProvenance::stream,
        });
        legacy_rows.front().metadata.fields.push_back(metadata::MetadataField{
            .canonical_name = "album_artist",
            .native_name = "ALBUM_ARTIST",
            .values = {"Independent freeform value"},
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        });
        model->replaceRows(std::move(legacy_rows));
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
    QTRY_COMPARE(model->rowCount(), 1);
    const auto& row = model->rows().front();
    QCOMPARE(row.artist, std::string{"First Artist"});
    QCOMPARE(row.metadata.effective_values("artist"),
             (std::vector<std::string>{"First Artist", "Second Artist"}));
    QCOMPARE(row.metadata.effective_values("custom_field"),
             (std::vector<std::string>{"first custom value", "second custom value"}));
    QCOMPARE(metadata::project_musicbrainz(row.metadata).release_ids,
             (std::vector<std::string>{"33333333-3333-3333-3333-333333333333"}));
    QCOMPARE(row.metadata.effective_values("tracknumber"), (std::vector<std::string>{"3"}));
    QVERIFY(!row.metadata.effective_native_field("track").has_value());
    const auto freeform_album_artist = row.metadata.effective_native_field("ALBUM_ARTIST");
    QVERIFY(freeform_album_artist.has_value());
    QCOMPARE(freeform_album_artist->values,
             (std::vector<std::string>{"Independent freeform value"}));
    // The snapshot and revision are explicitly stale evidence. The write-plan
    // boundary observes the file again and compares this value before commit;
    // retaining it lets dependent-state replay distinguish an older queued
    // list save from a later external refresh.
    QVERIFY(row.source_revision.has_value());
    QCOMPARE(row.source_revision->size, 2'308U);
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::Tagger)

#include "window_tagger_test.moc"
