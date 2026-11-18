// SPDX-License-Identifier: GPL-3.0-only

// The window test: tabs: the tab bar, closing, drops and what is kept.

#include "window_test.hpp"

#include "trackknife/core/engine_state.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "uicommon/library_tree_view.hpp"
#include "workspace/artwork_session.hpp"
#include "workspace/sources.hpp"
#include "workspace/startup.hpp"
#include "workspace/tab_store.hpp"

#include <QCheckBox>
#include <QGroupBox>
#include <QMessageBox>
#include <QPainter>
#include <QStandardItemModel>
#include <sqlite3.h>

namespace trackknife::bench {

class BenchMainWindowTest::Tabs final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void activePlaybackTabRemainsMarkedWhileBrowsing();
    void doubleClickOnEmptyTabBarMakesAList();
    void theTabStoreKeepsWhatItCannotRead();
    void selectionActionsFollowTheActiveTab();
    void closingATabReturnsToThePreviousOne();
    void contextTransfersCreateTabs();
    void tabBarDropsTransferLocalRows();
    void filesDroppedOnTheTabBarMakeATab();
    void listTabCloseButtonsClose();
    void windowListTabsCloseFromTheirButtons();
    void trackListFindActionsFollowActiveTab();
    void persistsPinnedDuplicatedAndDirtyTabs();
    void theWindowKeepsItsTabsNotTheLists();
};

void BenchMainWindowTest::Tabs::doubleClickOnEmptyTabBarMakesAList() {
    BenchMainWindow window;
    window.resize(1200, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY(!window.list_tabs_.empty());
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    auto* bar = tabs->tabBar();
    const auto before = tabs->count();
    const auto prompt = [&window] {
        for (auto* dialog : window.findChildren<QInputDialog*>())
            if (dialog->isVisible())
                return dialog;
        return static_cast<QInputDialog*>(nullptr);
    };

    // On a tab it is no new list.
    QTest::mouseDClick(bar, Qt::LeftButton, {}, bar->tabRect(0).center());
    QTest::qWait(50);
    QVERIFY(prompt() == nullptr);
    QCOMPARE(tabs->count(), before);

    // Beside the tabs it is, named as File -> New list names one.
    QTimer answer;
    connect(&answer, &QTimer::timeout, &window, [&prompt] {
        if (auto* dialog = prompt()) {
            dialog->setTextValue(QStringLiteral("Fresh"));
            dialog->accept();
        }
    });
    answer.start(10);
    const QPoint beside(tabs->width() - 8, bar->geometry().center().y());
    QVERIFY(bar->tabAt(bar->mapFrom(tabs, beside)) < 0);
    QTest::mouseDClick(tabs, Qt::LeftButton, {}, beside);
    QTRY_COMPARE(tabs->count(), before + 1);
    QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("Fresh"));
}

void BenchMainWindowTest::Tabs::activePlaybackTabRemainsMarkedWhileBrowsing() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(!window.list_tabs_.empty());
    auto& local = *window.list_tabs_.front();
    window.setActiveLocalList(QString::fromStdString(local.document.id.to_string()));
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    const auto index = tabs->indexOf(local.view);
    // Marked by the bar's dot; the icon is left to say which engine a tab
    // plays on, and this one plays here.
    QVERIFY(tabs->tabBar()->tabData(index).toBool());
    QVERIFY(tabs->tabIcon(index).isNull());
    QVERIFY(tabs->tabToolTip(index).contains(QStringLiteral("Active playback queue")));
    window.refreshUpNext();
    QVERIFY(tabs->tabBar()->tabData(index).toBool());
    window.stop_action_->trigger();
    window.playback_.anchors.document = core::StableId{};
    window.refreshTransport();
    QVERIFY(tabs->tabBar()->tabData(index).toBool());
    auto other = local.document;
    other.id = core::StableId::random();
    other.name = "Another list";
    auto* next = window.addListTab(std::move(other), true);
    QVERIFY(next != nullptr);
    QVERIFY(tabs->tabBar()->tabData(index).toBool());
    window.setActiveLocalList(QString::fromStdString(next->document.id.to_string()));
    QVERIFY(!tabs->tabBar()->tabData(index).toBool());
    QVERIFY(tabs->tabBar()->tabData(tabs->indexOf(next->view)).toBool());
}

