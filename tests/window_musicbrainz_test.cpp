// SPDX-License-Identifier: GPL-3.0-only

// The window test: MusicBrainz: identifying, matching and fingerprints.

#include "window_test.hpp"

#include "bench/file_scope_view.hpp"
#include "trackknife/core/engine_state.hpp"
#include "uicommon/library_tree_view.hpp"
#include "workspace/artwork_session.hpp"

#include <QPainter>
#include <QStandardItemModel>

namespace trackknife::bench {

class BenchMainWindowTest::MusicBrainz final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void musicBrainzMatchingHandlesUnequalCounts_data();
    void musicBrainzMatchingHandlesUnequalCounts();
    void theMatchingBoardPairsByDropClickAndDisc();
    void aSecondAlbumTakesTheDiscTheFirstLeft();
    void musicBrainzIdentifyStagesChosenVersion_data();
    void musicBrainzIdentifyStagesChosenVersion();
    void musicBrainzIdentifyLooksUpNamedReleases();
    void musicBrainzIdentifyStepsThroughAlbums();
    void identifyOpensTheTagEditorWithIdentify();
    void musicBrainzFingerprintScanRanksAndStages();
    void musicBrainzStagesFromCachedSearchMetadata();
    void identifyFetchesTheCoversOfTheAlbumsApplied();
};

void BenchMainWindowTest::MusicBrainz::musicBrainzMatchingHandlesUnequalCounts_data() {
    QTest::addColumn<int>("file_count");
    QTest::newRow("missing-file") << 1;
    QTest::newRow("extra-file") << 3;
    QTest::newRow("scroll-alignment") << 120;
}

void BenchMainWindowTest::MusicBrainz::musicBrainzMatchingHandlesUnequalCounts() {
    QFETCH(int, file_count);
    musicbrainz::Release release{};
    release.id = "2f2ac1b7-1111-4f4f-8f8f-123456789abc";
    release.title = "Two discs";
    for (std::size_t disc = 1; disc <= 2; ++disc) {
        musicbrainz::ReleaseMedium medium{};
        medium.position = disc;
        medium.track_count = 1;
        musicbrainz::ReleaseTrack track{};
        track.position = 1;
        track.title = "Disc " + std::to_string(disc);
        medium.tracks.push_back(track);
        release.media.push_back(medium);
    }
    std::vector<musicbrainz::LocalTrackDescriptor> local(static_cast<std::size_t>(file_count));
    std::vector<QString> paths;
    std::vector<std::size_t> indexes;
    for (int index = 0; index < file_count; ++index) {
        paths.push_back(QStringLiteral("/music/file-%1.flac").arg(index));
        indexes.push_back(static_cast<std::size_t>(index + 10));
    }
    std::optional<metadata::MetadataProposalSet> accepted;
    std::unique_ptr<QWidget> widget{createMusicBrainzTrackMatchWidget(
        release, local, paths, indexes,
        [&accepted](metadata::MetadataProposalSet proposal) { accepted = std::move(proposal); },
        [] {}, nullptr)};
    widget->show();
    auto* board = widget->findChild<QListWidget*>(QStringLiteral("bench-musicbrainz-match-board"));
    auto* up = widget->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-up"));
    auto* down = widget->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-down"));
    auto* stage = widget->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-stage"));
    QVERIFY(board && up && down && stage);
    QTRY_COMPARE(matchRowCount(board), std::max(file_count, 2));
    QCOMPARE(matchTrack(matchRow(board, 0)), QStringLiteral("1.1 · Disc 1"));
    QCOMPARE(matchTrack(matchRow(board, 1)), QStringLiteral("2.1 · Disc 2"));
    if (file_count == 1) {
        QVERIFY(matchRow(board, 1)->text().isEmpty());
        board->setCurrentItem(matchRow(board, 0));
        down->click();
        QVERIFY(matchRow(board, 0)->text().isEmpty());
    } else {
        QVERIFY(matchTrack(matchRow(board, 2)).contains(QStringLiteral("No tags")));
        // ADR-0330: the file on no track, put on the second track; the one
        // there goes back to the unmatched files.
        matchClickOnto(board, 2, 1);
        QTRY_COMPARE(matchRow(board, 1)->text(), QStringLiteral("file-2.flac"));
        QCOMPARE(matchRow(board, 2)->text(), QStringLiteral("file-1.flac"));
        QCOMPARE(matchRow(board, 2)->listWidget(), matchLoose(board));
    }
    QCOMPARE(matchTrack(matchRow(board, 1)), QStringLiteral("2.1 · Disc 2"));
    if (file_count > 3) {
        // ADR-0330: the files on no track are listed beside the tracks.
        QTRY_VERIFY(matchLoose(board)->verticalScrollBar()->maximum() > 0);
    }
    QVERIFY(!accepted);
    stage->click();
    QVERIFY(accepted.has_value());
    QCOMPARE(accepted->items.size(), static_cast<std::size_t>(std::min(file_count, 2)));
    QCOMPARE(accepted->items.back().item_index, file_count == 1 ? 10U : 12U);
}

namespace {

// Two discs of the same two titles: a set whose second disc repeats the
// first, recorded again.
musicbrainz::Release repeatingDiscs() {
    musicbrainz::Release release{};
    release.id = "3f3ac1b7-1111-4f4f-8f8f-123456789abc";
    release.title = "Again";
    for (std::size_t disc = 1; disc <= 2; ++disc) {
        musicbrainz::ReleaseMedium medium{};
        medium.position = disc;
        medium.format = "CD";
        medium.track_count = 2;
        for (std::size_t number = 1; number <= 2; ++number) {
            musicbrainz::ReleaseTrack track{};
            track.position = number;
            track.title = number == 1 ? "One" : "Two";
            track.length_ms = disc == 1 ? 180'000 : 200'000;
            medium.tracks.push_back(track);
        }
        release.media.push_back(medium);
    }
    return release;
}

std::vector<musicbrainz::LocalTrackDescriptor> twoFiles(const std::int64_t length_ms) {
    std::vector<musicbrainz::LocalTrackDescriptor> files(2U);
    files[0].title = "One";
    files[1].title = "Two";
    for (auto& file : files) {
        file.duration_ms = length_ms;
    }
    return files;
}

} // namespace

