// SPDX-License-Identifier: GPL-3.0-only

// The window test: covers: fetched, written, resized and shown.

#include "window_test.hpp"

#include "bench/file_scope_view.hpp"
#include "trackknife/core/engine_state.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "uicommon/library_tree_view.hpp"
#include "workspace/artwork_session.hpp"
#include "workspace/startup.hpp"
#include "workspace/tab_store.hpp"

#include <QCheckBox>
#include <QPainter>
#include <QStandardItemModel>

namespace trackknife::bench {

class BenchMainWindowTest::Artwork final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void coversAreResizedBeforeTheyTravel();
    void localArtworkSurvivesInvalidation();
    void artworkFetchesCoverArtFromArchiveAndAddsFront_data();
    void artworkFetchesCoverArtFromArchiveAndAddsFront();
    void closingWaitsForACoverThenCloses();
    void coverPolicyRoundTrip();
    void coverThumbnailAppliesPolicy_data();
    void coverThumbnailAppliesPolicy();
    void metadataApplyCombinesTagsAndArtwork_data();
    void metadataApplyCombinesTagsAndArtwork();
    void artworkArchivePickerAddsChosenImageWithItsRole();
    void aCoverBesideTheFilesIsOfferedNotShown();
    void metadataPropertiesArtworkSectionShowsProvenanceAndCapabilities();
    void artworkOfAWholeReleaseIsOneRowAndSkipsFilesWithoutCovers();
    void metadataPropertiesArtworkRemoveReviewsAppliesAndRefreshes_data();
    void metadataPropertiesArtworkRemoveReviewsAppliesAndRefreshes();
};

void BenchMainWindowTest::Artwork::localArtworkSurvivesInvalidation() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto flac = media.filePath(QStringLiteral("one.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), flac));
    {
        QImage cover{32, 32, QImage::Format_RGB32};
        cover.fill(Qt::darkRed);
        QVERIFY(cover.save(media.filePath(QStringLiteral("cover.png")), "PNG"));
    }

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({QFile::encodeName(flac).toStdString()});
    QTableView* view = nullptr;
    LocalListModel* model = nullptr;
    QTRY_VERIFY([&] {
        for (auto* candidate : window.findChildren<QTableView*>()) {
            if (qobject_cast<LocalListModel*>(candidate->model()) &&
                candidate->model()->rowCount() == 1) {
                view = candidate;
                model = qobject_cast<LocalListModel*>(candidate->model());
                return true;
            }
        }
        return false;
    }());
    if (model == nullptr) {
        QFAIL("the list's model is found");
    }
    QTRY_VERIFY(model->rows().front().probed);
    const auto artwork = [&] {
        return model->data(model->index(0, 0), static_cast<int>(ui::track_album_artwork_role))
            .value<QImage>();
    };
    QTRY_VERIFY(!artwork().isNull());

    window.invalidateArtwork(model->rows().front().raw_path);
    QTRY_VERIFY(!artwork().isNull());
}

// ADR-0257: the tagger opens on the tags the list cached, at once, and reads
// the files behind them. A value typed meanwhile is kept when the file's own
// tags take their place; scripts and Save wait for them.
// A cover picked on this computer for an engine's files is resized here, to
// the largest size any destination keeps, before it travels: what the engine
// would shrink anyway is not sent at full size -- an original past the
// engine's line limit could not be sent at all.
void BenchMainWindowTest::Artwork::coversAreResizedBeforeTheyTravel() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("scan.jpg"));
    QImage scan{2000, 1600, QImage::Format_RGB32};
    for (int y = 0; y < scan.height(); ++y) {
        for (int x = 0; x < scan.width(); ++x) {
            scan.setPixel(x, y, qRgb((x * 7) % 256, (y * 13) % 256, ((x + y) * 3) % 256));
        }
    }
    QVERIFY(scan.save(path, "JPEG", 95));
    std::vector<QSize> sent;
    FileWorkTools tools;
    tools.stage = [&sent](const std::span<const unsigned char> bytes)
        -> core::Result<metadata::ArtworkImageFile> {
        QImage arrived;
        arrived.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                             static_cast<int>(bytes.size()));
        sent.push_back(arrived.size());
        metadata::ArtworkImageFile there;
        there.raw_path = "/engine/staged.jpg";
        return there;
    };
    const auto send = [&](const metadata::ArtworkStoragePolicy& policy) {
        metadata::ArtworkWritePlanIntent intent;
        intent.kind = metadata::ArtworkWritePlanIntentKind::replace;
        intent.replacement_raw_path = QFile::encodeName(path).toStdString();
        auto staged = stageReplacements({intent}, tools, {}, policy);
        return staged && staged->front().replacement_raw_path == std::string{"/engine/staged.jpg"};
    };
    metadata::ArtworkStoragePolicy policy;
    policy.embed = true;
    policy.write_folder_image = true;
    policy.max_embedded_edge = 500U;
    policy.max_folder_edge = 1000U;
    QCOMPARE(largestCoverEdge(policy), 1000U);
    QVERIFY(send(policy));
    QCOMPARE(sent.back(), QSize(1000, 800));
    // A folder image kept as it is: the original has to travel.
    policy.max_folder_edge = 0U;
    QCOMPARE(largestCoverEdge(policy), 0U);
    QVERIFY(send(policy));
    QCOMPARE(sent.back(), QSize(2000, 1600));
    // Embedded only: the embedded limit.
    policy.write_folder_image = false;
    QVERIFY(send(policy));
    QCOMPARE(sent.back(), QSize(500, 400));
}

void BenchMainWindowTest::Artwork::metadataApplyCombinesTagsAndArtwork_data() {
    QTest::addColumn<bool>("save_tags");
    QTest::newRow("tags-and-covers") << true;
    QTest::newRow("covers-with-tags-disabled") << false;
}

void BenchMainWindowTest::Artwork::coverPolicyRoundTrip() {
    QSettings{}.setValue(QStringLiteral("convert/embed-artwork"), false);
    SettingsDialog dialog;
    dialog.showPage(SettingsDialog::Page::covers);
    dialog.show();
    auto* embed = dialog.findChild<QCheckBox*>(QStringLiteral("bench-artwork-embed"));
    auto* folder = dialog.findChild<QCheckBox*>(QStringLiteral("bench-artwork-folder-image"));
    auto* name = dialog.findChild<QComboBox*>(QStringLiteral("bench-artwork-folder-image-name"));
    auto* buttons = dialog.findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"));
    auto* embedded_edge =
        dialog.findChild<QSpinBox*>(QStringLiteral("bench-artwork-max-embedded-edge"));
    auto* folder_edge =
        dialog.findChild<QSpinBox*>(QStringLiteral("bench-artwork-max-folder-edge"));
    QVERIFY(embed && folder && name && buttons && embedded_edge && folder_edge);
    QVERIFY(embed->isChecked());
    QVERIFY(!folder->isChecked());
    // ADR-0317: 1000 px until another limit is chosen.
    QCOMPARE(embedded_edge->value(), 1000);
    QCOMPARE(SettingsDialog::artworkPolicy().max_embedded_edge, 1000U);
    embed->setChecked(false);
    folder->setChecked(true);
    name->setCurrentText(QStringLiteral("front.jpg"));
    embedded_edge->setValue(1000);
    folder_edge->setValue(3000);
    buttons->button(QDialogButtonBox::Save)->click();
    const auto policy = SettingsDialog::artworkPolicy();
    QVERIFY(!policy.embed && policy.write_folder_image);
    QCOMPARE(policy.max_embedded_edge, 1000U);
    QCOMPARE(policy.max_folder_edge, 3000U);
    QCOMPARE(policy.folder_image_name, std::string{"front.jpg"});
    QCOMPARE(policy.fetch_source, std::string{"coverartarchive+bandcamp"});
    SettingsDialog reopened;
    QVERIFY(!reopened.findChild<QCheckBox*>(QStringLiteral("bench-artwork-embed"))->isChecked());
    QVERIFY(
        reopened.findChild<QCheckBox*>(QStringLiteral("bench-artwork-folder-image"))->isChecked());
    QCOMPARE(reopened.findChild<QComboBox*>(QStringLiteral("bench-artwork-folder-image-name"))
                 ->currentText(),
             QStringLiteral("front.jpg"));
    QCOMPARE(
        reopened.findChild<QSpinBox*>(QStringLiteral("bench-artwork-max-folder-edge"))->value(),
        3000);
    QVERIFY(!QSettings{}.value(QStringLiteral("convert/embed-artwork")).toBool());
}

void BenchMainWindowTest::Artwork::coverThumbnailAppliesPolicy_data() {
    QTest::addColumn<bool>("embed");
    QTest::newRow("embed-and-folder") << true;
    QTest::newRow("folder-only") << false;
}