void BenchMainWindowTest::Tabs::closingATabReturnsToThePreviousOne() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_VERIFY(!window.list_tabs_.empty());
    auto* origin = window.list_tabs_.front()->view;
    const auto make = [&window](const char* name) {
        return window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                           .kind = persistence::ListKind::scratch,
                                                           .name = name,
                                                           .pinned = false,
                                                           .dirty = false,
                                                           .items = {}},
                                 true);
    };
    auto* first = make("First");
    auto* second = make("Second");
    QVERIFY(first != nullptr && second != nullptr);

    // Visit the origin, then the second list: closing it goes back to the
    // origin, even though "First" sits right beside it.
    tabs->setCurrentWidget(origin);
    tabs->setCurrentWidget(second->view);
    window.closeTabAt(tabs->indexOf(second->view));
    QTRY_COMPARE(tabs->currentWidget(), static_cast<QWidget*>(origin));
    QVERIFY(tabs->indexOf(first->view) >= 0);
}

// Delete and Play act on the tab you are looking at, not on whichever list
// last held a selection.
void BenchMainWindowTest::Tabs::selectionActionsFollowTheActiveTab() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_VERIFY(!window.list_tabs_.empty());
    auto& origin = *window.list_tabs_.front();
    LocalTrackRow row;
    row.raw_path = "/one.flac";
    row.title = "One";
    row.probed = true;
    origin.model->replaceRows({row});
    tabs->setCurrentWidget(origin.view);
    QCOMPARE(window.activeTrackView(), origin.view);

    auto* other =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Other",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    QVERIFY(other != nullptr);
    QCOMPARE(tabs->currentWidget(), other->view);
    QCOMPARE(window.activeTrackView(), other->view);

    // A selection in the list on screen is the one Delete would remove, and
    // one in a list that is not on screen is not.
    origin.view->selectRow(0);
    QVERIFY(other->view->selectionModel()->selectedRows().isEmpty());
    window.refreshSelectionActions();
    QVERIFY(!window.remove_selected_action_->isEnabled());
    tabs->setCurrentWidget(origin.view);
    window.refreshSelectionActions();
    QCOMPARE(window.activeTrackView(), origin.view);
    QVERIFY(window.remove_selected_action_->isEnabled());
}

void BenchMainWindowTest::Tabs::contextTransfersCreateTabs() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* source = qobject_cast<QTableView*>(tabs->widget(0));
    QVERIFY(source != nullptr);
    auto* model = qobject_cast<LocalListModel*>(source->model());
    QVERIFY(model != nullptr);
    LocalTrackRow row;
    row.raw_path = "/unmounted/é.flac";
    row.title = "Cached title";
    row.probed = true;
    model->appendRows({row, row});
    tabs->setCurrentWidget(source);
    source->selectAll();
    const auto open_menu = [&] {
        window.showTrackContextMenu(source, source->visualRect(model->index(0, 0)).center());
    };
    open_menu();
    auto* copy = window.findChild<QAction*>(QStringLiteral("action-copy-to-new-tab"));
    auto* move = window.findChild<QAction*>(QStringLiteral("action-move-to-new-tab"));
    QVERIFY(copy != nullptr);
    QVERIFY(move != nullptr);
    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
            dialog->reject();
    });
    copy->trigger();
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(model->rowCount(), 2);
    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            dialog->setTextValue(QStringLiteral("Copied selection"));
            dialog->accept();
        }
    });
    copy->trigger();
    QCOMPARE(tabs->count(), 2);
    auto* copied =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QCOMPARE(copied->rowCount(), 2);
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(copied->rows()[0].raw_path, row.raw_path);
    QCOMPARE(copied->rows()[1].title, row.title);
    QVERIFY(copied->rows()[0].probed);
    QVERIFY(!copied->rows()[0].source_revision.has_value());
    QVERIFY(tabs->tabText(tabs->currentIndex()).startsWith(QStringLiteral("Copied selection")));
    tabs->setCurrentWidget(source);
    open_menu();
    move = window.findChild<QAction*>(QStringLiteral("action-move-to-new-tab"));
    QVERIFY(move != nullptr);
    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            dialog->setTextValue(QStringLiteral("Moved selection"));
            dialog->accept();
        }
    });
    move->trigger();
    QCOMPARE(tabs->count(), 3);
    QCOMPARE(model->rowCount(), 0);
    auto* moved =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QCOMPARE(moved->rowCount(), 2);
    QCOMPARE(moved->rows()[1].raw_path, row.raw_path);
}

