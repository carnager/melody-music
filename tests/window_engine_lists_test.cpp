// SPDX-License-Identifier: GPL-3.0-only

// The window test: lists kept on the engine and changed by other clients.

#include "window_test.hpp"

#include "bench/file_scope_view.hpp"
#include "trackknife/core/engine_state.hpp"
#include "uicommon/library_tree_view.hpp"
#include "workspace/artwork_session.hpp"
#include "workspace/sources.hpp"

#include <QGroupBox>
#include <QMessageBox>
#include <QPainter>
#include <QStandardItemModel>
#include <sqlite3.h>

namespace trackknife::bench {

class BenchMainWindowTest::EngineLists final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void theWindowsListsAreOnItsEngine();
    void listsTravelAsEdits();
    void anotherClientsListChangesReachTheWindow();
    void aListChangedWhileClosedIsTakenUpOnOpening();
    void aListFromElsewhereOpensAsATab();
    void aListFromTheEngineCarriesItsLibraryTags();
    void aMoveIsFollowedInListsNotOpenHere();
    void libraryAndFoldersAddToAChosenList();
    void theListsPanelShowsEveryListAndTakesDrops();
    void aScratchTabNeverReachesItsEngine();
    void aPlayingTabsEditsReachTheQueueAsEdits();
    void aListDeletedElsewhereWhileBeingWrittenStaysDeleted();
    void upNextFromAnotherClientShows();
    void aListReplacedOnTheFollowedEngineMarksWhatPlays();
    void aRatingSetElsewhereShowsInTheTabs();
    void aSaveWritesOnlyListsThatChanged();
};

// ADR-0269: Up Next is the engine's. What another client -- melody-cli, a
// rofi script, the phone -- asks for shows in the window's Up Next, and
// what it clears goes from it.
void BenchMainWindowTest::EngineLists::upNextFromAnotherClientShows() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("asked.wav"));
    write_wave(path, wave_sample_rate / 10U);
    const auto raw = QFile::encodeName(path).toStdString();
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    auto client = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(client.has_value());
    // As melody-cli queues: the entry handed over, then asked for.
    const auto entry = core::StableId::random().to_string();
    QVERIFY((*client)
                ->call("playback.enqueue",
                       protocol::Json{{"entries", protocol::Json::array({protocol::Json{
                                                      {"entry", entry},
                                                      {"path", protocol::encode_raw_path(raw)},
                                                      {"title", "Asked elsewhere"},
                                                      {"group",
                                                       {{"album_artist", "Someone"},
                                                        {"artist", "Someone"},
                                                        {"album", "Elsewhere"},
                                                        {"date", "2026"}}}}})}})
                .has_value());
    QVERIFY((*client)
                ->call("playback.set_requests",
                       protocol::Json{{"entries", protocol::Json::array({entry})}})
                .has_value());
    QTRY_COMPARE_WITH_TIMEOUT(window.workspace_.playback_.requests.pending().size(), 1U, 10'000);
    const auto& shown = window.workspace_.playback_.requests.pending().front().source;
    QCOMPARE(shown.title, std::string{"Asked elsewhere"});
    QCOMPARE(shown.album, std::string{"Elsewhere"});
    QCOMPARE(shown.raw_path, raw);
    QCOMPARE(shown.entry_id.to_string(), entry);
    // Cleared elsewhere: gone here too, and not stated back.
    QVERIFY((*client)->call("playback.clear_requests").has_value());
    QTRY_VERIFY_WITH_TIMEOUT(window.workspace_.playback_.requests.pending().empty(), 10'000);
    QTest::qWait(500);
    const auto asked = (*client)->call("playback.requests");
    QVERIFY(asked.has_value());
    QVERIFY(asked->value("entries", protocol::Json::array()).empty());
}

// ADR-0286: a tab is the window's; only a saved playlist is on its engine,
// for the phone and the CLI to see. A tab that is none never reaches it; one
// saved is written with the tab's own id, and not while it has unsaved edits.
void BenchMainWindowTest::EngineLists::theWindowsListsAreOnItsEngine() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto engine = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(engine.has_value());
    const auto listed = [&engine](const std::string& id) -> std::optional<protocol::Json> {
        auto all = (*engine)->call("list.all");
        if (!all) {
            return std::nullopt;
        }
        for (const auto& list : all->value("lists", protocol::Json::array())) {
            if (list.value("id", std::string{}) == id) {
                return list;
            }
        }
        return std::nullopt;
    };
    const auto rows = [](const int count) {
        std::vector<LocalTrackRow> made;
        for (int index = 0; index < count; ++index) {
            LocalTrackRow row;
            row.raw_path = "/music/list/" + std::to_string(index) + ".flac";
            row.title = "Track " + std::to_string(index);
            made.push_back(std::move(row));
        }
        return made;
    };

    // The window's own first tab: its own, not the engine's.
    QVERIFY(!window.list_tabs_.empty());
    const auto first = window.list_tabs_.front()->document.id.to_string();
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(!listed(first).has_value());

    auto* tab = window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                            .kind = persistence::ListKind::scratch,
                                                            .name = "Party",
                                                            .pinned = false,
                                                            .dirty = false,
                                                            .items = {}},
                                  true);
    const auto id = tab->document.id.to_string();
    tab->model->appendRows(rows(3));
    window.markTabDirty(*tab);
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(!listed(id).has_value());

    // Saved: a saved playlist of the tab's own id, with its entries.
    tab->document.kind = persistence::ListKind::saved;
    tab->document.dirty = false;
    window.persistNow(false);
    QTRY_VERIFY(listed(id).has_value() && listed(id)->value("tracks", 0) == 3);
    QCOMPARE(listed(id)->value("name", std::string{}), std::string{"Party"});
    QCOMPARE(listed(id)->value("kind", std::string{}), std::string{"saved"});
    auto items = (*engine)->call("list.get", protocol::Json{{"id", id}});
    QVERIFY(items.has_value());
    QCOMPARE(items->at("items").at(1).value("title", std::string{}), std::string{"Track 1"});
    QCOMPARE(items->at("items").at(0).value("entry", std::string{}),
             tab->model->rows().front().entry_id.to_string());

    // Edited, unsaved: not written.
    tab->model->appendRows(rows(1));
    tab->document.dirty = true;
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QCOMPARE(listed(id)->value("tracks", 0), 3);

    // A tab that is no saved playlist, filled and closed: never on the
    // engine.
    auto* scratch =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Scratch",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    const auto scratch_id = scratch->document.id.to_string();
    scratch->model->appendRows(rows(1));
    window.persistNow(false);
    QTest::qWait(300);
    QVERIFY(!listed(scratch_id).has_value());
    window.closeTabAt(window.tabs_->indexOf(scratch->view));
    window.persistNow(false);
    QVERIFY(!listed(scratch_id).has_value());
    // Saved again -- the edits go too -- and then closed, with nothing to ask
    // about: the list stays on the engine.
    tab->document.dirty = false;
    window.persistNow(false);
    QTRY_COMPARE(listed(id)->value("tracks", 0), 4);
    window.closeTabAt(window.tabs_->indexOf(tab->view));
    QVERIFY(window.tabForDocument(QString::fromStdString(id)) == nullptr);
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(listed(id).has_value());
    (*engine)->close();
}