// ADR-0324: on the board a file goes onto a track by being dropped there or
// by a click on it and then on the track; a disc without files takes those
// of another in one go; a pairing whose lengths disagree is marked.
void BenchMainWindowTest::MusicBrainz::theMatchingBoardPairsByDropClickAndDisc() {
    std::unique_ptr<QWidget> widget{createMusicBrainzTrackMatchWidget(
        repeatingDiscs(), twoFiles(180'000),
        {QStringLiteral("/music/1.flac"), QStringLiteral("/music/2.flac")}, {10U, 11U},
        [](metadata::MetadataProposalSet) {}, [] {}, nullptr)};
    widget->resize(900, 500);
    widget->show();
    auto* board = widget->findChild<QListWidget*>(QStringLiteral("bench-musicbrainz-match-board"));
    auto* hint = widget->findChild<QLabel*>(QStringLiteral("bench-musicbrainz-match-hint"));
    QVERIFY(board && hint);
    QTRY_COMPARE(matchRowCount(board), 4);
    QCOMPARE(matchRow(board, 0)->text(), QStringLiteral("1.flac"));
    QCOMPARE(matchRow(board, 1)->text(), QStringLiteral("2.flac"));
    // Two disc headings among the rows.
    QCOMPARE(board->count(), 6);
    QCOMPARE(board->item(0)->text(), QStringLiteral("Disc 1 · CD"));

    // ADR-0330: a row is the track at the left, its file at the right.
    const auto file_side = [board](const int row) {
        const auto rect = board->visualItemRect(matchRow(board, row));
        return QPoint{rect.right() - 60, rect.center().y()};
    };
    const auto track_side = [board](const int row) {
        const auto rect = board->visualItemRect(matchRow(board, row));
        return QPoint{rect.left() + 60, rect.center().y()};
    };

    // Dropped on disc 2's first track.
    std::unique_ptr<QMimeData> mime{
        board->model()->mimeData({board->indexFromItem(matchRow(board, 0))})};
    const auto onto = track_side(2);
    QDragEnterEvent enter{dragPosition(onto), Qt::MoveAction, mime.get(), Qt::LeftButton,
                          Qt::NoModifier};
    QApplication::sendEvent(board->viewport(), &enter);
    QVERIFY(enter.isAccepted());
    QDragMoveEvent move{dragPosition(onto), Qt::MoveAction, mime.get(), Qt::LeftButton,
                        Qt::NoModifier};
    QApplication::sendEvent(board->viewport(), &move);
    QVERIFY(move.isAccepted());
    QVERIFY2(hint->text().contains(QStringLiteral("2.1")), qPrintable(hint->text()));
    QDropEvent drop{QPointF{onto}, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(board->viewport(), &drop);
    QVERIFY(drop.isAccepted());
    QTRY_COMPARE(matchRow(board, 2)->text(), QStringLiteral("1.flac"));
    QVERIFY(matchRow(board, 0)->text().isEmpty());
    // Disc 2's track is 20 s longer than this file: likely the wrong one,
    // and the line above says so (ADR-0330).
    QVERIFY(matchRow(board, 2)->toolTip().contains(QStringLiteral("20 s shorter")));
    QVERIFY(matchRow(board, 2)->toolTip().contains(QStringLiteral("Likely the wrong track")));
    const auto labels = widget->findChildren<QLabel*>();
    QVERIFY(std::ranges::any_of(labels, [](const QLabel* label) {
        return label->text().contains(QStringLiteral("1 likely wrong"));
    }));

    // Clicked, then its track clicked.
    QTest::mouseClick(board->viewport(), Qt::LeftButton, {}, file_side(1));
    QVERIFY(hint->text().contains(QStringLiteral("picked up")));
    QTest::mouseClick(board->viewport(), Qt::LeftButton, {}, track_side(3));
    QTRY_COMPARE(matchRow(board, 3)->text(), QStringLiteral("2.flac"));
    QVERIFY(matchRow(board, 1)->text().isEmpty());

    // ADR-0330: a file dragged from its track to the unmatched files is on
    // none; put back on its track by a click on it, then on the track.
    {
        auto* loose = matchLoose(board);
        QVERIFY(loose != nullptr && loose->count() == 0);
        std::unique_ptr<QMimeData> back{
            board->model()->mimeData({board->indexFromItem(matchRow(board, 3))})};
        const auto into = loose->viewport()->rect().center();
        QDragEnterEvent enter_loose{dragPosition(into), Qt::MoveAction, back.get(), Qt::LeftButton,
                                    Qt::NoModifier};
        QApplication::sendEvent(loose->viewport(), &enter_loose);
        QVERIFY(enter_loose.isAccepted());
        QVERIFY(hint->text().contains(QStringLiteral("unmatched")));
        QDropEvent drop_loose{QPointF{into}, Qt::MoveAction, back.get(), Qt::LeftButton,
                              Qt::NoModifier};
        QApplication::sendEvent(loose->viewport(), &drop_loose);
        QVERIFY(drop_loose.isAccepted());
        // Under its folder's name, as the tracks under their disc's.
        QTRY_COMPARE(loose->count(), 2);
        QCOMPARE(loose->item(0)->text(), QStringLiteral("music"));
        QCOMPARE(loose->item(1)->text(), QStringLiteral("2.flac"));
        QVERIFY(matchRow(board, 3)->text().isEmpty());
        const auto row = loose->item(1)->data(Qt::UserRole + 2).toInt();
        matchClickOnto(board, row, 3);
        QTRY_COMPARE(matchRow(board, 3)->text(), QStringLiteral("2.flac"));
        QCOMPARE(loose->count(), 0);
    }

    // Disc 1, empty now, takes disc 2's files back by its heading.
    const auto heading = board->visualItemRect(board->item(0));
    QTest::mouseClick(board->viewport(), Qt::LeftButton, {},
                      QPoint{heading.right() - 20, heading.center().y()});
    QTRY_COMPARE(matchRow(board, 0)->text(), QStringLiteral("1.flac"));
    QCOMPARE(matchRow(board, 1)->text(), QStringLiteral("2.flac"));
    QVERIFY(matchRow(board, 2)->text().isEmpty());
}

// ADR-0324: one Identify, two albums of the same release -- its discs
// repeating each other, the files split by their tags -- the second pairs
// with the disc the first left.
void BenchMainWindowTest::MusicBrainz::aSecondAlbumTakesTheDiscTheFirstLeft() {
    TrackMatchSession first{repeatingDiscs(),
                            twoFiles(180'000),
                            {QStringLiteral("/a/1.flac"), QStringLiteral("/a/2.flac")},
                            {0U, 1U}};
    QTRY_VERIFY(first.ready());
    QCOMPARE(first.rows()[0].file, QStringLiteral("1.flac"));
    QCOMPARE(first.rows()[1].file, QStringLiteral("2.flac"));
    TrackMatchSession second{repeatingDiscs(),
                             twoFiles(200'000),
                             {QStringLiteral("/b/1.flac"), QStringLiteral("/b/2.flac")},
                             {2U, 3U}};
    second.avoid(first.pairedTracks());
    QTRY_VERIFY(second.ready());
    QVERIFY(second.rows()[0].gap);
    QVERIFY(second.rows()[1].gap);
    QCOMPARE(second.rows()[2].file, QStringLiteral("1.flac"));
    QCOMPARE(second.rows()[3].file, QStringLiteral("2.flac"));
    QCOMPARE(second.rows()[2].disc, 2U);
    QVERIFY(!second.rows()[2].doubtful);
}

// The desktop says dark only after the window is up, and the scheme's
// palette comes later: the status bar's painted icons -- album shuffle --
// are painted again in its colours, not left in the light palette's dark.
// ADR-0311: Identify… from the Edit menu and a list's Tools: the tag editor
// on the selected tracks, or with none selected the whole list, Identify
// open in it once the files are read.
void BenchMainWindowTest::MusicBrainz::identifyOpensTheTagEditorWithIdentify() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    std::vector<std::string> paths;
    for (const auto* name : {"one.flac", "two.flac"}) {
        const auto path = media.filePath(QLatin1String(name));
        QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), path));
        paths.push_back(QFile::encodeName(path).toStdString());
    }
    BenchMainWindow window;
    window.setProperty("trackknife-identify-hold-lookup", true);
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    window.openLocalPaths(paths);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 2, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    auto* identify = window.findChild<QAction*>(QStringLiteral("action-identify"));
    QVERIFY(identify != nullptr);
    QCOMPARE(identify->text(), QStringLiteral("Identify…"));
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-identify-albums")) == nullptr);
    view->selectionModel()->clearSelection();
    window.refreshSelectionStatus();
    QTRY_VERIFY(identify->isEnabled());

    const auto open = [&window, identify](const QString& title) {
        identify->trigger();
        MetadataPropertiesDialog* editor = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (auto* candidate : window.findChildren<MetadataPropertiesDialog*>()) {
                    if (candidate->isVisible()) {
                        editor = candidate;
                    }
                }
                return editor != nullptr;
            }(),
            10'000);
        if (editor == nullptr) {
            QFAIL("no tag editor opened");
            return;
        }
        QCOMPARE(editor->windowTitle(), title);
        QVERIFY(editor->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify-albums")) ==
                nullptr);
        QDialog* dialog = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((dialog = editor->findChild<QDialog*>(
                                      QStringLiteral("bench-musicbrainz-identify"))) != nullptr &&
                                     dialog->isVisible(),
                                 10'000);
        dialog->close();
        editor->close();
        QTRY_VERIFY(!editor->isVisible());
    };
    // None selected: the whole list.
    open(QStringLiteral("Edit tags · 2 tracks"));
    if (QTest::currentTestFailed()) {
        return;
    }
    // One selected: that one.
    view->selectionModel()->select(list_model->index(1, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    open(QStringLiteral("Edit tags · 1 track"));
}

void BenchMainWindowTest::MusicBrainz::musicBrainzIdentifyStagesChosenVersion_data() {
    QTest::addColumn<bool>("untagged");
    QTest::newRow("tagged") << false;
    QTest::newRow("untagged-manual-mapping") << true;
}

void BenchMainWindowTest::MusicBrainz::musicBrainzIdentifyStagesChosenVersion() {
    QFETCH(bool, untagged);
    // Its covers are another test's (ADR-0316).
    QSettings{}.setValue(QStringLiteral("identify/fetch-covers"), false);
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto make_source = [&field, untagged](const QString& label, const QString& title,
                                                const QString& track_number) {
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = "/music/" + label.toStdString() + ".flac",
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = untagged ? std::vector<metadata::MetadataField>{}
                                               : std::vector{field("ALBUM", {"Alpha"}),
                                                             field("ARTIST", {"Band"}),
                                                             field("TITLE", {title.toStdString()}),
                                                             field("TRACKNUMBER",
                                                                   {track_number.toStdString()})},
                            .unsupported_native_objects = {},
                        },
                },
            .track_label = label,
        };
    };
    const std::vector sources{
        make_source(untagged ? QStringLiteral("10-second") : QStringLiteral("one"),
                    QStringLiteral("One"), QStringLiteral("1")),
        make_source(untagged ? QStringLiteral("2-first") : QStringLiteral("two"),
                    QStringLiteral("Two"), QStringLiteral("2"))};

    static constexpr auto search_body = R"json({
      "count": 1,
      "releases": [{
        "id": "11111111-2222-3333-4444-555555555555",
        "score": 100, "title": "Alpha", "status": "Official",
        "date": "1999-09-09", "country": "DE", "track-count": 2,
        "artist-credit": [{"name": "Band",
          "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
        "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
        "media": [{"format": "CD", "track-count": 2}]
      }]
    })json";
    static constexpr auto lookup_body = R"json({
      "id": "11111111-2222-3333-4444-555555555555",
      "title": "Alpha", "status": "Official", "date": "1999-09-09", "country": "DE",
      "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
      "artist-credit": [{"name": "Band",
        "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
      "media": [{"position": 1, "format": "CD", "track-count": 2, "tracks": [
        {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
         "title": "One", "length": 61000,
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}},
        {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
         "title": "Two", "length": 59000,
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000002", "title": "Two"}}
      ]}]
    })json";
    int fetches = 0;
    QString search_url;
    const MusicBrainzLookupService service{
        .fetch =
            [&fetches, &search_url](const QString& url,
                                    std::function<void(core::Result<QByteArray>)> completion) {
                ++fetches;
                if (url.contains(QStringLiteral("?query="))) {
                    search_url = QUrl::fromPercentEncoding(url.toUtf8());
                    completion(QByteArray{search_body});
                    return;
                }
                if (url.contains(QStringLiteral("11111111-2222-3333-4444-555555555555?inc="))) {
                    completion(QByteArray{lookup_body});
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

    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* identify = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify"));
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(identify != nullptr);
    QVERIFY(status != nullptr);
    files->selectAll();
    QTRY_VERIFY(identify->isEnabled());
    QTest::mouseClick(identify, Qt::LeftButton);

    // The in-app search surface opens pre-filled from the selection — no
    // MusicBrainz tags exist anywhere on these files.
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-musicbrainz-identify"))) != nullptr);
    auto* artist =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-musicbrainz-identify-artist"));
    auto* album =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-musicbrainz-identify-release"));
    auto* search =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-search"));
    auto* results =
        dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-identify-results"));
    auto* use = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-apply"));
    QVERIFY(artist && album);
    QCOMPARE(artist->text(), untagged ? QString{} : QStringLiteral("Band"));
    QCOMPARE(album->text(), untagged ? QString{} : QStringLiteral("Alpha"));
    artist->setText(QStringLiteral("Band"));
    album->setText(QStringLiteral("Alpha"));
    QVERIFY(search != nullptr);
    QVERIFY(results != nullptr);
    QVERIFY(use != nullptr);

    QTest::mouseClick(search, Qt::LeftButton);
    QTRY_COMPARE(results->topLevelItemCount(), 1);
    QVERIFY(search_url.contains(QStringLiteral("artist:")));
    QVERIFY(search_url.contains(QStringLiteral("release:")));
    QVERIFY(!search_url.contains(QStringLiteral("tracks:")));
    QCOMPARE(results->topLevelItem(0)->text(0), QStringLiteral("Band — Alpha"));
    QVERIFY(results->topLevelItem(0)
                ->data(0, Qt::UserRole + 1)
                .toString()
                .contains(QStringLiteral("1999-09-09")));
    // The version chosen is previewed beneath it: its pairing, to change
    // there; Apply stages it (ADR-0311).
    QTRY_VERIFY(dialog->findChild<QListWidget*>(QStringLiteral("bench-musicbrainz-match-board")) !=
                nullptr);
    auto* stage_matches = use;
    if (untagged) {
        auto* board =
            dialog->findChild<QListWidget*>(QStringLiteral("bench-musicbrainz-match-board"));
        auto* up = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-up"));
        auto* down =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-down"));
        auto* unmatch =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-unmatch"));
        auto* sort =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-sort"));
        QVERIFY(board && up && down && unmatch && sort);
        QTRY_COMPARE(matchRowCount(board), 2);
        sort->click();
        QVERIFY(matchRow(board, 0)->text().endsWith(QStringLiteral("2-first.flac")));
        const auto first_album_track = matchTrack(matchRow(board, 0));
        const auto second_album_track = matchTrack(matchRow(board, 1));
        QVERIFY(first_album_track.contains(QStringLiteral("One")));
        QVERIFY(second_album_track.contains(QStringLiteral("Two")));
        board->setCurrentItem(matchRow(board, 0));
        QVERIFY(!up->isEnabled());
        QVERIFY(down->isEnabled());
        QVERIFY(matchRow(board, 0)->toolTip().contains(QStringLiteral("/music/2-first.flac")));
        // A file carried between files, by the board's own drop handler and
        // the payload its model makes: dropped below the second file.
        std::unique_ptr<QMimeData> mime{
            board->model()->mimeData({board->indexFromItem(matchRow(board, 0))})};
        const auto rect = board->visualItemRect(matchRow(board, 1));
        const QPoint position{rect.left() + 30, rect.bottom() - 1};
        QDragEnterEvent enter{dragPosition(position), Qt::MoveAction, mime.get(), Qt::LeftButton,
                              Qt::NoModifier};
        QApplication::sendEvent(board->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop{QPointF{position}, Qt::MoveAction, mime.get(), Qt::LeftButton,
                        Qt::NoModifier};
        QApplication::sendEvent(board->viewport(), &drop);
        QVERIFY(drop.isAccepted());
        // A payload from before the board changed is not taken.
        QDragEnterEvent stale{dragPosition(position), Qt::MoveAction, mime.get(), Qt::LeftButton,
                              Qt::NoModifier};
        QApplication::sendEvent(board->viewport(), &stale);
        QVERIFY(!stale.isAccepted());
        QVERIFY(matchRow(board, 1)->text().endsWith(QStringLiteral("2-first.flac")));
        QCOMPARE(matchTrack(matchRow(board, 0)), first_album_track);
        QCOMPARE(matchTrack(matchRow(board, 1)), second_album_track);
        QVERIFY(!down->isEnabled());
        board->setFocus();
        QTest::keyClick(board, Qt::Key_Up, Qt::AltModifier);
        QVERIFY(matchRow(board, 0)->text().endsWith(QStringLiteral("2-first.flac")));
        unmatch->click();
        QCOMPARE(matchRowCount(board), 3);
        QVERIFY(matchRow(board, 0)->text().isEmpty());
        QVERIFY(matchTrack(matchRow(board, 2)).contains(QStringLiteral("No tags")));
        QVERIFY(!unmatch->isEnabled());
        up->click();
        up->click();
        QCOMPARE(matchRowCount(board), 3);
        // ADR-0330: the file on no track, put back on the empty track.
        matchClickOnto(board, 2, 0);
        QTRY_COMPARE(matchRowCount(board), 2);
        QVERIFY(matchRow(board, 0)->text().endsWith(QStringLiteral("2-first.flac")));
        QCOMPARE(matchTrack(matchRow(board, 0)), first_album_track);
        QCOMPARE(matchTrack(matchRow(board, 1)), second_album_track);
    }
    QTRY_VERIFY(stage_matches->isEnabled());
    const auto screenshot_dir = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
    if (untagged && !screenshot_dir.isEmpty()) {
        QVERIFY(dialog->grab().save(screenshot_dir + QStringLiteral("/musicbrainz-matching.png")));
    }
    QTest::mouseClick(stage_matches, Qt::LeftButton);

    // The chosen version stages as one ordinary colored draft transaction.
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-musicbrainz-identify")) ==
                nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(grid_model->patches().patch_count() > 0U, 5'000);
    QTRY_VERIFY(status->text().contains(QStringLiteral("MusicBrainz")));
    // Tagged, searched by its tags as it opened too, and its best version
    // previewed: its track list (ADR-0311).
    QCOMPARE(fetches, untagged ? 2 : 4);
    if (untagged) {
        const auto title = grid_model->fieldColumn(QStringLiteral("Title"));
        QVERIFY(title);
        QCOMPARE(grid_model->index(0, *title).data(metadata_cell_values_role).toStringList(),
                 QStringList{QStringLiteral("Two")});
        QCOMPARE(grid_model->index(1, *title).data(metadata_cell_values_role).toStringList(),
                 QStringList{QStringLiteral("One")});
    }
    const auto album_id_column = grid_model->fieldColumn(QStringLiteral("MUSICBRAINZ_ALBUMID"));
    QVERIFY(album_id_column.has_value());
    QCOMPARE(
        grid_model->index(0, *album_id_column).data(metadata_cell_staged_source_role).toString(),
        QStringLiteral("MusicBrainz"));
    const auto date_column = grid_model->fieldColumn(QStringLiteral("Date"));
    QVERIFY(album_id_column.has_value());
    QVERIFY(date_column.has_value());
    for (int row = 0; row < grid_model->rowCount(); ++row) {
        QCOMPARE(
            grid_model->index(row, *album_id_column).data(metadata_cell_values_role).toStringList(),
            QStringList{QStringLiteral("11111111-2222-3333-4444-555555555555")});
        QCOMPARE(
            grid_model->index(row, *date_column).data(metadata_cell_values_role).toStringList(),
            QStringList{QStringLiteral("1999-09-09")});
    }
    delete properties;
}

// ADR-0310: the release the files are tagged with, looked up as Identify
// opens; and a release named by its link where the album is typed, looked
// up rather than searched for -- each with its album's other versions
// beneath it, to change to; an album's link lists them all.
void BenchMainWindowTest::MusicBrainz::musicBrainzIdentifyLooksUpNamedReleases() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto make_source = [&field](const QString& title, const QString& track_number) {
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = "/music/" + title.toLower().toStdString() + ".flac",
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = {field("ALBUM", {"Alpha"}), field("ARTIST", {"Band"}),
                                       field("TITLE", {title.toStdString()}),
                                       field("TRACKNUMBER", {track_number.toStdString()}),
                                       field("MUSICBRAINZ_ALBUMID",
                                             {"11111111-2222-3333-4444-555555555555"})},
                            .unsupported_native_objects = {},
                        },
                },
            .track_label = title,
        };
    };
    const std::vector sources{make_source(QStringLiteral("One"), QStringLiteral("1")),
                              make_source(QStringLiteral("Two"), QStringLiteral("2"))};
    const auto release_body = [](const char* id, const char* title) {
        return QStringLiteral(R"json({
          "id": "%1", "title": "%2", "status": "Official", "date": "1999-09-09",
          "country": "DE", "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
          "artist-credit": [{"name": "Band",
            "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
          "media": [{"position": 1, "format": "CD", "track-count": 2, "tracks": [
            {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
             "title": "One", "length": 61000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}},
            {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
             "title": "Two", "length": 59000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000002", "title": "Two"}}
          ]}]
        })json")
            .arg(QLatin1String{id}, QLatin1String{title})
            .toUtf8();
    };
    // The album's versions: the two above, and one of three tracks.
    static constexpr auto group_body = R"json({
      "release-count": 3, "release-offset": 0,
      "releases": [
        {"id": "33333333-2222-3333-4444-555555555555", "title": "Alpha", "date": "1998",
         "media": [{"format": "Vinyl", "track-count": 3}]},
        {"id": "22222222-2222-3333-4444-555555555555", "title": "Alpha (Deluxe)",
         "date": "2005", "media": [{"format": "CD", "track-count": 2}]},
        {"id": "11111111-2222-3333-4444-555555555555", "title": "Alpha", "date": "1999-09-09",
         "media": [{"format": "CD", "track-count": 2}]}
      ]
    })json";
    QStringList asked;
    const MusicBrainzLookupService service{
        .fetch =
            [&asked, &release_body](const QString& url,
                                    std::function<void(core::Result<QByteArray>)> completion) {
                asked << url.section(QLatin1Char('?'), 0, 0).section(QLatin1Char('/'), -1);
                if (url.contains(QStringLiteral(
                        "release?release-group=99999999-8888-7777-6666-555555555555&"))) {
                    completion(QByteArray{group_body});
                    return;
                }
                if (url.contains(QStringLiteral("11111111-2222-3333-4444-555555555555?inc="))) {
                    completion(release_body("11111111-2222-3333-4444-555555555555", "Alpha"));
                    return;
                }
                if (url.contains(QStringLiteral("22222222-2222-3333-4444-555555555555?inc="))) {
                    completion(
                        release_body("22222222-2222-3333-4444-555555555555", "Alpha (Deluxe)"));
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

    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    properties->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* identify = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify"));
    QVERIFY(grid_model != nullptr && identify != nullptr);
    files->selectAll();
    QTRY_VERIFY(identify->isEnabled());
    QTest::mouseClick(identify, Qt::LeftButton);
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-musicbrainz-identify"))) != nullptr);
    auto* album =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-musicbrainz-identify-release"));
    auto* search =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-search"));
    auto* results =
        dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-identify-results"));
    auto* use = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-apply"));
    QVERIFY(album && search && results && use);

    const auto rows = [results] {
        QStringList shown;
        for (int row = 0; row < results->topLevelItemCount(); ++row) {
            // The album, and how it was found and how many tracks it has.
            shown << results->topLevelItem(row)->text(0).section(QStringLiteral(" — "), -1) +
                         QStringLiteral(": ") +
                         results->topLevelItem(row)
                             ->data(0, Qt::UserRole + 1)
                             .toString()
                             .split(QStringLiteral(" · "))
                             .filter(QRegularExpression{QStringLiteral(
                                 "^(Tagged release|Release named|same track count|\\d+ tracks?)$")})
                             .join(QStringLiteral(" · "));
        }
        return shown;
    };
    // The tagged release, looked up by its id -- no search -- and its
    // album's other versions beneath it, those of as many tracks first.
    QTRY_VERIFY(search->isEnabled() && results->topLevelItemCount() == 3);
    QCOMPARE(rows(), (QStringList{QStringLiteral("Alpha: Tagged release · 2 tracks"),
                                  QStringLiteral("Alpha (Deluxe): same track count · 2 tracks"),
                                  QStringLiteral("Alpha: 3 tracks")}));
    QCOMPARE(asked, (QStringList{QStringLiteral("11111111-2222-3333-4444-555555555555"),
                                 QStringLiteral("release")}));
    QCOMPARE(results->currentItem(), results->topLevelItem(0));

    // Another, by its link where the album is typed.
    album->setText(QStringLiteral(
        " https://musicbrainz.org/release/22222222-2222-3333-4444-555555555555/discids "));
    // Enter in the field searches.
    album->setFocus();
    QTest::keyClick(album, Qt::Key_Return);
    QTRY_VERIFY(search->isEnabled() && results->topLevelItemCount() == 3);
    // And is not Apply: the field leaves Enter to the window around it, whose
    // default button Apply is -- with a lookup that takes its time, still
    // enabled when it arrives. Delivered as Qt hands it on.
    QTRY_VERIFY(use->isEnabled());
    QVERIFY(use->isDefault());
    QSignalSpy applied{use, &QPushButton::clicked};
    QKeyEvent enter{QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QApplication::sendEvent(dialog, &enter);
    QCOMPARE(applied.count(), 0);
    QCOMPARE(rows(), (QStringList{QStringLiteral("Alpha (Deluxe): Release named · 2 tracks"),
                                  QStringLiteral("Alpha: same track count · 2 tracks"),
                                  QStringLiteral("Alpha: 3 tracks")}));
    QCOMPARE(asked.size(), 4);
    QCOMPARE(asked[2], QStringLiteral("22222222-2222-3333-4444-555555555555"));

    // The album by its link: every version of it.
    album->setText(QStringLiteral(
        "https://musicbrainz.org/release-group/99999999-8888-7777-6666-555555555555"));
    QTest::mouseClick(search, Qt::LeftButton);
    // Listed, and the first previewed: its track list.
    QTRY_VERIFY(search->isEnabled() && asked.size() == 6);
    QTRY_COMPARE(rows(), (QStringList{QStringLiteral("Alpha: same track count · 2 tracks"),
                                      QStringLiteral("Alpha (Deluxe): same track count · 2 tracks"),
                                      QStringLiteral("Alpha: 3 tracks")}));

    // Another version chosen, previewed and applied as a found one is.
    results->setCurrentItem(results->topLevelItem(1));
    QTRY_VERIFY(
        dialog->findChild<QLabel*>(QStringLiteral("bench-musicbrainz-identify-match-heading"))
            ->text()
            .contains(QStringLiteral("Alpha (Deluxe)")));
    QTRY_VERIFY(use->isEnabled());
    QTest::mouseClick(use, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-musicbrainz-identify")) ==
                nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(grid_model->patches().patch_count() > 0U, 5'000);
    const auto album_column = grid_model->fieldColumn(QStringLiteral("Album"));
    QVERIFY(album_column.has_value());
    QCOMPARE(grid_model->index(0, *album_column).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("Alpha (Deluxe)")});
    delete properties;
}

// ADR-0311: several albums chosen are identified one at a time -- each its
// own search, versions and matching -- beside a list of them; those ahead
// looked up meanwhile; each one's pairing chosen, or the album skipped, and
// the next shown; Apply stages every album chosen.
void BenchMainWindowTest::MusicBrainz::musicBrainzIdentifyStepsThroughAlbums() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto make_source = [&field](const std::string& album, const std::string& title,
                                      const std::string& track, const std::string& release) {
        std::vector fields{field("ALBUM", {album}), field("ARTIST", {"Band"}),
                           field("TITLE", {title}), field("TRACKNUMBER", {track})};
        if (!release.empty()) {
            fields.push_back(field("MUSICBRAINZ_ALBUMID", {release}));
        }
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = "/music/" + album + "/" + track + ".flac",
                    .source_revision = std::nullopt,
                    .baseline = metadata::MetadataDocument{.fields = std::move(fields),
                                                           .unsupported_native_objects = {}},
                },
            .track_label = QString::fromStdString(title),
        };
    };
    const std::string alpha_id = "11111111-2222-3333-4444-555555555555";
    const std::vector sources{
        make_source("Alpha", "One", "1", alpha_id), make_source("Alpha", "Two", "2", alpha_id),
        make_source("Beta", "Three", "1", {}), make_source("Beta", "Four", "2", {})};
    const auto release_body = [](const char* id, const char* title, const char* group,
                                 const char* first, const char* second) {
        return QStringLiteral(R"json({
          "id": "%1", "title": "%2", "status": "Official", "date": "1999-09-09",
          "country": "DE", "release-group": {"id": "%3"},
          "artist-credit": [{"name": "Band",
            "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
          "media": [{"position": 1, "format": "CD", "track-count": 2, "tracks": [
            {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
             "title": "%4", "length": 61000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "%4"}},
            {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
             "title": "%5", "length": 59000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000002", "title": "%5"}}
          ]}]
        })json")
            .arg(QLatin1String{id}, QLatin1String{title}, QLatin1String{group},
                 QLatin1String{first}, QLatin1String{second})
            .toUtf8();
    };
    static constexpr auto beta_search = R"json({
      "count": 1,
      "releases": [{
        "id": "44444444-2222-3333-4444-555555555555",
        "score": 100, "title": "Beta", "status": "Official", "track-count": 2,
        "artist-credit": [{"name": "Band",
          "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
        "release-group": {"id": "88888888-8888-7777-6666-555555555555"},
        "media": [{"format": "CD", "track-count": 2}]
      }]
    })json";
    QStringList asked;
    const MusicBrainzLookupService service{
        .fetch =
            [&asked, &release_body](const QString& url,
                                    std::function<void(core::Result<QByteArray>)> completion) {
                const auto plain = QUrl::fromPercentEncoding(url.toUtf8());
                if (plain.contains(QStringLiteral("?query="))) {
                    asked << QStringLiteral("search");
                    completion(plain.contains(QStringLiteral("Beta")) ? QByteArray{beta_search}
                                                                      : QByteArray{});
                    return;
                }
                if (plain.contains(QStringLiteral("release?release-group="))) {
                    asked << QStringLiteral("versions");
                    completion(QByteArray{R"json({"release-count": 0, "releases": []})json"});
                    return;
                }
                if (plain.contains(QStringLiteral("11111111-2222-3333-4444-555555555555?inc="))) {
                    asked << QStringLiteral("alpha");
                    completion(release_body("11111111-2222-3333-4444-555555555555", "Alpha",
                                            "99999999-8888-7777-6666-555555555555", "One", "Two"));
                    return;
                }
                if (plain.contains(QStringLiteral("44444444-2222-3333-4444-555555555555?inc="))) {
                    asked << QStringLiteral("beta");
                    completion(release_body("44444444-2222-3333-4444-555555555555", "Beta",
                                            "88888888-8888-7777-6666-555555555555", "Three",
                                            "Four"));
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

    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    properties->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* identify = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify"));
    QVERIFY(grid_model != nullptr && identify != nullptr);
    files->selectAll();
    QTRY_VERIFY(identify->isEnabled());
    QTest::mouseClick(identify, Qt::LeftButton);
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-musicbrainz-identify"))) != nullptr);
    QPointer<QDialog> open = dialog;

    // The two albums, listed; the first shown, the second looked up
    // meanwhile -- by its tags, having no release id.
    auto* albums =
        dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-identify-albums"));
    auto* place = dialog->findChild<QLabel*>(QStringLiteral("bench-musicbrainz-identify-place"));
    QVERIFY(albums != nullptr && place != nullptr);
    QVERIFY(albums->isVisible());
    QCOMPARE(albums->topLevelItemCount(), 2);
    QCOMPARE(albums->topLevelItem(0)->text(0), QStringLiteral("Band — Alpha"));
    QCOMPARE(albums->topLevelItem(1)->text(0), QStringLiteral("Band — Beta"));
    // The second's versions there; the first's best version previewed,
    // and with it chosen.
    QTRY_COMPARE(albums->topLevelItem(1)->data(0, Qt::UserRole + 1).toString(),
                 QStringLiteral("1 version"));
    QTRY_COMPARE(albums->topLevelItem(0)->data(0, Qt::UserRole + 1).toString(),
                 QStringLiteral("✓ Alpha · CD"));
    QVERIFY(place->text().startsWith(QStringLiteral("Album 1 of 2")));
    auto* apply =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-apply"));
    auto* skip = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-skip"));
    auto* next = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-next"));
    QVERIFY(apply != nullptr && skip != nullptr && next != nullptr);
    QTRY_COMPARE(apply->text(), QStringLiteral("Apply 1 album"));
    QVERIFY(apply->isEnabled());

    // Next: the second, its best version previewed too.
    next->click();
    QTRY_VERIFY(place->text().startsWith(QStringLiteral("Album 2 of 2")));
    QTRY_COMPARE(albums->topLevelItem(1)->data(0, Qt::UserRole + 1).toString(),
                 QStringLiteral("✓ Beta · CD"));
    QTRY_COMPARE(apply->text(), QStringLiteral("Apply 2 albums"));
    QCOMPARE(grid_model->patches().patch_count(), 0U);
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/identify-albums-steps.png")));
    }
    // Skipped, then not.
    skip->click();
    QCOMPARE(albums->topLevelItem(1)->data(0, Qt::UserRole + 1).toString(),
             QStringLiteral("Skipped"));
    QCOMPARE(apply->text(), QStringLiteral("Apply 1 album"));
    QCOMPARE(skip->text(), QStringLiteral("Don't skip"));
    skip->click();
    QCOMPARE(apply->text(), QStringLiteral("Apply 2 albums"));

    // Applied: both staged, and done.
    apply->click();
    QTRY_VERIFY(open.isNull());
    QCOMPARE(asked.count(QStringLiteral("search")), 1);
    const auto title = grid_model->fieldColumn(QStringLiteral("Album"));
    const auto release = grid_model->fieldColumn(QStringLiteral("MUSICBRAINZ_ALBUMID"));
    QVERIFY(title.has_value() && release.has_value());
    QTRY_COMPARE(grid_model->index(3, *release).data(metadata_cell_values_role).toStringList(),
                 QStringList{QStringLiteral("44444444-2222-3333-4444-555555555555")});
    QCOMPARE(grid_model->index(0, *release).data(metadata_cell_values_role).toStringList(),
             QStringList{QString::fromStdString(alpha_id)});
    QCOMPARE(grid_model->index(2, *title).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("Beta")});
    delete properties;
}

void BenchMainWindowTest::MusicBrainz::musicBrainzFingerprintScanRanksAndStages() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    // Only titles and track numbers — the fingerprint path must work with
    // no album, artist, or MusicBrainz tags at all.
    const auto make_source = [&field](const QString& label, const QString& title,
                                      const QString& track_number) {
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = "/music/" + label.toStdString() + ".flac",
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = {field("TITLE", {title.toStdString()}),
                                       field("TRACKNUMBER", {track_number.toStdString()})},
                            .unsupported_native_objects = {},
                        },
                },
            .track_label = label,
        };
    };
    const std::vector sources{
        make_source(QStringLiteral("one"), QStringLiteral("One"), QStringLiteral("1")),
        make_source(QStringLiteral("two"), QStringLiteral("Two"), QStringLiteral("2"))};

    static constexpr auto lookup_body = R"json({
      "id": "11111111-2222-3333-4444-555555555555",
      "title": "Alpha", "status": "Official", "date": "1999-09-09", "country": "DE",
      "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
      "artist-credit": [{"name": "Band",
        "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
      "media": [{"position": 1, "format": "CD", "track-count": 2, "tracks": [
        {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
         "title": "One", "length": 61000,
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}},
        {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
         "title": "Two", "length": 59000,
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000002", "title": "Two"}}
      ]}]
    })json";
    static constexpr auto acoustid_one = R"json({
      "status": "ok",
      "results": [{"id": "x", "score": 0.98, "recordings": [
        {"id": "bbbb1111-0000-0000-0000-000000000001",
         "releases": [{"id": "11111111-2222-3333-4444-555555555555"}]}]}]
    })json";
    static constexpr auto acoustid_two = R"json({
      "status": "ok",
      "results": [{"id": "y", "score": 0.97, "recordings": [
        {"id": "bbbb1111-0000-0000-0000-000000000002",
         "releases": [{"id": "11111111-2222-3333-4444-555555555555"},
                      {"id": "22222222-2222-3333-4444-555555555555"}]}]}]
    })json";
    int fingerprints = 0;
    int lookups = 0;
    const MusicBrainzLookupService service{
        .fetch =
            [](const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                if (url.contains(QStringLiteral("11111111-2222-3333-4444-555555555555?inc="))) {
                    completion(QByteArray{lookup_body});
                    return;
                }
                completion(std::unexpected(core::Error{
                    .code = core::ErrorCode::not_found,
                    .message = "no such release",
                    .context = {},
                }));
            },
        .fingerprint =
            [&fingerprints](const QString& file_path,
                            std::function<void(core::Result<AcoustIdFingerprint>)> completion) {
                ++fingerprints;
                completion(AcoustIdFingerprint{
                    .duration_seconds = file_path.contains(QStringLiteral("one")) ? 61U : 59U,
                    .fingerprint = QStringLiteral("AQAD-fake-") + file_path,
                });
            },
        .acoustid_lookup =
            [&lookups](const AcoustIdFingerprint& fingerprint,
                       std::function<void(core::Result<QByteArray>)> completion) {
                ++lookups;
                completion(QByteArray{fingerprint.fingerprint.contains(QStringLiteral("one"))
                                          ? acoustid_one
                                          : acoustid_two});
            },
    };

    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* identify = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(identify != nullptr);
    files->selectAll();
    QTRY_VERIFY(identify->isEnabled());
    QTest::mouseClick(identify, Qt::LeftButton);

    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-musicbrainz-identify"))) != nullptr);
    auto* scan = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-scan"));
    auto* results =
        dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-identify-results"));
    auto* use = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-apply"));
    QVERIFY(scan != nullptr && scan->isEnabled());
    QVERIFY(results != nullptr);
    QVERIFY(use != nullptr);
    QTest::mouseClick(scan, Qt::LeftButton);

    // Both files fingerprinted and looked up; the release both matched
    // ranks first with its coverage shown, and the unloadable second
    // candidate drops out instead of blocking.
    QTRY_COMPARE(results->topLevelItemCount(), 1);
    QCOMPARE(fingerprints, 2);
    QCOMPARE(lookups, 2);
    QVERIFY(results->topLevelItem(0)->text(0).endsWith(QStringLiteral("Alpha")));
    QVERIFY(results->topLevelItem(0)
                ->data(0, Qt::UserRole + 1)
                .toString()
                .startsWith(QStringLiteral("2/2 files")));

    QTRY_VERIFY(use->isEnabled());
    QTest::mouseClick(use, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-musicbrainz-identify")) ==
                nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(grid_model->patches().patch_count() > 0U, 5'000);
    const auto album_column = grid_model->fieldColumn(QStringLiteral("Album"));
    QVERIFY(album_column.has_value());
    QCOMPARE(grid_model->index(0, *album_column).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("Alpha")});
    delete properties;
}