// Files from a file manager dropped on empty space in the tab bar make a new
// tab of their own, named after the folder; dropped on a tab, they join it.
void BenchMainWindowTest::Tabs::filesDroppedOnTheTabBarMakeATab() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto album = media.filePath(QStringLiteral("Some Album"));
    QVERIFY(QDir{}.mkpath(album));
    write_wave(album + QStringLiteral("/01.wav"), wave_sample_rate);
    write_wave(album + QStringLiteral("/02.wav"), wave_sample_rate);
    const auto single = media.filePath(QStringLiteral("single.wav"));
    write_wave(single, wave_sample_rate);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    const auto before = tabs->count();
    const auto drop_files = [&](const QString& path, const QPoint position) {
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        QDragEnterEvent enter{dragPosition(position), Qt::CopyAction, &mime, Qt::LeftButton,
                              Qt::NoModifier};
        window.handleTabTrackDrop(nullptr, &enter, position);
        if (!enter.isAccepted()) {
            return false;
        }
        QDropEvent drop{QPointF{position}, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
        window.handleTabTrackDrop(nullptr, &drop, position);
        return drop.isAccepted();
    };
    const QPoint empty{tabs->tabBar()->tabRect(tabs->count() - 1).right() + 12,
                       tabs->tabBar()->height() / 2};
    QVERIFY(drop_files(album, empty));
    QCOMPARE(tabs->count(), before + 1);
    auto* made = window.currentListTab();
    QVERIFY(made != nullptr && EngineKey::of(made->document).isLocal());
    QCOMPARE(QString::fromStdString(made->document.name), QStringLiteral("Some Album"));
    QTRY_COMPARE(made->model->rowCount(), 2);

    // Onto a tab: into that one, no new tab.
    auto* first = window.list_tabs_.front().get();
    const auto first_rows = first->model->rowCount();
    QVERIFY(drop_files(single, tabs->tabBar()->tabRect(tabs->indexOf(first->view)).center()));
    QCOMPARE(tabs->count(), before + 1);
    QTRY_COMPARE(first->model->rowCount(), first_rows + 1);
}

