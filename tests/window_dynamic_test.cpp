// SPDX-License-Identifier: GPL-3.0-only

// The window test: dynamic playlists.

#include "window_test.hpp"

#include "trackknife/core/engine_state.hpp"

namespace trackknife::bench {

class BenchMainWindowTest::Dynamic final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void dynamicPlaylistsReadTheLibraryChosen();
    void dynamicPlaylistsAskTheEngineToPick();
    void dynamicPlaylistCatalogAndEditor();
    void dynamicPlaylistRetainsChangesDuringRefresh();
    void dynamicResultSelectionSurvivesRefresh();
    void dynamicResultActionsUseTheirOwnSources();
};

void BenchMainWindowTest::Dynamic::dynamicPlaylistRetainsChangesDuringRefresh() {
    using Service = DynamicPlaylistService;
    std::vector<Service::Completion> pending;
    DynamicPlaylistDialog dialog(
        {}, {},
        [&](const EngineKey&, DynamicPlaylistDefinition, core::CancellationToken, Service::Progress,
            Service::Completion done) { pending.push_back(std::move(done)); });
    dialog.setAttribute(Qt::WA_DeleteOnClose, false);
    auto* refresh = dialog.findChild<QPushButton*>(QStringLiteral("dynamic-refresh"));
    auto* stop = dialog.findChild<QPushButton*>(QStringLiteral("dynamic-stop"));
    refresh->click();
    QCOMPARE(pending.size(), 1U);
    dialog.libraryChanged();
    dialog.libraryChanged();
    LocalTrackRow stale;
    stale.raw_path = "/stale.flac";
    auto complete = std::move(pending.front());
    complete(Service::Picked{.tracks = {stale}, .matched = 1U, .unmatched = 0U});
    QCOMPARE(dialog.view()->model()->rowCount(), 0);
    QTRY_COMPARE(pending.size(), 2U);
    LocalTrackRow fresh;
    fresh.raw_path = "/fresh.flac";
    complete = std::move(pending.back());
    complete(Service::Picked{.tracks = {fresh}, .matched = 1U, .unmatched = 0U});
    QTRY_COMPARE(dialog.view()->model()->rowCount(), 1);
    QCOMPARE(qobject_cast<LocalListModel*>(dialog.view()->model())->rows().front().raw_path,
             fresh.raw_path);
    dialog.libraryChanged();
    stop->click();
    QTest::qWait(600);
    QCOMPARE(pending.size(), 2U);
}

void BenchMainWindowTest::Dynamic::dynamicResultSelectionSurvivesRefresh() {
    using Service = DynamicPlaylistService;
    {
        Service::Completion pending;
        DynamicPlaylistDialog dialog({}, {},
                                     [&](const EngineKey&, DynamicPlaylistDefinition,
                                         core::CancellationToken, Service::Progress,
                                         Service::Completion done) { pending = std::move(done); });
        dialog.setAttribute(Qt::WA_DeleteOnClose, false);
        dialog.show();
        auto* refresh = dialog.findChild<QPushButton*>(QStringLiteral("dynamic-refresh"));
        const auto deliver = [&](const std::vector<std::string>& paths) {
            std::vector<LocalTrackRow> rows;
            for (const auto& path : paths) {
                LocalTrackRow row;
                row.raw_path = path;
                rows.push_back(row);
            }
            const auto count = rows.size();
            pending(Service::Picked{.tracks = std::move(rows), .matched = count, .unmatched = 0U});
        };
        refresh->click();
        QVERIFY(pending);
        deliver({"a", "b", "a"});
        auto* view = dialog.view();
        QTRY_COMPARE(view->model()->rowCount(), 3);
        view->selectRow(2);
        view->setCurrentIndex(view->model()->index(2, ui::track_title_column));
        QSignalSpy played(&dialog, &DynamicPlaylistDialog::playRequested);
        refresh->click();
        QCOMPARE(view->model()->rowCount(), 3);
        deliver({"a", "a", "b"});
        QTRY_COMPARE(view->currentIndex().row(), 1);
        QCOMPARE(view->selectionModel()->selectedRows().size(), 1);
        QCOMPARE(view->selectionModel()->selectedRows().front().row(), 1);
        QCOMPARE(played.count(), 0);
        QVERIFY(view->dragEnabled());
        QVERIFY(!view->acceptDrops());
        QCOMPARE(view->defaultDropAction(), Qt::CopyAction);
        view->setFocus();
        QTest::keyClick(view, Qt::Key_Return);
        QCOMPARE(played.count(), 1);
        QCOMPARE(played.front().front().toInt(), 1);
        std::vector<std::string> many;
        for (int i = 0; i < 80; ++i)
            many.push_back("track-" + std::to_string(i));
        refresh->click();
        deliver(many);
        QTRY_COMPARE(view->model()->rowCount(), 80);
        view->scrollTo(view->model()->index(40, ui::track_title_column),
                       QAbstractItemView::PositionAtTop);
        const auto anchor = view->indexAt(QPoint{1, 1}).data(ui::track_source_role).toString();
        QVERIFY(!anchor.isEmpty());
        many.insert(many.begin(), "new-track");
        refresh->click();
        deliver(many);
        QTRY_COMPARE(view->model()->rowCount(), 81);
        QTRY_COMPARE(view->indexAt(QPoint{1, 1}).data(ui::track_source_role).toString(), anchor);
    }
}