void BenchMainWindowTest::Artwork::coverThumbnailAppliesPolicy() {
    QFETCH(bool, embed);
    const bool save_tags = false;
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_embed_key), embed);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_folder_image_key), true);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_folder_image_name_key),
                         QStringLiteral("folder.jpg"));
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("album.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), path));
    const auto raw = QFile::encodeName(path).toStdString();
    const auto seed_image = media.filePath(QStringLiteral("seed.jpg"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("external-blue-jpeg.b64"), seed_image));
    const auto initial = metadata::read_local_artwork_inventory(raw);
    QVERIFY(initial.has_value());
    metadata::ArtworkWritePlanIntent seed{.occurrence_index = 0,
                                          .raw_media_path = raw,
                                          .expected_media_revision = initial->media_revision,
                                          .target_ordinal = 0,
                                          .expected_target_fingerprint = {},
                                          .kind = metadata::ArtworkWritePlanIntentKind::add,
                                          .replacement_raw_path =
                                              QFile::encodeName(seed_image).toStdString(),
                                          .added_role = metadata::ArtworkRole::front,
                                          .added_description = {},
                                          .replacement_embedded_source = std::nullopt};
    auto seed_plan = metadata::revalidate_artwork_write_plan({seed});
    auto seed_journal = persistence::SqliteMetadataOperationJournal::open(
        std::filesystem::path{media.filePath(QStringLiteral("seed.sqlite3")).toStdString()});
    QVERIFY(seed_plan && seed_plan->ready() && seed_journal);
    QVERIFY(operations::commit_artwork_source(
        seed_plan->sources.front(), *seed_journal,
        [](const operations::MetadataCommitResult&) -> core::Result<void> { return {}; }));
    const auto before = metadata::read_local_metadata(raw);
    QVERIFY(before.has_value());
    const MetadataPropertiesSource source{.source = {.raw_path = raw,
                                                     .source_revision = before->source_revision,
                                                     .baseline = before->document},
                                          .track_label = QStringLiteral("Combined save")};
    const std::filesystem::path database{
        media.filePath(QStringLiteral("journal.sqlite3")).toStdString()};
    std::optional<operations::MetadataApplyResult> observed;
    auto* dialog = new MetadataPropertiesDialog(
        1, [source](std::size_t) -> std::optional<MetadataPropertiesSource> { return source; }, {},
        [database] {
            return MetadataWritePlanApplier{
                [database](const metadata::MetadataWritePlan& plan,
                           const operations::MetadataApplyProgressCallback& progress,
                           const core::CancellationToken& cancellation)
                    -> core::Result<operations::MetadataApplyResult> {
                    auto journal = persistence::SqliteMetadataOperationJournal::open(database);
                    if (!journal) {
                        return std::unexpected(journal.error());
                    }
                    return operations::apply_metadata_write_plan(
                        plan,
                        [&journal](const metadata::MetadataWritePlanSource& item,
                                   const core::CancellationToken& token) {
                            return operations::commit_metadata_source(
                                item, *journal,
                                [](const operations::MetadataCommitResult&) -> core::Result<void> {
                                    return {};
                                },
                                token);
                        },
                        {}, {}, progress, cancellation);
                }};
        },
        [&observed](const operations::MetadataApplyResult& result) { observed = result; });
    dialog->setArtworkMutationServices([] { return ArtworkWritePlanApplier{}; }, {});
    dialog->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = dialog->fileListView()) != nullptr);
    auto* grid = qobject_cast<MetadataGridModel*>(files->model());
    QVERIFY(grid != nullptr);
    const auto title = grid->ensureField(QStringLiteral("TITLE"));
    QVERIFY(title.has_value());
    const std::array items{std::size_t{0}};
    QVERIFY(grid->replaceFieldValues(items, title->field_index, {"One apply"}));
    auto* save_tags_box =
        dialog->findChild<QCheckBox*>(QStringLiteral("bench-preparation-save-tags"));
    QVERIFY(save_tags_box != nullptr);
    save_tags_box->setChecked(save_tags);
    auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("bench-metadata-sections"));
    QVERIFY(tabs != nullptr);
    QCOMPARE(tabs->currentIndex(), 0);
    auto* thumbnail = dialog->findChild<CoverThumbnail*>();
    auto* section = dialog->findChild<MetadataArtworkSection*>();
    auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(thumbnail && section && apply);
    QTRY_VERIFY(thumbnail->property("cover-editable").toBool());
    QTRY_VERIFY(!thumbnail->pixmap().isNull());
    QImage image(24, 24, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    const auto incoming = media.filePath(QStringLiteral("incoming.png"));
    QVERIFY(image.save(incoming));
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(incoming)});
    QDragEnterEvent enter(dragPosition(QPoint(10, 10)), Qt::CopyAction, &mime, Qt::LeftButton,
                          Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &drop);
    QVERIFY(drop.isAccepted());
    auto intents = section->pendingIntents();
    QCOMPARE(intents.size(), embed ? 2U : 1U);
    QCOMPARE(intents.back().kind, metadata::ArtworkWritePlanIntentKind::add);
    QCOMPARE(intents.back().added_role, metadata::ArtworkRole::front);
    section->discardPendingChanges();
    if (embed) {
        section->removeFrontCover();
        QCOMPARE(section->pendingIntents().size(), 1U);
        QCOMPARE(section->pendingIntents().front().kind,
                 metadata::ArtworkWritePlanIntentKind::remove);
        section->discardPendingChanges();
    }
    QApplication::clipboard()->setImage(image);
    QTest::keyClick(thumbnail, Qt::Key_V, Qt::ControlModifier);
    releaseHeldModifiers(thumbnail);
    QTRY_VERIFY(!section->isBusy());
    QTRY_VERIFY(section->hasPendingChanges());
    QCOMPARE(section->pendingIntents().size(), embed ? 2U : 1U);
    // ADR-0317: written as a JPEG, whatever was pasted.
    const auto destination = media.filePath(QStringLiteral("folder.jpg"));
    QVERIFY(!QFile::exists(destination));
    QVERIFY(!observed.has_value());
    QTRY_VERIFY(apply->isEnabled());
    QPointer guard{dialog};
    apply->click();
    QDialog* review = nullptr;
    QTRY_VERIFY((review = dialog->findChild<QDialog*>(
                     QStringLiteral("bench-folder-cover-review"))) != nullptr);
    QVERIFY(!observed.has_value());
    QVERIFY(!QFile::exists(destination));
    auto* review_buttons =
        review->findChild<QDialogButtonBox*>(QStringLiteral("bench-folder-cover-review-buttons"));
    QVERIFY(review_buttons != nullptr);
    QPointer<QDialog> cancelled_review{review};
    review_buttons->button(QDialogButtonBox::Cancel)->click();
    QTRY_VERIFY(cancelled_review.isNull());
    QVERIFY(!QFile::exists(destination));
    QVERIFY(section->hasPendingChanges());
    apply->click();
    QTRY_VERIFY((review = dialog->findChild<QDialog*>(
                     QStringLiteral("bench-folder-cover-review"))) != nullptr);
    review_buttons =
        review->findChild<QDialogButtonBox*>(QStringLiteral("bench-folder-cover-review-buttons"));
    QVERIFY(review_buttons != nullptr);
    review_buttons->button(QDialogButtonBox::Save)->click();
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 10000);
    QVERIFY2(observed->committed_source_count() == 1U,
             qPrintable(observed->sources.front().issue
                            ? QString::fromStdString(observed->sources.front().issue->message)
                            : QString{}));
    QVERIFY(window_test::closeTaggerAfterWrite(guard));
    const auto after = metadata::read_local_metadata(raw);
    const auto artwork = metadata::read_local_artwork_inventory(raw);
    QVERIFY(after && artwork);
    QVERIFY(QFile::exists(destination));
    auto folder = metadata::read_artwork_image_file(QFile::encodeName(destination).toStdString());
    QVERIFY(folder.has_value());
    const auto embedded = std::ranges::find_if(artwork->items, [](const auto& item) {
        return item.provenance == metadata::ArtworkProvenance::embedded &&
               item.role == metadata::ArtworkRole::front;
    });
    QVERIFY(embedded != artwork->items.end());
    QCOMPARE(embedded->content_fingerprint == folder->content_fingerprint, embed);
    if (!embed)
        QCOMPARE(after->source_revision, before->source_revision);
    QCOMPARE(after->document.effective_values("title"),
             save_tags ? std::vector<std::string>{"One apply"}
                       : before->document.effective_values("title"));
    auto journal = persistence::SqliteMetadataOperationJournal::open(database);
    QVERIFY(journal.has_value());
    const auto backups = journal->load_backups();
    QVERIFY(backups.has_value());
    QCOMPARE(backups->size(), embed ? 2U : 1U);
}