void BenchMainWindowTest::Tabs::tabBarDropsTransferLocalRows() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* source = qobject_cast<QTableView*>(tabs->widget(0));
    auto* model = qobject_cast<LocalListModel*>(source->model());
    LocalTrackRow row;
    row.raw_path = "/unmounted/track.flac";
    row.probed = true;
    model->appendRows({row, row});
    tabs->setCurrentWidget(source);
    source->selectAll();
    QMimeData mime;
    // Synthetic Qt drop events have no source; supply the real source view to
    // the handler used by the tab-bar event filter.
    const auto send_drop = [&](QTableView* from, const QPoint position,
                               const Qt::KeyboardModifiers modifiers, const bool commit) {
        QDragEnterEvent enter{dragPosition(position), Qt::CopyAction | Qt::MoveAction, &mime,
                              Qt::LeftButton, modifiers};
        window.handleTabTrackDrop(from, &enter, position);
        if (!enter.isAccepted() || !commit)
            return enter.isAccepted();
        QDropEvent drop{QPointF{position}, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                        modifiers};
        window.handleTabTrackDrop(from, &drop, position);
        return drop.isAccepted() &&
               drop.dropAction() ==
                   (modifiers.testFlag(Qt::ControlModifier) ? Qt::CopyAction : Qt::MoveAction);
    };
    const auto empty_position = [&] {
        return QPoint{tabs->tabBar()->tabRect(tabs->count() - 1).right() + 12,
                      tabs->tabBar()->height() / 2};
    };
    // Onto its own tab is not a transfer.
    QVERIFY(!send_drop(source, tabs->tabBar()->tabRect(0).center(), Qt::NoModifier, true));
    QVERIFY(!send_drop(source, QPoint{20, tabs->tabBar()->height() + 20}, Qt::NoModifier, true));
    QVERIFY(send_drop(source, empty_position(), Qt::ControlModifier, false));
    QCOMPARE(tabs->count(), 1);
    QVERIFY(send_drop(source, empty_position(), Qt::ControlModifier, true));
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(model->rowCount(), 2);
    auto* copied_view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* copied = qobject_cast<LocalListModel*>(copied_view->model());
    QCOMPARE(copied->rowCount(), 2);
    QCOMPARE(copied->rows()[0].raw_path, row.raw_path);
    QVERIFY(send_drop(source, empty_position(), Qt::NoModifier, true));
    QCOMPARE(tabs->count(), 3);
    QCOMPARE(model->rowCount(), 0);
    auto* moved_view = qobject_cast<QTableView*>(tabs->currentWidget());
    QCOMPARE(moved_view->model()->rowCount(), 2);
    moved_view->selectAll();
    QVERIFY(send_drop(moved_view, tabs->tabBar()->tabRect(tabs->indexOf(copied_view)).center(),
                      Qt::NoModifier, true));
    QCOMPARE(tabs->count(), 3);
    QCOMPARE(copied->rowCount(), 4);
    QCOMPARE(moved_view->model()->rowCount(), 0);
}

