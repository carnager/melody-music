// SPDX-License-Identifier: GPL-3.0-only

// The window test: converting files.

#include "trackknife/metadata/flac_mapping.hpp"
#include "window_test.hpp"

#include "uicommon/library_tree_view.hpp"

#include <QPainter>
#include <QStandardItemModel>

namespace trackknife::bench {

class BenchMainWindowTest::Convert final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void ffmpegEncoderIsTheTagTagLibCallsEncoding();
    void convertDialogPlansAndConvertsSelection();
    void convertDialogAppliesPermanentReplayGain();
    void convertDialogFetchesFilesItCannotReach();
};

void BenchMainWindowTest::Convert::ffmpegEncoderIsTheTagTagLibCallsEncoding() {
    // An MP3 converted by FFmpeg: TagLib reads its TSSE frame as ENCODING,
    // FFmpeg reports the same tag as "encoder". It is one tag, and must not
    // come back as a second, stream-only field that no edit can touch.
    metadata::MetadataDocument document;
    document.fields.push_back(
        metadata::MetadataField{.canonical_name = "encoding",
                                .native_name = "ENCODING",
                                .values = {"Lavf61.5.101"},
                                .qualifier = {},
                                .provenance = metadata::FieldProvenance::embedded});
    document.fields.push_back(
        metadata::MetadataField{.canonical_name = "encoder",
                                .native_name = "encoder",
                                .values = {"Lavf61.5.101", "Lavc61.11"},
                                .qualifier = {},
                                .provenance = metadata::FieldProvenance::stream});
    remove_shadowed_probed_metadata(document);
    QCOMPARE(document.fields.size(), std::size_t{1});
    QCOMPARE(document.fields.front().canonical_name, std::string{"encoding"});
    QCOMPARE(metadata::stream_tag_alias("ENCODER"), std::optional<std::string_view>{"encoding"});
}