void BenchMainWindowTest::Artwork::metadataApplyCombinesTagsAndArtwork() {
    QFETCH(bool, save_tags);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("album.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), path));
    const auto raw = QFile::encodeName(path).toStdString();
    const auto before = metadata::read_local_metadata(raw);
    QVERIFY(before.has_value());
    const MetadataPropertiesSource source{.source = {.raw_path = raw,
                                                     .source_revision = before->source_revision,
                                                     .baseline = before->document},
                                          .track_label = QStringLiteral("Combined save")};
    const std::filesystem::path database{
        media.filePath(QStringLiteral("journal.sqlite3")).toStdString()};
    std::optional<operations::MetadataApplyResult> observed;
    auto* dialog = new MetadataPropertiesDialog(
        1, [source](std::size_t) -> std::optional<MetadataPropertiesSource> { return source; }, {},
        [database] {
            return MetadataWritePlanApplier{
                [database](const metadata::MetadataWritePlan& plan,
                           const operations::MetadataApplyProgressCallback& progress,
                           const core::CancellationToken& cancellation)
                    -> core::Result<operations::MetadataApplyResult> {
                    auto journal = persistence::SqliteMetadataOperationJournal::open(database);
                    if (!journal) {
                        return std::unexpected(journal.error());
                    }
                    return operations::apply_metadata_write_plan(
                        plan,
                        [&journal](const metadata::MetadataWritePlanSource& item,
                                   const core::CancellationToken& token) {
                            return operations::commit_metadata_source(
                                item, *journal,
                                [](const operations::MetadataCommitResult&) -> core::Result<void> {
                                    return {};
                                },
                                token);
                        },
                        {}, {}, progress, cancellation);
                }};
        },
        [&observed](const operations::MetadataApplyResult& result) { observed = result; });
    dialog->setArtworkMutationServices([] { return ArtworkWritePlanApplier{}; }, {});
    dialog->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = dialog->fileListView()) != nullptr);
    auto* grid = qobject_cast<MetadataGridModel*>(files->model());
    QVERIFY(grid != nullptr);
    const auto title = grid->ensureField(QStringLiteral("TITLE"));
    QVERIFY(title.has_value());
    const std::array items{std::size_t{0}};
    QVERIFY(grid->replaceFieldValues(items, title->field_index, {"One apply"}));
    auto* save_tags_box =
        dialog->findChild<QCheckBox*>(QStringLiteral("bench-preparation-save-tags"));
    QVERIFY(save_tags_box != nullptr);
    save_tags_box->setChecked(save_tags);
    auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("bench-metadata-sections"));
    QVERIFY(tabs != nullptr);
    tabs->setCurrentIndex(1);
    auto* covers = dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-remove"));
    auto* old_save = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-save"));
    auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(covers && remove && old_save && apply);
    QTRY_COMPARE(covers->model()->rowCount(), 1);
    QVERIFY(!old_save->isVisible());
    covers->selectAll();
    QTRY_VERIFY(remove->isEnabled());
    remove->click();
    auto* pending =
        dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-pending"));
    QVERIFY(pending != nullptr);
    QCOMPARE(pending->model()->rowCount(), 1);
    QVERIFY(!pending->model()->index(0, 4).data(Qt::DecorationRole).value<QImage>().isNull());
    QCOMPARE(pending->model()->index(0, 5).data().toString(), QStringLiteral("Removed"));
    QVERIFY(!observed.has_value());
    QCOMPARE(metadata::read_local_artwork_inventory(raw)->items.size(), 1U);
    QTRY_VERIFY(apply->isEnabled());
    QPointer guard{dialog};
    apply->click();
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 10000);
    QVERIFY2(observed->committed_source_count() == 1U,
             qPrintable(observed->sources.front().issue
                            ? QString::fromStdString(observed->sources.front().issue->message)
                            : QString{}));
    QVERIFY(window_test::closeTaggerAfterWrite(guard));
    const auto after = metadata::read_local_metadata(raw);
    const auto artwork = metadata::read_local_artwork_inventory(raw);
    QVERIFY(after && artwork);
    QVERIFY(artwork->items.empty());
    QCOMPARE(after->document.effective_values("title"),
             save_tags ? std::vector<std::string>{"One apply"}
                       : before->document.effective_values("title"));
    auto journal = persistence::SqliteMetadataOperationJournal::open(database);
    QVERIFY(journal.has_value());
    const auto backups = journal->load_backups();
    QVERIFY(backups.has_value());
    QCOMPARE(backups->size(), 1U);
}