void BenchMainWindowTest::Tabs::trackListFindActionsFollowActiveTab() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* find = window.findChild<QAction*>(QStringLiteral("action-find-in-list"));
    auto* next = window.findChild<QAction*>(QStringLiteral("action-find-next-in-list"));
    auto* previous = window.findChild<QAction*>(QStringLiteral("action-find-previous-in-list"));
    auto* bar = window.findChild<TrackListFindBar*>();
    QVERIFY(tabs && find && next && previous && bar);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    LocalTrackRow row;
    row.raw_path = "/missing/track.flac";
    row.title = "Find me";
    row.probed = true;
    model->replaceRows({row, row});
    QTRY_VERIFY(find->isEnabled());
    QVERIFY(bar->isHidden());
    view->setFocus();
    QTest::keyClick(view, Qt::Key_F, Qt::ControlModifier);
    releaseHeldModifiers(view);
    QTRY_VERIFY(!bar->isHidden());
    auto* query = bar->findChild<QLineEdit*>(QStringLiteral("bench-list-find-query"));
    QVERIFY(query != nullptr);
    QTRY_VERIFY(query->hasFocus());
    query->setText(QStringLiteral("find me"));
    QTRY_COMPARE(view->currentIndex().row(), 0);
    QTest::keyClick(query, Qt::Key_F3);
    QTRY_COMPARE(view->currentIndex().row(), 1);
    QTest::keyClick(query, Qt::Key_F3, Qt::ShiftModifier);
    releaseHeldModifiers(query);
    QTRY_COMPARE(view->currentIndex().row(), 0);
    view->setFocus();
    QTest::keyClick(view, Qt::Key_Escape);
    QTRY_VERIFY(bar->isHidden());
    find->trigger();
    QVERIFY(!bar->isHidden());

    // Find follows the tab on screen: another list is searched on its own,
    // and the first list's cursor is left where it was.
    auto* other =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Other",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    QVERIFY(other != nullptr);
    QCOMPARE(tabs->currentWidget(), other->view);
    QVERIFY(bar->isHidden());
    QVERIFY(find->isEnabled() && next->isEnabled() && previous->isEnabled());
    LocalTrackRow elsewhere;
    elsewhere.raw_path = "/elsewhere.flac";
    elsewhere.title = "Elsewhere song";
    elsewhere.probed = true;
    other->model->replaceRows({elsewhere});
    other->view->setFocus();
    QTest::keyClick(other->view, Qt::Key_F, Qt::ControlModifier);
    releaseHeldModifiers(other->view);
    QTRY_VERIFY(query->hasFocus());
    query->setText(QStringLiteral("elsewhere song"));
    auto* status = bar->findChild<QLabel*>(QStringLiteral("bench-list-find-status"));
    QVERIFY(status != nullptr);
    QTRY_COMPARE(status->text(), QStringLiteral("Track 1 of 1"));
    QCOMPARE(other->view->currentIndex().row(), 0);
    QCOMPARE(view->currentIndex().row(), 0);
    tabs->setCurrentWidget(view);
    QVERIFY(find->isEnabled());
    QVERIFY(bar->isHidden());
    QCOMPARE(query->text(), QStringLiteral("elsewhere song"));

    view->selectionModel()->select(model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(properties_action != nullptr);
    QTRY_VERIFY(properties_action->isEnabled());
    properties_action->trigger();
    // ADR-0221: opening the tagger no longer replaces the active tab, so the
    // list stays current and its find actions stay available.
    QTRY_VERIFY(window.findChild<MetadataPropertiesDialog*>(
                    QStringLiteral("bench-metadata-properties")) != nullptr);
    QVERIFY(qobject_cast<MetadataPropertiesDialog*>(tabs->currentWidget()) == nullptr);
    QVERIFY(find->isEnabled());
    QVERIFY(bar->isHidden());
}

// A list tab's close button, clicked, asks for its tab to close -- under
// Trackknife's own style, and without the widget set closable, as the window
// has it: QTabWidget passes its bar's request on only when it is.
void BenchMainWindowTest::Tabs::listTabCloseButtonsClose() {
    followColorSchemes();
    PlaybackTabWidget tabs;
    tabs.setDocumentMode(true);
    tabs.addTab(new QWidget, QStringLiteral("First"));
    tabs.addTab(new QWidget, QStringLiteral("Second"));
    tabs.resize(700, 120);
    tabs.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tabs));
    QSignalSpy requested{&tabs, &QTabWidget::tabCloseRequested};
    auto* close = tabs.tabBar()->tabButton(1, QTabBar::RightSide);
    QVERIFY(close != nullptr && close->isVisible());
    QTest::mouseClick(close, Qt::LeftButton, {}, close->rect().center());
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.front().front().toInt(), 1);
}