// ADR-0256: a list is sent whole once, in batches the engine takes, and
// then as edits; a reconnect sends nothing; playing names the list.
void BenchMainWindowTest::EngineLists::listsTravelAsEdits() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto engine = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(engine.has_value());
    const auto summary = [&engine](const std::string& id) -> protocol::Json {
        auto all = (*engine)->call("list.all");
        for (const auto& list :
             all ? all->value("lists", protocol::Json::array()) : protocol::Json::array()) {
            if (list.value("id", std::string{}) == id) {
                return list;
            }
        }
        return protocol::Json::object();
    };
    const auto entries = [&engine](const std::string& id) {
        std::vector<std::string> found;
        auto got = (*engine)->call("list.get", protocol::Json{{"id", id}});
        for (const auto& item :
             got ? got->value("items", protocol::Json::array()) : protocol::Json::array()) {
            found.push_back(item.value("entry", std::string{}));
        }
        return found;
    };
    const auto shown = [](const LocalListModel& model) {
        std::vector<std::string> found;
        for (const auto& row : model.rows()) {
            found.push_back(row.entry_id.to_string());
        }
        return found;
    };

    // 8,000 tracks, over two megabytes of list: more than the engine takes
    // in one line, so it arrives in batches. A saved playlist: what is on an
    // engine (ADR-0286).
    auto* tab = window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                            .kind = persistence::ListKind::saved,
                                                            .name = "Long",
                                                            .pinned = false,
                                                            .dirty = false,
                                                            .items = {}},
                                  true);
    const auto id = tab->document.id.to_string();
    std::vector<LocalTrackRow> made;
    for (int index = 0; index < 8'000; ++index) {
        LocalTrackRow row;
        row.raw_path = "/music/a rather long folder name/an album of some length/" +
                       std::to_string(index) + " - a title of usual length.flac";
        row.title = "A title of usual length " + std::to_string(index);
        row.artist = "Someone";
        row.album = "An album of some length";
        row.album_artist = "Someone";
        row.date = "1999";
        row.duration_ms = 200'000;
        made.push_back(std::move(row));
    }
    tab->model->appendRows(std::move(made));
    window.markTabDirty(*tab);
    // Saved.
    tab->document.dirty = false;
    window.persistNow(false);
    QTRY_VERIFY_WITH_TIMEOUT(summary(id).value("tracks", 0) == 8'000 && !window.list_sync_->busy(),
                             30'000);
    QCOMPARE(entries(id), shown(*tab->model));
    const auto made_at = summary(id).value("revision", std::uint64_t{0});

    // Three rows dragged and saved: one edit, the list as shown.
    auto rows = tab->model->rows();
    std::rotate(rows.begin() + 5, rows.begin() + 8, rows.begin() + 101);
    tab->model->replaceRows(std::move(rows));
    window.markTabDirty(*tab);
    tab->document.dirty = false;
    window.persistNow(false);
    QTRY_VERIFY(summary(id).value("revision", std::uint64_t{0}) == made_at + 1U &&
                !window.list_sync_->busy());
    QCOMPARE(entries(id), shown(*tab->model));

    // Reconnecting compares; nothing is written.
    window.list_sync_->reconnected(window.localPlayback());
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QCOMPARE(summary(id).value("revision", std::uint64_t{0}), made_at + 1U);

    // Playing names the list: the engine's queue is its copy, entry for entry.
    window.playRow(*tab, 10);
    QTRY_VERIFY_WITH_TIMEOUT(window.localPlayback()->state().queue_size == 8'000U, 10'000);
    auto queue = (*engine)->call("playback.queue");
    QVERIFY(queue.has_value());
    const auto& held = queue->at("entries");
    QCOMPARE(held.size(), std::size_t{8'000});
    QCOMPARE(held.at(10).value("entry", std::string{}),
             tab->model->rows()[10].entry_id.to_string());
    QCOMPARE(held.at(10).at("group").value("album_artist", std::string{}), std::string{"Someone"});
    QCOMPARE(summary(id).value("revision", std::uint64_t{0}), made_at + 1U);

    // A row taken from the playing list leaves the engine's queue as well;
    // the playlist keeps it until Save.
    tab->model->removeRowIndexes({0});
    window.markTabDirty(*tab);
    QTRY_VERIFY(window.localPlayback()->state().queue_size == 7'999U);
    QTRY_VERIFY(!window.list_sync_->busy());
    QTest::qWait(300);
    QCOMPARE(tab->model->rowCount(), 7'999);
    QCOMPARE(entries(id).size(), std::size_t{8'000});

    // ADR-0286: a tab that is no saved playlist plays as long as it is: its
    // queue sent in parts, each within a line, and never a list.
    auto* scratch =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Long scratch",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    std::vector<LocalTrackRow> copied = tab->model->rows();
    for (auto& row : copied) {
        row.entry_id = core::StableId::random();
    }
    scratch->model->appendRows(std::move(copied));
    window.markTabDirty(*scratch);
    window.persistNow(false);
    window.playRow(*scratch, 7'000);
    QTRY_VERIFY_WITH_TIMEOUT(
        window.localPlayback()->state().queue_size == 7'999U &&
            window.localPlayback()->state().entry ==
                QString::fromStdString(scratch->model->rows()[7'000].entry_id.to_string()),
        10'000);
    queue = (*engine)->call("playback.queue");
    QVERIFY(queue.has_value());
    QCOMPARE(queue->at("entries").size(), std::size_t{7'999});
    QCOMPARE(queue->at("entries").at(7'998).value("entry", std::string{}),
             scratch->model->rows().back().entry_id.to_string());
    QCOMPARE(queue->at("entries").at(5'000).at("group").value("album", std::string{}),
             std::string{"An album of some length"});
    QVERIFY(summary(scratch->document.id.to_string()).empty());
    QVERIFY2(!engine_.log().contains("refused a"), "nothing the window sent was too long");
    (*engine)->close();
}

// ADR-0233: what another client does to a list open here reaches the tab: a
// change is shown, a deletion closes it. A saved list someone else saved
// since this window read it is not overwritten on Save: the choice is asked.
void BenchMainWindowTest::EngineLists::anotherClientsListChangesReachTheWindow() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    const auto revision = [&other](const std::string& id) {
        auto got = (*other)->call("list.get", protocol::Json{{"id", id}});
        return got ? got->value("revision", std::uint64_t{0}) : std::uint64_t{0};
    };

    // A saved playlist: what other clients see (ADR-0286).
    auto* tab = window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                            .kind = persistence::ListKind::saved,
                                                            .name = "Shared",
                                                            .pinned = false,
                                                            .dirty = false,
                                                            .items = {}},
                                  true);
    const auto id = tab->document.id.to_string();
    const auto qid = QString::fromStdString(id);
    LocalTrackRow row;
    row.raw_path = "/music/shared/a.flac";
    row.title = "a";
    tab->model->appendRows({row});
    window.persistNow(false);
    QTRY_VERIFY(revision(id) >= 1U);
    QTRY_VERIFY(!window.list_sync_->busy());

    // Changed elsewhere: the tab shows it.
    QVERIFY((*other)->call(
        "list.save",
        protocol::Json{{"id", id},
                       {"name", "Shared, renamed"},
                       {"items", listItems({"/music/shared/a.flac", "/music/shared/b.flac"})}}));
    QTRY_COMPARE(rowPaths(*tab->model),
                 (std::vector<std::string>{"/music/shared/a.flac", "/music/shared/b.flac"}));
    QCOMPARE(displayText(tab->document.name), QStringLiteral("Shared, renamed"));
    QVERIFY(!tab->document.dirty);

    // Saved here, then saved elsewhere, then edited here and saved: asked.
    tab->document.kind = persistence::ListKind::saved;
    window.persistNow(false);
    QTRY_COMPARE(
        (*other)->call("list.get", protocol::Json{{"id", id}})->value("kind", std::string{}),
        std::string{"saved"});
    QTRY_VERIFY(!window.list_sync_->busy());
    const auto theirs = revision(id);
    QVERIFY((*other)->call("list.save",
                           protocol::Json{{"id", id},
                                          {"name", "Shared, renamed"},
                                          {"kind", "saved"},
                                          {"revision", theirs},
                                          {"items", listItems({"/music/shared/theirs.flac"})}}));
    // Their change reached this window, which had no edits of its own, so it
    // shows theirs; now edited here, unsaved.
    QTRY_COMPARE(rowPaths(*tab->model), std::vector<std::string>{"/music/shared/theirs.flac"});
    QTRY_VERIFY(!window.list_sync_->busy());
    LocalTrackRow mine;
    mine.raw_path = "/music/shared/mine.flac";
    tab->model->appendRows({mine});
    tab->document.dirty = true;
    window.persistNow(false);
    QTRY_VERIFY(!window.list_sync_->busy());
    // Theirs again, meanwhile.
    QVERIFY((*other)->call("list.save",
                           protocol::Json{{"id", id},
                                          {"name", "Shared, renamed"},
                                          {"kind", "saved"},
                                          {"items", listItems({"/music/shared/again.flac"})}}));
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QCOMPARE(rowPaths(*tab->model),
             (std::vector<std::string>{"/music/shared/theirs.flac", "/music/shared/mine.flac"}));
    // Saved here: the choice.
    tab->document.dirty = false;
    window.persistNow(false);
    QTRY_VERIFY(window.findChild<QMessageBox*>(QStringLiteral("bench-list-conflict")) != nullptr);
    auto* question = window.findChild<QMessageBox*>(QStringLiteral("bench-list-conflict"));
    question->findChild<QAbstractButton*>(QStringLiteral("bench-list-conflict-copy"))->click();
    // Mine is a list of its own; the tab shows theirs.
    QTRY_COMPARE(rowPaths(*tab->model), std::vector<std::string>{"/music/shared/again.flac"});
    const auto copy = std::ranges::find_if(window.list_tabs_, [](const auto& candidate) {
        return displayText(candidate->document.name) == QStringLiteral("Shared, renamed (mine)");
    });
    QVERIFY(copy != window.list_tabs_.end());
    QCOMPARE(rowPaths(*(*copy)->model),
             (std::vector<std::string>{"/music/shared/theirs.flac", "/music/shared/mine.flac"}));
    const auto copy_id = (*copy)->document.id.to_string();
    window.persistNow(false);
    QTRY_VERIFY((*other)->call("list.get", protocol::Json{{"id", copy_id}}).has_value());

    // Deleted elsewhere: the tab closes, with nothing asked.
    QVERIFY((*other)->call("list.delete", protocol::Json{{"id", id}}));
    QTRY_VERIFY(window.tabForDocument(qid) == nullptr);
    (*other)->close();
}

// A list changed on its engine while this window was closed: the engine's
// version is what the tab shows when the window opens again, rather than the
// window's old copy being written back over it.
void BenchMainWindowTest::EngineLists::aListChangedWhileClosedIsTakenUpOnOpening() {
    std::string id;
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
        auto* tab =
            window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                        .kind = persistence::ListKind::saved,
                                                        .name = "Kept",
                                                        .pinned = false,
                                                        .dirty = false,
                                                        .items = {}},
                              true);
        id = tab->document.id.to_string();
        LocalTrackRow row;
        row.raw_path = "/music/kept/old.flac";
        tab->model->appendRows({row});
        window.persistNow(true);
        auto engine = protocol::Client::connect(protocol::Endpoint{
            .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
        QVERIFY(engine.has_value());
        QTRY_VERIFY((*engine)->call("list.get", protocol::Json{{"id", id}}).has_value());
        QTRY_VERIFY(!window.list_sync_->busy());
        (*engine)->close();
    }
    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    QVERIFY((*other)->call("list.save",
                           protocol::Json{{"id", id},
                                          {"name", "Kept"},
                                          {"kind", "saved"},
                                          {"items", listItems({"/music/kept/new.flac"})}}));

    BenchMainWindow reopened;
    reopened.show();
    QTRY_VERIFY(reopened.lists_restored_);
    auto* tab = reopened.tabForDocument(QString::fromStdString(id));
    QVERIFY(tab != nullptr);
    QTRY_COMPARE(rowPaths(*tab->model), std::vector<std::string>{"/music/kept/new.flac"});
    QTest::qWait(300);
    QTRY_VERIFY(!reopened.list_sync_->busy());
    auto stored = (*other)->call("list.get", protocol::Json{{"id", id}});
    QVERIFY(stored.has_value());
    QCOMPARE(stored->at("items").size(), std::size_t{1});
    (*other)->close();
}

// ADR-0233: a list this window does not have open -- saved from another
// window, the CLI, the phone -- is found under its engine and opened as a
// tab, without being written back just for having been opened.
void BenchMainWindowTest::EngineLists::aListFromElsewhereOpensAsATab() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    auto made = (*other)->call("list.save",
                               protocol::Json{{"name", "From elsewhere"},
                                              {"kind", "saved"},
                                              {"items", listItems({"/music/elsewhere/1.flac",
                                                                   "/music/elsewhere/2.flac"})}});
    QVERIFY(made.has_value());
    const auto id = made->value("id", std::string{});

    window.findChild<QAction*>(QStringLiteral("action-open-list"))->trigger();
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("bench-open-list"));
    QVERIFY(dialog != nullptr);
    auto* tree = dialog->findChild<QTreeWidget*>(QStringLiteral("bench-open-list-tree"));
    QTreeWidgetItem* found = nullptr;
    QTRY_VERIFY([&] {
        const auto items = tree->findItems(QStringLiteral("From elsewhere"),
                                           Qt::MatchExactly | Qt::MatchRecursive);
        found = items.isEmpty() ? nullptr : items.front();
        return found != nullptr;
    }());
    QCOMPARE(found->text(1), QStringLiteral("2"));
    QCOMPARE(found->parent()->text(0), QStringLiteral("This computer"));
    tree->setCurrentItem(found);
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Open)->click();

    auto* tab = static_cast<BenchMainWindow::ListTab*>(nullptr);
    QTRY_VERIFY((tab = window.tabForDocument(QString::fromStdString(id))) != nullptr);
    QCOMPARE(rowPaths(*tab->model),
             (std::vector<std::string>{"/music/elsewhere/1.flac", "/music/elsewhere/2.flac"}));
    QCOMPARE(tab->document.kind, persistence::ListKind::saved);
    QVERIFY(window.tabs_->currentWidget() == tab->view);
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QCOMPARE((*other)->call("list.get", protocol::Json{{"id", id}})->value("revision", 0), 1);
    (*other)->close();
}

// ADR-0259: a list opened from its engine shows what the engine's library
// knows of each file -- every tag and the revision -- not only the few names
// the list was saved with, described after it opens.
void BenchMainWindowTest::EngineLists::aListFromTheEngineCarriesItsLibraryTags() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music));
    const auto file = music + QStringLiteral("/rich.flac");
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), file));
    const auto raw = QFile::encodeName(file).toStdString();
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto catalogue = window.localCatalogue()->open();
    QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
    persistence::LibraryScanProgress progress;
    QVERIFY(catalogue->scan({}, progress).has_value());

    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    auto items = listItems({raw, "/music/not-indexed/outside.flac"});
    items[0]["title"] = "Saved title";
    items[1]["title"] = "Outside";
    auto made = (*other)->call(
        "list.save",
        protocol::Json{{"name", "Described"}, {"kind", "saved"}, {"items", std::move(items)}});
    QVERIFY(made.has_value());
    window.openEngineList(EngineKey::local(),
                          QString::fromStdString(made->value("id", std::string{})));
    auto* tab = static_cast<BenchMainWindow::ListTab*>(nullptr);
    QTRY_VERIFY((tab = window.tabForDocument(
                     QString::fromStdString(made->value("id", std::string{})))) != nullptr);
    QTRY_COMPARE(tab->model->rowCount(), 2);
    const auto& rows = tab->model->rows();
    // The names it was saved with first, then the library's.
    QTRY_COMPARE(rows[0].title, std::string{"Metadata Fixture"});
    QCOMPARE(rows[0].album, std::string{"Rich Metadata"});
    QStringList names;
    for (const auto& field : rows[0].metadata.fields) {
        names.append(QString::fromStdString(field.canonical_name));
    }
    // Named as the library folds them, as a search's rows are.
    QVERIFY2(names.contains(QStringLiteral("musicbrainztrackid")), qPrintable(names.join(u',')));
    QVERIFY(rows[0].source_revision.has_value());
    QCOMPARE(rows[0].source_revision->size, static_cast<std::uint64_t>(QFileInfo{file}.size()));
    QCOMPARE(rows[1].title, std::string{"Outside"});
    QVERIFY(!rows[1].source_revision.has_value());

    // Described a part at a time: the indexed file comes after a part's worth
    // of others.
    std::vector<std::string> many;
    for (int index = 0; index < 2'000; ++index) {
        many.push_back("/music/not-indexed/" + std::to_string(index) + ".flac");
    }
    many.push_back(raw);
    auto long_list = (*other)->call(
        "list.save",
        protocol::Json{{"name", "Long"}, {"kind", "saved"}, {"items", listItems(many)}});
    QVERIFY(long_list.has_value());
    const auto long_id = QString::fromStdString(long_list->value("id", std::string{}));
    window.openEngineList(EngineKey::local(), long_id);
    auto* long_tab = static_cast<BenchMainWindow::ListTab*>(nullptr);
    QTRY_VERIFY((long_tab = window.tabForDocument(long_id)) != nullptr);
    QTRY_COMPARE(long_tab->model->rowCount(), 2'001);
    QTRY_COMPARE(long_tab->model->rows().back().title, std::string{"Metadata Fixture"});
    (*other)->close();
}

// ADR-0233: a file moved from this window is followed in every list of its
// engine -- one open in no window included -- and an engine that is away at
// the time is told when it is back.
void BenchMainWindowTest::EngineLists::aMoveIsFollowedInListsNotOpenHere() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    const auto connect_other = [this] {
        return protocol::Client::connect(protocol::Endpoint{
            .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    };
    auto other = connect_other();
    QVERIFY(other.has_value());
    auto made = (*other)->call(
        "list.save",
        protocol::Json{{"name", "Not open here"},
                       {"kind", "saved"},
                       {"items", listItems({"/music/moving/a.flac", "/music/moving/b.flac"})}});
    QVERIFY(made.has_value());
    const auto id = made->value("id", std::string{});
    const auto paths = [&other, &id] {
        std::vector<std::string> found;
        auto got = (*other)->call("list.get", protocol::Json{{"id", id}});
        for (const auto& item :
             got ? got->value("items", protocol::Json::array()) : protocol::Json::array()) {
            found.push_back(*protocol::decode_raw_path(item.value("path", std::string{})));
        }
        return found;
    };
    const auto moved = [](const std::string& from, const std::string& to) {
        operations::FilePublicationCommitResult result;
        result.journal_id = core::StableId::random();
        result.source_raw_path = from;
        result.target_raw_path = to;
        return result;
    };

    window.applyCommittedRelocation(moved("/music/moving/a.flac", "/music/moved/a.flac"));
    QTRY_COMPARE(paths(),
                 (std::vector<std::string>{"/music/moved/a.flac", "/music/moving/b.flac"}));
    QTRY_VERIFY(!QSettings{}.contains(QStringLiteral("lists/pending-relocations")));

    // Away during the move: kept, and handed over when it is back.
    (*other)->close();
    engine_.stop();
    QTRY_VERIFY(!window.localPlayback()->active());
    window.applyCommittedRelocation(moved("/music/moving/b.flac", "/music/moved/b.flac"));
    QVERIFY(QSettings{}.contains(QStringLiteral("lists/pending-relocations")));
    QVERIFY2(engine_.start(), engine_.log().constData());
    QTRY_VERIFY_WITH_TIMEOUT(window.localPlayback()->active(), 10'000);
    other = connect_other();
    QVERIFY(other.has_value());
    QTRY_COMPARE_WITH_TIMEOUT(
        paths(), (std::vector<std::string>{"/music/moved/a.flac", "/music/moved/b.flac"}), 10'000);
    QTRY_VERIFY(!QSettings{}.contains(QStringLiteral("lists/pending-relocations")));
    (*other)->close();
}

// The library's "Add to list" puts a selection into a list of the user's
// choosing, not only the one on screen; and a folder dragged from the folder
// browser onto a list is read into it, as from a file manager.
void BenchMainWindowTest::EngineLists::libraryAndFoldersAddToAChosenList() {
    QTemporaryDir music_dir;
    QVERIFY(music_dir.isValid());
    const auto music = music_dir.path();
    QVERIFY(QDir{}.mkpath(music + QStringLiteral("/album")));
    write_wave(music + QStringLiteral("/album/one.wav"), wave_sample_rate);
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localLibrary() != nullptr);
    auto catalogue = window.localCatalogue()->open();
    QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
    persistence::LibraryScanProgress progress;
    QVERIFY(catalogue->scan({}, progress).has_value());

    auto* chosen = window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                               .kind = persistence::ListKind::saved,
                                                               .name = "Chosen",
                                                               .pinned = false,
                                                               .dirty = false,
                                                               .items = {}},
                                     false);
    auto* current = window.currentListTab();
    QVERIFY(current != nullptr && current != chosen);
    const auto chosen_id = QString::fromStdString(chosen->document.id.to_string());
    const auto targets = window.listTargets(EngineKey::local());
    QVERIFY(std::ranges::any_of(targets, [&](const auto& target) {
        return target.first == chosen_id && target.second == QStringLiteral("Chosen");
    }));
    QVERIFY(window
                .listTargets(window.remoteEngine() != nullptr
                                 ? window.remoteEngine()->key
                                 : EngineKey::fromText(QStringLiteral("elsewhere")))
                .empty());

    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = catalogue->query(albums);
    QVERIFY(page && !page->entries.empty());
    const auto before = current->model->rowCount();
    emit window.localLibrary()->browser().addToListRequested(page->entries, chosen_id);
    QTRY_COMPARE(chosen->model->rowCount(), 1);
    QCOMPARE(current->model->rowCount(), before);
    QTRY_VERIFY(!window.discovery_running_);

    // Or into a new list, called what it was named.
    const auto tabs_before = window.list_tabs_.size();
    emit window.localLibrary()->browser().newListRequested(page->entries, QStringLiteral("Picked"));
    QTRY_COMPARE(window.list_tabs_.size(), tabs_before + 1U);
    auto* picked = window.list_tabs_.back().get();
    QCOMPARE(picked->document.name, std::string{"Picked"});
    QTRY_COMPARE(picked->model->rowCount(), 1);
    QCOMPARE(chosen->model->rowCount(), 1);
    QTRY_VERIFY(!window.discovery_running_);

    // The folder, dragged from a file manager onto the list on screen.
    auto mime = std::make_unique<QMimeData>();
    mime->setUrls({QUrl::fromLocalFile(music + QStringLiteral("/album"))});
    auto* viewport = current->view->viewport();
    QDragEnterEvent enter{dragPosition(QPoint{10, 10}), Qt::CopyAction, mime.get(), Qt::LeftButton,
                          Qt::NoModifier};
    QApplication::sendEvent(viewport, &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop{QPointF{10, 10}, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(viewport, &drop);
    QVERIFY(drop.isAccepted());
    QTRY_COMPARE(current->model->rowCount(), before + 1);
    QCOMPARE(current->model->rows().back().raw_path,
             QFile::encodeName(music + QStringLiteral("/album/one.wav")).toStdString());
}

// ADR-0296: the library's Playlists tab -- its engine's saved lists, open
// here or not, not a tab never saved -- and a drop of files from a file
// manager on one not open yet adds to it.
void BenchMainWindowTest::EngineLists::theListsPanelShowsEveryListAndTakesDrops() {
    QTemporaryDir music_dir;
    QVERIFY(music_dir.isValid());
    const auto music = music_dir.path();
    QVERIFY(QDir{}.mkpath(music + QStringLiteral("/album")));
    write_wave(music + QStringLiteral("/album/one.wav"), wave_sample_rate);
    BenchMainWindow window;
    window.resize(1200, 700);
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());

    auto engine = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(engine.has_value());
    const auto make = [&engine](const std::string& name) {
        const auto made = (*engine)->call(
            "list.save",
            protocol::Json{{"name", name}, {"kind", "saved"}, {"items", protocol::Json::array()}});
        return made ? QString::fromStdString(made->value("id", std::string{})) : QString{};
    };
    const auto elsewhere = make("Elsewhere");
    const auto target = make("Target");
    QVERIFY(!elsewhere.isEmpty() && !target.isEmpty());

    window.local_source_tabs_->setCurrentIndex(window.playlists_tab_);
    auto* panel = window.playlists_view_;
    QTRY_VERIFY(panel->itemFor(elsewhere) != nullptr && panel->itemFor(target) != nullptr);
    // Flat, as the library's rows: no heading over them, and how many said.
    QVERIFY(panel->itemFor(elsewhere)->parent() == nullptr);
    QCOMPARE(window.findChild<QLabel*>(QStringLiteral("bench-playlists-status"))->text(),
             QStringLiteral("2 playlists on this computer"));
    // Filtered by name.
    auto* filter = window.findChild<QLineEdit*>(QStringLiteral("bench-playlists-filter"));
    QVERIFY(filter != nullptr);
    filter->setText(QStringLiteral("targ"));
    QVERIFY(panel->itemFor(elsewhere)->isHidden() && !panel->itemFor(target)->isHidden());
    filter->clear();
    QVERIFY(!panel->itemFor(elsewhere)->isHidden());
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(window.grab().save(directory + QStringLiteral("/playlists.png")));
    }
    window.local_source_tabs_->setCurrentIndex(0);
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QTest::qWait(300);
        QVERIFY(window.grab().save(directory + QStringLiteral("/library.png")));
    }
    window.local_source_tabs_->setCurrentIndex(window.playlists_tab_);
    // The window's own tab, never saved, is no playlist.
    auto* own = window.currentListTab();
    QVERIFY(own != nullptr);
    QVERIFY(panel->itemFor(QString::fromStdString(own->document.id.to_string())) == nullptr);

    // Chosen: opened, and its tracks are what is shown.
    panel->setCurrentItem(panel->itemFor(elsewhere));
    QTRY_VERIFY(window.tabForDocument(elsewhere) != nullptr);
    QCOMPARE(window.currentListTab(), window.tabForDocument(elsewhere));

    // Files dropped on a playlist not open here: opened, and added to.
    QVERIFY(window.tabForDocument(target) == nullptr);
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(music + QStringLiteral("/album/one.wav"))});
    QTRY_VERIFY(panel->itemFor(target) != nullptr);
    const auto at = panel->visualItemRect(panel->itemFor(target)).center();
    auto* viewport = panel->viewport();
    QDragEnterEvent enter{dragPosition(at), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(viewport, &enter);
    QVERIFY(enter.isAccepted());
    QDragMoveEvent move{dragPosition(at), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(viewport, &move);
    QVERIFY(move.isAccepted());
    QDropEvent drop{QPointF{at}, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(viewport, &drop);
    QVERIFY(drop.isAccepted());
    QTRY_VERIFY(window.tabForDocument(target) != nullptr);
    QTRY_COMPARE(window.tabForDocument(target)->model->rowCount(), 1);
    QTRY_VERIFY(!window.discovery_running_);
    // Lists are tabs only: no pane to show them in instead.
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-lists-panel")) == nullptr);
}

// ADR-0286: a tab that is no saved playlist is the window's alone: never on
// its engine -- not after a reconnect, not on closing -- and nothing is left
// to remove there.
void BenchMainWindowTest::EngineLists::aScratchTabNeverReachesItsEngine() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto engine = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(engine.has_value());
    const auto listed = [&engine](const std::string& id) {
        auto all = (*engine)->call("list.all");
        return all && std::ranges::any_of(all->value("lists", protocol::Json::array()),
                                          [&id](const protocol::Json& list) {
                                              return list.value("id", std::string{}) == id;
                                          });
    };
    auto* scratch =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Scratch",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    const auto id = scratch->document.id.to_string();
    LocalTrackRow row;
    row.raw_path = "/music/scratch.flac";
    scratch->model->appendRows({row});
    window.markTabDirty(*scratch);
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(!listed(id));
    window.list_sync_->reconnected(window.localPlayback());
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(!listed(id));
    // Closed with nothing to ask: its contents let go of, as Discard would.
    scratch->document.dirty = false;
    window.closeTabAt(window.tabs_->indexOf(scratch->view));
    window.persistNow(false);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(!listed(id));
    QVERIFY(QSettings{}.value(QStringLiteral("lists/pending-removals")).toStringList().isEmpty());
    (*engine)->close();
}

// Deleted by another client while this window was writing it: the delete
// was taken for this window's own write and ignored, the tab stayed, and its
// next write made the list again -- on the phone, a list that would not go.
void BenchMainWindowTest::EngineLists::aListDeletedElsewhereWhileBeingWrittenStaysDeleted() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto engine = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(engine.has_value());
    const auto listed = [&engine](const std::string& id) {
        auto all = (*engine)->call("list.all");
        return all && std::ranges::any_of(all->value("lists", protocol::Json::array()),
                                          [&id](const protocol::Json& list) {
                                              return list.value("id", std::string{}) == id;
                                          });
    };
    auto* scratch =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::saved,
                                                    .name = "Scratch",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    const auto id = scratch->document.id.to_string();
    LocalTrackRow row;
    row.raw_path = "/music/one.flac";
    scratch->model->appendRows({row});
    window.persistNow(false);
    QTRY_VERIFY(listed(id));
    QTRY_VERIFY(!window.list_sync_->busy());

    // Changed here and being written; deleted there, and told so before the
    // write has answered.
    row.raw_path = "/music/two.flac";
    scratch->model->appendRows({row});
    window.persistNow(false);
    QVERIFY(window.list_sync_->busy());
    QVERIFY((*engine)->call("list.delete", protocol::Json{{"id", id}}).has_value());
    window.list_sync_->listChanged(window.localPlayback(), QString::fromStdString(id), 0, true);

    QTRY_VERIFY(window.tabForDocument(QString::fromStdString(id)) == nullptr);
    QTRY_VERIFY(!window.list_sync_->busy());
    window.persistNow(false);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(!listed(id));
    (*engine)->close();
}

