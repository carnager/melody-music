// SPDX-License-Identifier: GPL-3.0-only

// The window test: the tagger's edits: fields, patterns, transformations, selections.

#include "window_test.hpp"

#include "bench/file_scope_view.hpp"
#include "trackknife/core/engine_state.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "workspace/artwork_session.hpp"
#include "workspace/startup.hpp"
#include "workspace/tab_store.hpp"

#include <QCheckBox>

namespace trackknife::bench {

class BenchMainWindowTest::TaggerEdits final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void metadataGridDisplaysUnicodePaths();
    void metadataGridReusesExactNativeFieldWithoutInvalidIndexes();
    void metadataTransformationChainPreviewsAndStagesOneUndo();
    void metadataCapturePatternSavesReloadsAndStagesAllFields();
    void metadataSuggestionsStageSelectionConsistency();
    void automaticScriptsStageOnOpen();
    void metadataPropertiesFileSelectionDrivesIndividualAndBulkEdits();
    void metadataFieldReviewPreservesDraftAndSelectionScope();
};

void BenchMainWindowTest::TaggerEdits::metadataGridDisplaysUnicodePaths() {
    const std::vector<std::pair<std::string, QString>> cases{
        {"/music/Sangue Cássia/02-Lótus.flac",
         QStringLiteral("/music/Sangue Cássia/02-Lótus.flac")},
        {"/music/日本語/🎵.flac", QStringLiteral("/music/日本語/🎵.flac")},
        {"/music/Pétalas-" + std::string(1, static_cast<char>(0xFF)) + ".flac",
         QStringLiteral("/music/Pétalas-\\xFF.flac")},
        {"/music/\n\\xFF.flac", QStringLiteral("/music/\\x0A\\\\xFF.flac")},
        {"/music/" + std::string{"\xE2\x82"}, QStringLiteral("/music/\\xE2\\x82")},
        {"/music/" + std::string{"\xC0\xAF"}, QStringLiteral("/music/\\xC0\\xAF")},
        {"/music/\u202E.flac", QStringLiteral("/music/\\xE2\\x80\\xAE.flac")},
    };
    for (const auto& [raw_path, expected] : cases) {
        auto selection = metadata::StagedMetadataSelection::create({metadata::StagedMetadataSource{
            .raw_path = raw_path, .source_revision = std::nullopt, .baseline = {}}});
        QVERIFY(selection.has_value());
        MetadataGridModel grid{std::move(*selection), {}};
        QCOMPARE(grid.data(grid.index(0, 0), Qt::DisplayRole).toString(), expected);
        QCOMPARE(grid.data(grid.index(0, 0), Qt::ToolTipRole).toString(), expected);
        QCOMPARE(grid.trackLabel(0), expected);
    }
}

void BenchMainWindowTest::TaggerEdits::metadataGridReusesExactNativeFieldWithoutInvalidIndexes() {
    std::vector<metadata::StagedMetadataSource> sources;
    sources.reserve(20U);
    for (std::size_t item = 0U; item < 20U; ++item) {
        metadata::MetadataDocument document;
        document.fields.reserve(39U);
        for (std::size_t field = 0U; field < 39U; ++field) {
            const auto name = field == 38U ? std::string{"TEST"} : "FIELD_" + std::to_string(field);
            document.fields.push_back(metadata::MetadataField{
                .canonical_name = metadata::canonicalize_field_name(name),
                .native_name = name,
                .values = {"value"},
                .qualifier = {},
                .provenance = metadata::FieldProvenance::embedded,
            });
        }
        sources.push_back(metadata::StagedMetadataSource{
            .raw_path = "/music/" + std::to_string(item) + ".flac",
            .source_revision = std::nullopt,
            .baseline = std::move(document),
        });
    }
    auto selection = metadata::StagedMetadataSelection::create(std::move(sources));
    QVERIFY(selection.has_value());
    QCOMPARE(selection->item_count(), std::size_t{20U});
    QCOMPARE(selection->field_count(), std::size_t{39U});

    MetadataGridModel grid{std::move(*selection), {}};
    MetadataAggregateModel aggregate{&grid};
    QTableView view;
    view.setModel(&grid);
    view.setCurrentIndex(grid.index(19, 39));
    QPersistentModelIndex current{view.currentIndex()};
    QVERIFY(current.isValid());

    QSignalSpy about_to_insert{&grid, &QAbstractItemModel::columnsAboutToBeInserted};
    QSignalSpy inserted_columns{&grid, &QAbstractItemModel::columnsInserted};
    const auto inserted = aggregate.ensureField(QStringLiteral("test"));
    QVERIFY(inserted.has_value());
    QCOMPARE(*inserted, 38);
    QCOMPARE(grid.rowCount(), 20);
    QCOMPARE(grid.columnCount(), 40);
    QCOMPARE(aggregate.rowCount(), 39);
    QCOMPARE(about_to_insert.count(), 0);
    QCOMPARE(inserted_columns.count(), 0);
    QVERIFY(current.isValid());
    QCOMPARE(current.row(), 19);
    QCOMPARE(current.column(), 39);
}

