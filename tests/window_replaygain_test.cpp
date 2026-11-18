// SPDX-License-Identifier: GPL-3.0-only

// The window test: ReplayGain scans.

#include "window_test.hpp"

#include "bench/file_scope_view.hpp"
#include "trackknife/core/engine_state.hpp"
#include "workspace/artwork_session.hpp"

namespace trackknife::bench {

class BenchMainWindowTest::ReplayGain final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void applyMeasuresReplayGainAndWritesOnce();
    void replayGainScanStagesMeasuredGainsAsDrafts();
    void replayGainScanUsesTruePeakWhenOptedIn();
    void replayGainScanStagesR128ForOpusTags();
    void contextReplayGainScansAndApplies_data();
    void contextReplayGainScansAndApplies();
    void replayGainScanPreservesLogicalSources();
    void contextReplayGainWritesCueSheets();
};

// ReplayGain as an Apply action: measured first, staged, and written with
// the tag edits in the same write; an album with gain already left alone.
void BenchMainWindowTest::ReplayGain::applyMeasuresReplayGainAndWritesOnce() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto work = std::make_shared<engine::RemoteFileWork>(
        protocol::Endpoint{.socket = QFile::encodeName(engine_.socket()).toStdString(),
                           .host = {},
                           .port = 0,
                           .token = {}});
    const auto raw = [](const QString& path) { return QFile::encodeName(path).toStdString(); };
    const auto wav = media.filePath(QStringLiteral("tone.wav"));
    write_sine_wav_fixture(wav, 0.6);
    const auto preset = convert::find_encoder_preset("flac");
    QVERIFY(preset);
    std::vector<MetadataPropertiesSource> sources;
    for (const auto& [name, album, gain] :
         {std::tuple{"a1.flac", "Measured", ""}, std::tuple{"a2.flac", "Measured", ""},
          std::tuple{"b1.flac", "Has gain", "-3.00 dB"},
          std::tuple{"b2.flac", "Has gain", "-3.00 dB"}}) {
        const auto path = media.filePath(QLatin1String(name));
        convert::AudioConversionRequest request{};
        request.source_raw_path = raw(wav);
        request.preset = *preset;
        request.destination_raw_path = raw(path);
        const auto field = [](const char* native, const char* value) {
            return metadata::MetadataField{.canonical_name =
                                               metadata::canonicalize_field_name(native),
                                           .native_name = native,
                                           .values = {value},
                                           .qualifier = {},
                                           .provenance = metadata::FieldProvenance::embedded};
        };
        request.metadata.fields = {field("ALBUM", album), field("ARTIST", "Band"),
                                   field("TITLE", name)};
        QVERIFY(convert::convert_audio_file(request));
        // Conversion writes no gains: those it has already, written after.
        if (std::string_view{gain}.size() > 0U) {
            const auto before = metadata::read_local_metadata(raw(path));
            QVERIFY(before.has_value());
            auto selection = metadata::StagedMetadataSelection::create(
                {metadata::StagedMetadataSource{.raw_path = raw(path),
                                                .source_revision = before->source_revision,
                                                .baseline = before->document}});
            QVERIFY(selection.has_value());
            metadata::StagedMetadataPatchSet patches;
            for (const auto* native : {"REPLAYGAIN_ALBUM_GAIN", "REPLAYGAIN_TRACK_GAIN"}) {
                const auto canonical = metadata::canonicalize_field_name(native);
                auto index = selection->field_index(canonical);
                if (!index) {
                    auto added = selection->ensure_missing_field(canonical, native);
                    QVERIFY(added.has_value());
                    index = *added;
                }
                QVERIFY(patches.replace_values(*selection, 0U, *index, {gain}).has_value());
            }
            const auto plan = metadata::build_metadata_write_plan(
                *selection, patches, metadata::local_metadata_file_access());
            QVERIFY(plan && plan->ready());
            const auto written = work->apply(*plan, {}, {});
            QVERIFY(written && written->committed_source_count() == 1U);
        }
        const auto read = metadata::read_local_metadata(raw(path));
        QVERIFY(read.has_value());
        sources.push_back(MetadataPropertiesSource{
            .source = metadata::StagedMetadataSource{.raw_path = raw(path),
                                                     .source_revision = read->source_revision,
                                                     .baseline = read->document},
            .track_label = {},
        });
    }
    std::size_t writes = 0U;
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {},
        TaggerServices{
            .plan_applier_factory =
                [work, &writes] {
                    return MetadataWritePlanApplier{
                        [work, &writes](const metadata::MetadataWritePlan& plan,
                                        const operations::MetadataApplyProgressCallback& progress,
                                        const core::CancellationToken& cancellation) {
                            ++writes;
                            return work->apply(plan, progress, cancellation, true);
                        }};
                },
            .apply_observer = {},
            .transformation_store =
                MetadataTransformationStore{.load = [](MetadataTransformationStore::LoadCompletion
                                                           completion) { completion({}, {}); },
                                            .save = {},
                                            .remove = {}},
            .output_profile_store = {},
            .file_plan_applier_factory = {},
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
    properties->show();
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* tagger = properties->findChild<TaggerSession*>();
    QVERIFY(tagger != nullptr);
    QTRY_VERIFY(tagger->canTransform());
    const auto artist_row = aggregate_model->fieldRow(QStringLiteral("artist"));
    QVERIFY(artist_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*artist_row, 2),
                                     QStringLiteral("Renamed Band"), Qt::EditRole));
    tagger->setReplayGainGrouping(0);
    tagger->chooseReplayGain(true);
    tagger->chooseSkipExistingGain(true);
    QCOMPARE(tagger->itemsNeedingGain({0U, 1U, 2U, 3U}), (std::vector<std::size_t>{0U, 1U}));
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QTRY_VERIFY(apply->isEnabled());
    apply->click();
    // Written once; the editor stays open to undo it, and is closed.
    QVERIFY(window_test::closeTaggerAfterWrite(guard, 20'000));
    QCOMPARE(writes, std::size_t{1U});
    const auto value_of = [&raw, &media](const char* name, std::string_view field) {
        const auto read = metadata::read_local_metadata(raw(media.filePath(QLatin1String(name))));
        return read ? read->document.first_effective_value(field).value_or(std::string{})
                    : std::string{"unread"};
    };
    QCOMPARE(value_of("a1.flac", "artist"), std::string{"Renamed Band"});
    QVERIFY(!value_of("a1.flac", "replaygainalbumgain").empty());
    QVERIFY(!value_of("a2.flac", "replaygaintrackgain").empty());
    QCOMPARE(value_of("b1.flac", "artist"), std::string{"Renamed Band"});
    QCOMPARE(value_of("b1.flac", "replaygainalbumgain"), std::string{"-3.00 dB"});
    QSettings{}.remove(QStringLiteral("properties/actions"));
}

void BenchMainWindowTest::ReplayGain::replayGainScanStagesMeasuredGainsAsDrafts() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto loud_path = media.filePath(QStringLiteral("loud.wav"));
    const auto quiet_path = media.filePath(QStringLiteral("quiet.wav"));
    write_sine_wav_fixture(loud_path, 0.8);
    write_sine_wav_fixture(quiet_path, 0.2);

    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto make_source = [&field](const QString& path, const QString& title) {
        const auto encoded = QFile::encodeName(path);
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path =
                        std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())},
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = {field("TITLE", {title.toStdString()}),
                                       field("ALBUM", {"Gain Album"}),
                                       field("ALBUMARTIST", {"Band"}),
                                       field("MUSICBRAINZ_ALBUMID",
                                             {"11111111-2222-3333-4444-555555555555"})},
                            .unsupported_native_objects = {},
                        },
                },
            .track_label = title,
        };
    };
    const std::vector sources{make_source(loud_path, QStringLiteral("Loud")),
                              make_source(quiet_path, QStringLiteral("Quiet"))};

    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {});
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* scan = properties->findChild<QPushButton*>(QStringLiteral("bench-replaygain-scan"));
    auto* grouping = properties->findChild<QComboBox*>(QStringLiteral("bench-replaygain-grouping"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(scan != nullptr);
    QVERIFY(grouping != nullptr);
    QCOMPARE(grouping->currentIndex(), 0);
    files->selectAll();
    QTRY_VERIFY(scan->isEnabled());
    QTest::mouseClick(scan, Qt::LeftButton);

    // The measured gains stage as ordinary colored drafts with ReplayGain
    // provenance; the shared release id makes both files one programme.
    QTRY_VERIFY_WITH_TIMEOUT(grid_model->patches().patch_count() >= 8U, 15'000);
    const auto track_gain_column = grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_GAIN"));
    const auto track_peak_column = grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_PEAK"));
    const auto album_gain_column = grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_ALBUM_GAIN"));
    const auto album_peak_column = grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_ALBUM_PEAK"));
    QVERIFY(track_gain_column.has_value());
    QVERIFY(track_peak_column.has_value());
    QVERIFY(album_gain_column.has_value());
    QVERIFY(album_peak_column.has_value());
    const auto loud_gain =
        grid_model->index(0, *track_gain_column).data(metadata_cell_values_role).toStringList();
    const auto quiet_gain =
        grid_model->index(1, *track_gain_column).data(metadata_cell_values_role).toStringList();
    QCOMPARE(loud_gain.size(), 1);
    QCOMPARE(quiet_gain.size(), 1);
    QVERIFY(loud_gain.front().endsWith(QStringLiteral(" dB")));
    // The quiet tone needs roughly 12 dB more gain than the loud one.
    const auto loud_value = loud_gain.front().chopped(3).toDouble();
    const auto quiet_value = quiet_gain.front().chopped(3).toDouble();
    QVERIFY(std::abs((quiet_value - loud_value) - 12.04) < 0.3);
    // One programme: identical album gain and peak on both rows.
    QCOMPARE(
        grid_model->index(0, *album_gain_column).data(metadata_cell_values_role).toStringList(),
        grid_model->index(1, *album_gain_column).data(metadata_cell_values_role).toStringList());
    const auto album_peak =
        grid_model->index(0, *album_peak_column).data(metadata_cell_values_role).toStringList();
    QCOMPARE(album_peak.size(), 1);
    QVERIFY(album_peak.front().startsWith(QStringLiteral("0.79")) ||
            album_peak.front().startsWith(QStringLiteral("0.80")));
    QCOMPARE(
        grid_model->index(0, *track_gain_column).data(metadata_cell_staged_source_role).toString(),
        QStringLiteral("ReplayGain"));

    // Staging inserted new columns; the file selection must survive so the
    // fields pane keeps projecting the selection instead of going blank.
    QCOMPARE(files->selectionModel()->selectedRows().size(), 2);

    // ADR-0147: the measurement exports as CSV through the status link.
    QTemporaryDir export_directory;
    QVERIFY(export_directory.isValid());
    const auto export_path = export_directory.filePath(QStringLiteral("results.csv"));
    properties->setProperty("trackknife-replaygain-export-path", export_path);
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(status != nullptr);
    emit status->linkActivated(QStringLiteral("export-replaygain"));
    QFile exported{export_path};
    QVERIFY(exported.open(QIODevice::ReadOnly));
    const auto csv = QString::fromUtf8(exported.readAll());
    QVERIFY(csv.startsWith(QStringLiteral(
        "track,file,integrated_lufs,track_gain_db,track_peak,album_key,album_gain_db,"
        "album_peak,status,peak_kind")));
    QVERIFY(csv.contains(loud_gain.front()));
    // ADR-0148: the default policy exports sample peaks.
    QVERIFY(csv.contains(QStringLiteral("analyzed,sample")));
    QCOMPARE(csv.count(QLatin1Char('\n')), 3);

    // ADR-0147: the provenance view marks the unsaved scan values as
    // drafts, one row per track.
    auto* provenance_button =
        properties->findChild<QPushButton*>(QStringLiteral("bench-replaygain-provenance"));
    QVERIFY(provenance_button != nullptr);
    QVERIFY(provenance_button->isEnabled());
    provenance_button->click();
    QTableWidget* provenance_table = nullptr;
    QTRY_VERIFY((provenance_table = properties->findChild<QTableWidget*>(
                     QStringLiteral("bench-replaygain-provenance-table"))) != nullptr);
    QCOMPARE(provenance_table->rowCount(), 2);
    QCOMPARE(provenance_table->columnCount(), 7);
    QVERIFY(provenance_table->item(0, 1) != nullptr);
    QCOMPARE(provenance_table->item(0, 1)->text(),
             QStringLiteral("%1 · draft").arg(loud_gain.front()));

    // Closing would rightly demand draft confirmation; tear down directly.
    delete properties;
}

// ADR-0148: opting in routes the oversampled true peak into the
// REPLAYGAIN_*_PEAK drafts and the export names the peak kind.
void BenchMainWindowTest::ReplayGain::replayGainScanUsesTruePeakWhenOptedIn() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("tone.wav"));
    write_sine_wav_fixture(path, 0.5);
    const auto encoded = QFile::encodeName(path);
    const std::vector sources{MetadataPropertiesSource{
        .source =
            metadata::StagedMetadataSource{
                .raw_path =
                    std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())},
                .source_revision = std::nullopt,
                .baseline = metadata::MetadataDocument{},
            },
        .track_label = QStringLiteral("Tone"),
    }};
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {});
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* scan = properties->findChild<QPushButton*>(QStringLiteral("bench-replaygain-scan"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(scan != nullptr);
    // ADR-0185: the true-peak preference lives in Settings; the scan reads
    // it from QSettings.
    QSettings{}.setValue(QLatin1String(SettingsDialog::replaygain_true_peak_key), true);
    files->selectAll();
    QTRY_VERIFY(scan->isEnabled());
    QTest::mouseClick(scan, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(grid_model->patches().patch_count() >= 2U, 15'000);

    // A clean sine's true peak barely exceeds its sample peak; the draft
    // must still be the plausible linear amplitude.
    const auto peak_column = grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_PEAK"));
    QVERIFY(peak_column.has_value());
    const auto peak_values =
        grid_model->index(0, *peak_column).data(metadata_cell_values_role).toStringList();
    QCOMPARE(peak_values.size(), 1);
    const auto peak = peak_values.front().toDouble();
    QVERIFY(peak > 0.45 && peak < 0.6);

    QTemporaryDir export_directory;
    QVERIFY(export_directory.isValid());
    const auto export_path = export_directory.filePath(QStringLiteral("results.csv"));
    properties->setProperty("trackknife-replaygain-export-path", export_path);
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(status != nullptr);
    emit status->linkActivated(QStringLiteral("export-replaygain"));
    QFile exported{export_path};
    QVERIFY(exported.open(QIODevice::ReadOnly));
    const auto csv = QString::fromUtf8(exported.readAll());
    QVERIFY(csv.contains(QStringLiteral("analyzed,true_peak")));

    // The setting is sticky; restore the default so later dialogs scan
    // sample peaks again.
    QSettings{}.remove(QLatin1String(SettingsDialog::replaygain_true_peak_key));
    delete properties;
}

// ADR-0149: measured Opus sources stage RFC 7845 R128 Q7.8 comments for
// tag writes; the sidecar-only policy outranks that and keeps the
// conventional -18 LUFS fields because the audio file is never touched.
void BenchMainWindowTest::ReplayGain::replayGainScanStagesR128ForOpusTags() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("tone.opus"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("loudness-tone-opus.b64"), path));
    const auto encoded = QFile::encodeName(path);
    const std::vector sources{MetadataPropertiesSource{
        .source =
            metadata::StagedMetadataSource{
                .raw_path =
                    std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())},
                .source_revision = std::nullopt,
                .baseline = metadata::MetadataDocument{},
            },
        .track_label = QStringLiteral("Opus tone"),
    }};
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {});
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* scan = properties->findChild<QPushButton*>(QStringLiteral("bench-replaygain-scan"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(scan != nullptr);
    QSettings{}.remove(QLatin1String(SettingsDialog::replaygain_sidecar_only_key));
    files->selectAll();
    QTRY_VERIFY(scan->isEnabled());
    QTest::mouseClick(scan, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(grid_model->patches().patch_count() >= 1U, 15'000);

    const auto r128_column = grid_model->fieldColumn(QStringLiteral("R128_TRACK_GAIN"));
    QVERIFY(r128_column.has_value());
    const auto r128_values =
        grid_model->index(0, *r128_column).data(metadata_cell_values_role).toStringList();
    QCOMPARE(r128_values.size(), 1);
    // A strict Q7.8 integer, and one the reader lifts back into a sane
    // ReplayGain-scale gain for this quiet tone.
    bool integral = false;
    const auto q78 = r128_values.front().toInt(&integral);
    QVERIFY(integral);
    const auto lifted = q78 / 256.0 + 5.0;
    QVERIFY(lifted > -20.0 && lifted < 20.0);
    // RFC 7845 defines no peak field.
    QVERIFY(!grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_PEAK")).has_value());
    QVERIFY(!grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_GAIN")).has_value());

    // Under the sidecar-only policy the same file stages conventional
    // fields for the Trackbench-owned carrier.
    QSettings{}.setValue(QLatin1String(SettingsDialog::replaygain_sidecar_only_key), true);
    QTest::mouseClick(scan, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(
        grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_GAIN")).has_value(), 15'000);
    const auto conventional_column =
        grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_GAIN"));
    const auto conventional =
        grid_model->index(0, *conventional_column).data(metadata_cell_values_role).toStringList();
    QCOMPARE(conventional.size(), 1);
    QVERIFY(conventional.front().endsWith(QStringLiteral(" dB")));

    delete properties;
    QSettings{}.remove(QLatin1String(SettingsDialog::replaygain_sidecar_only_key));
}

void BenchMainWindowTest::ReplayGain::contextReplayGainScansAndApplies_data() {
    QTest::addColumn<bool>("cached");
    QTest::addColumn<bool>("embedded");
    // ADR-0237: done by the engine holding the files.
    QTest::newRow("imported") << false << false;
    QTest::newRow("cached-sidecar") << true << false;
    QTest::newRow("cached-embedded") << true << true;
}

void BenchMainWindowTest::ReplayGain::contextReplayGainScansAndApplies() {
    QFETCH(bool, cached);
    QFETCH(bool, embedded);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto first =
        media.filePath(embedded ? QStringLiteral("one.flac") : QStringLiteral("one.wav"));
    const auto second =
        media.filePath(embedded ? QStringLiteral("two.flac") : QStringLiteral("two.wav"));
    if (embedded) {
        const auto wav = media.filePath(QStringLiteral("tone.wav"));
        write_sine_wav_fixture(wav, 0.6);
        const auto preset = convert::find_encoder_preset("flac");
        QVERIFY(preset);
        convert::AudioConversionRequest request{};
        request.source_raw_path = QFile::encodeName(wav).toStdString();
        request.preset = *preset;
        request.metadata.fields.push_back({.canonical_name = "title",
                                           .native_name = "TITLE",
                                           .values = {"Preserve this title"},
                                           .qualifier = {},
                                           .provenance = metadata::FieldProvenance::embedded});
        for (const auto& output : {first, second}) {
            request.destination_raw_path = QFile::encodeName(output).toStdString();
            QVERIFY(convert::convert_audio_file(request));
        }
    } else {
        write_sine_wav_fixture(first, 0.6);
        write_sine_wav_fixture(second, 0.3);
    }
    const auto original = metadata::read_local_metadata(QFile::encodeName(first).toStdString());
    QVERIFY(original);

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(
        {QFile::encodeName(first).toStdString(), QFile::encodeName(second).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    QTRY_COMPARE(view->model()->rowCount(), 2);
    auto* local_model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(local_model != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(local_model->rows()[0].probed && local_model->rows()[1].probed, 5000);
    if (cached) {
        auto rows = local_model->rows();
        for (auto& row : rows) {
            row.source_revision.reset();
            row.metadata.fields.clear();
        }
        local_model->replaceRows(std::move(rows));
    }
    view->selectAll();

    if (cached && embedded) {
        window.findChild<QAction*>(QStringLiteral("action-track-properties"))->trigger();
        auto* properties = window.findChild<MetadataPropertiesDialog*>();
        QVERIFY(properties);
        MetadataGridModel* grid = nullptr;
        QTRY_VERIFY((grid = properties->findChild<MetadataGridModel*>()) != nullptr);
        QVERIFY(grid->selection().source(0).source_revision.has_value());
        QCOMPARE(grid->selection().source(0).baseline, original->document);
        delete properties;
        tabs->setCurrentWidget(view);
        view->selectAll();
    }

    // This build's engine does file work; once it has said so, the dialog
    // goes through it -- unless it is taken as an older one.
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    auto* action = window.findChild<QAction*>(QStringLiteral("action-replaygain-dialog"));
    QVERIFY(action != nullptr);
    action->trigger();
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<QDialog*>(QStringLiteral("bench-replaygain-dialog"))) !=
                nullptr);
    QCOMPARE(dialog->property("trackknife-file-work").toString(), QStringLiteral("engine"));
    auto* grouping =
        dialog->findChild<QComboBox*>(QStringLiteral("bench-replaygain-dialog-grouping"));
    auto* run = dialog->findChild<QPushButton*>(QStringLiteral("bench-replaygain-dialog-run"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-replaygain-dialog-status"));
    QVERIFY(grouping != nullptr && run != nullptr && status != nullptr);
    grouping->setCurrentIndex(3);
    run->click();
    QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(), 30'000);
    const auto problems = dialog->findChild<QPlainTextEdit*>();
    QVERIFY2(status->text().startsWith(QStringLiteral("Saved ReplayGain tags to 2")),
             qPrintable(status->text() + QStringLiteral(" · ") +
                        (problems ? problems->toPlainText() : QString{})));

    // Unwritable WAVs landed in sidecars with the measured track gains.
    for (const auto& path : {first, second}) {
        const auto encoded = QFile::encodeName(path);
        if (embedded) {
            const auto reread = metadata::read_local_metadata(encoded.toStdString());
            QVERIFY(reread);
            QVERIFY(reread->document.first_effective_value("replaygaintrackgain"));
            for (const auto& field : original->document.effective_fields()) {
                if (!field.canonical_name.starts_with("replaygain")) {
                    QCOMPARE(reread->document.effective_values(field.canonical_name), field.values);
                }
            }
            continue;
        }
        const auto sidecar = metadata::read_loudness_sidecar(
            std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())});
        QVERIFY(sidecar.has_value());
        QVERIFY(sidecar->has_value());
        QCOMPARE((*sidecar)->entries.size(), 1U);
        QVERIFY((*sidecar)->entries.front().track_gain_db.has_value());
    }
    dialog->close();
}

// ADR-0357: the dialog asks the engine for the whole round -- here a CUE
// sheet's tracks, previewed as one album, then measured over their ranges
// and their gains written into the sheet.
void BenchMainWindowTest::ReplayGain::contextReplayGainWritesCueSheets() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    write_sine_wav_fixture(media.filePath(QStringLiteral("disc.wav")), 0.8, 0.2);
    const auto input = media.filePath(QStringLiteral("disc.cue"));
    {
        QFile cue{input};
        QVERIFY(cue.open(QIODevice::WriteOnly));
        cue.write("PERFORMER \"Gain Artist\"\nTITLE \"Gain Album\"\nFILE \"disc.wav\" WAVE\n"
                  "TRACK 01 AUDIO\nTITLE \"Loud\"\nINDEX 01 00:00:00\n"
                  "TRACK 02 AUDIO\nTITLE \"Quiet\"\nINDEX 01 00:01:00\n");
    }
    BenchMainWindow window;
    window.show();
    window.openLocalPaths({QFile::encodeName(input).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(view->model()->rowCount(), 2, 5'000);
    view->selectAll();
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    window.findChild<QAction*>(QStringLiteral("action-replaygain-dialog"))->trigger();
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<QDialog*>(QStringLiteral("bench-replaygain-dialog"))) !=
                nullptr);
    auto* grouping =
        dialog->findChild<QComboBox*>(QStringLiteral("bench-replaygain-dialog-grouping"));
    auto* preview =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-replaygain-dialog-preview"));
    auto* run = dialog->findChild<QPushButton*>(QStringLiteral("bench-replaygain-dialog-run"));
    auto* groups =
        dialog->findChild<QListWidget*>(QStringLiteral("bench-replaygain-dialog-groups"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-replaygain-dialog-status"));
    QVERIFY(grouping != nullptr && preview != nullptr && run != nullptr && groups != nullptr &&
            status != nullptr);
    grouping->setCurrentIndex(2);
    preview->click();
    QTRY_VERIFY_WITH_TIMEOUT(groups->count() == 1 &&
                                 groups->item(0)->text() ==
                                     QStringLiteral("Selection as one album · 2 tracks"),
                             10'000);
    QTRY_VERIFY(run->isEnabled());
    run->click();
    QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(), 30'000);
    const auto problems = dialog->findChild<QPlainTextEdit*>();
    QVERIFY2(status->text().startsWith(QStringLiteral("Saved ReplayGain tags to 1 target")),
             qPrintable(status->text() + QStringLiteral(" · ") +
                        (problems ? problems->toPlainText() : QString{})));
    QFile written{input};
    QVERIFY(written.open(QIODevice::ReadOnly));
    const auto text = written.readAll();
    QCOMPARE(text.count("REM REPLAYGAIN_TRACK_GAIN"), 2);
    QCOMPARE(text.count("REM REPLAYGAIN_ALBUM_GAIN"), 1);
    dialog->close();
}

void BenchMainWindowTest::ReplayGain::replayGainScanPreservesLogicalSources() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    write_sine_wav_fixture(media.filePath(QStringLiteral("disc.wav")), 0.8, 0.2);
    const auto input = media.filePath(QStringLiteral("disc.cue"));
    {
        QFile cue{input};
        QVERIFY(cue.open(QIODevice::WriteOnly));
        cue.write("TITLE \"Gain Album\"\nFILE \"disc.wav\" WAVE\n"
                  "TRACK 01 AUDIO\nTITLE \"Loud\"\nINDEX 01 00:00:00\n"
                  "TRACK 02 AUDIO\nTITLE \"Quiet\"\nINDEX 01 00:01:00\n");
    }
    BenchMainWindow window;
    window.show();
    window.openLocalPaths({QFile::encodeName(input).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr && properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);

    std::vector<loudness::LoudnessScanItem> reference_items;
    for (const auto& row : model->rows()) {
        reference_items.push_back({.item_index = reference_items.size(),
                                   .raw_path = row.raw_path,
                                   .selection = row.selection,
                                   .range = row.segment,
                                   .album_key = "album"});
    }
    const auto reference = loudness::scan_loudness(reference_items);
    QVERIFY(reference.has_value());
    QCOMPARE(reference->analyzed_track_count(), 2U);
    QVERIFY(reference->tracks[0].loudness->measurable());
    QVERIFY(reference->tracks[1].loudness->measurable());
    // The fixture must distinguish the two logical signals.
    QVERIFY(std::abs(reference->tracks[0].loudness->track_gain_db() -
                     reference->tracks[1].loudness->track_gain_db()) > 0.1);

    view->selectAll();
    QTRY_VERIFY(properties_action->isEnabled());
    properties_action->trigger();
    auto* properties = window.findChild<MetadataPropertiesDialog*>();
    QVERIFY(properties != nullptr);
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid = qobject_cast<MetadataGridModel*>(files->model());
    QVERIFY(grid != nullptr);
    auto* scan = properties->findChild<QPushButton*>(QStringLiteral("bench-replaygain-scan"));
    auto* grouping = properties->findChild<QComboBox*>(QStringLiteral("bench-replaygain-grouping"));
    QVERIFY(scan != nullptr && grouping != nullptr);
    grouping->setCurrentIndex(2);
    files->selectAll();
    QTRY_VERIFY(scan->isEnabled());
    scan->click();
    QTRY_VERIFY_WITH_TIMEOUT(grid->patches().patch_count() >= 8U, 15'000);
    const auto gain = grid->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_GAIN"));
    const auto peak = grid->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_PEAK"));
    const auto album = grid->fieldColumn(QStringLiteral("REPLAYGAIN_ALBUM_GAIN"));
    QVERIFY(gain && peak && album);
    for (int row = 0; row < 2; ++row) {
        const auto values = [&](const int column) {
            return grid->index(row, column).data(metadata_cell_values_role).toStringList().front();
        };
        const auto& expected = *reference->tracks[static_cast<std::size_t>(row)].loudness;
        QVERIFY(std::abs(values(*gain).chopped(3).toDouble() - expected.track_gain_db()) < 0.011);
        QVERIFY(std::abs(values(*peak).toDouble() - expected.sample_peak) < 0.000002);
        QVERIFY(std::abs(values(*album).chopped(3).toDouble() -
                         *reference->albums.front().album_gain_db()) < 0.011);
    }
    QCOMPARE(files->selectionModel()->selectedRows().size(), 2);
    QVERIFY(grid->selection().source(0U).logical_track);
    QVERIFY(grid->selection().source(1U).logical_track);

    // A rescan of only the second row must retain its Properties index,
    // decoder selection, and nonzero range start after draft undo.
    QVERIFY(grid->undo());
    QCOMPARE(grid->patches().patch_count(), 0U);
    files->selectRow(1);
    grouping->setCurrentIndex(3);
    QTRY_VERIFY(scan->isEnabled());
    scan->click();
    QTRY_COMPARE_WITH_TIMEOUT(grid->patches().patch_count(), 2U, 15'000);
    QVERIFY(grid->index(0, *gain).data(metadata_cell_values_role).toStringList().isEmpty());
    const auto second_gain = grid->index(1, *gain).data(metadata_cell_values_role).toStringList();
    QCOMPARE(second_gain.size(), 1);
    QVERIFY(std::abs(second_gain.front().chopped(3).toDouble() -
                     reference->tracks[1].loudness->track_gain_db()) < 0.011);
    delete properties;
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::ReplayGain)

#include "window_replaygain_test.moc"