// ADR-0286: what changes in the playing tab reaches the engine's queue as the
// edits it is -- so an entry the engine let go of meanwhile (consumed, say)
// is not brought back by sending the tab whole.
void BenchMainWindowTest::EngineLists::aPlayingTabsEditsReachTheQueueAsEdits() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    const auto queued = [&other] {
        std::vector<std::string> entries;
        auto queue = (*other)->call("playback.queue");
        for (const auto& entry :
             queue ? queue->value("entries", protocol::Json::array()) : protocol::Json::array()) {
            entries.push_back(entry.value("entry", std::string{}));
        }
        return entries;
    };
    auto* tab = window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                            .kind = persistence::ListKind::scratch,
                                                            .name = "Playing",
                                                            .pinned = false,
                                                            .dirty = false,
                                                            .items = {}},
                                  true);
    std::vector<LocalTrackRow> made;
    for (int index = 0; index < 5; ++index) {
        LocalTrackRow row;
        row.raw_path = "/music/playing/" + std::to_string(index) + ".flac";
        row.title = "Track " + std::to_string(index);
        row.duration_ms = 60'000;
        made.push_back(std::move(row));
    }
    tab->model->appendRows(std::move(made));
    std::vector<std::string> ids;
    for (const auto& row : tab->model->rows()) {
        ids.push_back(row.entry_id.to_string());
    }
    window.playRow(*tab, 1);
    QTRY_VERIFY_WITH_TIMEOUT(queued() == ids, 10'000);
    // The window knows the queue it handed over.
    QTRY_VERIFY(window.engine_queue_.has_value() && window.engine_queue_->size() == 5U);

    // Let go of on the engine -- as consume does -- before the window hears.
    QVERIFY((*other)->call(
        "playback.edit_queue",
        protocol::Json{{"list", tab->document.id.to_string()},
                       {"edits", protocol::Json::array({protocol::Json{{"remove", {ids[4]}}}})}}));
    // And now a row taken out here.
    tab->model->removeRowIndexes({0});
    window.markTabDirty(*tab);
    QTRY_VERIFY(queued() == (std::vector<std::string>{ids[1], ids[2], ids[3]}));
    QTest::qWait(500);
    QCOMPARE(queued(), (std::vector<std::string>{ids[1], ids[2], ids[3]}));
    (*other)->close();
}