void BenchMainWindowTest::TaggerEdits::metadataTransformationChainPreviewsAndStagesOneUndo() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("transform.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const auto title = std::ranges::find(read->document.fields, std::string_view{"title"},
                                         &metadata::MetadataField::canonical_name);
    QVERIFY(title != read->document.fields.end());
    title->values = {"chain Title"};
    read->document.fields.push_back(metadata::MetadataField{
        .canonical_name = "date",
        .native_name = "DATE",
        .values = {"2024-08-30"},
        .qualifier = {},
        .provenance = metadata::FieldProvenance::embedded,
    });

    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = raw_path,
                .source_revision = read->source_revision,
                .baseline = read->document,
            },
        .track_label = QStringLiteral("Transformation fixture"),
    };
    std::vector<persistence::SavedMetadataTransformationChain> saved_chains;
    MetadataTransformationStore transformation_store{
        .load =
            [&saved_chains](MetadataTransformationStore::LoadCompletion completion) {
                completion(saved_chains, {});
            },
        .save =
            [&saved_chains](persistence::SavedMetadataTransformationChain chain,
                            MetadataTransformationStore::Completion completion) {
                const auto found = std::ranges::find(
                    saved_chains, chain.id, &persistence::SavedMetadataTransformationChain::id);
                if (found == saved_chains.end()) {
                    saved_chains.push_back(std::move(chain));
                } else {
                    *found = std::move(chain);
                }
                completion({});
            },
        .remove =
            [&saved_chains](core::StableId id, MetadataTransformationStore::Completion completion) {
                std::erase_if(saved_chains, [id](const auto& chain) { return chain.id == id; });
                completion({});
            },
    };
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, transformation_store);
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* transform =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-transform"));
    auto* undo = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-undo"));
    auto* redo = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-redo"));
    auto* write_plan =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(transform != nullptr);
    QVERIFY(undo != nullptr);
    QVERIFY(redo != nullptr);
    QVERIFY(write_plan != nullptr);
    auto* empty_script_list =
        properties->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-list"));
    auto* empty_script_status =
        properties->findChild<QLabel*>(QStringLiteral("bench-metadata-transformation-status"));
    QVERIFY(empty_script_list != nullptr);
    QVERIFY(empty_script_status != nullptr);
    QTRY_COMPARE(empty_script_list->count(), 0);
    QCOMPARE(empty_script_list->property("bench-empty-state-text").toString(),
             QStringLiteral("No saved scripts yet"));
    QTRY_VERIFY(empty_script_status->text().isEmpty());
    QVERIFY(!empty_script_status->isVisible());
    QTRY_VERIFY(transform->isEnabled());
    QTest::mouseClick(transform, Qt::LeftButton);

    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-transformation"))) != nullptr);
    auto* kind =
        dialog->findChild<QComboBox*>(QStringLiteral("bench-metadata-transformation-kind"));
    auto* target =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-metadata-transformation-target"));
    auto* target_completer = dialog->findChild<QCompleter*>(
        QStringLiteral("bench-metadata-transformation-target-completer"));
    auto* input =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-metadata-transformation-input"));
    auto* number_start =
        dialog->findChild<QSpinBox*>(QStringLiteral("bench-metadata-transformation-number-start"));
    auto* number_padding = dialog->findChild<QSpinBox*>(
        QStringLiteral("bench-metadata-transformation-number-padding"));
    auto* character_count = dialog->findChild<QSpinBox*>(
        QStringLiteral("bench-metadata-transformation-character-count"));
    auto* add =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-add"));
    auto* import_script = dialog->findChild<QPushButton*>(
        QStringLiteral("bench-metadata-transformation-import-script"));
    auto* import_native = dialog->findChild<QPushButton*>(
        QStringLiteral("bench-metadata-transformation-import-native"));
    auto* export_native = dialog->findChild<QPushButton*>(
        QStringLiteral("bench-metadata-transformation-export-native"));
    auto* steps =
        dialog->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-steps"));
    auto* remove =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-remove"));
    auto* stage =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-stage"));
    auto* preview_table =
        dialog->findChild<QTreeView*>(QStringLiteral("bench-metadata-transformation-table"));
    auto* preview_summary =
        dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-transformation-summary"));
    QVERIFY(kind != nullptr);
    QVERIFY(target != nullptr);
    QVERIFY(target_completer != nullptr);
    QVERIFY(input != nullptr);
    QVERIFY(number_start != nullptr);
    QVERIFY(number_padding != nullptr);
    QVERIFY(character_count != nullptr);
    QVERIFY(add != nullptr);
    QVERIFY(import_script != nullptr);
    QVERIFY(import_native != nullptr);
    QVERIFY(export_native != nullptr);
    QVERIFY(import_native->isEnabled());
    QVERIFY(!export_native->isEnabled());
    QVERIFY(steps != nullptr);
    QVERIFY(remove != nullptr);
    QVERIFY(stage != nullptr);
    QVERIFY(preview_table != nullptr);
    QVERIFY(preview_summary != nullptr);
    // 20 step kinds under 4 unselectable group headers; kinds are found by
    // name because the row index no longer matches the action kind.
    QCOMPARE(kind->count(), 24);
    for (const auto& kind_name : {QStringLiteral("Capitalize first character"),
                                  QStringLiteral("Convert rating to FMPS_RATING"),
                                  QStringLiteral("Remove exact matching values"),
                                  QStringLiteral("Replace exact matching values"),
                                  QStringLiteral("Number by selected-file order"),
                                  QStringLiteral("Keep first characters of each value"),
                                  QStringLiteral("Remove field when condition matches"),
                                  QStringLiteral("Remove listed fields (blocklist)"),
                                  QStringLiteral("Keep only listed fields (allowlist)"),
                                  QStringLiteral("Capture fields with tkcapture-1")}) {
        QVERIFY2(kind->findText(kind_name) >= 0, qPrintable(kind_name));
    }
    QVERIFY(!kind->model()->flags(kind->model()->index(0, 0)).testFlag(Qt::ItemIsSelectable));

    // The field-filter Fields box completes each comma-separated name from
    // the present fields plus the standard conventional and MusicBrainz
    // catalog, while custom names stay freely typable.
    auto* fields_completions = dialog->findChild<QStringListModel*>(
        QStringLiteral("bench-metadata-transformation-fields-completions"));
    QVERIFY(fields_completions != nullptr);
    kind->setCurrentIndex(kind->findText(QStringLiteral("Remove listed fields (blocklist)")));
    input->setFocus(Qt::OtherFocusReason);
    input->clear();
    QTest::keyClicks(input, QStringLiteral("musicb"));
    // The selection's present spelling deduplicates the catalog entry and
    // wins the suggestion, so the box reflects the files being edited.
    QVERIFY2(fields_completions->stringList().contains(QStringLiteral("MUSICBRAINZ_TRACKID")),
             qPrintable(fields_completions->stringList().join(QStringLiteral(" | "))));
    input->clear();
    QTest::keyClicks(input, QStringLiteral("Custom Field, gen"));
    QVERIFY(fields_completions->stringList().contains(QStringLiteral("Genre")));
    QMetaObject::invokeMethod(dialog->findChild<QCompleter*>(
                                  QStringLiteral("bench-metadata-transformation-fields-completer")),
                              "activated", Qt::DirectConnection,
                              Q_ARG(QString, QStringLiteral("Genre")));
    QCOMPARE(input->text(), QStringLiteral("Custom Field, Genre"));
    input->clear();
    kind->setCurrentIndex(kind->findText(QStringLiteral("Capitalize first character")));

    QTimer::singleShot(0, dialog, [dialog] {
        auto* importer =
            dialog->findChild<QDialog*>(QStringLiteral("bench-metadata-rule-script-import"));
        QVERIFY(importer != nullptr);
        auto* source_edit = importer->findChild<QPlainTextEdit*>(
            QStringLiteral("bench-metadata-rule-script-source"));
        auto* diagnostics = importer->findChild<QPlainTextEdit*>(
            QStringLiteral("bench-metadata-rule-script-diagnostics"));
        auto* replace =
            importer->findChild<QPushButton*>(QStringLiteral("bench-metadata-rule-script-replace"));
        QVERIFY(source_edit != nullptr);
        QVERIFY(diagnostics != nullptr);
        QVERIFY(replace != nullptr);
        source_edit->setPlainText(QStringLiteral(
            "$delete(comment:)\n"
            "$if($or($not(%totaldiscs%),$eq(%totaldiscs%,1)),"
            "$delete(discnumber)$delete(totaldiscs))\n"
            "$if(%originaldate%,$set(date,$left(%originaldate%,4))"
            "$set(originaldate,$left(%originaldate%,4)),$set(date,$left(%date%,4)))"));
        QVERIFY(replace->isEnabled());
        QVERIFY(diagnostics->toPlainText().contains(QStringLiteral("5 generated rules")));
        QTest::mouseClick(replace, Qt::LeftButton);
    });
    QTest::mouseClick(import_script, Qt::LeftButton);
    QCOMPARE(steps->count(), 5);
    QVERIFY(steps->item(0)->text().contains(QStringLiteral("Remove exact native field comment")));
    QVERIFY(steps->item(1)->text().contains(
        QStringLiteral("Remove exact native field discnumber when "
                       "$or($not(%totaldiscs%),$eq(%totaldiscs%,1))")));
    QVERIFY(steps->item(2)->text().contains(
        QStringLiteral("Remove exact native field totaldiscs when "
                       "$or($not(%totaldiscs%),$eq(%totaldiscs%,1))")));
    QVERIFY(steps->item(3)->text().contains(QStringLiteral("Format date as $if(%originaldate%")));
    QVERIFY(steps->item(4)->text().contains(
        QStringLiteral("Keep the first 4 characters of each value of originaldate")));
    auto* editor_tabs =
        dialog->findChild<QTabWidget*>(QStringLiteral("bench-metadata-transformation-editor-tabs"));
    auto* raw_source = dialog->findChild<QPlainTextEdit*>(
        QStringLiteral("bench-metadata-transformation-raw-source"));
    auto* raw_diagnostics = dialog->findChild<QPlainTextEdit*>(
        QStringLiteral("bench-metadata-transformation-raw-diagnostics"));
    QVERIFY(editor_tabs != nullptr);
    QVERIFY(raw_source != nullptr);
    QVERIFY(raw_diagnostics != nullptr);
    QCOMPARE(editor_tabs->tabText(1), QStringLiteral("Raw script"));
    QVERIFY(raw_source->toPlainText().contains(
        QStringLiteral("$or($not(%totaldiscs%),$eq(%totaldiscs%,1))")));
    auto revised_source = raw_source->toPlainText();
    revised_source.replace(QStringLiteral("$or($not(%totaldiscs%),$eq(%totaldiscs%,1))"),
                           QStringLiteral("$eq(%totaldiscs%,1)"));
    raw_source->setPlainText(revised_source);
    QTRY_VERIFY(raw_diagnostics->toPlainText().contains(QStringLiteral("Ready")));
    QVERIFY(steps->item(1)->text().contains(QStringLiteral("$eq(%totaldiscs%,1)")));
    QVERIFY(!steps->item(1)->text().contains(QStringLiteral("$not(%totaldiscs%)")));
    QVERIFY(dialog->isWindowModified());
    QTimer::singleShot(0, dialog, [] {
        auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        QVERIFY(confirmation != nullptr);
        auto* cancel = confirmation->button(QMessageBox::Cancel);
        QVERIFY(cancel != nullptr);
        QTest::mouseClick(cancel, Qt::LeftButton);
    });
    dialog->close();
    QVERIFY(dialog->isVisible());
    auto* save =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-save"));
    QVERIFY(save != nullptr);
    QVERIFY(save->isEnabled());
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(saved_chains.size(), std::size_t{1U});
    const auto* saved_conditional =
        std::get_if<metadata::MetadataRemoveFieldIfAction>(&saved_chains.front().chain.actions[1]);
    QVERIFY(saved_conditional != nullptr);
    QCOMPARE(saved_conditional->condition, std::string{"$eq(%totaldiscs%,1)"});
    QVERIFY(!dialog->isWindowModified());
    for (auto count = 0; count < 5; ++count) {
        QTest::mouseClick(remove, Qt::LeftButton);
    }
    QCOMPARE(steps->count(), 0);

    target->setText(QStringLiteral("custom"));
    QTRY_VERIFY(target_completer->model()->rowCount() > 0);
    QCOMPARE(target_completer->model()->index(0, 0).data().toString(),
             QStringLiteral("CUSTOM_FIELD"));
    target->clear();

    kind->setCurrentIndex(kind->findText(QStringLiteral("Capitalize first character")));
    target->setText(QStringLiteral("Album Artist"));
    QTest::mouseClick(add, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(preview_table->model() != nullptr, 5'000);
    QCOMPARE(preview_table->model()->rowCount(), 0);
    QTRY_VERIFY(preview_summary->text().contains(
        QStringLiteral("every existing Album Artist value already starts with its uppercase "
                       "form")));

    kind->setCurrentIndex(kind->findText(QStringLiteral("Capitalize first character")));
    target->setText(QStringLiteral("Title"));
    QTest::mouseClick(add, Qt::LeftButton);
    kind->setCurrentIndex(kind->findText(QStringLiteral("Format with tkfmt-1")));
    target->setText(QStringLiteral("Comment"));
    input->setText(QStringLiteral("%artist% — %title%"));
    QTest::mouseClick(add, Qt::LeftButton);
    kind->setCurrentIndex(kind->findText(QStringLiteral("Number by selected-file order")));
    target->setText(QStringLiteral("Track Number"));
    number_start->setValue(7);
    number_padding->setValue(2);
    QTest::mouseClick(add, Qt::LeftButton);
    kind->setCurrentIndex(kind->findText(QStringLiteral("Keep first characters of each value")));
    target->setText(QStringLiteral("Date"));
    QCOMPARE(character_count->value(), 4);
    QTest::mouseClick(add, Qt::LeftButton);

    auto* saved =
        dialog->findChild<QComboBox*>(QStringLiteral("bench-metadata-transformation-saved"));
    QVERIFY(saved != nullptr);
    QVERIFY(save->isEnabled());
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(saved_chains.size(), std::size_t{1U});
    QCOMPARE(saved->count(), 2);
    QCOMPARE(saved->currentIndex(), 1);

    QVERIFY(export_native->isEnabled());
    dialog->close();
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-transformation")) ==
                nullptr);
    auto* script_panel =
        properties->findChild<QWidget*>(QStringLiteral("bench-metadata-side-panel"));
    auto* script_list =
        properties->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-list"));
    auto* script_status =
        properties->findChild<QLabel*>(QStringLiteral("bench-metadata-transformation-status"));
    QVERIFY(script_panel != nullptr);
    QVERIFY(script_list != nullptr);
    QVERIFY(script_status != nullptr);
    QTRY_COMPARE(script_list->count(), 1);
    QCOMPARE(script_list->currentRow(), 0);
    QCOMPARE(script_list->item(0)->checkState(), Qt::Unchecked);

    QTRY_VERIFY(transform->isEnabled());
    QCOMPARE(transform->text(), QStringLiteral("Edit selected script…"));
    QTest::mouseClick(transform, Qt::LeftButton);
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-transformation"))) != nullptr);
    saved = dialog->findChild<QComboBox*>(QStringLiteral("bench-metadata-transformation-saved"));
    steps = dialog->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-steps"));
    stage = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-stage"));
    preview_table =
        dialog->findChild<QTreeView*>(QStringLiteral("bench-metadata-transformation-table"));
    preview_summary =
        dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-transformation-summary"));
    QVERIFY(saved != nullptr);
    QVERIFY(steps != nullptr);
    QVERIFY(stage != nullptr);
    QVERIFY(preview_table != nullptr);
    QVERIFY(preview_summary != nullptr);
    QCOMPARE(saved->count(), 2);
    QCOMPARE(saved->currentIndex(), 1);
    QCOMPARE(steps->count(), 5);
    QVERIFY(steps->item(4)->text().contains(
        QStringLiteral("Keep the first 4 characters of each value of Date")));
    QTRY_VERIFY_WITH_TIMEOUT(preview_table->model() != nullptr, 5'000);
    QTRY_COMPARE(preview_table->model()->rowCount(), 4);
    QCOMPARE(preview_table->model()->headerData(0, Qt::Horizontal).toString(),
             QStringLiteral("Field"));
    QCOMPARE(preview_table->model()->headerData(1, Qt::Horizontal).toString(),
             QStringLiteral("Old"));
    QCOMPARE(preview_table->model()->headerData(2, Qt::Horizontal).toString(),
             QStringLiteral("New"));
    QCOMPARE(preview_table->model()->index(0, 0).data().toString(), QStringLiteral("Title"));
    QCOMPARE(preview_table->model()->index(0, 2).data().toString(), QStringLiteral("Chain Title"));
    QCOMPARE(preview_table->model()->index(1, 0).data().toString(), QStringLiteral("Comment"));
    QCOMPARE(preview_table->model()->index(1, 2).data().toString(),
             QStringLiteral("First Artist; Second Artist — Chain Title"));
    QCOMPARE(preview_table->model()->index(2, 0).data().toString(), QStringLiteral("Track Number"));
    QCOMPARE(preview_table->model()->index(2, 2).data().toString(), QStringLiteral("07"));
    QCOMPARE(preview_table->model()->index(3, 0).data().toString(), QStringLiteral("Date"));
    QCOMPARE(preview_table->model()->index(3, 1).data().toString(), QStringLiteral("2024-08-30"));
    QCOMPARE(preview_table->model()->index(3, 2).data().toString(), QStringLiteral("2024"));
    const auto first_change = preview_table->model()->index(0, 0);
    QCOMPARE(preview_table->model()->rowCount(first_change), 1);
    QCOMPARE(preview_table->model()->index(0, 0, first_change).data().toString(),
             QStringLiteral("File"));
    QVERIFY(preview_table->model()
                ->index(0, 2, first_change)
                .data()
                .toString()
                .contains(QStringLiteral("step 2")));
    QVERIFY(preview_summary->text().contains(QStringLiteral("4 final cell changes")));
    QVERIFY(stage->isEnabled());
    QTest::mouseClick(stage, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-transformation")) ==
                nullptr);

    const auto title_column = grid_model->fieldColumn(QStringLiteral("title"));
    const auto comment_column = grid_model->fieldColumn(QStringLiteral("comment"));
    const auto track_number_column = grid_model->fieldColumn(QStringLiteral("track number"));
    const auto date_column = grid_model->fieldColumn(QStringLiteral("date"));
    QVERIFY(title_column.has_value());
    QVERIFY(comment_column.has_value());
    QVERIFY(track_number_column.has_value());
    QVERIFY(date_column.has_value());
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{4U});
    QCOMPARE(grid_model->index(0, *title_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Chain Title")}));
    QCOMPARE(grid_model->index(0, *comment_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("First Artist; Second Artist — Chain Title")}));
    QCOMPARE(
        grid_model->index(0, *track_number_column).data(metadata_cell_values_role).toStringList(),
        (QStringList{QStringLiteral("07")}));
    QCOMPARE(grid_model->index(0, *date_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("2024")}));

    QTRY_VERIFY(undo->isEnabled());
    QTest::mouseClick(undo, Qt::LeftButton);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{0U});
    QTRY_VERIFY(redo->isEnabled());
    QTest::mouseClick(redo, Qt::LeftButton);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{4U});

    QTest::mouseClick(undo, Qt::LeftButton);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{0U});
    const auto album_column = grid_model->fieldColumn(QStringLiteral("album"));
    QVERIFY(album_column.has_value());
    const std::array automatic_items{std::size_t{0U}};
    QVERIFY(grid_model->replaceFieldValues(
        automatic_items, static_cast<std::size_t>(*album_column - 1), {"Automatic workflow"}));
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{1U});

    QTRY_VERIFY(write_plan->isEnabled());
    QTest::mouseClick(write_plan, Qt::LeftButton);
    // Direct apply plans in the background; this fixture wires no applier, so
    // the ready plan stops at the unavailable notice and the draft survives.
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(status != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("Apply is unavailable")),
                             5'000);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{1U});
    QTRY_VERIFY(write_plan->isEnabled());
    QTest::mouseClick(write_plan, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("Apply is unavailable")),
                             5'000);
    QTRY_VERIFY(write_plan->isEnabled());
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{1U});

    // Checking a script stages its edits immediately as one more colored
    // draft transaction — Apply stays WYSIWYG, nothing runs hidden at write
    // time.
    script_list->item(0)->setCheckState(Qt::Checked);
    QTRY_VERIFY(saved_chains.front().automatic);
    QTRY_VERIFY(script_status->text().contains(QStringLiteral("1 of 1 checked")));
    QTRY_COMPARE_WITH_TIMEOUT(grid_model->patches().patch_count(), std::size_t{5U}, 5'000);
    // The sticky footer summary names the script and offers one-click undo.
    QTRY_VERIFY(status->text().contains(QStringLiteral("staged 4 edits")));
    QVERIFY(status->text().contains(QStringLiteral("undo-automatic")));
    QCOMPARE(grid_model->index(0, *date_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("2024")}));
    // Script edits speak in italics and name their source; the hand edit
    // stays upright and unattributed.
    const auto date_index = grid_model->index(0, *date_column);
    QVERIFY(date_index.data(metadata_cell_staged_source_role)
                .toString()
                .contains(QStringLiteral("step")));
    QVERIFY(date_index.data(Qt::FontRole).value<QFont>().italic());
    QVERIFY(date_index.data(Qt::ToolTipRole).toString().contains(QStringLiteral("Staged by")));
    const auto album_index = grid_model->index(0, *album_column);
    QVERIFY(album_index.data(metadata_cell_staged_source_role).toString().isEmpty());
    QVERIFY(!album_index.data(Qt::FontRole).value<QFont>().italic());
    QTRY_VERIFY(undo->isEnabled());
    QTest::mouseClick(undo, Qt::LeftButton);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{1U});
    delete properties;
}