void BenchMainWindowTest::Convert::convertDialogPlansAndConvertsSelection() {
    QTemporaryDir media;
    QTemporaryDir destination;
    QVERIFY(media.isValid());
    QVERIFY(destination.isValid());
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
    const auto make_item = [&field](const QString& path, const QString& title,
                                    const QString& track) {
        const auto encoded = QFile::encodeName(path);
        return ConvertDialogItem{
            .raw_path = std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())},
            .selection = {},
            .segment = {},
            .source_revision = {},
            .metadata =
                metadata::MetadataDocument{
                    .fields = {field("TITLE", {title.toStdString()}),
                               field("ALBUM", {"Converted Album"}),
                               field("TRACKNUMBER", {track.toStdString()})},
                    .unsupported_native_objects = {},
                },
            .label = title,
            .fetch = {},
        };
    };
    const auto saved_layouts = std::vector{persistence::SavedOutputLayoutProfile{
        .id = core::StableId::random(),
        .profile =
            operations::OutputLayoutProfile{
                .schema_version = 1U,
                .name = "Saved album layout",
                .dialect = {},
                .relative_directory_expression = "saved/%album%",
                .basename_expression = "%title%",
                .sanitization_policy = {"linux", 1U},
            },
    }};
    const auto saved_destinations = std::vector{persistence::SavedDestinationProfile{
        .id = core::StableId::random(),
        .profile =
            operations::DestinationProfile{
                .schema_version = 1U,
                .name = "Saved root",
                .root_raw_path = destination.path().toStdString(),
                .containment_policy = {"lexical-beneath-root", 1U},
            },
    }};
    std::vector<persistence::SavedEncoderPreset> preset_catalog;
    ConvertPresetStore preset_store{
        .load =
            [&preset_catalog](ConvertPresetStore::LoadCompletion completion) {
                completion(preset_catalog, QString{});
            },
        .save =
            [&preset_catalog](persistence::SavedEncoderPreset preset,
                              ConvertPresetStore::Completion completion) {
                preset_catalog.push_back(std::move(preset));
                completion(QString{});
            },
        .remove =
            [&preset_catalog](const core::StableId id, ConvertPresetStore::Completion completion) {
                std::erase_if(preset_catalog, [&id](const auto& entry) { return entry.id == id; });
                completion(QString{});
            },
    };
    auto* dialog = new ConvertDialog(
        {make_item(loud_path, QStringLiteral("Loud"), QStringLiteral("01")),
         make_item(quiet_path, QStringLiteral("Quiet"), QStringLiteral("02"))},
        [&saved_layouts, &saved_destinations](auto completion) {
            completion(saved_layouts, saved_destinations, QString{});
        },
        preset_store);
    dialog->show();

    auto* preset = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-preset"));
    auto* root = dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-destination"));
    auto* directories =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-directory-expression"));
    auto* names =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-basename-expression"));
    auto* preview = dialog->findChild<QListWidget*>(QStringLiteral("bench-convert-preview"));
    auto* run = dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-run"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-convert-status"));
    auto* problems = dialog->findChild<QPlainTextEdit*>(QStringLiteral("bench-convert-problems"));
    auto* channels = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-channels"));
    auto* gain_mode = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-gain"));
    QVERIFY(preset != nullptr && root != nullptr && directories != nullptr && names != nullptr);
    QVERIFY(preview != nullptr && run != nullptr && status != nullptr && problems != nullptr);
    QVERIFY(channels != nullptr && gain_mode != nullptr);
    // Processing choices restore from persisted settings; this test's items
    // carry no ReplayGain, so pin the gain policy off explicitly.
    channels->setCurrentIndex(channels->findData(0));
    gain_mode->setCurrentIndex(gain_mode->findData(0));
    // Problem reports scroll inside a bounded pane instead of stretching
    // the dialog; without problems it stays hidden.
    QVERIFY(!problems->isVisible());
    QVERIFY(problems->maximumHeight() <= 200);

    // The app's saved naming layouts and destination roots are offered
    // directly; picking them fills the editable fields.
    auto* layout_choice = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-layout"));
    auto* root_choice =
        dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-destination-choice"));
    QVERIFY(layout_choice != nullptr && root_choice != nullptr);
    QTRY_COMPARE(layout_choice->count(), 2);
    QTRY_COMPARE(root_choice->count(), 2);
    layout_choice->setCurrentIndex(1);
    emit layout_choice->activated(1);
    QCOMPARE(directories->text(), QStringLiteral("saved/%album%"));
    QCOMPARE(names->text(), QStringLiteral("%title%"));
    root_choice->setCurrentIndex(1);
    emit root_choice->activated(1);
    QCOMPARE(root->text(), destination.path());
    // Hand-editing drifts the choice back to Custom.
    QTest::keyClick(names, Qt::Key_X);
    QCOMPARE(layout_choice->currentIndex(), 0);

    // A new encoder preset saves from the editor, joins the combo as a
    // selectable profile, and deletes again; built-ins are untouched.
    const auto builtin_count = preset->count();
    auto* preset_new = dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-preset-new"));
    auto* preset_export =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-preset-export"));
    auto* preset_delete =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-preset-delete"));
    QVERIFY(preset_export != nullptr);
    QVERIFY(preset_new != nullptr && preset_delete != nullptr);
    QVERIFY(!preset_delete->isVisible());
    QTest::mouseClick(preset_new, Qt::LeftButton);
    auto* editor = dialog->findChild<QDialog*>(QStringLiteral("bench-preset-editor"));
    QVERIFY(editor != nullptr);
    auto* editor_name = editor->findChild<QLineEdit*>(QStringLiteral("bench-preset-editor-name"));
    auto* editor_format =
        editor->findChild<QComboBox*>(QStringLiteral("bench-preset-editor-format"));
    auto* editor_bitrate =
        editor->findChild<QSpinBox*>(QStringLiteral("bench-preset-editor-bitrate"));
    auto* editor_buttons =
        editor->findChild<QDialogButtonBox*>(QStringLiteral("bench-preset-editor-buttons"));
    QVERIFY(editor_name != nullptr && editor_format != nullptr && editor_bitrate != nullptr &&
            editor_buttons != nullptr);
    QVERIFY(!editor_buttons->button(QDialogButtonBox::Save)->isEnabled());
    editor_name->setText(QStringLiteral("Phone Opus"));
    editor_format->setCurrentIndex(1);
    editor_bitrate->setValue(96);
    channels->setCurrentIndex(1);
    gain_mode->setCurrentIndex(1);
    QVERIFY(editor_buttons->button(QDialogButtonBox::Save)->isEnabled());
    QTest::mouseClick(editor_buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QTRY_COMPARE(preset_catalog.size(), 1U);
    QCOMPARE(preset_catalog.front().preset.codec_name, std::string{"libopus"});
    QCOMPARE(preset_catalog.front().preset.bit_rate, std::optional<std::int64_t>{96'000});
    // Separator + the saved preset, selected as the current choice.
    QTRY_COMPARE(preset->count(), builtin_count + 2);
    QCOMPARE(preset->currentText(), QStringLiteral("Phone Opus"));
    QTRY_VERIFY(preset_delete->isVisible());
    // Job settings restore, but permanent gain is never restored (including
    // legacy presets that still contain a gain field).
    const auto gain_key =
        QStringLiteral("convert/job-presets/%1/gain").arg(preset->currentData().toString());
    QVERIFY(!QSettings{}.contains(gain_key));
    QSettings{}.setValue(gain_key, 2);
    preset->setCurrentIndex(0);
    channels->setCurrentIndex(0);
    gain_mode->setCurrentIndex(1);
    preset->setCurrentIndex(preset->count() - 1);
    QCOMPARE(channels->currentData().toInt(), 1);
    QCOMPARE(gain_mode->currentData().toInt(), 0);
    QTest::mouseClick(preset_delete, Qt::LeftButton);
    QTRY_VERIFY(preset_catalog.empty());
    QTRY_COMPARE(preset->count(), builtin_count);
    QTRY_VERIFY(!preset_delete->isVisible());

    preset->setCurrentIndex(preset->findData(QStringLiteral("opus-192")));
    // Preset selection cannot enable permanent gain.
    gain_mode->setCurrentIndex(gain_mode->findData(0));
    root->setText(destination.path());
    directories->setText(QStringLiteral("%album%"));
    names->setText(QStringLiteral("%tracknumber% - %title%"));
    QTRY_VERIFY(preview->count() > 0 &&
                preview->item(0)->text() == QStringLiteral("Converted Album/01 - Loud.opus"));
    QCOMPARE(preview->count(), 2);
    QCOMPARE(preview->item(1)->text(), QStringLiteral("Converted Album/02 - Quiet.opus"));
    QTRY_VERIFY(run->isEnabled());

    // A colliding layout blocks Convert; the explanation scrolls in the
    // bounded problems pane while the status stays a one-line count.
    names->setText(QStringLiteral("same"));
    QTRY_VERIFY(!run->isEnabled());
    QTRY_VERIFY(problems->isVisible() &&
                problems->toPlainText().contains(QStringLiteral("target")));
    QVERIFY(status->text().contains(QStringLiteral("problem")));
    names->setText(QStringLiteral("%tracknumber% - %title%"));
    QTRY_VERIFY(run->isEnabled());
    QTRY_VERIFY(!problems->isVisible());

    QTest::mouseClick(run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("Converted 2 of 2 files.")),
                             15'000);
    const auto album_dir = QDir{destination.path()}.filePath(QStringLiteral("Converted Album"));
    QVERIFY(QFileInfo::exists(album_dir + QStringLiteral("/01 - Loud.opus")));
    QVERIFY(QFileInfo::exists(album_dir + QStringLiteral("/02 - Quiet.opus")));
    const auto encoded = QFile::encodeName(album_dir + QStringLiteral("/02 - Quiet.opus"));
    const auto reread = metadata::read_local_metadata(
        std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())});
    QVERIFY(reread.has_value());
    QCOMPARE(reread->document.first_effective_value("title"), std::optional<std::string>{"Quiet"});

    // Artwork carriage is a persisted opt-out (ADR-0131), enabled by default.
    auto* embed_artwork = dialog->findChild<QCheckBox*>(QStringLiteral("bench-convert-artwork"));
    QVERIFY(embed_artwork != nullptr);
    QVERIFY(embed_artwork->isChecked());

    // The resampling and bit-depth choices reach the pipeline: FLAC at a
    // forced 96 kHz stored as dithered 16-bit.
    auto* resample = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-resample"));
    auto* bit_depth = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-bit-depth"));
    QVERIFY(resample != nullptr && bit_depth != nullptr);
    QCOMPARE(resample->currentData().toInt(), 0);
    QCOMPARE(bit_depth->currentData().toInt(), 0);
    resample->setCurrentIndex(resample->findData(96'000));
    bit_depth->setCurrentIndex(bit_depth->findData(16));
    preset->setCurrentIndex(preset->findData(QStringLiteral("flac")));
    names->setText(QStringLiteral("%tracknumber% - %title% 96k"));
    QTRY_VERIFY(preview->count() > 0 &&
                preview->item(0)->text() == QStringLiteral("Converted Album/01 - Loud 96k.flac"));
    QTRY_VERIFY(run->isEnabled());
    QTest::mouseClick(run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("Converted 2 of 2 files.")),
                             15'000);
    const auto resampled_path = album_dir + QStringLiteral("/01 - Loud 96k.flac");
    QVERIFY(QFileInfo::exists(resampled_path));
    const auto resampled_encoded = QFile::encodeName(resampled_path);
    auto resampled = formats::AudioDecoder::open(std::string{
        resampled_encoded.constData(), static_cast<std::size_t>(resampled_encoded.size())});
    QVERIFY(resampled.has_value());
    QCOMPARE(resampled->output_format().sample_rate, 96'000);
    const auto probed = formats::probe_local_media(std::string{
        resampled_encoded.constData(), static_cast<std::size_t>(resampled_encoded.size())});
    QVERIFY(probed.has_value() && probed->best_audio_stream.has_value());
    QCOMPARE(
        probed->audio_streams[static_cast<std::size_t>(*probed->best_audio_stream)].sample_format,
        std::string{"s16"});

    // Mirror mode (ADR-0132) disables the expressions and previews the
    // source structure below the inferred common root.
    auto* mirror = dialog->findChild<QCheckBox*>(QStringLiteral("bench-convert-mirror"));
    auto* directory_field =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-directory-expression"));
    QVERIFY(mirror != nullptr && directory_field != nullptr);
    QVERIFY(!mirror->isChecked());
    mirror->setChecked(true);
    QVERIFY(!directory_field->isEnabled());
    QTRY_VERIFY(preview->count() >= 3 && preview->item(0)->text().startsWith(
                                             QStringLiteral("Recreating full source paths")));

    // ADR-0154: mirror recreates the complete source path beneath the
    // destination, so single-album selections keep their folders too.
    const auto media_relative = QString{media.path()}.mid(1) + QStringLiteral("/loud.flac");
    QTRY_VERIFY(preview->count() >= 2 && preview->item(1)->text() == media_relative);
    mirror->setChecked(false);
    QVERIFY(directory_field->isEnabled());
    delete dialog;
}

// ADR-0237 stage 6: a file of an engine elsewhere is fetched first and
// converted from the copy; the preview and the output are as for any file.
void BenchMainWindowTest::Convert::convertDialogFetchesFilesItCannotReach() {
    QTemporaryDir media;
    QTemporaryDir destination;
    QVERIFY(media.isValid());
    QVERIFY(destination.isValid());
    const auto original = media.filePath(QStringLiteral("original.wav"));
    write_sine_wav_fixture(original, 0.5);
    std::vector<std::filesystem::path> fetched_to;
    ConvertDialogItem item{
        .raw_path = "/srv/elsewhere/Album/fetched.wav",
        .selection = {},
        .segment = {},
        .source_revision = std::nullopt,
        .metadata =
            metadata::MetadataDocument{
                .fields = {metadata::MetadataField{.canonical_name = "title",
                                                   .native_name = "TITLE",
                                                   .values = {"Fetched"},
                                                   .qualifier = {},
                                                   .provenance =
                                                       metadata::FieldProvenance::embedded}},
                .unsupported_native_objects = {},
            },
        .label = QStringLiteral("Fetched"),
        .fetch = [&fetched_to, original](const std::filesystem::path& to,
                                         const core::CancellationToken&) -> core::Result<void> {
            fetched_to.push_back(to);
            std::error_code copied;
            std::filesystem::copy_file(QFile::encodeName(original).toStdString(), to, copied);
            if (copied) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::io, .message = copied.message(), .context = {}});
            }
            return {};
        }};
    auto* dialog = new ConvertDialog({std::move(item)},
                                     [](auto completion) { completion({}, {}, QString{}); }, {});
    dialog->show();
    auto* preset = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-preset"));
    auto* root = dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-destination"));
    auto* directories =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-directory-expression"));
    auto* names =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-basename-expression"));
    auto* mirror = dialog->findChild<QCheckBox*>(QStringLiteral("bench-convert-mirror"));
    auto* gain_mode = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-gain"));
    auto* preview = dialog->findChild<QListWidget*>(QStringLiteral("bench-convert-preview"));
    auto* run = dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-run"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-convert-status"));
    QVERIFY(preset != nullptr && root != nullptr && directories != nullptr && names != nullptr &&
            mirror != nullptr && gain_mode != nullptr && preview != nullptr && run != nullptr &&
            status != nullptr);
    mirror->setChecked(false);
    gain_mode->setCurrentIndex(gain_mode->findData(0));
    preset->setCurrentIndex(preset->findData(QStringLiteral("flac")));
    root->setText(destination.path());
    directories->setText({});
    names->setText(QStringLiteral("%title%"));
    // Planned from what the engine knows of it, before anything is fetched.
    QTRY_VERIFY(preview->count() > 0 && preview->item(0)->text() == QStringLiteral("Fetched.flac"));
    QVERIFY(fetched_to.empty());
    QTRY_VERIFY(run->isEnabled());
    QTest::mouseClick(run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("Converted 1 of 1 files.")),
                             15'000);
    QCOMPARE(fetched_to.size(), 1U);
    QCOMPARE(fetched_to.front().filename().string(), std::string{"fetched.wav"});
    QVERIFY(QFileInfo::exists(QDir{destination.path()}.filePath(QStringLiteral("Fetched.flac"))));
    // The copy goes once converted.
    QVERIFY(!std::filesystem::exists(fetched_to.front()));
    delete dialog;
}