// The engine the window already follows has its list replaced by another
// client, and plays the new one's first track: that row is marked at once
// -- not the row where the old list's track was, and not only once the
// next track starts.
void BenchMainWindowTest::EngineLists::aListReplacedOnTheFollowedEngineMarksWhatPlays() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    std::vector<std::string> paths;
    for (const auto* name : {"old-a.wav", "old-b.wav", "old-c.wav", "new-a.wav", "new-b.wav"}) {
        const auto path = media.filePath(QString::fromLatin1(name));
        write_wave(path, wave_sample_rate * 60U);
        paths.push_back(QFile::encodeName(path).toStdString());
    }

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());

    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = remote.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    const auto queue = [&](const std::vector<std::size_t>& which) {
        std::vector<std::string> ids;
        auto entries = protocol::Json::array();
        for (const auto index : which) {
            ids.push_back(core::StableId::random().to_string());
            entries.push_back(protocol::Json{{"entry", ids.back()},
                                             {"path", protocol::encode_raw_path(paths[index])},
                                             {"title", "Track " + std::to_string(index)}});
        }
        static_cast<void>((*other)->call("playback.replace_queue",
                                         protocol::Json{{"entries", std::move(entries)}}));
        return ids;
    };
    const auto id = [](const std::string& text) { return *core::StableId::parse(text); };
    // What the server plays is followed in its playing list, one list
    // however often another client replaces the queue.
    const auto marked = [&](const std::string& entry) {
        auto* tab = window.playingTab(window.remoteEngine()->key);
        const auto row = tab->model->rowOfEntry(id(entry), -1);
        return row >= 0 && tab->model->index(row, 0).data(ui::track_current_role).toBool();
    };

    // The server plays the old list's second track; the window follows it.
    const auto old_list = queue({0, 1, 2});
    QVERIFY((*other)->call("playback.play", protocol::Json{{"entry", old_list[1]}}).has_value());
    QTRY_VERIFY_WITH_TIMEOUT(window.transport_ == window.remotePlayback(), 10'000);
    QTRY_VERIFY(marked(old_list[1]));

    // Replaced, and its first track played -- in one report.
    const auto album = queue({3, 4});
    QVERIFY((*other)->call("playback.play", protocol::Json{{"entry", album[0]}}).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    QTRY_VERIFY(
        window.playingTab(window.remoteEngine()->key)->model->rowOfEntry(id(album[0]), -1) >= 0);
    QTRY_VERIFY2(marked(album[0]), "the new list's playing row is not marked");
    QVERIFY2(!marked(album[1]), "the row where the old track was is marked instead");
    // Past two of the engine's state reports.
    QTest::qWait(1'100);
    QVERIFY2(marked(album[0]), "the playing row lost its mark");
    static_cast<void>((*other)->call("playback.stop"));
    (*other)->close();
}

void BenchMainWindowTest::EngineLists::aRatingSetElsewhereShowsInTheTabs() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));

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
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = window.remoteCatalogue()->open()->query(albums);
    QVERIFY(page && page->entries.size() == 1U);
    emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                           LocalLibraryAction::append);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    QTRY_VERIFY(!tab->model->rows().front().rating_hash.empty());
    const auto hash = tab->model->rows().front().rating_hash;
    QCOMPARE(tab->model->rows().front().rating, 0U);

    // Another client -- the phone, a script -- rates it on the engine.
    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = remote.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    QVERIFY((*other)
                ->call("catalogue.set_rating", protocol::Json{{"hash", hash}, {"rating", 8}})
                .has_value());
    (*other)->close();

    // Shown without anyone asking again.
    QTRY_COMPARE(tab->model->rows().front().rating, 8U);
}