// The same in the window itself: the click goes to whatever is under the
// pointer there, as a hand's would.
void BenchMainWindowTest::Tabs::windowListTabsCloseFromTheirButtons() {
    followColorSchemes();
    BenchMainWindow window;
    window.resize(1100, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    // The restored lists first: restoring is done off the window's thread.
    QTRY_VERIFY(window.lists_restored_);
    window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                .kind = persistence::ListKind::scratch,
                                                .name = "Second",
                                                .pinned = false,
                                                .dirty = false,
                                                .items = {}},
                      true);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_VERIFY(tabs->count() >= 2);
    const auto before = tabs->count();
    auto* close = tabs->tabBar()->tabButton(tabs->count() - 1, QTabBar::RightSide);
    QVERIFY(close != nullptr && close->isVisible());
    const auto at = window.mapFromGlobal(close->mapToGlobal(close->rect().center()));
    auto* under = window.childAt(at);
    QVERIFY2(
        under == close,
        qPrintable(QStringLiteral("under the button: %1 (%2)")
                       .arg(QString::fromLatin1(under != nullptr ? under->metaObject()->className()
                                                                 : "nothing"),
                            under != nullptr ? under->objectName() : QString{})));
    // Asked for, not answered: closing an unsaved list asks first, in a box
    // a test cannot answer.
    QObject::disconnect(tabs, &QTabWidget::tabCloseRequested, &window, nullptr);
    QSignalSpy requested{tabs, &QTabWidget::tabCloseRequested};
    QTest::mouseClick(under, Qt::LeftButton, {}, under->mapFrom(&window, at));
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.front().front().toInt(), before - 1);
}

// ADR-0259: the window keeps which lists it has open, and a cache of their
// rows -- not the lists, which are their engine's. A tab whose engine is
// away opens from its cache, and one whose cache is gone from its engine.
void BenchMainWindowTest::Tabs::theWindowKeepsItsTabsNotTheLists() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("kept.wav"));
    write_wave(path, wave_sample_rate / 10U);
    const auto raw_path = QFile::encodeName(path).toStdString();
    const auto kept_id = core::StableId::random();
    const auto scratch_id = core::StableId::random();
    QVERIFY(!TabStore::hasState());
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        persistence::ListItem item;
        item.source_reference = raw_path;
        item.duration_ms = 100;
        item.fields.push_back({.name = "title", .value = "Seeded"});
        auto* kept =
            window.addListTab(persistence::ListDocument{.id = kept_id,
                                                        .kind = persistence::ListKind::saved,
                                                        .name = "Kept",
                                                        .pinned = true,
                                                        .dirty = false,
                                                        .items = {item},
                                                        .engine = {}},
                              false);
        QVERIFY(kept != nullptr);
        QCOMPARE(kept->model->rows().front().title, std::string{"Seeded"});
        QVERIFY(window.addListTab(persistence::ListDocument{.id = scratch_id,
                                                            .kind = persistence::ListKind::scratch,
                                                            .name = "Scratch",
                                                            .pinned = false,
                                                            .dirty = false,
                                                            .items = {},
                                                            .engine = {}},
                                  false) != nullptr);
        window.persistNow(true);
        QVERIFY(TabStore::hasState());
        // A saved list is its engine's.
        auto other = protocol::Client::connect(protocol::Endpoint{
            .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
        QVERIFY(other.has_value());
        QTRY_VERIFY_WITH_TIMEOUT(
            (*other)->call("list.get", protocol::Json{{"id", kept_id.to_string()}}).has_value(),
            5'000);
        (*other)->close();
        QVERIFY(window.close());
    }
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        auto* kept = window.tabForDocument(QString::fromStdString(kept_id.to_string()));
        QVERIFY(kept != nullptr);
        QCOMPARE(kept->document.name, std::string{"Kept"});
        QVERIFY(kept->document.pinned);
        QCOMPARE(kept->model->rowCount(), 1);
        // Shown last, shown first.
        window.tabs_->setCurrentWidget(kept->view);
        QTRY_VERIFY(!window.list_sync_->busy());
        window.persistNow(true);
        QVERIFY(window.close());
    }
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QCOMPARE(window.currentListTab()->document.id, kept_id);
        QVERIFY(window.close());
    }
    // The engine away: the tab opens from its cache.
    engine_.stop();
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        auto* kept = window.tabForDocument(QString::fromStdString(kept_id.to_string()));
        QVERIFY(kept != nullptr);
        QCOMPARE(kept->model->rowCount(), 1);
        QCOMPARE(kept->model->rows().front().title, std::string{"Seeded"});
        QVERIFY(window.close());
    }
    QVERIFY2(engine_.start(), engine_.log().constData());

    // The cache gone: the tab is its engine's list again once it answers.
    QDir{QStandardPaths::writableLocation(QStandardPaths::CacheLocation)}.removeRecursively();
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        auto* kept = window.tabForDocument(QString::fromStdString(kept_id.to_string()));
        QVERIFY(kept != nullptr);
        QCOMPARE(kept->document.name, std::string{"Kept"});
        QTRY_COMPARE_WITH_TIMEOUT(kept->model->rowCount(), 1, 5'000);
        QCOMPARE(kept->model->rows().front().raw_path, raw_path);
        QVERIFY(window.close());
    }
}