void BenchMainWindowTest::TaggerEdits::metadataCapturePatternSavesReloadsAndStagesAllFields() {
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = "/library/Alpha/Album/03. Song.flac",
                .source_revision = std::nullopt,
                .baseline = metadata::MetadataDocument{},
            },
        .track_label = QStringLiteral("Capture fixture"),
    };
    std::vector<persistence::SavedMetadataTransformationChain> saved_chains;
    MetadataTransformationStore store{
        .load =
            [&saved_chains](MetadataTransformationStore::LoadCompletion completion) {
                completion(saved_chains, {});
            },
        .save =
            [&saved_chains](persistence::SavedMetadataTransformationChain chain,
                            MetadataTransformationStore::Completion completion) {
                const auto found = std::ranges::find(
                    saved_chains, chain.id, &persistence::SavedMetadataTransformationChain::id);
                if (found == saved_chains.end()) {
                    saved_chains.push_back(std::move(chain));
                } else {
                    *found = std::move(chain);
                }
                completion({});
            },
        .remove = {},
    };
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, store);
    properties->show();
    auto* transform =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-transform"));
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    QVERIFY(transform != nullptr);
    QVERIFY(grid_model != nullptr);
    QTRY_VERIFY(transform->isEnabled());
    QTest::mouseClick(transform, Qt::LeftButton);

    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-transformation"))) != nullptr);
    auto* kind =
        dialog->findChild<QComboBox*>(QStringLiteral("bench-metadata-transformation-kind"));
    auto* source_kind = dialog->findChild<QComboBox*>(
        QStringLiteral("bench-metadata-transformation-capture-source"));
    auto* source_argument = dialog->findChild<QLineEdit*>(
        QStringLiteral("bench-metadata-transformation-capture-argument"));
    auto* input =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-metadata-transformation-input"));
    auto* add =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-add"));
    auto* steps =
        dialog->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-steps"));
    auto* save_as = dialog->findChild<QPushButton*>(
        QStringLiteral("bench-metadata-transformation-save-as-new"));
    QVERIFY(kind != nullptr);
    QVERIFY(source_kind != nullptr);
    QVERIFY(source_argument != nullptr);
    QVERIFY(input != nullptr);
    QVERIFY(add != nullptr);
    QVERIFY(steps != nullptr);
    QVERIFY(save_as != nullptr);
    kind->setCurrentIndex(kind->findText(QStringLiteral("Capture fields with tkcapture-1")));
    QCOMPARE(source_kind->count(), 4);
    source_kind->setCurrentIndex(0);
    QVERIFY(!source_argument->isVisible());
    input->setText(QStringLiteral("%artist%/%album%/%tracknumber%. %title%"));
    QTest::mouseClick(add, Qt::LeftButton);
    QCOMPARE(steps->count(), 1);
    QVERIFY(steps->item(0)->text().contains(QStringLiteral("Capture filename with")));
    QTest::mouseClick(save_as, Qt::LeftButton);
    QCOMPARE(saved_chains.size(), std::size_t{1U});
    const auto* saved_capture = std::get_if<metadata::MetadataCaptureValuesAction>(
        &saved_chains.front().chain.actions.front());
    QVERIFY(saved_capture != nullptr);
    QCOMPARE(saved_capture->pattern, std::string{"%artist%/%album%/%tracknumber%. %title%"});

    dialog->close();
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-transformation")) ==
                nullptr);
    auto* script_list =
        properties->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-list"));
    QVERIFY(script_list != nullptr);
    QTRY_COMPARE(script_list->count(), 1);
    script_list->setCurrentRow(0);
    QTRY_VERIFY(transform->isEnabled());
    QTest::mouseClick(transform, Qt::LeftButton);
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-transformation"))) != nullptr);
    steps = dialog->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-steps"));
    auto* stage =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-stage"));
    auto* table =
        dialog->findChild<QTreeView*>(QStringLiteral("bench-metadata-transformation-table"));
    QVERIFY(steps != nullptr);
    QVERIFY(stage != nullptr);
    QVERIFY(table != nullptr);
    QCOMPARE(steps->count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(table->model() != nullptr, 5'000);
    QCOMPARE(table->model()->rowCount(), 4);
    QStringList fields;
    for (auto row = 0; row < table->model()->rowCount(); ++row) {
        fields.push_back(table->model()->index(row, 0).data().toString().toCaseFolded());
    }
    QCOMPARE(fields, (QStringList{QStringLiteral("artist"), QStringLiteral("album"),
                                  QStringLiteral("tracknumber"), QStringLiteral("title")}));
    QVERIFY(stage->isEnabled());
    QTest::mouseClick(stage, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-transformation")) ==
                nullptr);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{4U});
    const auto artist = grid_model->fieldColumn(QStringLiteral("artist"));
    const auto album = grid_model->fieldColumn(QStringLiteral("album"));
    const auto track = grid_model->fieldColumn(QStringLiteral("tracknumber"));
    const auto title = grid_model->fieldColumn(QStringLiteral("title"));
    QVERIFY(artist && album && track && title);
    QCOMPARE(grid_model->index(0, *artist).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("Alpha")});
    QCOMPARE(grid_model->index(0, *album).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("Album")});
    QCOMPARE(grid_model->index(0, *track).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("03")});
    QCOMPARE(grid_model->index(0, *title).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("Song")});
    delete properties;
}