// ADR-0173: the dialog's Gain choice permanently applies the item's
// ReplayGain metadata to the encoded PCM, exactly like the core path.
void BenchMainWindowTest::Convert::convertDialogAppliesPermanentReplayGain() {
    QSettings{}.setValue(QStringLiteral("convert/gain"), 2);
    QTemporaryDir media;
    QTemporaryDir destination;
    QVERIFY(media.isValid());
    QVERIFY(destination.isValid());
    const auto source_path = media.filePath(QStringLiteral("gained.wav"));
    write_sine_wav_fixture(source_path, 0.8);

    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto encoded_source = QFile::encodeName(source_path);
    ConvertDialogItem item{
        .raw_path = std::string{encoded_source.constData(),
                                static_cast<std::size_t>(encoded_source.size())},
        .selection = {},
        .segment = {},
        .source_revision = {},
        .metadata =
            metadata::MetadataDocument{
                .fields = {field("TITLE", {"Gained"}),
                           field("REPLAYGAIN_TRACK_GAIN", {"-12.04 dB"}),
                           field("REPLAYGAIN_TRACK_PEAK", {"0.8"})},
                .unsupported_native_objects = {},
            },
        .label = QStringLiteral("Gained"),
        .fetch = {},
    };
    ConvertPresetStore preset_store{
        .load = [](ConvertPresetStore::LoadCompletion completion) { completion({}, QString{}); },
        .save = [](persistence::SavedEncoderPreset,
                   ConvertPresetStore::Completion completion) { completion(QString{}); },
        .remove = [](core::StableId,
                     ConvertPresetStore::Completion completion) { completion(QString{}); },
    };
    auto* dialog = new ConvertDialog(
        {std::move(item)}, [](auto completion) { completion({}, {}, QString{}); }, preset_store);
    dialog->show();
    auto* preset = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-preset"));
    auto* root = dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-destination"));
    auto* directories =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-directory-expression"));
    auto* names =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-basename-expression"));
    auto* gain_mode = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-gain"));
    auto* run = dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-run"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-convert-status"));
    auto* mirror = dialog->findChild<QCheckBox*>(QStringLiteral("bench-convert-mirror"));
    QVERIFY(preset != nullptr && root != nullptr && names != nullptr && gain_mode != nullptr);
    QVERIFY(run != nullptr && status != nullptr && directories != nullptr && mirror != nullptr);
    QCOMPARE(gain_mode->currentData().toInt(), 0);
    auto* warning = dialog->findChild<QLabel*>(QStringLiteral("bench-convert-gain-warning"));
    QVERIFY(warning);
    mirror->setChecked(false);
    preset->setCurrentIndex(preset->findData(QStringLiteral("flac")));
    root->setText(destination.path());
    directories->setText(QString{});
    names->setText(QStringLiteral("%title%"));
    gain_mode->setCurrentIndex(gain_mode->findData(1));
    QVERIFY(warning->text().contains(QStringLiteral("permanently changes the audio samples")));
    QTRY_VERIFY(run->isEnabled());
    QTimer::singleShot(0, dialog, [dialog] {
        auto* confirmation =
            dialog->findChild<QMessageBox*>(QStringLiteral("bench-convert-gain-confirmation"));
        QVERIFY(confirmation);
        QCOMPARE(confirmation->standardButton(confirmation->defaultButton()), QMessageBox::Cancel);
        confirmation->button(QMessageBox::Cancel)->click();
    });
    QTest::mouseClick(run, Qt::LeftButton);
    QVERIFY(!QFileInfo::exists(destination.filePath(QStringLiteral("Gained.flac"))));
    QVERIFY(run->isVisible());
    QTimer::singleShot(0, dialog, [dialog] {
        auto* confirmation =
            dialog->findChild<QMessageBox*>(QStringLiteral("bench-convert-gain-confirmation"));
        QVERIFY(confirmation);
        confirmation->button(QMessageBox::Yes)->click();
    });
    QTest::mouseClick(run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("Converted 1 of 1 files.")),
                             15'000);

    const auto converted =
        QFile::encodeName(QDir{destination.path()}.filePath(QStringLiteral("Gained.flac")));
    auto decoder = formats::AudioDecoder::open(
        std::string{converted.constData(), static_cast<std::size_t>(converted.size())});
    QVERIFY(decoder.has_value());
    float peak = 0.0F;
    while (true) {
        auto chunk = decoder->next_chunk();
        QVERIFY(chunk.has_value());
        if (!*chunk) {
            break;
        }
        for (const auto sample : (*chunk)->interleaved_samples) {
            peak = std::max(peak, std::abs(sample));
        }
    }
    // -12.04 dB over a 0.8 peak must land near 0.2; an untouched copy at
    // 0.8 means the dialog's gain option silently did nothing.
    QVERIFY2(peak > 0.17F && peak < 0.23F, qPrintable(QString::number(static_cast<double>(peak))));
    delete dialog;
    ConvertDialog reopened{{}};
    QCOMPARE(
        reopened.findChild<QComboBox*>(QStringLiteral("bench-convert-gain"))->currentData().toInt(),
        0);
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::Convert)

#include "window_convert_test.moc"