void BenchMainWindowTest::Tabs::persistsPinnedDuplicatedAndDirtyTabs() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("tab-state.wav"));
    write_wave(path, wave_sample_rate / 10U);
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        auto* duplicate = window.findChild<QAction*>(QStringLiteral("action-duplicate-tab"));
        auto* pin = window.findChild<QAction*>(QStringLiteral("action-pin-tab"));
        auto* save = window.findChild<QAction*>(QStringLiteral("action-save-list"));
        auto* close = window.findChild<QAction*>(QStringLiteral("action-close-tab"));
        QVERIFY(tabs != nullptr);
        QVERIFY(duplicate != nullptr);
        QVERIFY(pin != nullptr);
        QVERIFY(save != nullptr);
        QVERIFY(close != nullptr);
        QCOMPARE(duplicate->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+D")));
        QCOMPARE(pin->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+P")));
        QCOMPARE(save->shortcut(), QKeySequence::Save);
        QCOMPARE(close->shortcut(), QKeySequence(QStringLiteral("Ctrl+W")));
        QTRY_COMPARE(tabs->count(), 1);

        window.openLocalPaths({raw_path});
        auto* source = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(source != nullptr);
        QTRY_COMPARE(source->model()->rowCount(), 1);
        const auto source_index = tabs->indexOf(source);
        QVERIFY(source_index >= 0);
        QTRY_VERIFY(tabs->tabText(source_index).endsWith(QStringLiteral(" *")));
        QVERIFY(tabs->tabToolTip(source_index).contains(QStringLiteral("modified")));

        pin->trigger();
        QVERIFY(pin->isChecked());
        QVERIFY(tabs->tabToolTip(source_index).contains(QStringLiteral("pinned")));
        auto* source_close = tabs->tabBar()->tabButton(source_index, QTabBar::RightSide);
        QVERIFY(source_close != nullptr);
        QVERIFY(!source_close->isVisible());
        QVERIFY(!close->isEnabled());

        duplicate->trigger();
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->currentIndex(), 1);
        auto* copied = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(copied != nullptr);
        QCOMPARE(copied->model()->rowCount(), 1);
        const auto copied_index = tabs->indexOf(copied);
        QCOMPARE(tabs->tabText(copied_index), QStringLiteral("Untitled copy *"));
        QVERIFY(tabs->tabBar()->tabButton(copied_index, QTabBar::RightSide)->isVisible());

        QTimer::singleShot(0, [] {
            if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                dialog->setTextValue(QStringLiteral("Saved copy"));
                dialog->accept();
            }
        });
        save->trigger();
        QCOMPARE(tabs->tabText(copied_index), QStringLiteral("Saved copy"));
        QVERIFY(!tabs->tabToolTip(copied_index).contains(QStringLiteral("modified")));

        // A later edit makes the explicitly saved list dirty again. Closing
        // must offer discard and honor both answers.
        window.openLocalPaths({raw_path});
        QTRY_COMPARE(copied->model()->rowCount(), 2);
        QTRY_VERIFY(tabs->tabText(copied_index).endsWith(QStringLiteral(" *")));
        QTimer::singleShot(0, [] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                box->done(QMessageBox::No);
            }
        });
        close->trigger();
        QCOMPARE(tabs->count(), 2);
        QTimer::singleShot(0, [] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                box->done(QMessageBox::Yes);
            }
        });
        close->trigger();
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->currentWidget(), source);
        QVERIFY(pin->isChecked());
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    const auto local_index = tabs->currentIndex();
    QCOMPARE(tabs->tabText(local_index), QStringLiteral("Untitled *"));
    QVERIFY(tabs->tabToolTip(local_index).contains(QStringLiteral("pinned")));
    QVERIFY(tabs->tabToolTip(local_index).contains(QStringLiteral("modified")));
    QVERIFY(!tabs->tabBar()->tabButton(local_index, QTabBar::RightSide)->isVisible());
    auto* view = qobject_cast<QTableView*>(tabs->widget(local_index));
    QVERIFY(view != nullptr);
    QCOMPARE(view->model()->rowCount(), 1);
}