void BenchMainWindowTest::TaggerEdits::metadataSuggestionsStageSelectionConsistency() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto make_source = [&field](const QString& label, const QString& track_number) {
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = "/music/" + label.toStdString() + ".flac",
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = {field("ALBUM", {"Alpha"}), field("ARTIST", {"Band"}),
                                       field("TRACKNUMBER", {track_number.toStdString()})},
                            .unsupported_native_objects = {},
                        },
                },
            .track_label = label,
        };
    };
    const std::vector sources{make_source(QStringLiteral("one"), QStringLiteral("1")),
                              make_source(QStringLiteral("two"), QStringLiteral("2")),
                              make_source(QStringLiteral("three"), QStringLiteral("3"))};

    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {});
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* suggest = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-suggest"));
    auto* undo = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-undo"));
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(suggest != nullptr);
    QVERIFY(undo != nullptr);
    QVERIFY(status != nullptr);
    files->selectAll();
    QTRY_VERIFY(suggest->isEnabled());
    suggest->click();

    // The internal consistency provider stages album artist and total tracks
    // for every file as one ordinary colored draft transaction.
    QTRY_COMPARE_WITH_TIMEOUT(grid_model->patches().patch_count(), std::size_t{6U}, 5'000);
    QVERIFY(status->text().contains(QStringLiteral("6 suggestions")));
    QVERIFY(status->text().contains(QStringLiteral("Selection consistency")));
    QVERIFY(aggregate_model->fieldRow(QStringLiteral("Album Artist")).has_value());
    QVERIFY(aggregate_model->fieldRow(QStringLiteral("Total Tracks")).has_value());
    const auto album_artist_column = grid_model->fieldColumn(QStringLiteral("Album Artist"));
    const auto totals_column = grid_model->fieldColumn(QStringLiteral("Total Tracks"));
    QVERIFY(album_artist_column.has_value());
    QVERIFY(totals_column.has_value());
    for (int row = 0; row < grid_model->rowCount(); ++row) {
        QCOMPARE(grid_model->index(row, *album_artist_column)
                     .data(metadata_cell_values_role)
                     .toStringList(),
                 QStringList{QStringLiteral("Band")});
        QCOMPARE(
            grid_model->index(row, *totals_column).data(metadata_cell_values_role).toStringList(),
            QStringList{QStringLiteral("3")});
    }

    // One undo removes the whole suggestion transaction.
    QTRY_VERIFY(undo->isEnabled());
    QTest::mouseClick(undo, Qt::LeftButton);
    QTRY_COMPARE(grid_model->patches().patch_count(), std::size_t{0U});

    // A second run restages; files that already agree produce nothing new.
    // Undoing the staged field extension resets the grid, so reselect first.
    files->selectAll();
    QTRY_VERIFY(suggest->isEnabled());
    QTest::mouseClick(suggest, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(grid_model->patches().patch_count(), std::size_t{6U}, 5'000);
    files->selectAll();
    QTRY_VERIFY(suggest->isEnabled());
    QTest::mouseClick(suggest, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("already agree")), 5'000);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{6U});
    delete properties;
}

void BenchMainWindowTest::TaggerEdits::automaticScriptsStageOnOpen() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = "/music/open-stage.flac",
                .source_revision = std::nullopt,
                .baseline =
                    metadata::MetadataDocument{
                        .fields = {field("TITLE", {"Song"}), field("ARTIST", {"Band"}),
                                   field("TOTALTRACKS", {"9"})},
                        .unsupported_native_objects = {},
                    },
            },
        .track_label = QStringLiteral("Open staging fixture"),
    };
    std::vector<persistence::SavedMetadataTransformationChain> saved_chains{
        persistence::SavedMetadataTransformationChain{
            .id = core::StableId::random(),
            .chain =
                metadata::MetadataTransformationChain{
                    .schema_version = 1U,
                    .name = "Library cleanup",
                    .actions = {metadata::MetadataRemoveFieldAction{
                        .target_field = "totaltracks",
                        .match_mode = metadata::MetadataFieldMatchMode::logical,
                    }},
                },
            .automatic = true,
        }};
    MetadataTransformationStore transformation_store{
        .load =
            [&saved_chains](MetadataTransformationStore::LoadCompletion completion) {
                completion(saved_chains, {});
            },
        .save = [](persistence::SavedMetadataTransformationChain,
                   MetadataTransformationStore::Completion completion) { completion({}); },
        .remove = [](core::StableId,
                     MetadataTransformationStore::Completion completion) { completion({}); },
    };
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, transformation_store);
    properties->show();

    // Picard-style: the automatic script's edits appear as ordinary colored
    // drafts the moment the files load — the grid is the write, no hidden
    // apply-time pass remains.
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    auto* undo = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-undo"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(undo != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(grid_model->patches().patch_count(), std::size_t{1U}, 5'000);
    const auto totals_column = grid_model->fieldColumn(QStringLiteral("Total Tracks"));
    QVERIFY(totals_column.has_value());
    const auto* patch =
        grid_model->patches().patch(0U, static_cast<std::size_t>(*totals_column) - 1U);
    QVERIFY(patch != nullptr);
    QCOMPARE(patch->kind, metadata::StagedMetadataPatchKind::remove_field);
    const auto totals_index = grid_model->index(0, *totals_column);
    QCOMPARE(totals_index.data(metadata_cell_staged_source_role).toString(),
             QStringLiteral("Library cleanup · step 1"));
    QVERIFY(totals_index.data(Qt::FontRole).value<QFont>().italic());

    // The staging is one ordinary undoable transaction the user can reject.
    QTRY_VERIFY(undo->isEnabled());
    QTest::mouseClick(undo, Qt::LeftButton);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{0U});

    QPointer guard{properties};
    properties->close();
    QTRY_VERIFY(guard.isNull());
}