// Closed while covers are being saved, the editor waits for the files to be
// safe -- and then closes by itself, not left open saying it is closing.
// A cover still being fetched is not waited for.
void BenchMainWindowTest::Artwork::closingWaitsForACoverThenCloses() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("01.flac"));
    // Tagged with release 33333333-…
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const MetadataPropertiesSource source{
        .source = metadata::StagedMetadataSource{.raw_path = raw_path,
                                                 .source_revision = read->source_revision,
                                                 .baseline = read->document},
        .track_label = QStringLiteral("01"),
    };
    QImage cover{16, 16, QImage::Format_ARGB32};
    cover.fill(Qt::darkCyan);
    QByteArray png;
    QBuffer buffer{&png};
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(cover.save(&buffer, "PNG"));
    buffer.close();
    // The image is held until the test lets it go.
    std::function<void(core::Result<QByteArray>)> held;
    const MusicBrainzLookupService service{
        .fetch =
            [&held](const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                if (url.startsWith(QStringLiteral("https://coverartarchive.org/release/")) &&
                    url.endsWith(QStringLiteral("/7.png"))) {
                    held = std::move(completion);
                    return;
                }
                if (url.startsWith(QStringLiteral("https://coverartarchive.org/release/"))) {
                    completion(QByteArray{R"json({"images": [{"id": 7, "front": true,
                        "approved": true, "types": ["Front"], "image":
                        "https://coverartarchive.org/release/33333333-3333-3333-3333-333333333333/7.png"}]})json"});
                    return;
                }
                completion(std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                                       .message = "unexpected url",
                                                       .context = {}}));
            },
        .fingerprint = {},
        .acoustid_lookup = {},
    };
    // The save is held until the test lets it go.
    auto released = std::make_shared<std::atomic_bool>(false);
    auto saving = std::make_shared<std::atomic_bool>(false);
    const auto open = [&] {
        QPointer properties = new MetadataPropertiesDialog(
            1U,
            [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
                return index == 0U ? std::optional{source} : std::nullopt;
            },
            {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
        properties->setAttribute(Qt::WA_DeleteOnClose);
        properties->setArtworkMutationServices(
            ArtworkWritePlanApplierFactory{[released, saving] {
                return ArtworkWritePlanApplier{
                    [released, saving](const metadata::ArtworkWritePlan&,
                                       const operations::ArtworkApplyProgressCallback&,
                                       const core::CancellationToken&)
                        -> core::Result<operations::ArtworkApplyResult> {
                        *saving = true;
                        while (!*released) {
                            std::this_thread::sleep_for(std::chrono::milliseconds{10});
                        }
                        return operations::ArtworkApplyResult{};
                    }};
            }},
            {});
        properties->show();
        return properties;
    };
    const auto choose = [](QWidget* properties) {
        QPushButton* fetch = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((fetch = properties->findChild<QPushButton*>(
                                      QStringLiteral("bench-metadata-cover-fetch"))) != nullptr &&
                                     fetch->isEnabled(),
                                 10'000);
        QTest::mouseClick(fetch, Qt::LeftButton);
        QVERIFY(chooseInCoverPicker(properties, QStringLiteral("Cover Art Archive"),
                                    QStringLiteral("Front")));
    };

    // Fetching: closed at once.
    auto fetching = open();
    choose(fetching);
    if (QTest::currentTestFailed()) {
        return;
    }
    QTRY_VERIFY_WITH_TIMEOUT(static_cast<bool>(held), 10'000);
    QVERIFY(fetching->close());
    QTRY_VERIFY(fetching.isNull());
    std::exchange(held, {})(std::unexpected(
        core::Error{.code = core::ErrorCode::cancelled, .message = "closed", .context = {}}));

    // Saving: waited for, saying so, then closed by itself.
    auto properties = open();
    choose(properties);
    if (QTest::currentTestFailed()) {
        return;
    }
    QTRY_VERIFY_WITH_TIMEOUT(static_cast<bool>(held), 10'000);
    std::exchange(held, {})(png);
    QPushButton* save = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((save = properties->findChild<QPushButton*>(
                                  QStringLiteral("bench-metadata-artwork-save"))) != nullptr &&
                                 save->isEnabled(),
                             10'000);
    QTest::mouseClick(save, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(saving->load(), 10'000);
    QVERIFY(!properties->close());
    QVERIFY(properties->isVisible());
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(status != nullptr);
    QTRY_VERIFY2(status->text().contains(QStringLiteral("closing once")),
                 qPrintable(status->text()));
    *released = true;
    QTRY_VERIFY_WITH_TIMEOUT(properties.isNull(), 10'000);
}

void BenchMainWindowTest::Artwork::artworkFetchesCoverArtFromArchiveAndAddsFront_data() {
    // ADR-0237: a downloaded cover is handed to the engine and written by it,
    // or written by this process -- the same section, the same result.
    QTest::addColumn<bool>("through_engine");
    QTest::newRow("engine") << true;
    QTest::newRow("this-process") << false;
}

void BenchMainWindowTest::Artwork::artworkFetchesCoverArtFromArchiveAndAddsFront() {
    // ADR-0317: covers are JPEGs: a colour within its loss.
    const auto jpeg_close = [](const QColor& shown, const QColor& expected) {
        return std::abs(shown.red() - expected.red()) <= 4 &&
               std::abs(shown.green() - expected.green()) <= 4 &&
               std::abs(shown.blue() - expected.blue()) <= 4;
    };
    QFETCH(bool, through_engine);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto media_path = media.filePath(QStringLiteral("cover-fetch.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), media_path));
    const auto encoded = QFile::encodeName(media_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const auto release_id = QStringLiteral("2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = raw_path,
                .source_revision = read->source_revision,
                .baseline = read->document,
            },
        .track_label = QStringLiteral("Cover fetch fixture"),
    };

    QImage cover{16, 16, QImage::Format_ARGB32};
    cover.fill(Qt::darkCyan);
    QByteArray png_bytes;
    QBuffer png_buffer{&png_bytes};
    QVERIFY(png_buffer.open(QIODevice::WriteOnly));
    QVERIFY(cover.save(&png_buffer, "PNG"));
    png_buffer.close();
    QImage second_cover{24, 24, QImage::Format_ARGB32};
    second_cover.fill(Qt::darkMagenta);
    QByteArray second_png_bytes;
    QBuffer second_png_buffer{&second_png_bytes};
    QVERIFY(second_png_buffer.open(QIODevice::WriteOnly));
    QVERIFY(second_cover.save(&second_png_buffer, "PNG"));
    second_png_buffer.close();
    QVERIFY(second_png_bytes.size() != png_bytes.size());
    int image_serves = 0;
    static constexpr auto listing_text = R"json({
      "images": [{"id": 42, "front": true, "approved": true, "types": ["Front"],
        "image":
          "http://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/42.png"}]
    })json";
    const QByteArray listing_body{listing_text};
    int fetches = 0;
    const MusicBrainzLookupService service{
        .fetch =
            [&fetches, &image_serves, listing_body, png_bytes, second_png_bytes, release_id](
                const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                ++fetches;
                if (url == QStringLiteral("https://coverartarchive.org/release/") + release_id) {
                    completion(listing_body);
                    return;
                }
                // The listing carried an http URL; the fetch must be the
                // https upgrade.
                if (url == QStringLiteral("https://coverartarchive.org/release/"
                                          "2f2ac1b7-1111-4f4f-8f8f-123456789abc/42.png")) {
                    completion(++image_serves == 1 ? png_bytes : second_png_bytes);
                    return;
                }
                completion(std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "unexpected url",
                    .context = {},
                }));
            },
        .fingerprint = {},
        .acoustid_lookup = {},
    };

    const auto database_path =
        std::filesystem::path{media.filePath(QStringLiteral("cover.sqlite3")).toStdString()};
    std::optional<operations::ArtworkApplyResult> observed;
    std::optional<operations::MetadataApplyResult> tags_observed;
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
        {},
        work ? MetadataWritePlanApplierFactory{[work] {
            return MetadataWritePlanApplier{
                [work](const metadata::MetadataWritePlan& plan,
                       const operations::MetadataApplyProgressCallback& progress,
                       const core::CancellationToken& cancellation) {
                    return work->apply(plan, progress, cancellation);
                }};
        }}
             : MetadataWritePlanApplierFactory{[database_path] {
                   return MetadataWritePlanApplier{
                       [database_path](const metadata::MetadataWritePlan& plan,
                                       const operations::MetadataApplyProgressCallback& progress,
                                       const core::CancellationToken& cancellation)
                           -> core::Result<operations::MetadataApplyResult> {
                           auto opened =
                               persistence::SqliteMetadataOperationJournal::open(database_path);
                           if (!opened) {
                               return std::unexpected(std::move(opened.error()));
                           }
                           auto journal = std::move(*opened);
                           return operations::apply_metadata_write_plan(
                               plan,
                               [&journal](const metadata::MetadataWritePlanSource& source_plan,
                                          const core::CancellationToken& source_cancellation) {
                                   return operations::commit_metadata_source(
                                       source_plan, journal,
                                       [](const operations::MetadataCommitResult&)
                                           -> core::Result<void> { return {}; },
                                       source_cancellation);
                               },
                               {}, {}, progress, cancellation);
                       }};
               }},
        [&tags_observed](const operations::MetadataApplyResult& result) { tags_observed = result; },
        {}, {}, {}, {}, nullptr, {}, service, work ? engineFileWorkTools(work) : FileWorkTools{});
    properties->setArtworkMutationServices(
        work ? ArtworkWritePlanApplierFactory{[work] {
            return ArtworkWritePlanApplier{
                [work](const metadata::ArtworkWritePlan& plan,
                       const operations::ArtworkApplyProgressCallback& progress,
                       const core::CancellationToken& cancellation) {
                    return work->artwork_apply(plan, progress, cancellation);
                }};
        }}
             : ArtworkWritePlanApplierFactory{[database_path] {
                   return ArtworkWritePlanApplier{
                       [database_path](const metadata::ArtworkWritePlan& plan,
                                       const operations::ArtworkApplyProgressCallback& progress,
                                       const core::CancellationToken& cancellation)
                           -> core::Result<operations::ArtworkApplyResult> {
                           auto opened =
                               persistence::SqliteMetadataOperationJournal::open(database_path);
                           if (!opened) {
                               return std::unexpected(std::move(opened.error()));
                           }
                           auto journal = std::move(*opened);
                           return operations::apply_artwork_write_plan(
                               plan,
                               [&journal](const metadata::ArtworkWritePlanSource& source_plan,
                                          const core::CancellationToken& source_cancellation) {
                                   return operations::commit_artwork_source(
                                       source_plan, journal,
                                       [](const operations::MetadataCommitResult&)
                                           -> core::Result<void> { return {}; },
                                       source_cancellation);
                               },
                               progress, cancellation,
                               operations::ArtworkApplyOptions{.maximum_parallelism = 2U});
                       }};
               }},
        [&observed](const operations::ArtworkApplyResult& result) { observed = result; });
    properties->show();

    QTabWidget* sections = nullptr;
    QTRY_VERIFY((sections = properties->findChild<QTabWidget*>(
                     QStringLiteral("bench-metadata-sections"))) != nullptr);
    auto* files = properties->fileListView();
    QVERIFY(files != nullptr);
    auto* grid = qobject_cast<MetadataGridModel*>(files->model());
    QVERIFY(grid != nullptr);
    const auto captured = grid->sharedSelection();
    // Stage the same provider preview used by Identify, including a new
    // release ID. The cover must be fetchable before these tags are saved.
    const metadata::MetadataProposalSet proposals{
        .provider_name = "MusicBrainz",
        .provider_detail = "Matched release",
        .items = {{.item_index = 0U,
                   .fields = {{.canonical_field =
                                   metadata::canonicalize_field_name("MUSICBRAINZ_ALBUMID"),
                               .display_field = "MUSICBRAINZ_ALBUMID",
                               .values = {release_id.toStdString()},
                               .confidence = 1.0,
                               .rationale = "Matched release"},
                              {.canonical_field = "title",
                               .display_field = "TITLE",
                               .values = {"Identified title"},
                               .confidence = 1.0,
                               .rationale = "Matched track"}},
                   .artwork = {}}}};
    const auto proposal =
        metadata::metadata_proposal_preview(grid->selection(), grid->patches(), proposals, 0.5);
    QVERIFY(proposal.has_value());
    QVERIFY(grid->stageTransformation(*proposal, {QStringLiteral("MusicBrainz")}));
    const auto draft = grid->patches().patches();
    files->selectAll();
    sections->setCurrentIndex(0);
    auto* items =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    auto* fetch = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-cover-fetch"));
    QVERIFY(items != nullptr);
    QVERIFY(fetch != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 1, 5'000);

    // One unambiguous release across the selection enables the fetch; the
    // download stages an addition; Save artwork is the explicit commit.
    QTRY_VERIFY(fetch->isEnabled());
    QTest::mouseClick(fetch, Qt::LeftButton);
    QVERIFY(chooseInCoverPicker(properties, QStringLiteral("Cover Art Archive"),
                                QStringLiteral("Front")));
    auto* save = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-save"));
    QVERIFY(save != nullptr);
    QTRY_VERIFY(save->isEnabled());
    auto* pending =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-pending"));
    QVERIFY(pending != nullptr);
    QTRY_VERIFY(!pending->model()->index(0, 5).data(Qt::DecorationRole).value<QImage>().isNull());
    const auto preview = pending->model()->index(0, 5).data(Qt::DecorationRole).value<QImage>();
    // Shown at 60 px or less, in more device pixels for HiDPI (ADR-0251).
    const auto shown = preview.deviceIndependentSize();
    QVERIFY(shown.width() <= 60.0 && shown.height() <= 60.0);
    QVERIFY(preview.devicePixelRatio() > 1.0);
    // A JPEG now (ADR-0317): the colour within its loss.
    const auto shown_colour = preview.pixelColor(0, 0);
    const QColor expected_colour{Qt::darkCyan};
    QVERIFY(std::abs(shown_colour.red() - expected_colour.red()) <= 4 &&
            std::abs(shown_colour.green() - expected_colour.green()) <= 4 &&
            std::abs(shown_colour.blue() - expected_colour.blue()) <= 4);
    QCOMPARE(pending->model()->index(0, 4).data().toString(), QStringLiteral("None"));
    QVERIFY(!observed.has_value());
    QTest::mouseClick(save, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 10'000);
    const auto apply_issue = observed->sources.front().issue
                                 ? QString::fromStdString(observed->sources.front().issue->message)
                                 : QStringLiteral("no per-source issue");
    QVERIFY2(observed->committed_source_count() == 1U, qPrintable(apply_issue));
    QCOMPARE(fetches, 2);
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 2, 5'000);
    QVERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) ==
            nullptr);
    const auto inventory = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(inventory.has_value());
    QCOMPARE(inventory->items.size(), 2U);
    const auto added = std::ranges::find_if(inventory->items, [](const auto& item) {
        return item.role == metadata::ArtworkRole::front;
    });
    QVERIFY(added != inventory->items.end());
    // ADR-0317: the archive's PNG written as Melody writes covers.
    QCOMPARE(added->mime_type, std::string{"image/jpeg"});
    QCOMPARE(added->provenance, metadata::ArtworkProvenance::embedded);

    QCOMPARE(grid->selection().source(0).source_revision,
             std::optional{observed->sources.front().commit->published_revision});
    QCOMPARE(captured->source(0).source_revision, std::optional{read->source_revision});
    QCOMPARE(grid->patches().patches(), draft);
    QVERIFY(grid->undo());
    QCOMPARE(grid->patches().patch_count(), 0U);
    QVERIFY(grid->redo());
    QCOMPARE(grid->patches().patches(), draft);
    const auto title_column = grid->fieldColumn(QStringLiteral("title"));
    QVERIFY(title_column.has_value());
    QCOMPARE(grid->index(0, *title_column).data(metadata_cell_staged_source_role).toString(),
             QStringLiteral("MusicBrainz"));
    // Selection refreshes must also use the new revision, not put the
    // Artwork section back on the revision captured when Properties opened.
    files->clearSelection();
    files->selectAll();
    QTRY_VERIFY(fetch->isEnabled());
    sections->setCurrentIndex(0);
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(apply != nullptr);
    QTRY_VERIFY(apply->isEnabled());
    QPointer guard{properties};
    QTest::mouseClick(apply, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(tags_observed.has_value(), 5'000);
    QCOMPARE(tags_observed->committed_source_count(), 1U);
    QVERIFY(window_test::closeTaggerAfterWrite(guard));
    const auto tagged = metadata::read_local_metadata(raw_path);
    QVERIFY(tagged.has_value());
    QCOMPARE(tagged->document.effective_values("title"),
             (std::vector<std::string>{"Identified title"}));
    QCOMPARE(tagged->document.effective_values("MUSICBRAINZ_ALBUMID"),
             (std::vector<std::string>{release_id.toStdString()}));
    const auto tagged_artwork = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(tagged_artwork.has_value());
    QCOMPARE(tagged_artwork->items.size(), inventory->items.size());
    for (std::size_t index = 0U; index < inventory->items.size(); ++index) {
        QCOMPARE(tagged_artwork->items[index].content_fingerprint,
                 inventory->items[index].content_fingerprint);
    }

    // A later session fetching again replaces the existing front cover
    // instead of stacking a second front picture.
    auto second_read = metadata::read_local_metadata(raw_path);
    QVERIFY(second_read.has_value());
    const MetadataPropertiesSource second_source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = raw_path,
                .source_revision = second_read->source_revision,
                .baseline = second_read->document,
            },
        .track_label = QStringLiteral("Cover fetch fixture"),
    };
    observed.reset();
    auto* second_properties = new MetadataPropertiesDialog(
        1U,
        [second_source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{second_source} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    second_properties->setArtworkMutationServices(
        [database_path] {
            return ArtworkWritePlanApplier{
                [database_path](const metadata::ArtworkWritePlan& plan,
                                const operations::ArtworkApplyProgressCallback& progress,
                                const core::CancellationToken& cancellation)
                    -> core::Result<operations::ArtworkApplyResult> {
                    auto opened = persistence::SqliteMetadataOperationJournal::open(database_path);
                    if (!opened) {
                        return std::unexpected(std::move(opened.error()));
                    }
                    auto journal = std::move(*opened);
                    return operations::apply_artwork_write_plan(
                        plan,
                        [&journal](const metadata::ArtworkWritePlanSource& source_plan,
                                   const core::CancellationToken& source_cancellation) {
                            return operations::commit_artwork_source(
                                source_plan, journal,
                                [](const operations::MetadataCommitResult&) -> core::Result<void> {
                                    return {};
                                },
                                source_cancellation);
                        },
                        progress, cancellation,
                        operations::ArtworkApplyOptions{.maximum_parallelism = 2U});
                }};
        },
        [&observed](const operations::ArtworkApplyResult& result) { observed = result; });
    second_properties->show();
    QTabWidget* second_sections = nullptr;
    QTRY_VERIFY((second_sections = second_properties->findChild<QTabWidget*>(
                     QStringLiteral("bench-metadata-sections"))) != nullptr);
    second_sections->setCurrentIndex(1);
    auto* second_items =
        second_properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    auto* second_fetch = second_properties->findChild<QPushButton*>(
        QStringLiteral("bench-metadata-artwork-fetch-cover"));
    QVERIFY(second_items != nullptr);
    QVERIFY(second_fetch != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(second_items->model()->rowCount(), 2, 5'000);
    QTRY_VERIFY(second_fetch->isEnabled());
    QTest::mouseClick(second_fetch, Qt::LeftButton);
    QVERIFY(chooseInCoverPicker(second_properties, QStringLiteral("Cover Art Archive"),
                                QStringLiteral("Front")));
    auto* second_save =
        second_properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-save"));
    QVERIFY(second_save != nullptr);
    QTRY_VERIFY(second_save->isEnabled());
    auto* second_pending =
        second_properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-pending"));
    QVERIFY(second_pending != nullptr);
    QCOMPARE(second_pending->model()->rowCount(), 2);
    QTRY_VERIFY(
        !second_pending->model()->index(1, 5).data(Qt::DecorationRole).value<QImage>().isNull());
    QVERIFY(jpeg_close(second_pending->model()
                           ->index(1, 5)
                           .data(Qt::DecorationRole)
                           .value<QImage>()
                           .pixelColor(0, 0),
                       QColor{Qt::darkMagenta}));
    auto* second_discard = second_properties->findChild<QPushButton*>(
        QStringLiteral("bench-metadata-artwork-discard"));
    QVERIFY(second_discard != nullptr);
    second_discard->click();
    QCOMPARE(second_pending->model()->rowCount(), 0);
    QTRY_VERIFY(second_fetch->isEnabled());
    second_fetch->click();
    QVERIFY(chooseInCoverPicker(second_properties, QStringLiteral("Cover Art Archive"),
                                QStringLiteral("Front")));
    QTRY_VERIFY(second_save->isEnabled());
    QTRY_VERIFY(
        !second_pending->model()->index(1, 5).data(Qt::DecorationRole).value<QImage>().isNull());
    QVERIFY(jpeg_close(second_pending->model()
                           ->index(1, 5)
                           .data(Qt::DecorationRole)
                           .value<QImage>()
                           .pixelColor(0, 0),
                       QColor{Qt::darkMagenta}));
    QVERIFY(!observed.has_value());
    QTest::mouseClick(second_save, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 10'000);
    const auto second_issue = observed->sources.front().issue
                                  ? QString::fromStdString(observed->sources.front().issue->message)
                                  : QStringLiteral("no per-source issue");
    QVERIFY2(observed->committed_source_count() == 1U, qPrintable(second_issue));
    QTRY_COMPARE_WITH_TIMEOUT(second_items->model()->rowCount(), 2, 5'000);
    const auto replaced = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(replaced.has_value());
    QCOMPARE(replaced->items.size(), 2U);
    const auto front_count = std::ranges::count_if(replaced->items, [](const auto& item) {
        return item.role == metadata::ArtworkRole::front;
    });
    QCOMPARE(front_count, 1);
    const auto new_front = std::ranges::find_if(replaced->items, [](const auto& item) {
        return item.role == metadata::ArtworkRole::front;
    });
    // The second image, as Melody writes covers (ADR-0317): a 24 px JPEG.
    QCOMPARE(new_front->mime_type, std::string{"image/jpeg"});
    QCOMPARE(new_front->width, std::optional<std::uint32_t>{24U});

    QPointer second_guard{second_properties};
    second_properties->close();
    QTRY_VERIFY(second_guard.isNull());
}

// With covers embedded in the files, a cover.jpg beside them is not the
// files' cover: the editor does not show it as theirs, and Fetch offers it.
void BenchMainWindowTest::Artwork::aCoverBesideTheFilesIsOfferedNotShown() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto media_path = media.filePath(QStringLiteral("track.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), media_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("external-blue-jpeg.b64"),
                                      media.filePath(QStringLiteral("cover.jpg"))));
    const auto encoded = QFile::encodeName(media_path);
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
        .track_label = QStringLiteral("Beside fixture"),
    };
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, {});
    // Nothing is saved here; changes only need to be possible.
    properties->setArtworkMutationServices(
        [] {
            return ArtworkWritePlanApplier{
                [](const metadata::ArtworkWritePlan&,
                   const operations::ArtworkApplyProgressCallback&,
                   const core::CancellationToken&) -> core::Result<operations::ArtworkApplyResult> {
                    return std::unexpected(core::Error{.code = core::ErrorCode::invariant,
                                                       .message = "not in this test",
                                                       .context = {}});
                }};
        },
        {});
    properties->show();
    QTabWidget* sections = nullptr;
    QTRY_VERIFY((sections = properties->findChild<QTabWidget*>(
                     QStringLiteral("bench-metadata-sections"))) != nullptr);
    MetadataArtworkSection* section = nullptr;
    QTRY_VERIFY((section = properties->findChild<MetadataArtworkSection*>()) != nullptr);
    // Watched before the Artwork page is opened, which is when it reads.
    QSignalSpy shown{section, &MetadataArtworkSection::frontCoverChanged};
    sections->setCurrentIndex(1);
    auto* fetch =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-fetch-cover"));
    QVERIFY(fetch != nullptr);
    // No release to ask the archive about: the image beside the file is
    // what makes Fetch worth pressing.
    QTRY_VERIFY_WITH_TIMEOUT(fetch->isEnabled(), 5'000);
    QVERIFY(!shown.isEmpty());
    QVERIFY2(shown.back().front().value<QImage>().isNull(),
             "the files carry no cover, so none is shown as theirs");

    QTest::mouseClick(fetch, Qt::LeftButton);
    QVERIFY(chooseInCoverPicker(properties, QStringLiteral("cover.jpg"), QStringLiteral("Front")));
    auto* pending =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-pending"));
    QVERIFY(pending != nullptr);
    QTRY_COMPARE(pending->model()->rowCount(), 1);
    // The staged front is that image: blue, as the fixture is.
    QTRY_VERIFY(!pending->model()->index(0, 5).data(Qt::DecorationRole).value<QImage>().isNull());
    const auto staged = pending->model()->index(0, 5).data(Qt::DecorationRole).value<QImage>();
    const auto centre = staged.pixelColor(staged.width() / 2, staged.height() / 2);
    QVERIFY2(centre.blue() > 150 && centre.red() < 100,
             qPrintable(QStringLiteral("staged image centre %1").arg(centre.name())));
    section->discardPendingChanges();
    properties->close();
}

void BenchMainWindowTest::Artwork::artworkArchivePickerAddsChosenImageWithItsRole() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto media_path = media.filePath(QStringLiteral("picker.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), media_path));
    const auto encoded = QFile::encodeName(media_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const auto release_id = QStringLiteral("2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    read->document.fields.push_back(metadata::MetadataField{
        .canonical_name = metadata::canonicalize_field_name("MUSICBRAINZ_ALBUMID"),
        .native_name = "MUSICBRAINZ_ALBUMID",
        .values = {release_id.toStdString()},
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
        .track_label = QStringLiteral("Picker fixture"),
    };

    QImage back_cover{16, 16, QImage::Format_ARGB32};
    back_cover.fill(Qt::darkGreen);
    QByteArray back_bytes;
    QBuffer back_buffer{&back_bytes};
    QVERIFY(back_buffer.open(QIODevice::WriteOnly));
    QVERIFY(back_cover.save(&back_buffer, "PNG"));
    back_buffer.close();
    static constexpr auto picker_listing = R"json({
      "images": [
        {"id": 1, "front": true, "approved": true, "types": ["Front"],
         "image":
           "https://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/1.png"},
        {"id": 2, "front": false, "back": true, "approved": true, "types": ["Back"],
         "comment": "tray insert",
         "image":
           "https://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/2.png",
         "thumbnails": {"250":
           "https://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/2-250.jpg"}}
      ]
    })json";
    const MusicBrainzLookupService service{
        .fetch =
            [back_bytes, release_id](const QString& url,
                                     std::function<void(core::Result<QByteArray>)> completion) {
                if (url == QStringLiteral("https://coverartarchive.org/release/") + release_id) {
                    completion(QByteArray{picker_listing});
                    return;
                }
                if (url.endsWith(QStringLiteral("/2.png")) ||
                    url.endsWith(QStringLiteral("/2-250.jpg"))) {
                    completion(back_bytes);
                    return;
                }
                completion(std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "unexpected url",
                    .context = {},
                }));
            },
        .fingerprint = {},
        .acoustid_lookup = {},
    };

    const auto database_path =
        std::filesystem::path{media.filePath(QStringLiteral("picker.sqlite3")).toStdString()};
    std::optional<operations::ArtworkApplyResult> observed;
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    properties->setArtworkMutationServices(
        [database_path] {
            return ArtworkWritePlanApplier{
                [database_path](const metadata::ArtworkWritePlan& plan,
                                const operations::ArtworkApplyProgressCallback& progress,
                                const core::CancellationToken& cancellation)
                    -> core::Result<operations::ArtworkApplyResult> {
                    auto opened = persistence::SqliteMetadataOperationJournal::open(database_path);
                    if (!opened) {
                        return std::unexpected(std::move(opened.error()));
                    }
                    auto journal = std::move(*opened);
                    return operations::apply_artwork_write_plan(
                        plan,
                        [&journal](const metadata::ArtworkWritePlanSource& source_plan,
                                   const core::CancellationToken& source_cancellation) {
                            return operations::commit_artwork_source(
                                source_plan, journal,
                                [](const operations::MetadataCommitResult&) -> core::Result<void> {
                                    return {};
                                },
                                source_cancellation);
                        },
                        progress, cancellation,
                        operations::ArtworkApplyOptions{.maximum_parallelism = 2U});
                }};
        },
        [&observed](const operations::ArtworkApplyResult& result) { observed = result; });
    properties->show();

    QTabWidget* sections = nullptr;
    QTRY_VERIFY((sections = properties->findChild<QTabWidget*>(
                     QStringLiteral("bench-metadata-sections"))) != nullptr);
    sections->setCurrentIndex(1);
    auto* items =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    auto* covers =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-fetch-cover"));
    QVERIFY(items != nullptr);
    QVERIFY(covers != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 1, 5'000);
    QTRY_VERIFY(covers->isEnabled());
    QTest::mouseClick(covers, Qt::LeftButton);

    // The picker lists every archive image with its type and comment.
    QDialog* picker = nullptr;
    QTRY_VERIFY((picker = properties->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-artwork-picker"))) != nullptr);
    auto* list =
        picker->findChild<QTreeWidget*>(QStringLiteral("bench-metadata-artwork-picker-list"));
    auto* use =
        picker->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-picker-use"));
    QVERIFY(list != nullptr);
    QVERIFY(use != nullptr);
    QTRY_COMPARE(list->topLevelItemCount(), 2);
    QCOMPARE(list->topLevelItem(0)->text(2), QStringLiteral("Front"));
    QCOMPARE(list->topLevelItem(1)->text(2), QStringLiteral("Back"));
    QCOMPARE(list->topLevelItem(1)->text(3), QStringLiteral("tray insert"));
    // The scripted service already answered the thumbnail request.
    QTRY_VERIFY(!list->topLevelItem(1)->icon(0).isNull());

    list->setCurrentItem(list->topLevelItem(1));
    QTest::mouseClick(use, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-artwork-picker")) ==
                nullptr);
    auto* save = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-save"));
    QVERIFY(save != nullptr);
    QTRY_VERIFY(save->isEnabled());
    QVERIFY(!observed.has_value());
    QTest::mouseClick(save, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 10'000);
    const auto apply_issue = observed->sources.front().issue
                                 ? QString::fromStdString(observed->sources.front().issue->message)
                                 : QStringLiteral("no per-source issue");
    QVERIFY2(observed->committed_source_count() == 1U, qPrintable(apply_issue));
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 2, 5'000);
    const auto inventory = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(inventory.has_value());
    QCOMPARE(inventory->items.size(), 2U);
    const auto added = std::ranges::find_if(inventory->items, [](const auto& item) {
        return item.role == metadata::ArtworkRole::back;
    });
    QVERIFY(added != inventory->items.end());
    // ADR-0317: as Melody writes covers, a JPEG.
    QCOMPARE(added->mime_type, std::string{"image/jpeg"});
    QVERIFY(added->byte_size > 0U);

    QPointer guard{properties};
    properties->close();
    QTRY_VERIFY(guard.isNull());
}

void BenchMainWindowTest::Artwork::
    metadataPropertiesArtworkSectionShowsProvenanceAndCapabilities() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto media_path = media.filePath(QStringLiteral("artwork.flac"));
    const auto cover_path = media.filePath(QStringLiteral("cover.jpg"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), media_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("external-blue-jpeg.b64"), cover_path));

    const auto encoded = QFile::encodeName(media_path);
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
        .track_label = QStringLiteral("Artwork fixture"),
    };

    auto* properties = new MetadataPropertiesDialog(
        2U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < 2U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {});
    properties->show();

    QTabWidget* sections = nullptr;
    QTRY_VERIFY((sections = properties->findChild<QTabWidget*>(
                     QStringLiteral("bench-metadata-sections"))) != nullptr);
    QCOMPARE(sections->tabText(0), QStringLiteral("Fields"));
    QCOMPARE(sections->tabText(1), QStringLiteral("Artwork"));
    QCOMPARE(sections->currentIndex(), 0);

    auto* artwork =
        properties->findChild<QWidget*>(QStringLiteral("bench-metadata-artwork-section"));
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-artwork-status"));
    auto* items =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    auto* issues =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-issues"));
    auto* progress =
        properties->findChild<QProgressBar*>(QStringLiteral("bench-metadata-artwork-progress"));
    QVERIFY(artwork != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(items != nullptr);
    QVERIFY(issues != nullptr);
    QVERIFY(progress != nullptr);
    QCOMPARE(items->model()->rowCount(), 0);
    QVERIFY(!progress->isVisible());
    QVERIFY(artwork->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-save")) !=
            nullptr);
    QVERIFY(artwork->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-discard")) !=
            nullptr);
    for (auto* button : artwork->findChildren<QPushButton*>()) {
        QVERIFY(!button->isEnabled());
    }

    sections->setCurrentIndex(1);
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 2, 5'000);
    QVERIFY(status->text().contains(QStringLiteral("2 images")));

    // Thumbnails plus compact columns: File, Role, Image, Source. Exact
    // fingerprints, native types, and ordinals live in tooltips.
    QCOMPARE(items->model()->headerData(1, Qt::Horizontal).toString(), QStringLiteral("File"));
    QCOMPARE(items->model()->headerData(2, Qt::Horizontal).toString(), QStringLiteral("Role"));
    QCOMPARE(items->model()->headerData(3, Qt::Horizontal).toString(), QStringLiteral("Image"));
    QCOMPARE(items->model()->headerData(4, Qt::Horizontal).toString(), QStringLiteral("Source"));
    QVERIFY(items->model()->index(0, 0).data(Qt::DecorationRole).value<QImage>().isNull() == false);
    QVERIFY(items->model()->index(1, 0).data(Qt::DecorationRole).value<QImage>().isNull() == false);
    QVERIFY(
        items->model()->index(0, 1).data().toString().contains(QStringLiteral("2 occurrences")));
    QCOMPARE(items->model()->index(0, 2).data().toString(), QStringLiteral("Other"));
    QVERIFY(
        items->model()->index(0, 3).data().toString().startsWith(QStringLiteral("PNG · 64 × 64")));
    QCOMPARE(items->model()->index(0, 4).data().toString(), QStringLiteral("Embedded"));
    QCOMPARE(items->model()->index(1, 2).data().toString(), QStringLiteral("Front"));
    QVERIFY(
        items->model()->index(1, 3).data().toString().startsWith(QStringLiteral("JPEG · 8 × 6")));
    QCOMPARE(items->model()->index(1, 4).data().toString(), QStringLiteral("External"));
    QVERIFY(items->model()
                ->index(0, 3)
                .data(Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("SHA-256:")));
    QVERIFY(items->model()
                ->index(1, 4)
                .data(Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("cover.jpg")));

    // No mutation service is wired in this fixture, so the file reports as
    // view-only in the problems pane instead of a permanent capability table.
    QTRY_COMPARE(issues->model()->rowCount(), 1);
    QCOMPARE(issues->model()->index(0, 2).data().toString(), QStringLiteral("View-only"));
    QVERIFY(status->text().contains(QStringLiteral("1 view-only")));

    QPointer guard{properties};
    properties->close();
    QTRY_VERIFY(guard.isNull());
}

// ADR-0348: a release of 99 tracks -- a CD with short silent ones -- is
// not too many files for its covers, the cover every file carries is one row,
// and a file that cannot hold a cover is skipped instead of blocking the rest.
void BenchMainWindowTest::Artwork::artworkOfAWholeReleaseIsOneRowAndSkipsFilesWithoutCovers() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    constexpr std::size_t flac_count = 99U;
    std::vector<MetadataArtworkScopeSource> scope;
    const auto add_source = [&scope](const QString& path) {
        const auto encoded = QFile::encodeName(path);
        const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
        const auto revision = core::observe_local_source_revision(raw_path);
        QVERIFY(revision.has_value());
        const auto index = scope.size();
        scope.push_back(MetadataArtworkScopeSource{
            .raw_path = raw_path,
            .captured_revision = *revision,
            .label = QFileInfo{path}.fileName(),
            .occurrence_indexes = {index},
            .occurrence_count = 1U,
            .captured_revision_consistent = true,
        });
    };
    for (std::size_t index = 0U; index < flac_count; ++index) {
        const auto path =
            media.filePath(QStringLiteral("%1.flac").arg(index + 1U, 2, 10, QLatin1Char('0')));
        QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), path));
        add_source(path);
    }
    const auto wav_path = media.filePath(QStringLiteral("silence.wav"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rf64-tone-wav.b64"), wav_path));
    add_source(wav_path);
    const auto wav_raw = scope.back().raw_path;
    QTemporaryDir images;
    QVERIFY(images.isValid());
    const auto cover_path = images.filePath(QStringLiteral("front.jpg"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("external-blue-jpeg.b64"), cover_path));

    ArtworkSession session;
    session.setMutationServices([] { return ArtworkWritePlanApplier{}; }, {});
    session.setScope(std::move(scope));
    session.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(session.status().contains(QStringLiteral("across 100 files")), 20'000);

    // The fixture's one embedded picture, the same in all 99 files.
    QCOMPARE(session.items()->rowCount(), 1);
    QCOMPARE(session.items()->index(0, 1).data().toString(), QStringLiteral("99 files"));
    QCOMPARE(session.items()->index(0, 4).data().toString(), QStringLiteral("Embedded"));
    QVERIFY(session.status().contains(QStringLiteral("1 view-only")));
    QCOMPARE(session.issues()->rowCount(), 1);
    QVERIFY(session.issues()->index(0, 3).data().toString().contains(QStringLiteral("skip")));
    QVERIFY(session.canAdd());
    QVERIFY(session.frontEditable());

    session.stageFrontCover(cover_path);
    const auto intents = session.pendingIntents();
    QCOMPARE(intents.size(), flac_count);
    QVERIFY(std::ranges::all_of(intents, [&wav_raw](const auto& intent) {
        return intent.kind == metadata::ArtworkWritePlanIntentKind::add &&
               intent.raw_media_path != wav_raw;
    }));

    // Selecting the row reaches the picture in every file that holds it.
    session.discardPendingChanges();
    session.itemSelection()->select(session.items()->index(0, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QVERIFY(session.canRemove());
    session.removeSelected();
    QCOMPARE(session.pendingIntents().size(), flac_count);
    QVERIFY(session.items()->item(0, 1)->font().strikeOut());
}

void BenchMainWindowTest::Artwork::
    metadataPropertiesArtworkRemoveReviewsAppliesAndRefreshes_data() {
    // ADR-0237: the artwork shown, reviewed and written by the engine holding
    // the file, or by this process -- the same section, the same result.
    QTest::addColumn<bool>("through_engine");
    QTest::newRow("engine") << true;
    QTest::newRow("this-process") << false;
}

void BenchMainWindowTest::Artwork::metadataPropertiesArtworkRemoveReviewsAppliesAndRefreshes() {
    QFETCH(bool, through_engine);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto media_path = media.filePath(QStringLiteral("artwork-remove.flac"));
    const auto cover_path = media.filePath(QStringLiteral("cover.jpg"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), media_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("external-blue-jpeg.b64"), cover_path));
    const auto encoded = QFile::encodeName(media_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    const auto initial = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(initial.has_value());
    const metadata::ArtworkWritePlanIntent duplicate_role{
        .occurrence_index = 0,
        .raw_media_path = raw_path,
        .expected_media_revision = initial->media_revision,
        .target_ordinal = 0,
        .expected_target_fingerprint = {},
        .kind = metadata::ArtworkWritePlanIntentKind::add,
        .replacement_raw_path = QFile::encodeName(cover_path).toStdString(),
        .added_role = initial->items.front().role,
        .added_description = {},
        .replacement_embedded_source = std::nullopt};
    const auto seed_plan = metadata::revalidate_artwork_write_plan({duplicate_role});
    QVERIFY(seed_plan && seed_plan->ready());
    auto seed_journal = persistence::SqliteMetadataOperationJournal::open(
        std::filesystem::path{media.filePath(QStringLiteral("seed.sqlite3")).toStdString()});
    QVERIFY(seed_journal.has_value());
    QVERIFY(operations::commit_artwork_source(
                seed_plan->sources.front(), *seed_journal,
                [](const operations::MetadataCommitResult&) -> core::Result<void> { return {}; })
                .has_value());
    const auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = raw_path,
                .source_revision = read->source_revision,
                .baseline = read->document,
            },
        .track_label = QStringLiteral("Artwork remove fixture"),
    };
    const auto database_path =
        std::filesystem::path{media.filePath(QStringLiteral("artwork.sqlite3")).toStdString()};
    std::optional<operations::ArtworkApplyResult> observed;
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
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, {},
        work ? engineFileWorkTools(work) : FileWorkTools{});
    properties->setArtworkMutationServices(
        work ? ArtworkWritePlanApplierFactory{[work] {
            return ArtworkWritePlanApplier{
                [work](const metadata::ArtworkWritePlan& plan,
                       const operations::ArtworkApplyProgressCallback& progress,
                       const core::CancellationToken& cancellation) {
                    return work->artwork_apply(plan, progress, cancellation);
                }};
        }}
             : ArtworkWritePlanApplierFactory{[database_path] {
                   return ArtworkWritePlanApplier{
                       [database_path](const metadata::ArtworkWritePlan& plan,
                                       const operations::ArtworkApplyProgressCallback& progress,
                                       const core::CancellationToken& cancellation)
                           -> core::Result<operations::ArtworkApplyResult> {
                           auto opened =
                               persistence::SqliteMetadataOperationJournal::open(database_path);
                           if (!opened) {
                               return std::unexpected(std::move(opened.error()));
                           }
                           auto journal = std::move(*opened);
                           return operations::apply_artwork_write_plan(
                               plan,
                               [&journal](const metadata::ArtworkWritePlanSource& source_plan,
                                          const core::CancellationToken& source_cancellation) {
                                   return operations::commit_artwork_source(
                                       source_plan, journal,
                                       [](const operations::MetadataCommitResult&)
                                           -> core::Result<void> { return {}; },
                                       source_cancellation);
                               },
                               progress, cancellation,
                               operations::ArtworkApplyOptions{.maximum_parallelism = 2U});
                       }};
               }},
        [&observed](const operations::ArtworkApplyResult& result) { observed = result; });
    properties->show();

    QTabWidget* sections = nullptr;
    QTRY_VERIFY((sections = properties->findChild<QTabWidget*>(
                     QStringLiteral("bench-metadata-sections"))) != nullptr);
    sections->setCurrentIndex(1);
    auto* items =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    auto* replace =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-replace"));
    auto* remove =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-remove"));
    auto* add = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-add"));
    auto* copy = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-copy"));
    QVERIFY(items != nullptr);
    QVERIFY(replace != nullptr);
    QVERIFY(remove != nullptr);
    QVERIFY(add != nullptr);
    QVERIFY(copy != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 3, 5'000);
    QVERIFY(add->isEnabled());
    items->selectRow(2);
    QTRY_VERIFY(!replace->isEnabled());
    QVERIFY(!remove->isEnabled());
    QVERIFY(copy->isEnabled());
    items->clearSelection();
    items->selectRow(0);
    QTRY_VERIFY(replace->isEnabled());
    QTRY_VERIFY(remove->isEnabled());
    QVERIFY(!copy->isEnabled());
    items->selectAll(); // Including an external image must not block embedded removal.
    QTRY_VERIFY(remove->isEnabled());
    QTest::mouseClick(remove, Qt::LeftButton);
    auto* pending =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-pending"));
    auto* discard =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-discard"));
    QVERIFY(pending != nullptr);
    QVERIFY(discard != nullptr);
    QCOMPARE(pending->model()->rowCount(), 2);
    auto* undo_selected =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-undo-selected"));
    QVERIFY(undo_selected != nullptr);
    pending->selectRow(0);
    QTRY_VERIFY(undo_selected->isEnabled());
    QTest::mouseClick(undo_selected, Qt::LeftButton);
    QCOMPARE(pending->model()->rowCount(), 1);
    QTest::mouseClick(remove, Qt::LeftButton);
    QCOMPARE(pending->model()->rowCount(), 2);
    const auto screenshot_dir = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
    if (!screenshot_dir.isEmpty()) {
        properties->resize(1200, 850);
        QVERIFY(properties->grab().save(screenshot_dir + QStringLiteral("/artwork-pending.png")));
    }
    bool warned_about_draft = false;
    QTimer::singleShot(0, properties, [&] {
        if (auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            warned_about_draft = warning->windowTitle().contains(QStringLiteral("artwork"));
            warning->done(QMessageBox::Cancel);
        }
    });
    QTest::keyClick(properties, Qt::Key_Escape);
    QVERIFY(warned_about_draft);
    QVERIFY(properties->isVisible());
    QCOMPARE(pending->model()->rowCount(), 2);
    const auto before_save = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(before_save.has_value());
    QCOMPARE(before_save->items.size(), 3U);
    QTest::mouseClick(discard, Qt::LeftButton);
    QCOMPARE(pending->model()->rowCount(), 0);
    QTest::mouseClick(remove, Qt::LeftButton);

    // Removal is staged until Save artwork; the external cover is retained.
    auto* save = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-save"));
    QVERIFY(save != nullptr);
    QTRY_VERIFY(save->isEnabled());
    QVERIFY(!observed.has_value());
    QTest::mouseClick(save, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 10'000);
    const auto apply_issue = observed->sources.front().issue
                                 ? QString::fromStdString(observed->sources.front().issue->message)
                                 : QStringLiteral("no per-source issue");
    QVERIFY2(observed->committed_source_count() == 1U, qPrintable(apply_issue));
    QCOMPARE(observed->sources.front().commit->occurrence_indexes, (std::vector<std::size_t>{0U}));
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 1, 5'000);
    QCOMPARE(items->model()->index(0, 4).data().toString(), QStringLiteral("External"));
    QVERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) ==
            nullptr);
    const auto inventory = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(inventory.has_value());
    QCOMPARE(inventory->items.size(), 1U);
    QCOMPARE(inventory->items.front().provenance, metadata::ArtworkProvenance::external);

    // Supply a local image after removing every embedded cover.
    observed.reset();
    QTimer chooser;
    chooser.setInterval(10);
    int file_choices = 0;
    int role_choices = 0;
    connect(&chooser, &QTimer::timeout, properties, [&] {
        if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
            if (file_choices == 0) {
                // selectFile() can leave the focused filename edit empty in
                // an already-visible dialog. Enter the path as a user would.
                auto* filename = dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
                QVERIFY(filename != nullptr);
                filename->setText(cover_path);
                ++file_choices;
            } else {
                QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
            }
        } else if (auto* role = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            role->accept();
            ++role_choices;
        }
    });
    QTimer chooser_timeout;
    chooser_timeout.setSingleShot(true);
    connect(&chooser_timeout, &QTimer::timeout, properties, [] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            dialog->reject();
        }
    });
    const auto used_native_dialogs = !QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    chooser_timeout.start(5000);
    chooser.start();
    QTest::mouseClick(add, Qt::LeftButton);
    chooser_timeout.stop();
    chooser.stop();
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, !used_native_dialogs);
    QCOMPARE(file_choices, 1);
    QCOMPARE(role_choices, 1);
    QCOMPARE(pending->model()->rowCount(), 1);
    auto* files = properties->fileListView();
    QVERIFY(files != nullptr);
    QVERIFY2(files->height() >= 120, "adding an artwork draft must not collapse the file list");
    QVERIFY(!observed.has_value());
    QTRY_VERIFY(save->isEnabled());
    QTest::mouseClick(save, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 10'000);
    QCOMPARE(observed->committed_source_count(), 1U);
    const auto supplied = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(supplied.has_value());
    QCOMPARE(supplied->items.size(), 2U);
    QCOMPARE(supplied->items.front().role, metadata::ArtworkRole::front);
    QCOMPARE(supplied->items.front().provenance, metadata::ArtworkProvenance::embedded);

    QPointer guard{properties};
    properties->close();
    QTRY_VERIFY(guard.isNull());
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::Artwork)

#include "window_artwork_test.moc"