// A save writes a list's items only when they changed since the last save:
// two kept searches of 66,841 tracks had every save -- a rating, a tab
// switch -- rewrite 3.2 million rows, and the window froze on every start.
void BenchMainWindowTest::EngineLists::aSaveWritesOnlyListsThatChanged() {
    const auto rows = [](const int count, const std::string& folder) {
        std::vector<LocalTrackRow> made;
        for (int index = 0; index < count; ++index) {
            LocalTrackRow row;
            row.raw_path = "/music/" + folder + "/" + std::to_string(index) + ".flac";
            row.title = "Track " + std::to_string(index);
            made.push_back(std::move(row));
        }
        return made;
    };
    const auto document = [](const std::string& name) {
        return persistence::ListDocument{.id = core::StableId::random(),
                                         .kind = persistence::ListKind::scratch,
                                         .name = name,
                                         .pinned = false,
                                         .dirty = false,
                                         .items = {}};
    };
    const auto carries_items = [](BenchMainWindow& window, const core::StableId& id) {
        for (const auto& write : window.workspace_.collectWrites()) {
            if (write.document.id == id) {
                return std::optional{write.items};
            }
        }
        return std::optional<bool>{};
    };
    core::StableId big_id;
    core::StableId small_id;
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        auto* big = window.addListTab(document("Big"), false);
        auto* small = window.addListTab(document("Small"), false);
        big_id = big->document.id;
        small_id = small->document.id;
        // Larger than one part of a restore, which is made in parallel.
        big->model->appendRows(rows(5000, "big"));
        small->model->appendRows(rows(2, "small"));
        QCOMPARE(carries_items(window, big_id), std::optional{true});
        window.persistNow(true);
        // Saved and unchanged: headers only.
        QCOMPARE(carries_items(window, big_id), std::optional{false});
        QCOMPARE(carries_items(window, small_id), std::optional{false});
        // One list edited: its items, and only its.
        small->model->appendRows(rows(1, "more"));
        QCOMPARE(carries_items(window, small_id), std::optional{true});
        QCOMPARE(carries_items(window, big_id), std::optional{false});
        // Renamed: written whole, as an engine's copy names it.
        big->document.name = "Big, renamed";
        QCOMPARE(carries_items(window, big_id), std::optional{true});
        window.persistNow(true);
        QCOMPARE(carries_items(window, big_id), std::optional{false});
    }
    // Restored as stored, and stored right: nothing to write again.
    BenchMainWindow reopened;
    reopened.show();
    QTRY_VERIFY(reopened.lists_restored_);
    auto* big = reopened.workspace_.tabForDocument(big_id);
    auto* small = reopened.workspace_.tabForDocument(small_id);
    QVERIFY(big != nullptr && small != nullptr);
    QCOMPARE(big->model->rows().size(), std::size_t{5000});
    // In order, across the parts it was restored in.
    for (const std::size_t index : {std::size_t{0}, std::size_t{2047}, std::size_t{2048},
                                    std::size_t{4096}, std::size_t{4999}}) {
        QCOMPARE(QString::fromStdString(big->model->rows()[index].raw_path),
                 QStringLiteral("/music/big/%1.flac").arg(index));
    }
    QCOMPARE(displayText(big->document.name), QStringLiteral("Big, renamed"));
    QCOMPARE(small->model->rows().size(), std::size_t{3});
    QCOMPARE(carries_items(reopened, big_id), std::optional{false});
    QCOMPARE(carries_items(reopened, small_id), std::optional{false});
    reopened.closeTabAt(reopened.tabs_->indexOf(big->view));
    reopened.closeTabAt(reopened.tabs_->indexOf(small->view));
    reopened.persistNow(true);
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::EngineLists)

#include "window_engine_lists_test.moc"