void BenchMainWindowTest::TaggerEdits::metadataFieldReviewPreservesDraftAndSelectionScope() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    std::vector<MetadataPropertiesSource> sources;
    for (int index = 0; index < 2; ++index) {
        const auto path = media.filePath(QStringLiteral("review-%1.flac").arg(index));
        QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
        const auto encoded = QFile::encodeName(path);
        const std::string raw{encoded.constData(), static_cast<std::size_t>(encoded.size())};
        const auto read = metadata::read_local_metadata(raw);
        QVERIFY(read.has_value());
        sources.push_back(MetadataPropertiesSource{
            .source = {.raw_path = raw,
                       .source_revision = read->source_revision,
                       .baseline = read->document},
            .track_label = QStringLiteral("Review %1").arg(index),
        });
    }
    auto dialog = std::make_unique<MetadataPropertiesDialog>(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        std::span<const std::string_view>{}, MetadataWritePlanApplierFactory{},
        MetadataApplyObserver{});
    dialog->show();
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = dialog->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* files = dialog->fileListView();
    auto* model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* grid = qobject_cast<MetadataGridModel*>(files->model());
    auto* search = dialog->findChild<QLineEdit*>(QStringLiteral("bench-metadata-field-filter"));
    auto* changed = dialog->findChild<QCheckBox*>(QStringLiteral("bench-metadata-changed-only"));
    auto* show_files = dialog->findChild<QCheckBox*>(QStringLiteral("bench-metadata-show-files"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-field-filter-status"));
    QVERIFY(model && grid && search && changed && show_files && status);
    QTRY_VERIFY(model->summaryReady() && model->draftPreviewReady());
    const auto title = model->fieldRow(QStringLiteral("title"));
    const auto artist = model->fieldRow(QStringLiteral("artist"));
    QVERIFY(title && artist);

    show_files->setChecked(true);
    QTRY_VERIFY(files->isVisible());
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/tag-file-scope.png")));
    }
    // Checkboxes toggle the editing scope without clearing the other files.
    QCOMPARE(files->selectionModel()->selectedRows().size(), 2);
    const auto first = grid->index(0, 0);
    const auto checkbox =
        QPoint(files->visualRect(first).left() + 14, files->visualRect(first).center().y());
    QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, checkbox);
    QCOMPARE(files->selectionModel()->selectedRows().size(), 1);
    QVERIFY(files->selectionModel()->isRowSelected(1));
    QTRY_COMPARE(model->selectedItemCount(), 1U);
    QTest::keyClick(files, Qt::Key_Space);
    QCOMPARE(files->selectionModel()->selectedRows().size(), 2);
    QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, checkbox);
    QCOMPARE(files->selectionModel()->selectedRows().size(), 1);
    QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, checkbox);
    QCOMPARE(files->selectionModel()->selectedRows().size(), 2);
    QTRY_COMPARE(model->selectedItemCount(), 2U);

    // Filtering is a view operation: hidden selections cannot be removed accidentally.
    fields->setCurrentIndex(model->index(*artist, 2));
    fields->selectRow(*artist);
    search->setText(QStringLiteral("TiTlE"));
    QTRY_VERIFY(fields->isRowHidden(*artist));
    QVERIFY(!fields->isRowHidden(*title));
    QVERIFY(fields->selectionModel()->selectedRows().empty());
    QVERIFY(!fields->currentIndex().isValid());
    QVERIFY(grid->patches().empty());
    search->setText(QStringLiteral("musicbrainz_albumid"));
    const auto mbid = model->fieldRow(QStringLiteral("musicbrainz_albumid"));
    QVERIFY(mbid);
    QTRY_VERIFY(!fields->isRowHidden(*mbid));
    search->clear();

    // Stage only one file, then switch the selected-file scope under the filter.
    files->selectionModel()->select(grid->index(0, 0), QItemSelectionModel::ClearAndSelect |
                                                           QItemSelectionModel::Rows);
    QTRY_COMPARE(model->selectedItemCount(), 1U);
    QTRY_VERIFY(model->summaryReady());
    QVERIFY(
        model->setData(model->index(*title, 2), QStringLiteral("Reviewed title"), Qt::EditRole));
    changed->setChecked(true);
    QTRY_VERIFY(status->text().contains(QStringLiteral("1 changed")));
    QVERIFY(!fields->isRowHidden(*title));
    QVERIFY(fields->isRowHidden(*artist));
    QVERIFY(model->index(*title, 0).data(Qt::FontRole).value<QFont>().bold());
    QVERIFY(!model->index(*title, 0).data(Qt::AccessibleDescriptionRole).toString().isEmpty());
    files->selectionModel()->select(grid->index(1, 0), QItemSelectionModel::ClearAndSelect |
                                                           QItemSelectionModel::Rows);
    QTRY_VERIFY(status->text().contains(QStringLiteral("0 changed")));
    QVERIFY(fields->isRowHidden(*title));
    QVERIFY(!grid->patches().empty());
    files->selectionModel()->select(grid->index(0, 0), QItemSelectionModel::ClearAndSelect |
                                                           QItemSelectionModel::Rows);
    QTRY_VERIFY2(
        !fields->isRowHidden(*title),
        qPrintable(QStringLiteral("%1; selected=%2; summary=%3 draft=%4; query=%5; value=%6")
                       .arg(status->text())
                       .arg(model->selectedItemCount())
                       .arg(model->summaryReady())
                       .arg(model->draftPreviewReady())
                       .arg(search->text())
                       .arg(model->index(*title, 2).data(Qt::DisplayRole).toString())));

    // Collapsing the selector preserves scope; hiding all fields preserves the draft.
    show_files->setChecked(false);
    QVERIFY(files->isHidden());
    QCOMPARE(model->selectedItemCount(), 1U);
    search->setText(QStringLiteral("no-such-field"));
    QTRY_VERIFY(fields->isRowHidden(*title));
    QVERIFY(status->text().contains(QStringLiteral("Apply includes hidden edits")));
    QCOMPARE(model->index(*title, 2).data(Qt::EditRole).toString(),
             QStringLiteral("Reviewed title"));
    search->clear();
    QTRY_VERIFY(!fields->isRowHidden(*title));
    const auto screenshot_dir = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
    if (!screenshot_dir.isEmpty()) {
        QVERIFY(dialog->grab().save(screenshot_dir + QStringLiteral("/tag-field-review.png")));
    }
    QVERIFY(grid->undo());
    QTRY_VERIFY(fields->isRowHidden(*title));
    QVERIFY(!model->index(*title, 0).data(Qt::FontRole).value<QFont>().bold());
    QVERIFY(grid->redo());
    QTRY_VERIFY(!fields->isRowHidden(*title));

    // Adding a field clears the review filter so the new blank field is editable.
    auto* add = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-add-field"));
    QVERIFY(add);
    add->click();
    QInputDialog* prompt = nullptr;
    QTRY_VERIFY((prompt = dialog->findChild<QInputDialog*>()) != nullptr);
    prompt->setTextValue(QStringLiteral("REVIEW_CUSTOM"));
    prompt->accept();
    QTRY_VERIFY(model->fieldRow(QStringLiteral("REVIEW_CUSTOM")).has_value());
    const auto custom = *model->fieldRow(QStringLiteral("REVIEW_CUSTOM"));
    QTRY_VERIFY(!changed->isChecked());
    QVERIFY(!fields->isRowHidden(custom));
    QCOMPARE(fields->currentIndex().row(), custom);
    QVERIFY(grid->discardAll());
    QTRY_VERIFY(status->text().contains(QStringLiteral("0 changed")));
    QVERIFY(!fields->isRowHidden(*artist));
}