void BenchMainWindowTest::Dynamic::dynamicResultActionsUseTheirOwnSources() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    window.tabs_->setCurrentWidget(window.list_tabs_.front()->view);
    window.showDynamicPlaylists();
    auto* dialog = window.findChild<DynamicPlaylistDialog*>();
    QVERIFY(dialog);
    LocalTrackRow row;
    row.raw_path = "/dynamic/source.flac";
    row.title = "Dynamic source";
    auto* service = dialog->findChild<DynamicPlaylistService*>();
    QVERIFY(service);
    emit service->finished(DynamicPlaylistService::Tracks{std::vector{row}}, 0, {});
    auto* view = dialog->view();
    view->selectRow(0);
    QMenu menu;
    window.addUpNextActions(&menu, view);
    QVERIFY(!menu.actions().empty());
    menu.actions().front()->trigger();
    QCOMPARE(window.playback_.requests.pending().size(), 1U);
    QCOMPARE(window.playback_.requests.pending().front().source.raw_path, row.raw_path);
    auto* target = window.currentListTab();
    QVERIFY(target);
    const auto count = target->model->rowCount();
    const auto id = QString::fromStdString(target->document.id.to_string());
    QVERIFY(!window.transferRows(view, {0}, id, true, -1));
    QVERIFY(window.transferRows(view, {0}, id, false, -1));
    QCOMPARE(target->model->rowCount(), count + 1);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    LocalTrackRow other = row;
    other.raw_path = "/dynamic/other.flac";
    target->model->replaceRows({row, other, row});
    window.playback_.anchors.document = document_identity(id);
    window.playback_.row = 2;
    window.playback_.anchors.source = target->model->source(2);
    dialog->setProperty("playback-context", id);
    model->replaceRows({row, row, other});
    emit dialog->resultsChanged();
    QVERIFY(!model->index(0, 0).data(ui::track_current_role).toBool());
    QVERIFY(model->index(1, 0).data(ui::track_current_role).toBool());
    const auto reader =
        window.selectionSourceReader(model, {QPersistentModelIndex{model->index(0, 0)}});
    model->replaceRows({});
    QVERIFY(reader(0));
    QCOMPARE(reader(0)->source.raw_path, row.raw_path);
    dialog->close();
}

void BenchMainWindowTest::Dynamic::dynamicPlaylistsReadTheLibraryChosen() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));

    // ADR-0286: the remote holds the library.
    testing::holdLibraryOnTheRemote();
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    {
        auto catalogue = window.remoteCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* remote_tab = window.remoteQueueTab();
    QVERIFY(remote_tab != nullptr);

    // Opened from a tab of this computer's, it reads the library all the
    // same: the one library, on the engine holding it.
    if (!window.list_tabs_.empty()) {
        for (auto& candidate : window.list_tabs_) {
            if (EngineKey::of(candidate->document).isLocal()) {
                window.tabs_->setCurrentWidget(candidate->view);
                break;
            }
        }
    }
    window.showDynamicPlaylists();
    auto* dialog = window.findChild<DynamicPlaylistDialog*>();
    QVERIFY(dialog != nullptr);
    auto* library = dialog->findChild<QComboBox*>(QStringLiteral("dynamic-library"));
    QVERIFY(library != nullptr);
    QCOMPARE(library->count(), 1);
    QVERIFY(!dialog->engine().isLocal());
    dialog->findChild<QLineEdit*>(QStringLiteral("dynamic-query"))->setText(QStringLiteral("ALL"));
    dialog->findChild<QPushButton*>(QStringLiteral("dynamic-refresh"))->click();
    const auto remote_path = QFile::encodeName(music + QStringLiteral("/art.flac")).toStdString();
    QTRY_COMPARE(dialog->tracks().size(), 1U);
    QCOMPARE(dialog->tracks().front().raw_path, remote_path);

    // What it finds opens on the remote's engine.
    const auto before = window.list_tabs_.size();
    dialog->findChild<QPushButton*>(QStringLiteral("dynamic-open"))->click();
    QTRY_COMPARE(window.list_tabs_.size(), before + 1);
    QVERIFY(!EngineKey::of(window.list_tabs_.back()->document).isLocal());
    QCOMPARE(window.list_tabs_.back()->model->rows().front().raw_path, remote_path);

    dialog->close();
    testing::holdLibraryHere();
}