// ADR-0286: a tab's rows are its only copy now: kept with the window's data,
// and a file that cannot be read set aside and said -- never taken for an
// empty tab.
void BenchMainWindowTest::Tabs::theTabStoreKeepsWhatItCannotRead() {
    QDir{TabStore::defaultDirectory()}.removeRecursively();
    const auto id = core::StableId::random();
    persistence::ListItem item;
    item.source_reference = "/music/kept.flac";
    TabStore store;
    QCOMPARE(store.directory(), TabStore::defaultDirectory());
    store.writeRows(persistence::ListDocument{.id = id,
                                              .kind = persistence::ListKind::scratch,
                                              .name = "Kept",
                                              .pinned = false,
                                              .dirty = false,
                                              .items = {item},
                                              .engine = {}});
    store.flush();
    const auto kept = store.readRows(id);
    QVERIFY(kept.has_value() && kept->items.size() == 1U &&
            kept->items.front().source_reference == "/music/kept.flac");
    QVERIFY(store.takeUnreadable().empty());

    // Damaged: set aside, named, and nothing read.
    const auto broken = core::StableId::random();
    const auto broken_path = TabStore::defaultDirectory() + QStringLiteral("/") +
                             QString::fromStdString(broken.to_string()) +
                             QStringLiteral(".tabcache");
    {
        QFile file{broken_path};
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a tab at all");
    }
    QVERIFY(!store.readRows(broken).has_value());
    QVERIFY(!QFile::exists(broken_path));
    const auto unreadable = store.takeUnreadable();
    QCOMPARE(unreadable.size(), std::size_t{1});
    QVERIFY(unreadable.front().startsWith(broken_path + QStringLiteral(".unreadable-")));
    QVERIFY(QFile::exists(unreadable.front()));
    // A tab with no file is just that: nothing set aside.
    QVERIFY(!store.readRows(core::StableId::random()).has_value());
    QVERIFY(store.takeUnreadable().empty());

    // Backed up and restored with the rest, the tabs restored over set aside.
    QTemporaryDir backup;
    QVERIFY(backup.isValid());
    QVERIFY(store.copyTo(backup.filePath(QStringLiteral("w.tabs"))));
    QVERIFY(QFile::exists(backup.filePath(QStringLiteral("w.tabs/") +
                                          QString::fromStdString(id.to_string()) +
                                          QStringLiteral(".tabcache"))));
    QDir{TabStore::defaultDirectory()}.removeRecursively();
    QVERIFY(TabStore::restoreFrom(backup.filePath(QStringLiteral("w.tabs"))));
    QVERIFY(QFile::exists(TabStore::defaultDirectory() + QStringLiteral("/") +
                          QString::fromStdString(id.to_string()) + QStringLiteral(".tabcache")));
    QDir{TabStore::defaultDirectory()}.removeRecursively();
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::Tabs)

#include "window_tabs_test.moc"