void BenchMainWindowTest::MusicBrainz::musicBrainzStagesFromCachedSearchMetadata() {
    QTemporaryDir media;
    const auto path = media.filePath(QStringLiteral("cached-release.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    auto native = metadata::read_local_metadata(QFile::encodeName(path).toStdString());
    QVERIFY(native && native->document.first_effective_value("musicbrainzworkid"));
    auto cached = native->document;
    for (auto& field : cached.fields) {
        // The library index retains canonical names, not native tag spellings.
        field.native_name = field.canonical_name;
        field.provenance = metadata::FieldProvenance::cached_snapshot;
    }
    metadata::StagedMetadataSource source{.raw_path = QFile::encodeName(path).toStdString(),
                                          .source_revision = {},
                                          .baseline = cached,
                                          .needs_metadata_capture = true};
    const metadata::MetadataProposalSet proposals{
        .provider_name = "MusicBrainz",
        .provider_detail = "Matched release",
        .items = {{.item_index = 0,
                   .fields = {{.canonical_field = "musicbrainzworkid",
                               .display_field = "MUSICBRAINZ_WORKID",
                               .values = {"11111111-2222-3333-4444-555555555555"},
                               .confidence = 1.0,
                               .rationale = "Matched recording"}},
                   .artwork = {}}}};
    // Reproduce the old false-stale rejection when a cache document is used
    // directly as a native baseline.
    auto old_selection = metadata::StagedMetadataSelection::create({source}, {});
    QVERIFY(old_selection);
    MetadataGridModel old_grid{std::move(*old_selection), {QStringLiteral("Cached")}};
    auto old_preview = metadata::metadata_proposal_preview(old_grid.selection(), old_grid.patches(),
                                                           proposals, 0.5);
    QVERIFY(old_preview);
    QSignalSpy rejected{&old_grid, &MetadataGridModel::editRejected};
    QVERIFY(!old_grid.stageTransformation(*old_preview));
    QVERIFY(rejected.front().front().toString().contains(QStringLiteral("draft changed")));

    MetadataPropertiesDialog properties{
        1,
        [source](std::size_t) -> std::optional<MetadataPropertiesSource> {
            return MetadataPropertiesSource{.source = source,
                                            .track_label = QStringLiteral("Cached")};
        },
        {},
        {},
        {}};
    properties.show();
    MetadataGridModel* grid = nullptr;
    QTRY_VERIFY((grid = properties.findChild<MetadataGridModel*>()) != nullptr);
    // ADR-0257: shown on the cached tags first, then on the file's own.
    QTRY_VERIFY(grid->selection().source(0).source_revision);
    const auto preview =
        metadata::metadata_proposal_preview(grid->selection(), grid->patches(), proposals, 0.5);
    QVERIFY(preview);
    QVERIFY(grid->stageTransformation(*preview, {QStringLiteral("MusicBrainz")}));
    QCOMPARE(grid->patches().patch_count(), 1U);
    QVERIFY(grid->undo());
    QCOMPARE(grid->patches().patch_count(), 0U);
}

// ADR-0316: with Fetch covers ticked, each album Identify applies has its
// release's front cover from the Cover Art Archive staged, as Fetch cover…
// stages one chosen: each album's for its own files.
void BenchMainWindowTest::MusicBrainz::identifyFetchesTheCoversOfTheAlbumsApplied() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto alpha_id = QStringLiteral("11111111-2222-3333-4444-555555555555");
    const auto beta_id = QStringLiteral("44444444-2222-3333-4444-555555555555");
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    std::vector<MetadataPropertiesSource> sources;
    for (const auto& [album, release] : {std::pair{std::string{"Alpha"}, alpha_id.toStdString()},
                                         std::pair{std::string{"Beta"}, std::string{}}}) {
        QVERIFY(QDir{media.path()}.mkpath(QString::fromStdString(album)));
        const auto path = media.filePath(QString::fromStdString(album + "/01.flac"));
        QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), path));
        const auto encoded = QFile::encodeName(path);
        const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
        auto read = metadata::read_local_metadata(raw_path);
        QVERIFY(read.has_value());
        auto document = read->document;
        std::erase_if(document.fields, [](const metadata::MetadataField& each) {
            return each.canonical_name == "ALBUM" || each.canonical_name == "ARTIST" ||
                   each.canonical_name == "ALBUMARTIST" || each.canonical_name == "TITLE" ||
                   each.canonical_name == "TRACKNUMBER" ||
                   each.canonical_name == "MUSICBRAINZ_ALBUMID";
        });
        document.fields.push_back(field("ALBUM", {album}));
        document.fields.push_back(field("ARTIST", {"Band"}));
        document.fields.push_back(field("TITLE", {"One"}));
        document.fields.push_back(field("TRACKNUMBER", {"1"}));
        if (!release.empty()) {
            document.fields.push_back(field("MUSICBRAINZ_ALBUMID", {release}));
        }
        sources.push_back(MetadataPropertiesSource{
            .source = metadata::StagedMetadataSource{.raw_path = raw_path,
                                                     .source_revision = read->source_revision,
                                                     .baseline = std::move(document)},
            .track_label = QString::fromStdString(album),
        });
    }
    QImage cover{16, 16, QImage::Format_ARGB32};
    cover.fill(Qt::darkCyan);
    QByteArray png;
    QBuffer buffer{&png};
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(cover.save(&buffer, "PNG"));
    buffer.close();
    const auto release_body = [](const QString& id, const char* title, const char* group) {
        return QStringLiteral(R"json({
          "id": "%1", "title": "%2", "status": "Official", "date": "1999-09-09",
          "country": "DE", "release-group": {"id": "%3"},
          "artist-credit": [{"name": "Band",
            "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
          "media": [{"position": 1, "format": "CD", "track-count": 1, "tracks": [
            {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
             "title": "One",
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}}
          ]}]
        })json")
            .arg(id, QLatin1String{title}, QLatin1String{group})
            .toUtf8();
    };
    // ADR-0326: Beta has nothing in the archive; its links name a Bandcamp
    // album page, whose cover is fetched instead. ADR-0332: Alpha's release
    // has nothing either, and no Bandcamp page; another release of its album
    // has a cover, which the archive gives for the release group.
    const auto listing = [] { return QByteArray{R"json({"images": []})json"}; };
    const auto alpha_group = QStringLiteral("99999999-8888-7777-6666-555555555555");
    const auto other_release = QStringLiteral("77777777-2222-3333-4444-555555555555");
    QStringList covers_asked;
    QStringList bandcamp_asked;
    QStringList groups_asked;
    const MusicBrainzLookupService service{
        .fetch =
            [&](const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                const auto plain = QUrl::fromPercentEncoding(url.toUtf8());
                if (plain.contains(QStringLiteral("?query="))) {
                    completion(QStringLiteral(R"json({"count": 1, "releases": [{
                        "id": "%1", "score": 100, "title": "Beta", "status": "Official",
                        "track-count": 1,
                        "artist-credit": [{"name": "Band", "artist": {
                          "id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
                        "release-group": {"id": "88888888-8888-7777-6666-555555555555"},
                        "media": [{"format": "CD", "track-count": 1}]}]})json")
                                   .arg(beta_id)
                                   .toUtf8());
                    return;
                }
                if (plain.contains(QStringLiteral("release?release-group="))) {
                    completion(QByteArray{R"json({"release-count": 0, "releases": []})json"});
                    return;
                }
                if (plain.contains(beta_id + QStringLiteral("?inc=url-rels"))) {
                    completion(QByteArray{R"json({"id": "x", "relations": [
                        {"type": "purchase for download",
                         "url": {"resource": "http://band.bandcamp.com/album/beta"}}],
                        "release-group": {"id": "88888888-8888-7777-6666-555555555555"}})json"});
                    return;
                }
                if (plain.contains(alpha_id + QStringLiteral("?inc=url-rels"))) {
                    completion(QStringLiteral(R"json({"id": "%1", "relations": [],
                        "release-group": {"id": "%2"}})json")
                                   .arg(alpha_id, alpha_group)
                                   .toUtf8());
                    return;
                }
                if (plain.startsWith(
                        QStringLiteral("https://coverartarchive.org/release-group/"))) {
                    groups_asked << plain.section(QLatin1Char('/'), -1);
                    completion(QStringLiteral(R"json({"images": [{"id": 9, "front": true,
                        "approved": true, "types": ["Front"],
                        "image": "http://coverartarchive.org/release/%1/9.png"}],
                        "release": "https://musicbrainz.org/release/%1"})json")
                                   .arg(other_release)
                                   .toUtf8());
                    return;
                }
                if (plain == QStringLiteral("https://coverartarchive.org/release/") +
                                 other_release + QStringLiteral("/9.png")) {
                    completion(png);
                    return;
                }
                if (plain == QStringLiteral("https://band.bandcamp.com/album/beta")) {
                    bandcamp_asked << plain;
                    completion(
                        QByteArray{R"(<html><head><meta property="og:image" )"
                                   R"(content="https://f4.bcbits.com/img/a0123456789_5.jpg">)"
                                   R"(</head></html>)"});
                    return;
                }
                if (plain == QStringLiteral("https://f4.bcbits.com/img/a0123456789_10.jpg")) {
                    bandcamp_asked << plain;
                    completion(png);
                    return;
                }
                for (const auto& [id, title, group] :
                     {std::tuple{alpha_id, "Alpha", "99999999-8888-7777-6666-555555555555"},
                      std::tuple{beta_id, "Beta", "88888888-8888-7777-6666-555555555555"}}) {
                    if (plain.contains(id + QStringLiteral("?inc="))) {
                        completion(release_body(id, title, group));
                        return;
                    }
                    if (plain == QStringLiteral("https://coverartarchive.org/release/") + id) {
                        covers_asked << title;
                        completion(listing());
                        return;
                    }
                }
                completion(std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                                       .message = "unexpected url",
                                                       .context = {}}));
            },
        .fingerprint = {},
        .acoustid_lookup = {},
    };
    const auto database_path =
        std::filesystem::path{media.filePath(QStringLiteral("cover.sqlite3")).toStdString()};
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    properties->setArtworkMutationServices(
        ArtworkWritePlanApplierFactory{[database_path] {
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
                        progress, cancellation, operations::ArtworkApplyOptions{});
                }};
        }},
        {});
    properties->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid = qobject_cast<MetadataGridModel*>(files->model());
    auto* identify = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify"));
    QVERIFY(grid != nullptr && identify != nullptr);
    files->selectAll();
    QTRY_VERIFY_WITH_TIMEOUT(identify->isEnabled(), 10'000);
    QTest::mouseClick(identify, Qt::LeftButton);
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-musicbrainz-identify"))) != nullptr);
    QPointer<QDialog> open = dialog;
    auto* fetch =
        dialog->findChild<QCheckBox*>(QStringLiteral("bench-musicbrainz-identify-fetch-covers"));
    auto* apply =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-apply"));
    auto* next = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-next"));
    QVERIFY(fetch != nullptr && apply != nullptr && next != nullptr);
    QVERIFY(fetch->isChecked());
    // Both albums seen, each previewed.
    QTRY_COMPARE_WITH_TIMEOUT(apply->text(), QStringLiteral("Apply 1 album"), 10'000);
    next->click();
    QTRY_COMPARE_WITH_TIMEOUT(apply->text(), QStringLiteral("Apply 2 albums"), 10'000);
    apply->click();
    QTRY_VERIFY_WITH_TIMEOUT(open.isNull(), 10'000);
    // Their covers, each staged for its own album's file.
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(status != nullptr);
    QTRY_VERIFY2_WITH_TIMEOUT(
        status->text().contains(QStringLiteral("Covers fetched for 2 albums")),
        qPrintable(status->text()), 20'000);
    QCOMPARE(covers_asked.size(), 2);
    // Alpha's from another release of its album -- asked only for Alpha:
    // Beta's own Bandcamp page came first.
    QCOMPARE(groups_asked, QStringList{alpha_group});
    // Beta's from Bandcamp, at 1200 pixels.
    QCOMPARE(bandcamp_asked,
             (QStringList{QStringLiteral("https://band.bandcamp.com/album/beta"),
                          QStringLiteral("https://f4.bcbits.com/img/a0123456789_10.jpg")}));
    auto* artwork = properties->findChild<ArtworkSession*>();
    QVERIFY(artwork != nullptr);
    std::set<std::string> covered;
    for (const auto& intent : artwork->pendingIntents()) {
        if (intent.kind == metadata::ArtworkWritePlanIntentKind::add &&
            intent.added_role == metadata::ArtworkRole::front) {
            covered.insert(intent.raw_media_path);
        }
    }
    QCOMPARE(covered.size(), std::size_t{2U});
    QVERIFY(covered.contains(sources[0].source.raw_path));
    QVERIFY(covered.contains(sources[1].source.raw_path));
    delete properties;
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::MusicBrainz)

#include "window_musicbrainz_test.moc"