// ADR-0350: the window does not pick -- not from the library, not from
// Last.fm. It asks the engine with the rule as written, shows its progress,
// and shows what it picked; matching Last.fm's suggestions and preferring
// tracks not picked the last time are the engine's (dynamic-rules).
void BenchMainWindowTest::Dynamic::dynamicPlaylistsAskTheEngineToPick() {
    using Service = DynamicPlaylistService;
    std::vector<DynamicPlaylistDefinition> asked;
    std::vector<Service::Progress> progressing;
    std::vector<Service::Completion> pending;
    std::vector<core::CancellationToken> tokens;
    Service service([&](DynamicPlaylistDefinition rule, core::CancellationToken cancellation,
                        Service::Progress progress, Service::Completion completion) {
        asked.push_back(std::move(rule));
        tokens.push_back(std::move(cancellation));
        progressing.push_back(std::move(progress));
        pending.push_back(std::move(completion));
    });
    QStringList said;
    connect(&service, &Service::progress, &service,
            [&said](const QString& message) { said.push_back(message); });
    int completions = 0;
    Service::Tracks output;
    QString failure;
    int unmatched = 0;
    connect(&service, &Service::finished, &service,
            [&](const Service::Tracks& rows, int missing, const QString& error) {
                ++completions;
                output = rows;
                unmatched = missing;
                failure = error;
            });
    LocalTrackRow row;
    row.raw_path = std::string("/raw-\xff.flac");
    row.artist = "A & B";
    row.title = "Song";

    // A Last.fm rule goes to the engine as it is.
    DynamicPlaylistDefinition similar{.id = {},
                                      .name = QStringLiteral("Like it"),
                                      .profile = {},
                                      .source = QStringLiteral("similar"),
                                      .artist = QStringLiteral("Seed"),
                                      .track = QStringLiteral("Song"),
                                      .limit = 2};
    service.refresh(similar);
    QCOMPARE(asked.size(), 1U);
    QCOMPARE(asked.back().source, QStringLiteral("similar"));
    QCOMPARE(asked.back().artist, QStringLiteral("Seed"));
    QCOMPARE(said.back(), QStringLiteral("Asking Last.fm…"));
    progressing.back()(20U, 120U);
    QCOMPARE(said.back(), QStringLiteral("Matching library tracks: 20 / 120"));
    pending.back()(Service::Picked{.tracks = {row}, .matched = 7U, .unmatched = 3U});
    QCOMPARE(completions, 1);
    QVERIFY(failure.isEmpty());
    QCOMPARE(output.size(), 1U);
    QCOMPARE(output.front().raw_path, row.raw_path);
    QCOMPARE(unmatched, 3);
    QCOMPARE(service.matchedPoolSize(), 7U);

    // What the engine says went wrong is said.
    service.refresh(similar);
    pending.back()(std::unexpected(core::Error{
        .code = core::ErrorCode::unsupported, .message = "Sign in to Last.fm", .context = {}}));
    QCOMPARE(completions, 2);
    QCOMPARE(failure, QStringLiteral("Sign in to Last.fm"));

    // A library rule, the same way.
    DynamicPlaylistDefinition favourites{.id = QStringLiteral("rules"),
                                         .name = QStringLiteral("Favorites"),
                                         .profile = QStringLiteral("local")};
    service.refresh(favourites);
    QCOMPARE(asked.back().query, QStringLiteral("rating GREATER 6"));
    QCOMPARE(said.back(), QStringLiteral("Updating from library tags and ratings…"));
    // Refreshed again before the engine answered: the first is cancelled,
    // and its answer, coming late, is not shown.
    auto late = pending.back();
    const auto first_token = tokens.back();
    service.refresh(favourites);
    QVERIFY(first_token.is_cancellation_requested());
    late(Service::Picked{.tracks = {}, .matched = 0U, .unmatched = 0U});
    progressing[progressing.size() - 2U](1U, 2U);
    QCOMPARE(completions, 2);
    QCOMPARE(said.back(), QStringLiteral("Updating from library tags and ratings…"));
    pending.back()(Service::Picked{.tracks = {row}, .matched = 1U, .unmatched = 0U});
    QCOMPARE(completions, 3);
    QCOMPARE(output.size(), 1U);
    service.refresh(favourites);
    auto cancelled = pending.back();
    service.cancel();
    cancelled(Service::Picked{.tracks = {row}, .matched = 1U, .unmatched = 0U});
    QCOMPARE(completions, 3);
}