void BenchMainWindowTest::TaggerEdits::
    metadataPropertiesFileSelectionDrivesIndividualAndBulkEdits() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto rich_path = media.filePath(QStringLiteral("rich.flac"));
    const auto ordinary_path = media.filePath(QStringLiteral("ordinary.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), rich_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), ordinary_path));
    const auto rich_encoded = QFile::encodeName(rich_path);
    const auto ordinary_encoded = QFile::encodeName(ordinary_path);
    const std::string rich_raw{rich_encoded.constData(),
                               static_cast<std::size_t>(rich_encoded.size())};
    const std::string ordinary_raw{ordinary_encoded.constData(),
                                   static_cast<std::size_t>(ordinary_encoded.size())};

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({rich_raw, ordinary_raw});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr);
    QVERIFY(properties != nullptr);
    QCOMPARE(properties->shortcut(), QKeySequence(QStringLiteral("Alt+Return")));
    QCOMPARE(properties->text(), QStringLiteral("Edit tags…"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 2, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows()[0].probed && list_model->rows()[1].probed, 5'000);
    const auto ordinary_row =
        std::ranges::find(list_model->rows(), ordinary_raw, &LocalTrackRow::raw_path);
    QVERIFY(ordinary_row != list_model->rows().end());
    QCOMPARE(ordinary_row->metadata.effective_values("tracknumber"),
             (std::vector<std::string>{"3"}));
    QVERIFY(!ordinary_row->metadata.effective_native_field("track").has_value());
    const auto rich_row = std::ranges::find(list_model->rows(), rich_raw, &LocalTrackRow::raw_path);
    QVERIFY(rich_row != list_model->rows().end());
    QVERIFY(!rich_row->metadata.effective_native_field("album_artist").has_value());
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    view->selectionModel()->select(list_model->index(1, 0),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties->isEnabled());
    properties->trigger();

    auto* dialog =
        window.findChild<MetadataPropertiesDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(dialog != nullptr);
    QVERIFY(dialog->isVisible());
    // ADR-0221: a window, and still non-modal so the workspace stays usable
    // while edits are staged.
    QVERIFY(!dialog->isModal());
    QVERIFY(dialog->isWindow());
    QCOMPARE(tabs->count(), 1);
    QVERIFY(qobject_cast<MetadataPropertiesDialog*>(tabs->currentWidget()) == nullptr);
    QCOMPARE(dialog->windowTitle(), QStringLiteral("Edit tags · 2 tracks"));
    QVERIFY(dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-loading")) != nullptr);

    QTableView* files = nullptr;
    QTRY_VERIFY((files = dialog->fileListView()) != nullptr);
    auto* fields = dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-fields"));
    auto* summary = dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-summary"));
    auto* read_only = dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-metadata-buttons"));
    QVERIFY(fields != nullptr);
    QVERIFY(summary != nullptr);
    QVERIFY(read_only != nullptr);
    QVERIFY(buttons != nullptr);
    QVERIFY(dialog->findChild<QTabWidget*>(QStringLiteral("bench-metadata-pages")) == nullptr);
    QVERIFY(dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-grid")) == nullptr);
    QVERIFY(dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-values")) == nullptr);
    QVERIFY2(summary->text().contains(QStringLiteral("2 of 2 files selected")),
             qPrintable(QStringLiteral("unexpected summary: %1").arg(summary->text())));
    QVERIFY(summary->text().contains(QStringLiteral("2 sources")));
    // The files are read on opening, whatever the rows knew (ADR-0268) --
    // shown first on the cached tags and then read ("Tags read"), or read
    // before the cached tags could be shown; which comes first is a race
    // (the cached path is theTaggerOpensOnCachedTagsAndReadsBehind's).
    auto* session = dialog->findChild<TaggerSession*>();
    QVERIFY(session != nullptr);
    QTRY_VERIFY(session->ready() && session->canStageProposals());
    QVERIFY2(!read_only->text().contains(QStringLiteral("cached")),
             qPrintable(QStringLiteral("unexpected status: %1").arg(read_only->text())));
    QCOMPARE(buttons->standardButtons(), QDialogButtonBox::Close);
    QVERIFY(buttons->button(QDialogButtonBox::Apply) == nullptr);
    QCOMPARE(files->selectionBehavior(), QAbstractItemView::SelectRows);
    QCOMPARE(files->selectionMode(), QAbstractItemView::ExtendedSelection);
    QCOMPARE(files->editTriggers(), QAbstractItemView::NoEditTriggers);
    QVERIFY(!files->wordWrap());
    QVERIFY(fields->editTriggers().testFlag(QAbstractItemView::EditKeyPressed));
    QVERIFY(fields->editTriggers().testFlag(QAbstractItemView::AnyKeyPressed));
    QVERIFY(!fields->wordWrap());

    auto* undo = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-undo"));
    auto* redo = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-redo"));
    auto* discard = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-discard"));
    auto* add_field = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-add-field"));
    auto* remove_field =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-remove-field"));
    auto* edit_values =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-edit-values"));
    auto* preview_write_plan =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(undo != nullptr);
    QVERIFY(redo != nullptr);
    QVERIFY(discard != nullptr);
    QVERIFY(add_field != nullptr);
    QVERIFY(remove_field != nullptr);
    QVERIFY(edit_values != nullptr);
    QVERIFY(preview_write_plan != nullptr);
    QVERIFY(!undo->isEnabled());
    QVERIFY(!redo->isEnabled());
    QVERIFY(!discard->isEnabled());
    QVERIFY(!preview_write_plan->isEnabled());

    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    QVERIFY(grid_model != nullptr);
    QVERIFY(aggregate_model != nullptr);
    QCOMPARE(grid_model->rowCount(), 2);
    QCOMPARE(files->selectionModel()->selectedRows(0).size(), 2);
    QCOMPARE(aggregate_model->selectedItemCount(), std::size_t{2U});
    QCOMPARE(aggregate_model->columnCount(), 3);
    QCOMPARE(aggregate_model->headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Field"));
    QCOMPARE(aggregate_model->headerData(1, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Original"));
    QCOMPARE(aggregate_model->headerData(2, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Draft"));
    for (auto column = 1; column < grid_model->columnCount(); ++column) {
        QVERIFY(files->isColumnHidden(column));
    }

    const auto title_column = grid_model->fieldColumn(QStringLiteral("title"));
    const auto artist_column = grid_model->fieldColumn(QStringLiteral("artist"));
    const auto date_column = grid_model->fieldColumn(QStringLiteral("date"));
    const auto genre_column = grid_model->fieldColumn(QStringLiteral("genre"));
    const auto custom_column = grid_model->fieldColumn(QStringLiteral("custom_field"));
    const auto track_number_column = grid_model->fieldColumn(QStringLiteral("tracknumber"));
    QVERIFY(title_column.has_value());
    QVERIFY(artist_column.has_value());
    QVERIFY(date_column.has_value());
    QVERIFY(genre_column.has_value());
    QVERIFY(custom_column.has_value());
    QVERIFY(track_number_column.has_value());
    QVERIFY(!grid_model->fieldColumn(QStringLiteral("track")).has_value());
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    const auto artist_row = aggregate_model->fieldRow(QStringLiteral("artist"));
    const auto date_row = aggregate_model->fieldRow(QStringLiteral("date"));
    const auto genre_row = aggregate_model->fieldRow(QStringLiteral("genre"));
    const auto custom_row = aggregate_model->fieldRow(QStringLiteral("custom_field"));
    QVERIFY(title_row.has_value());
    QVERIFY(artist_row.has_value());
    QVERIFY(date_row.has_value());
    QVERIFY(genre_row.has_value());
    QVERIFY(custom_row.has_value());
    QVERIFY(aggregate_model->index(*title_row, 1)
                .data(Qt::DisplayRole)
                .toString()
                .contains(QStringLiteral("various")));
    QVERIFY(aggregate_model->index(*date_row, 1)
                .data(Qt::DisplayRole)
                .toString()
                .contains(QStringLiteral("present on 1 of 2")));
    QCOMPARE(aggregate_model->index(*genre_row, 1).data(Qt::DisplayRole).toString(),
             QStringLiteral("—"));

    // Arbitrary fields join the same Original/Draft table. The field starts
    // missing, its inline Draft edit applies to all selected files, and
    // removing it again cancels those additions because no baseline existed.
    const auto initial_field_count = aggregate_model->rowCount();
    QTRY_VERIFY(add_field->isEnabled());
    QTest::mouseClick(add_field, Qt::LeftButton);
    QInputDialog* field_prompt = nullptr;
    QTRY_VERIFY((field_prompt = dialog->findChild<QInputDialog*>(
                     QStringLiteral("bench-metadata-add-field-dialog"))) != nullptr);
    QVERIFY(!add_field->isEnabled());
    auto* field_name =
        field_prompt->findChild<QLineEdit*>(QStringLiteral("bench-metadata-add-field-name"));
    auto* field_completer =
        field_prompt->findChild<QCompleter*>(QStringLiteral("bench-metadata-field-completer"));
    QVERIFY(field_name != nullptr);
    QVERIFY(field_completer != nullptr);
    field_prompt->setTextValue(QStringLiteral("alb art"));
    QTRY_VERIFY(field_completer->model()->rowCount() > 0);
    QCOMPARE(field_completer->model()->index(0, 0).data().toString(),
             QStringLiteral("Album Artist"));
    field_prompt->setTextValue(QStringLiteral("mb track"));
    QTRY_VERIFY(field_completer->model()->rowCount() >= 2);
    QCOMPARE(field_completer->model()->index(0, 0).data().toString(),
             QStringLiteral("MUSICBRAINZ_TRACKID"));
    field_prompt->setTextValue(QStringLiteral("Mood"));
    field_prompt->accept();
    QTRY_VERIFY(dialog->findChild<QInputDialog*>(
                    QStringLiteral("bench-metadata-add-field-dialog")) == nullptr);
    QTRY_COMPARE(aggregate_model->rowCount(), initial_field_count + 1);
    const auto mood_row = aggregate_model->fieldRow(QStringLiteral("mood"));
    const auto mood_column = grid_model->fieldColumn(QStringLiteral("mood"));
    QVERIFY(mood_row.has_value());
    QVERIFY(mood_column.has_value());
    QVERIFY(files->isColumnHidden(*mood_column));
    const auto mood_draft = aggregate_model->index(*mood_row, 2);
    QTRY_VERIFY(mood_draft.flags().testFlag(Qt::ItemIsEditable));
    QTRY_COMPARE(fields->currentIndex(), mood_draft);
    QTest::keyClick(fields, Qt::Key_F2);
    QLineEdit* field_editor = nullptr;
    QTRY_VERIFY((field_editor = fields->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(field_editor, QStringLiteral("Energetic"));
    QTest::keyClick(field_editor, Qt::Key_Return);
    QTRY_VERIFY(fields->findChild<QLineEdit*>() == nullptr);
    QCOMPARE(mood_draft.data(Qt::DisplayRole).toString(), QStringLiteral("Energetic"));
    QCOMPARE(grid_model->index(0, *mood_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Energetic")}));
    QCOMPARE(grid_model->index(1, *mood_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Energetic")}));
    QTRY_VERIFY(remove_field->isEnabled());
    QTest::mouseClick(remove_field, Qt::LeftButton);
    QTRY_VERIFY(!grid_model->index(0, *mood_column).data(metadata_cell_staged_role).toBool());
    QVERIFY(!grid_model->index(1, *mood_column).data(metadata_cell_staged_role).toBool());
    QCOMPARE(mood_draft.data(Qt::DisplayRole).toString(), QStringLiteral("—"));

    // Successful arbitrary names become session-recent completion candidates.
    QTest::mouseClick(add_field, Qt::LeftButton);
    QTRY_VERIFY((field_prompt = dialog->findChild<QInputDialog*>(
                     QStringLiteral("bench-metadata-add-field-dialog"))) != nullptr);
    field_completer =
        field_prompt->findChild<QCompleter*>(QStringLiteral("bench-metadata-field-completer"));
    QVERIFY(field_completer != nullptr);
    field_prompt->setTextValue(QStringLiteral("moo"));
    QTRY_VERIFY(field_completer->model()->rowCount() > 0);
    QCOMPARE(field_completer->model()->index(0, 0).data().toString(), QStringLiteral("Mood"));
    field_prompt->reject();
    QTRY_VERIFY(dialog->findChild<QInputDialog*>(
                    QStringLiteral("bench-metadata-add-field-dialog")) == nullptr);

    // ADR-0276: lyrics are many lines. Their editor takes Enter as a new
    // line, keeps the text on Ctrl+Enter and drops it on Escape; the row
    // shows the line breaks as marks.
    QTest::mouseClick(add_field, Qt::LeftButton);
    QTRY_VERIFY((field_prompt = dialog->findChild<QInputDialog*>(
                     QStringLiteral("bench-metadata-add-field-dialog"))) != nullptr);
    field_prompt->setTextValue(QStringLiteral("Lyrics"));
    field_prompt->accept();
    std::optional<int> lyrics_row;
    QTRY_VERIFY((lyrics_row = aggregate_model->fieldRow(QStringLiteral("lyrics"))).has_value());
    const auto lyrics_column = grid_model->fieldColumn(QStringLiteral("lyrics"));
    QVERIFY(lyrics_column.has_value());
    const auto lyrics_draft = aggregate_model->index(*lyrics_row, 2);
    QTRY_COMPARE(fields->currentIndex(), lyrics_draft);
    QTest::keyClick(fields, Qt::Key_F2);
    QPlainTextEdit* lyrics_editor = nullptr;
    QTRY_VERIFY((lyrics_editor = fields->findChild<QPlainTextEdit*>(
                     QStringLiteral("metadata-long-text-editor"))) != nullptr);
    QVERIFY(fields->findChild<QLineEdit*>() == nullptr);
    QVERIFY(lyrics_editor->height() > fields->visualRect(lyrics_draft).height() * 4);
    QTest::keyClicks(lyrics_editor, QStringLiteral("First line"));
    QTest::keyClick(lyrics_editor, Qt::Key_Return);
    QTest::keyClicks(lyrics_editor, QStringLiteral("Second line"));
    QVERIFY(fields->findChild<QPlainTextEdit*>(QStringLiteral("metadata-long-text-editor")) !=
            nullptr);
    QTest::keyClick(lyrics_editor, Qt::Key_Return, Qt::ControlModifier);
    releaseHeldModifiers(fields);
    QTRY_VERIFY(fields->findChild<QPlainTextEdit*>(QStringLiteral("metadata-long-text-editor")) ==
                nullptr);
    QCOMPARE(grid_model->index(0, *lyrics_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("First line\nSecond line")}));
    QCOMPARE(lyrics_draft.data(Qt::DisplayRole).toString(),
             QStringLiteral("First line ↵ Second line"));
    QTest::keyClick(fields, Qt::Key_F2);
    QTRY_VERIFY((lyrics_editor = fields->findChild<QPlainTextEdit*>(
                     QStringLiteral("metadata-long-text-editor"))) != nullptr);
    QCOMPARE(lyrics_editor->toPlainText(), QStringLiteral("First line\nSecond line"));
    QTest::keyClicks(lyrics_editor, QStringLiteral(" and more"));
    QTest::keyClick(lyrics_editor, Qt::Key_Escape);
    QTRY_VERIFY(fields->findChild<QPlainTextEdit*>(QStringLiteral("metadata-long-text-editor")) ==
                nullptr);
    QCOMPARE(grid_model->index(0, *lyrics_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("First line\nSecond line")}));
    QTRY_VERIFY(remove_field->isEnabled());
    QTest::mouseClick(remove_field, Qt::LeftButton);
    QTRY_VERIFY(!grid_model->index(0, *lyrics_column).data(metadata_cell_staged_role).toBool());

    // One selected file exposes exact values in the same lower table and limits
    // edits to that source row.
    files->selectionModel()->select(grid_model->index(0, 0), QItemSelectionModel::ClearAndSelect |
                                                                 QItemSelectionModel::Rows);
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{1U});
    QTRY_COMPARE(aggregate_model->index(*title_row, 1).data(Qt::DisplayRole).toString(),
                 QStringLiteral("Metadata Fixture"));
    QVERIFY(summary->text().contains(QStringLiteral("1 of 2 files selected")));
    const auto custom_draft = aggregate_model->index(*custom_row, 2);
    fields->setCurrentIndex(custom_draft);
    QTRY_VERIFY(edit_values->isEnabled());
    QTest::mouseClick(edit_values, Qt::LeftButton);

    QDialog* exact_dialog = nullptr;
    QTRY_VERIFY((exact_dialog = dialog->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-exact-values"))) != nullptr);
    auto* exact_table =
        exact_dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-exact-values-table"));
    auto* exact_add =
        exact_dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-exact-values-add"));
    auto* exact_up =
        exact_dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-exact-values-up"));
    auto* exact_buttons = exact_dialog->findChild<QDialogButtonBox*>(
        QStringLiteral("bench-metadata-exact-values-buttons"));
    QVERIFY(exact_table != nullptr);
    QVERIFY(exact_add != nullptr);
    QVERIFY(exact_up != nullptr);
    QVERIFY(exact_buttons != nullptr);
    QCOMPARE(exact_table->model()->rowCount(), 2);
    QCOMPARE(exact_table->model()->index(0, 0).data(Qt::EditRole).toString(),
             QStringLiteral("first custom value"));
    QVERIFY(
        exact_table->model()->setData(exact_table->model()->index(0, 0), QString{}, Qt::EditRole));
    QTest::mouseClick(exact_add, Qt::LeftButton);
    QLineEdit* exact_editor = nullptr;
    QTRY_VERIFY((exact_editor = exact_table->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(exact_editor, QStringLiteral("third; value"));
    QTest::keyClick(exact_editor, Qt::Key_Return);
    QTRY_VERIFY(exact_table->findChild<QLineEdit*>() == nullptr);
    exact_table->setCurrentIndex(exact_table->model()->index(2, 0));
    QTest::mouseClick(exact_up, Qt::LeftButton);
    QTest::mouseClick(exact_buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    QTRY_VERIFY(dialog->findChild<QDialog*>(QStringLiteral("bench-metadata-exact-values")) ==
                nullptr);

    const auto first_custom = grid_model->index(0, *custom_column);
    const auto second_custom = grid_model->index(1, *custom_column);
    const QStringList custom_draft_values{QString{}, QStringLiteral("third; value"),
                                          QStringLiteral("second custom value")};
    QCOMPARE(first_custom.data(metadata_cell_values_role).toStringList(), custom_draft_values);
    QCOMPARE(second_custom.data(metadata_cell_values_role).toStringList(), QStringList{});
    QCOMPARE(custom_draft.data(metadata_cell_values_role).toStringList(), custom_draft_values);
    QVERIFY(summary->text().contains(QStringLiteral("1 staged change")));

    // Planning is an explicit fresh-read operation. Native FLAC now has a
    // preservation-proven text writer, but this exact draft deliberately
    // contains an empty value that TagLib would reinterpret as deletion. The
    // blocked apply changes nothing and reports the problem compactly.
    QTRY_VERIFY(preview_write_plan->isEnabled());
    QTest::mouseClick(preview_write_plan, Qt::LeftButton);
    QVERIFY(!preview_write_plan->isEnabled());
    QDialog* feedback = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((feedback = dialog->findChild<QDialog*>(
                                  QStringLiteral("bench-preparation-feedback"))) != nullptr,
                             5'000);
    auto* feedback_summary =
        feedback->findChild<QLabel*>(QStringLiteral("bench-preparation-feedback-summary"));
    auto* feedback_table =
        feedback->findChild<QTreeWidget*>(QStringLiteral("bench-preparation-feedback-table"));
    QVERIFY(feedback_summary != nullptr);
    QVERIFY(feedback_table != nullptr);
    QCOMPARE(feedback->windowTitle(), QStringLiteral("Apply blocked"));
    QVERIFY(feedback_summary->text().contains(QStringLiteral("Nothing was changed")));
    QCOMPARE(feedback_table->topLevelItemCount(), 1);
    QVERIFY(feedback_table->topLevelItem(0)->text(0).endsWith(QStringLiteral(".flac")));
    QVERIFY(feedback_table->topLevelItem(0)->text(1).contains(
        QStringLiteral("unsupported field mapping")));
    feedback->close();
    QTRY_VERIFY(dialog->findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) ==
                nullptr);
    QTRY_VERIFY(preview_write_plan->isEnabled());

    // Switching the file scope away and back reconstructs the selected file's
    // exact draft. With both files selected, the background result preview
    // reports the real partial state instead of a staged-count placeholder.
    files->selectAll();
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{2U});
    QTRY_VERIFY(aggregate_model->draftPreviewReady());
    QTRY_COMPARE(custom_draft.data(metadata_field_state_role).toInt(),
                 static_cast<int>(metadata::MetadataSelectionFieldState::partial));
    QCOMPARE(custom_draft.data(Qt::DisplayRole).toString(),
             QStringLiteral("(various · present on 1 of 2 files)"));
    QVERIFY(
        custom_draft.data(Qt::ToolTipRole).toString().contains(QStringLiteral("Complete draft")));
    files->selectionModel()->select(grid_model->index(0, 0), QItemSelectionModel::ClearAndSelect |
                                                                 QItemSelectionModel::Rows);
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{1U});
    QTRY_COMPARE(custom_draft.data(metadata_cell_values_role).toStringList(), custom_draft_values);
    QTest::keyClick(fields, Qt::Key_Z, Qt::ControlModifier);
    releaseHeldModifiers(fields);
    QTRY_VERIFY(!first_custom.data(metadata_cell_staged_role).toBool());
    QCOMPARE(
        first_custom.data(metadata_cell_values_role).toStringList(),
        (QStringList{QStringLiteral("first custom value"), QStringLiteral("second custom value")}));

    // A per-file exception can also make previously mixed values converge.
    // The complete preview exposes the resulting exact common value.
    const auto second_title_values =
        grid_model->index(1, *title_column).data(metadata_cell_values_role).toStringList();
    QCOMPARE(second_title_values.size(), 1);
    const auto title_draft = aggregate_model->index(*title_row, 2);
    QVERIFY(aggregate_model->setData(title_draft, second_title_values.front(), Qt::EditRole));
    files->selectAll();
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{2U});
    QTRY_VERIFY(aggregate_model->draftPreviewReady());
    QTRY_COMPARE(title_draft.data(metadata_field_state_role).toInt(),
                 static_cast<int>(metadata::MetadataSelectionFieldState::common));
    QCOMPARE(title_draft.data(metadata_cell_values_role).toStringList(), second_title_values);
    QCOMPARE(title_draft.data(Qt::DisplayRole).toString(), second_title_values.front());
    fields->setCurrentIndex(title_draft);
    fields->setFocus();
    QTest::keyClick(fields, Qt::Key_Z, Qt::ControlModifier);
    releaseHeldModifiers(fields);
    QTRY_VERIFY(!grid_model->index(0, *title_column).data(metadata_cell_staged_role).toBool());
    QTRY_VERIFY(aggregate_model->draftPreviewReady());
    QTRY_COMPARE(title_draft.data(metadata_field_state_role).toInt(),
                 static_cast<int>(metadata::MetadataSelectionFieldState::mixed));

    // Selecting both rows makes the same editor a bulk editor.
    QTRY_VERIFY(aggregate_model->index(*title_row, 1)
                    .data(Qt::DisplayRole)
                    .toString()
                    .contains(QStringLiteral("various")));
    const auto artist_draft = aggregate_model->index(*artist_row, 2);
    fields->setCurrentIndex(artist_draft);
    QTest::mouseClick(edit_values, Qt::LeftButton);
    exact_dialog = nullptr;
    QTRY_VERIFY((exact_dialog = dialog->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-exact-values"))) != nullptr);
    exact_table =
        exact_dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-exact-values-table"));
    exact_add =
        exact_dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-exact-values-add"));
    exact_buttons = exact_dialog->findChild<QDialogButtonBox*>(
        QStringLiteral("bench-metadata-exact-values-buttons"));
    QVERIFY(exact_table != nullptr);
    QVERIFY(exact_add != nullptr);
    QVERIFY(exact_buttons != nullptr);
    QCOMPARE(exact_table->model()->rowCount(), 0);
    QVERIFY(!exact_buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QTest::mouseClick(exact_add, Qt::LeftButton);
    QTRY_VERIFY((exact_editor = exact_table->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(exact_editor, QStringLiteral("Lead Artist"));
    QTest::keyClick(exact_editor, Qt::Key_Return);
    QTRY_VERIFY(exact_table->findChild<QLineEdit*>() == nullptr);
    QTest::mouseClick(exact_add, Qt::LeftButton);
    QTRY_VERIFY((exact_editor = exact_table->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(exact_editor, QStringLiteral("Guest Artist"));
    QTest::keyClick(exact_editor, Qt::Key_Return);
    QTRY_VERIFY(exact_table->findChild<QLineEdit*>() == nullptr);
    QTest::mouseClick(exact_buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    QTRY_VERIFY(dialog->findChild<QDialog*>(QStringLiteral("bench-metadata-exact-values")) ==
                nullptr);
    const QStringList artist_draft_values{QStringLiteral("Lead Artist"),
                                          QStringLiteral("Guest Artist")};
    QCOMPARE(grid_model->index(0, *artist_column).data(metadata_cell_values_role).toStringList(),
             artist_draft_values);
    QCOMPARE(grid_model->index(1, *artist_column).data(metadata_cell_values_role).toStringList(),
             artist_draft_values);
    QCOMPARE(artist_draft.data(metadata_cell_values_role).toStringList(), artist_draft_values);
    QVERIFY(summary->text().contains(QStringLiteral("2 staged changes")));
    QTest::keyClick(fields, Qt::Key_Z, Qt::ControlModifier);
    releaseHeldModifiers(fields);
    QTRY_VERIFY(!grid_model->index(0, *artist_column).data(metadata_cell_staged_role).toBool());
    QVERIFY(!grid_model->index(1, *artist_column).data(metadata_cell_staged_role).toBool());

    const auto genre_draft = aggregate_model->index(*genre_row, 2);
    fields->setCurrentIndex(genre_draft);
    fields->setFocus();
    QTest::keyClick(fields, Qt::Key_F2);
    QLineEdit* aggregate_editor = nullptr;
    QTRY_VERIFY((aggregate_editor = fields->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(aggregate_editor, QStringLiteral("Rock"));
    QTest::keyClick(aggregate_editor, Qt::Key_Return);
    QTRY_COMPARE(genre_draft.data(Qt::DisplayRole).toString(), QStringLiteral("Rock"));
    QCOMPARE(grid_model->index(0, *genre_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Rock")}));
    QCOMPARE(grid_model->index(1, *genre_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Rock")}));
    QTest::keyClick(fields, Qt::Key_Z, Qt::ControlModifier);
    releaseHeldModifiers(fields);
    QTRY_VERIFY(!grid_model->index(0, *genre_column).data(metadata_cell_staged_role).toBool());
    QVERIFY(!grid_model->index(1, *genre_column).data(metadata_cell_staged_role).toBool());

    // Delete and revert are also scoped by the file list.
    const auto date_file = grid_model->index(0, *date_column)
                                   .data(metadata_cell_baseline_values_role)
                                   .toStringList()
                                   .isEmpty()
                               ? 1
                               : 0;
    const auto other_file = 1 - date_file;
    files->selectionModel()->select(grid_model->index(date_file, 0),
                                    QItemSelectionModel::ClearAndSelect |
                                        QItemSelectionModel::Rows);
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{1U});
    const auto date_draft = aggregate_model->index(*date_row, 2);
    fields->setCurrentIndex(date_draft);
    fields->selectionModel()->select(date_draft, QItemSelectionModel::ClearAndSelect |
                                                     QItemSelectionModel::Rows);
    QTRY_VERIFY(remove_field->isEnabled());
    QTest::mouseClick(remove_field, Qt::LeftButton);
    QTRY_VERIFY(
        grid_model->index(date_file, *date_column).data(metadata_cell_staged_role).toBool());
    QVERIFY(!grid_model->index(other_file, *date_column).data(metadata_cell_staged_role).toBool());
    QCOMPARE(date_draft.data(Qt::DisplayRole).toString(), QStringLiteral("(remove)"));
    QTest::keyClick(fields, Qt::Key_Backspace, Qt::ControlModifier);
    releaseHeldModifiers(fields);
    QTRY_VERIFY(
        !grid_model->index(date_file, *date_column).data(metadata_cell_staged_role).toBool());

    files->clearSelection();
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{0U});
    QTRY_VERIFY(!add_field->isEnabled());
    QVERIFY(!remove_field->isEnabled());
    QTRY_VERIFY(!edit_values->isEnabled());
    QVERIFY(summary->text().contains(QStringLiteral("0 of 2 files selected")));

    files->selectAll();
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{2U});
    QTRY_VERIFY(add_field->isEnabled());
    QVERIFY(edit_values->isEnabled());
    QVERIFY(!summary->text().contains(QStringLiteral("staged change")));
    QVERIFY(!discard->isEnabled());
    // ADR-0221: the tagger is closed as a window. Asking the tab strip to
    // close its current tab would now close a list, not the editor.
    QPointer<MetadataPropertiesDialog> lifetime = dialog;
    QVERIFY(dialog->close());
    QTRY_VERIFY(lifetime.isNull());
    QCOMPARE(tabs->count(), 1);
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::TaggerEdits)

#include "window_tagger_edits_test.moc"