void BenchMainWindowTest::Dynamic::dynamicPlaylistCatalogAndEditor() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.findChild<LocalLibraryPanel*>());
    auto* action = window.findChild<QAction*>(QStringLiteral("action-dynamic-playlists"));
    QVERIFY(action);
    action->trigger();
    auto* dialog = window.findChild<DynamicPlaylistDialog*>();
    QVERIFY(dialog);
    auto* catalog = dialog->findChild<QComboBox*>(QStringLiteral("dynamic-catalog"));
    QVERIFY(catalog);
    // "New" and the seven that ship -- the engine's.
    QTRY_COMPARE_WITH_TIMEOUT(catalog->count(), 8, 5'000);
    QVERIFY(window.localPlayback()->dynamicRules().front().shipped());
    auto* save = dialog->findChild<QPushButton*>(QStringLiteral("dynamic-save"));
    auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("dynamic-remove"));
    // A shipped rule is used and copied, not changed or removed.
    catalog->setCurrentIndex(2);
    emit catalog->activated(2);
    QCOMPARE(catalog->itemText(2), QStringLiteral("Random album"));
    QCOMPARE(dialog->findChild<QSpinBox*>(QStringLiteral("dynamic-groups"))->value(), 1);
    QVERIFY(!remove->isEnabled());
    QCOMPARE(save->text(), QStringLiteral("Save as copy"));
    dialog->findChild<QSpinBox*>(QStringLiteral("dynamic-per-group"))->setValue(2);
    save->click();
    QTRY_COMPARE(catalog->count(), 9);
    QCOMPARE(catalog->itemText(catalog->currentIndex()), QStringLiteral("Random album (copy)"));
    QVERIFY(remove->isEnabled());
    // The engine keeps it.
    QTRY_COMPARE(window.localPlayback()->dynamicRules().size(), 8);
    QCOMPARE(window.localPlayback()->dynamicRules().back().per_group, 2);
    QCOMPARE(window.localPlayback()->dynamicRules()[1].per_group, 0);
    remove->click();
    QTRY_COMPARE(catalog->count(), 8);
    QTRY_COMPARE(window.localPlayback()->dynamicRules().size(), 7);
    catalog->setCurrentIndex(0);
    emit catalog->activated(0);
    dialog->findChild<QLineEdit*>(QStringLiteral("dynamic-name"))->setText(QStringLiteral("Rock"));
    dialog->findChild<QLineEdit*>(QStringLiteral("dynamic-query"))
        ->setText(QStringLiteral("genre HAS rock"));
    save->click();
    QTRY_COMPARE(catalog->count(), 9);
    dialog->findChild<QPushButton*>(QStringLiteral("dynamic-refresh"))->click();
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("dynamic-status"));
    QTRY_VERIFY(status->text().startsWith(QStringLiteral("0 tracks")));
    QVERIFY(!dialog->findChild<QPushButton*>(QStringLiteral("dynamic-open"))->isEnabled());
    // ADR-0350: a Last.fm rule is the engine's to pick too; this one has no
    // Last.fm account, and says so.
    auto* source = dialog->findChild<QComboBox*>(QStringLiteral("dynamic-source"));
    source->setCurrentIndex(source->findData(QStringLiteral("loved")));
    dialog->findChild<QLineEdit*>(QStringLiteral("dynamic-user"))->setText(QStringLiteral("me"));
    dialog->findChild<QPushButton*>(QStringLiteral("dynamic-refresh"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("Sign in to Last.fm")),
                             10000);
    source->setCurrentIndex(source->findData(QStringLiteral("rules")));
    if (const auto screenshot_dir = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !screenshot_dir.isEmpty()) {
        QVERIFY(dialog->grab().save(screenshot_dir + QStringLiteral("/dynamic-playlists.png")));
    }
    // Opening a snapshot must keep the flat preview layout, even when the
    // ordinary workspace default groups albums.
    LocalTrackRow row;
    row.raw_path = "/dynamic-layout-test.flac";
    row.title = "A track";
    emit dialog->snapshotRequested(QStringLiteral("Dynamic layout"),
                                   DynamicPlaylistService::Tracks{row});
    auto* local_tab = window.currentListTab();
    QVERIFY(local_tab);
    QCOMPARE(local_tab->view_layout.presentation, ui::TrackViewPresentation::plain_columns);
    QVERIFY(!static_cast<ui::QueueTableView*>(local_tab->view)->albumGroupingEnabled());
    QVERIFY(local_tab->view->isColumnHidden(local_artwork_column));
    QPointer<DynamicPlaylistDialog> previous(dialog);
    dialog->close();
    QTRY_VERIFY(previous.isNull());
    action->trigger();
    dialog = window.findChild<DynamicPlaylistDialog*>();
    QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QComboBox*>(QStringLiteral("dynamic-catalog"))->count(), 9);
    dialog->close();
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::Dynamic)

#include "window_dynamic_test.moc"
