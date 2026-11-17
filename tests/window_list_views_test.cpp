// SPDX-License-Identifier: GPL-3.0-only

// The window test: list views: columns, groups, layouts and listening history.

#include "window_test.hpp"

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

class BenchMainWindowTest::ListViews final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void localListeningColumnsLoadRefreshAndRespectAuthority();
    void theTrackLayoutMenuListsEveryListView();
    void newListsStartInTheViewChosenForThem();
    void trackViewLayoutMatchesGroupedQueueAndPersists();
    void listViewsShowExpressionsGroupAndSort();
    void listViewEditorChecksAndSavesACopy();
    void builtinColumnsShowTheirExpressions();
    void listViewsReadListeningHistory();
    void groupsFoldToTheirHeaders();
    void rowsHaveMoreLines();
    void listLayoutsFromBeforeViewsMigrate();
};

// ADR-0282: the play count and last played columns are the list's engine's
// history of each file -- asked of the engine that holds the files, as a
// view reading history asks; a file it cannot see is unknown, not unplayed.
void BenchMainWindowTest::ListViews::localListeningColumnsLoadRefreshAndRespectAuthority() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* tab = window.currentListTab();
    QVERIFY(tab);
    QTemporaryDir music;
    const auto heard = music.filePath(QStringLiteral("heard.flac"));
    {
        QFile file{heard};
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not really audio");
    }
    LocalTrackRow row;
    // As an engine's list gives its rows: a path, no revision.
    row.raw_path = heard.toStdString();
    row.title = "History fixture";
    row.probed = true;
    auto gone = row;
    gone.raw_path = music.filePath(QStringLiteral("gone.flac")).toStdString();
    tab->model->replaceRows({row, gone});
    auto* model = tab->model;
    const auto plays = [&](int n) {
        return model->index(n, local_play_count_column).data().toString();
    };
    QVERIFY(tab->view->isColumnHidden(local_play_count_column));
    const auto hidden_layout = window.captureTrackViewLayout(*tab);
    QCOMPARE(hidden_layout.columns.back().id, QStringLiteral("last-played"));
    QCOMPARE(hidden_layout.columns.back().width, 170);
    window.setTrackColumnVisible(QStringLiteral("play-count"), true);
    window.setTrackColumnVisible(QStringLiteral("last-played"), true);
    QVERIFY(!tab->view->isColumnHidden(local_play_count_column));
    QCOMPARE(tab->view->columnWidth(local_last_played_column), 170);
    QTRY_COMPARE(plays(0), QStringLiteral("0"));
    QCOMPARE(model->index(0, local_last_played_column).data().toString(), QStringLiteral("Never"));
    QCOMPARE(plays(1), QStringLiteral("—"));
    QVERIFY(model->index(0, local_play_count_column)
                .data(Qt::TextAlignmentRole)
                .value<Qt::Alignment>()
                .testFlag(Qt::AlignRight));

    // A listen recorded: the file is asked for again, and what the engine
    // says now is drawn, the selection kept.
    tab->view->selectRow(1);
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    model->forgetHistory({row.raw_path});
    QTRY_VERIFY(model->rows()[0].history_loaded);
    const qint64 played_at = 1700000000000;
    model->rows_[0].history_requested = true;
    QSignalSpy drawn(model, &QAbstractItemModel::dataChanged);
    model->applyHistory({model->rows()[0].entry_id}, {{3, played_at, 0, 3, played_at, 0}});
    QCOMPARE(plays(0), QStringLiteral("3"));
    QCOMPARE(model->index(0, local_last_played_column).data().toString(),
             QLocale{}.toString(QDateTime::fromMSecsSinceEpoch(played_at), QLocale::ShortFormat));
    QVERIFY(std::ranges::any_of(drawn, [](const QList<QVariant>& signal) {
        return signal.at(0).toModelIndex().column() == local_play_count_column;
    }));
    QCOMPARE(resets.count(), 0);
    QCOMPARE(tab->view->selectionModel()->selectedRows().front().row(), 1);
    const auto restored_layout = ui::deserializeTrackViewLayout(
        ui::serializeTrackViewLayout(window.captureTrackViewLayout(*tab)), trackColumnIds());
    QVERIFY(restored_layout);
    window.applyTrackViewLayout(*tab, *restored_layout);
    QVERIFY(!tab->view->isColumnHidden(local_last_played_column));
    window.copyTrackViewLayoutToAllTabs();
    QVERIFY(window.track_column_actions_.value(QStringLiteral("play-count"))->isVisible());
}

// ADR-0282: Workspace › Track list layout lists every list view -- the
// shipped ones and the user's own -- the one shown checked, and choosing one
// shows it.
void BenchMainWindowTest::ListViews::theTrackLayoutMenuListsEveryListView() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* menu = window.findChild<QMenu*>(QStringLiteral("menu-track-list-layout"));
    QVERIFY(menu != nullptr);
    const auto listed = [&window] {
        QStringList ids;
        for (auto* action : window.track_presentation_group_->actions()) {
            ids << action->data().toString();
        }
        return ids;
    };
    QStringList shipped;
    for (const auto& view : listViews()) {
        shipped << QString::fromStdString(view.id);
    }
    QCOMPARE(listed(), shipped);
    // One of the user's own: listed as soon as it is made.
    auto base = findListView(default_list_view_id);
    QVERIFY(base.has_value());
    auto made = listViewCopy(*base, base->columns, QStringLiteral("Mine"));
    QVERIFY2(made.has_value(), made ? "" : made.error().message.c_str());
    emit ListViewCatalog::instance().changed();
    QTRY_VERIFY(listed().contains(*made));
    auto* mine = window.findChild<QAction*>(QStringLiteral("action-list-view-menu-%1").arg(*made));
    QVERIFY(mine != nullptr && menu->actions().contains(mine));
    QCOMPARE(mine->text(), QStringLiteral("Mine"));
    mine->trigger();
    QTRY_COMPARE(window.currentListTab()->view_layout.view, *made);
    QVERIFY(mine->isChecked());
    QVERIFY(saveCustomListViews({}).has_value());
    emit ListViewCatalog::instance().changed();
    QTRY_VERIFY(!listed().contains(*made));
}

// A new list starts in the view chosen with "Use current view for new
// lists"; unchecking it goes back to the default, and so does a chosen view
// that is gone.
void BenchMainWindowTest::ListViews::newListsStartInTheViewChosenForThem() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* use = window.findChild<QAction*>(QStringLiteral("action-new-lists-view"));
    QVERIFY(use != nullptr && use->isCheckable());
    const auto plain = ui::trackViewPresentationId(ui::TrackViewPresentation::plain_columns);
    QVERIFY(plain != default_list_view_id);
    const auto add = [&window] {
        return window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                           .kind = persistence::ListKind::scratch,
                                                           .name = "New",
                                                           .pinned = false,
                                                           .dirty = false,
                                                           .items = {},
                                                           .engine = {}},
                                 true);
    };
    QCOMPARE(add()->view_layout.view, default_list_view_id);
    QVERIFY(use->isChecked());
    window.chooseListView(plain);
    QTRY_COMPARE(window.currentListTab()->view_layout.view, plain);
    QVERIFY(!use->isChecked());
    use->trigger();
    QCOMPARE(newListsViewId(), plain);
    QCOMPARE(add()->view_layout.view, plain);
    QVERIFY(use->isChecked());
    // Reset goes to it too.
    window.chooseListView(default_list_view_id);
    window.resetTrackViewLayout();
    QCOMPARE(window.currentListTab()->view_layout.view, plain);
    use->trigger();
    QVERIFY(!use->isChecked());
    QCOMPARE(newListsViewId(), default_list_view_id);
    QCOMPARE(add()->view_layout.view, default_list_view_id);
    // A view of the user's that is removed: new lists are the default again.
    auto base = findListView(default_list_view_id);
    auto made = listViewCopy(*base, base->columns, QStringLiteral("Mine"));
    QVERIFY(made.has_value());
    setNewListsViewId(*made);
    QCOMPARE(add()->view_layout.view, *made);
    QVERIFY(saveCustomListViews({}).has_value());
    QCOMPARE(newListsViewId(), default_list_view_id);
    QCOMPARE(add()->view_layout.view, default_list_view_id);
}

void BenchMainWindowTest::ListViews::trackViewLayoutMatchesGroupedQueueAndPersists() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto first_path = media.filePath(QStringLiteral("first.wav"));
    const auto second_path = media.filePath(QStringLiteral("second.wav"));
    write_wave(first_path, wave_sample_rate / 10U);
    write_wave(second_path, wave_sample_rate / 10U);
    const auto first_encoded = QFile::encodeName(first_path);
    const auto second_encoded = QFile::encodeName(second_path);
    const std::string first_raw{first_encoded.constData(),
                                static_cast<std::size_t>(first_encoded.size())};
    const std::string second_raw{second_encoded.constData(),
                                 static_cast<std::size_t>(second_encoded.size())};
    QString binding;
    int persisted_title_width = 0;
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        auto* side =
            window.findChild<QAction*>(QStringLiteral("action-list-view-menu-albums-side-artwork"));
        auto* plain =
            window.findChild<QAction*>(QStringLiteral("action-list-view-menu-plain-columns"));
        auto* date = window.findChild<QAction*>(QStringLiteral("action-track-column-date"));
        QVERIFY(tabs != nullptr);
        QVERIFY(side != nullptr);
        QVERIFY(plain != nullptr);
        QVERIFY(date != nullptr);
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QVERIFY(view != nullptr);
        QCOMPARE(view->model()->columnCount(), local_column_count);
        QVERIFY(side->isChecked());
        QCOMPARE(view->albumArtworkColumn(), local_artwork_column);
        QVERIFY(view->albumGroupingEnabled());
        QVERIFY(qobject_cast<ui::QueueItemDelegate*>(view->itemDelegate()) != nullptr);
        const auto viewport_children =
            view->viewport()->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly);
        for (const auto* child : viewport_children) {
            QVERIFY2(!child->isVisible() || !child->geometry().contains(view->viewport()->rect()),
                     "A child widget must not cover Trackbench's delegate-painted track rows");
        }
        QVERIFY(!view->wordWrap());
        QCOMPARE(view->textElideMode(), Qt::ElideRight);
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        window.openLocalPaths({first_raw, second_raw});
        QTRY_COMPARE(view->model()->rowCount(), 2);
        QCOMPARE(view->verticalHeader()->sectionResizeMode(0), QHeaderView::Fixed);
        QTRY_VERIFY(view->rowHeight(0) > view->rowHeight(1));
        QCOMPARE(view->model()->headerData(local_artist_column, Qt::Horizontal).toString(),
                 QStringLiteral("Artist"));
        QCOMPARE(view->model()->headerData(local_track_number_column, Qt::Horizontal).toString(),
                 QStringLiteral("#"));

        LocalTrackRow singleton{
            .raw_path = "/standalone.wav",
            .logical_reference = std::nullopt,
            .selection = {},
            .segment = std::nullopt,
            .title = "Standalone",
            .artist = "Solo artist",
            .album = "One-off",
            .album_artist = {},
            .date = "2026",
            .track_number = {},
            .duration_ms = 1'000,
            .metadata = {},
            .source_revision = std::nullopt,
        };
        auto* local_model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(local_model != nullptr);
        local_model->appendRows({std::move(singleton)});
        // A lone track is an album of its own (ADR-0250): its header above it.
        QTRY_COMPARE(view->rowHeight(2),
                     view->rowHeight(1) + ui::QueueItemDelegate::album_header_height);
        QImage singleton_cover{12, 12, QImage::Format_RGB32};
        singleton_cover.fill(Qt::red);
        local_model->setArtwork(local_model->groupKey(2), singleton_cover);
        QCoreApplication::processEvents();
        // Its cover beside its header, in the cover gutter, as any album's.
        const auto artwork_rect = view->visualRect(local_model->index(2, local_artwork_column));
        const auto shows_cover = [](const QImage& render) {
            for (int y = 0; y < render.height(); ++y) {
                for (int x = 0; x < render.width(); ++x) {
                    if (render.pixelColor(x, y) == QColor(Qt::red)) {
                        return true;
                    }
                }
            }
            return false;
        };
        QVERIFY(shows_cover(view->viewport()->grab(artwork_rect).toImage()));

        view->selectionModel()->select(local_model->index(2, 0),
                                       QItemSelectionModel::ClearAndSelect |
                                           QItemSelectionModel::Rows);
        QCoreApplication::processEvents();
        // Selected or playing, the cover stays on top of the row's tint.
        QVERIFY(shows_cover(view->viewport()->grab(artwork_rect).toImage()));
        view->clearSelection();
        local_model->setCurrentSource(local_model->source(2), 2);
        QCoreApplication::processEvents();
        QVERIFY(shows_cover(view->viewport()->grab(artwork_rect).toImage()));

        plain->trigger();
        QVERIFY(plain->isChecked());
        QCOMPARE(view->albumArtworkColumn(), -1);
        QVERIFY(qobject_cast<ui::QueueItemDelegate*>(view->itemDelegate()) == nullptr);
        QVERIFY(view->isColumnHidden(local_artwork_column));
        QCOMPARE(view->rowHeight(0), view->rowHeight(1));
        QCOMPARE(view->rowHeight(0), view->verticalHeader()->defaultSectionSize());
        QVERIFY(!view->albumGroupingEnabled());
        QCOMPARE(view->verticalHeader()->sectionResizeMode(0), QHeaderView::Fixed);

        side->trigger();
        QVERIFY(view->albumGroupingEnabled());
        QTRY_VERIFY(view->rowHeight(0) > view->rowHeight(1));
        auto* header = view->horizontalHeader();
        header->moveSection(header->visualIndex(local_title_column), 1);
        view->setColumnWidth(local_title_column, 333);
        // The album view leaves the date to its header; asked for, it shows.
        QVERIFY(view->isColumnHidden(local_date_column));
        date->trigger();
        QVERIFY(!view->isColumnHidden(local_date_column));
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        persisted_title_width = view->columnWidth(local_title_column);
        window.resize(1'350, 720);
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        QVERIFY(view->columnWidth(local_title_column) > persisted_title_width);
        window.resize(1'100, 720);
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        persisted_title_width = view->columnWidth(local_title_column);
        binding = QStringLiteral("local:%1").arg(view->property("bench-document-id").toString());
        QVERIFY(window.close());
    }

    {
        BenchMainWindow restored;
        restored.show();
        auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QCOMPARE(view->albumArtworkColumn(), local_artwork_column);
        QCOMPARE(view->horizontalHeader()->logicalIndex(1), local_title_column);
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        QCOMPARE(view->columnWidth(local_title_column), persisted_title_width);
        QVERIFY(!view->isColumnHidden(local_date_column));
        QVERIFY(restored.close());
    }

    const QByteArray future_layout{
        R"({"schema":99,"presentation":"future","columns":[],"keep":true})"};
    // ADR-0259: a layout this release cannot read, as a later one would keep it.
    {
        auto kept = TabStore::loadState();
        QCOMPARE(kept.tabs.size(), 1U);
        kept.tabs.front().layout = future_layout;
        TabStore::saveState(kept);
    }
    {
        BenchMainWindow fallback;
        fallback.show();
        auto* tabs = fallback.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QCOMPARE(view->albumArtworkColumn(), local_artwork_column);
        QVERIFY(fallback.statusBar()->currentMessage().contains(QStringLiteral("preserved")));
        QVERIFY(fallback.close());
    }
    const auto kept = TabStore::loadState();
    // One per list tab, kept as it was.
    QCOMPARE(kept.tabs.size(), 1U);
    QCOMPARE(
        QStringLiteral("local:%1").arg(QString::fromStdString(kept.tabs.front().id.to_string())),
        binding);
    QCOMPARE(kept.tabs.front().layout, future_layout);
}

// ADR-0282: a list view of the user's own -- an expression column, rows
// grouped by an expression with a header and subgroups of their own -- and a
// header click sorting the list by a column, again the other way. Changing
// a shipped view makes a copy, which the list is then drawn by after a
// restart too.
void BenchMainWindowTest::ListViews::listViewsShowExpressionsGroupAndSort() {
    auto by_label = *lists::shipped_list_view(lists::columns_view_id);
    by_label.id = "view-test";
    by_label.name = "By label";
    by_label.builtin = false;
    by_label.columns.push_back({.id = "expression-1",
                                .visible = true,
                                .title = "Label",
                                .display = "%label%",
                                .sort = {},
                                .alignment = lists::ColumnAlignment::right,
                                .expands = false,
                                .style = {},
                                .second_line = {},
                                .width = 0});
    by_label.grouping = lists::ViewGrouping::expression;
    by_label.group_key = "%label%";
    by_label.group_title = "%label%";
    by_label.group_details = "%group_tracks% tracks";
    by_label.subgroup = lists::ViewSubgroup{.key = "%discnumber%", .label = "Disc %discnumber%"};
    // ADR-0283: Interscope's rows quiet, the label itself bold.
    by_label.row_style = "$if($eq(%label%,Interscope),quiet,)";
    by_label.columns.back().style = "bold";
    QVERIFY(saveCustomListViews({by_label}).has_value());

    const auto row = [](const char* title, const char* label, const char* disc) {
        LocalTrackRow made;
        made.raw_path = std::string{"/music/"} + title + ".flac";
        made.title = title;
        made.artist = "Artist";
        made.duration_ms = 60'000;
        for (const auto& [name, value] : {std::pair{"TITLE", title}, std::pair{"LABEL", label},
                                          std::pair{"DISCNUMBER", disc}}) {
            made.metadata.fields.push_back(
                {.canonical_name = metadata::canonicalize_field_name(name),
                 .native_name = name,
                 .values = {value},
                 .qualifier = {},
                 .provenance = metadata::FieldProvenance::embedded});
        }
        made.probed = true;
        return made;
    };
    QString customised;
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        auto* model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(model != nullptr);
        model->appendRows({row("Beta", "Warp", "1"), row("alpha", "Warp", "2"),
                           row("Gamma", "Warp", "2"), row("Delta", "Interscope", "1")});
        window.chooseListView(QStringLiteral("view-test"));

        QCOMPARE(model->columnCount(), local_column_count + 1);
        const auto label = model->columnOf(QStringLiteral("expression-1"));
        QCOMPARE(label, local_column_count);
        QCOMPARE(model->headerData(label, Qt::Horizontal, Qt::DisplayRole).toString(),
                 QStringLiteral("Label"));
        QTRY_COMPARE(model->index(0, label).data().toString(), QStringLiteral("Warp"));
        QCOMPARE(model->index(3, label).data().toString(), QStringLiteral("Interscope"));
        QCOMPARE(model->index(0, label).data(Qt::TextAlignmentRole).value<Qt::Alignment>() &
                     Qt::AlignRight,
                 Qt::AlignRight);
        QVERIFY(!view->isColumnHidden(label));

        // Grouped by label once every row is known; the groups are not
        // albums, so nothing is left to their headers.
        QTRY_VERIFY(model->expressionGroupsReady());
        QTRY_VERIFY(view->albumGroupingEnabled());
        QVERIFY(!view->property(ui::track_album_rules_property).toBool());
        const auto starts = [model] {
            QList<bool> found;
            for (int at = 0; at < model->rowCount(); ++at) {
                found << model->index(at, 0).data(ui::track_album_group_start_role).toBool();
            }
            return found;
        };
        QCOMPARE(starts(), (QList<bool>{true, false, false, true}));
        QTRY_COMPARE(model->index(0, 0).data(ui::track_group_title_role).toString(),
                     QStringLiteral("Warp"));
        QCOMPARE(model->index(0, 0).data(ui::track_group_details_role).toString(),
                 QStringLiteral("3 tracks"));
        QCOMPARE(model->index(0, 0).data(ui::track_disc_start_role).toString(),
                 QStringLiteral("Disc 1"));
        QCOMPARE(model->index(1, 0).data(ui::track_disc_start_role).toString(),
                 QStringLiteral("Disc 2"));
        QCOMPARE(model->index(2, 0).data(ui::track_disc_start_role).toString(), QString{});
        // One disc only: nothing to tell apart.
        QCOMPARE(model->index(3, 0).data(ui::track_disc_start_role).toString(), QString{});
        // Drawn as its styles say: Interscope's row quiet, in every column;
        // the label bold, in every row.
        QCOMPARE(
            model->index(3, local_title_column).data(Qt::ForegroundRole).value<QBrush>().color(),
            QGuiApplication::palette().color(QPalette::PlaceholderText));
        QVERIFY(!model->index(0, local_title_column).data(Qt::ForegroundRole).isValid());
        QVERIFY(model->index(0, label).data(Qt::FontRole).value<QFont>().weight() >=
                QFont::DemiBold);
        QVERIFY(!model->index(0, local_title_column).data(Qt::FontRole).isValid());
        // Drawn as the view says: its header above the group, not the album's.
        QTRY_VERIFY(view->rowHeight(0) > view->rowHeight(2));

        const auto titles = [model] {
            QStringList found;
            for (int at = 0; at < model->rowCount(); ++at) {
                found << model->index(at, local_title_column).data().toString();
            }
            return found;
        };
        // A click on Title sorts the list by it -- natural, case ignored --
        // as an edit that can be undone; again, the other way.
        emit view->horizontalHeader()->sectionClicked(local_title_column);
        QTRY_COMPARE(titles(), (QStringList{QStringLiteral("alpha"), QStringLiteral("Beta"),
                                            QStringLiteral("Delta"), QStringLiteral("Gamma")}));
        QVERIFY(model->canUndo());
        QCOMPARE(model->undoLabel(), QStringLiteral("Sort by Title"));
        // Groups follow the list's order.
        QTRY_COMPARE(starts(), (QList<bool>{true, false, true, true}));
        emit view->horizontalHeader()->sectionClicked(local_title_column);
        QTRY_COMPARE(titles(), (QStringList{QStringLiteral("Gamma"), QStringLiteral("Delta"),
                                            QStringLiteral("Beta"), QStringLiteral("alpha")}));
        // The Label column sorts by what it shows.
        emit view->horizontalHeader()->sectionClicked(label);
        QTRY_COMPARE(model->index(0, label).data().toString(), QStringLiteral("Interscope"));

        // A shipped view is not changed: hiding one of its columns makes a
        // copy, which the list is drawn by.
        window.chooseListView(QStringLiteral("plain-columns"));
        QCOMPARE(model->columnCount(), local_column_count);
        QVERIFY(!view->isColumnHidden(local_date_column));
        window.setTrackColumnVisible(QStringLiteral("date"), false);
        auto* tab = window.currentListTab();
        customised = tab->view_layout.view;
        QVERIFY(customised.startsWith(QStringLiteral("view-")));
        const auto copy = findListView(customised);
        QVERIFY(copy.has_value());
        QCOMPARE(copy->name, std::string{"Plain columns (customised)"});
        QVERIFY(view->isColumnHidden(local_date_column));
        QVERIFY(lists::shipped_list_view(lists::columns_view_id)->columns ==
                findListView(QStringLiteral("plain-columns"))->columns);
        QVERIFY(window.close());
    }
    {
        BenchMainWindow restored;
        restored.show();
        auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QCOMPARE(restored.currentListTab()->view_layout.view, customised);
        QVERIFY(view->isColumnHidden(local_date_column));
        QVERIFY(restored.close());
    }
}

// ADR-0282: a view that reads listening history has the list's history
// fetched from its engine before its rows are shown; what arrives is read by
// its expressions, and a listen the engine records is fetched again.
void BenchMainWindowTest::ListViews::listViewsReadListeningHistory() {
    auto plays = *lists::shipped_list_view(lists::columns_view_id);
    plays.id = "view-plays";
    plays.name = "Plays";
    plays.builtin = false;
    plays.columns.push_back({.id = "expression-1",
                             .visible = true,
                             .title = "Plays",
                             .display = "$if(%playcount%,%playcount% plays,none)",
                             .sort = {},
                             .alignment = lists::ColumnAlignment::left,
                             .expands = false,
                             .style = {},
                             .second_line = {},
                             .width = 0});
    QVERIFY(saveCustomListViews({plays}).has_value());
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* model =
        qobject_cast<LocalListModel*>(static_cast<QTableView*>(tabs->currentWidget())->model());
    LocalTrackRow played;
    played.raw_path = "/music/played.flac";
    played.title = "Played";
    played.probed = true;
    model->appendRows({played});
    QSignalSpy wanted{model, &LocalListModel::historyWanted};
    window.chooseListView(QStringLiteral("view-plays"));
    const auto column = model->columnOf(QStringLiteral("expression-1"));
    // Asked of the engine, which knows no listens of it: none.
    QTRY_VERIFY(wanted.count() > 0);
    QTRY_COMPARE(model->index(0, column).data().toString(), QStringLiteral("none"));
    QVERIFY(model->rows().front().history_loaded);

    // History that says otherwise is read by the expression.
    model->forgetHistory({played.raw_path});
    const auto requests = model->takeHistoryRequests();
    QCOMPARE(requests.size(), 1U);
    QCOMPARE(requests.front().source.source.source_reference, played.raw_path);
    model->applyHistory({requests.front().entry},
                        {std::array<std::int64_t, 6>{7, 1'700'000'000'000, 2, 9, -1, -1}});
    QTRY_COMPARE(model->index(0, column).data().toString(), QStringLiteral("7 plays"));

    // A listen recorded: asked again, and the engine's answer shown.
    window.workspace_.adoptListen(EngineKey::local(), played.raw_path);
    QTRY_COMPARE(model->index(0, column).data().toString(), QStringLiteral("none"));
    QVERIFY(window.close());
}

// ADR-0283: a click on the mark at a header's right end folds its group to
// the header -- its rows stay in the list, not drawn -- and again unfolds
// it; the folds are kept with the tab across a restart.
void BenchMainWindowTest::ListViews::groupsFoldToTheirHeaders() {
    const auto row = [](const char* title, const char* album) {
        LocalTrackRow made;
        made.raw_path = std::string{"/music/"} + album + "/" + title + ".flac";
        made.title = title;
        made.artist = "Artist";
        made.album = album;
        made.duration_ms = 60'000;
        made.probed = true;
        return made;
    };
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        auto* model = qobject_cast<LocalListModel*>(view->model());
        model->appendRows({row("One", "First"), row("Two", "First"), row("Three", "Second"),
                           row("Four", "Second")});
        QTRY_VERIFY(view->rowHeight(0) > view->rowHeight(1));
        const auto open_length = view->verticalHeader()->length();
        // The mark at the header's right end.
        const QPoint mark{view->viewport()->width() - ui::QueueTableView::collapse_mark_width / 2,
                          view->rowViewportPosition(0) + 8};
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, mark);
        QVERIFY(view->groupCollapsed(0));
        QVERIFY(view->isRowHidden(1));
        QVERIFY(!view->isRowHidden(2) && !view->isRowHidden(3));
        // Shorter by its folded rows; beside the rows the header keeps its
        // cover's height.
        QVERIFY(view->verticalHeader()->length() < open_length);
        QVERIFY(view->rowHeight(0) >= ui::QueueItemDelegate::album_header_height);
        // Still in the list.
        QCOMPARE(model->rowCount(), 4);
        // A click on the rest of the header still selects the group, folded
        // rows and all.
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {},
                          QPoint{view->viewport()->width() / 2, view->rowViewportPosition(0) + 8});
        QCOMPARE(view->selectionModel()->selectedRows().size(), 2);
        QVERIFY(window.close());
    }
    {
        BenchMainWindow restored;
        restored.show();
        auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QTRY_COMPARE(view->model()->rowCount(), 4);
        QTRY_VERIFY(view->isRowHidden(1));
        QVERIFY(!view->isRowHidden(3));
        // Every group, then none.
        view->setAllGroupsCollapsed(true);
        QVERIFY(view->isRowHidden(1) && view->isRowHidden(3));
        view->setAllGroupsCollapsed(false);
        QVERIFY(!view->isRowHidden(1) && !view->isRowHidden(3));
        QVERIFY(restored.close());
    }
}

// ADR-0284: a view of two-line rows -- the title's artist and album beneath
// it -- draws every row taller by a line, grouped or not.
void BenchMainWindowTest::ListViews::rowsHaveMoreLines() {
    auto two_lines = *lists::shipped_list_view(lists::columns_view_id);
    two_lines.id = "view-two-lines";
    two_lines.name = "Two lines";
    two_lines.builtin = false;
    two_lines.row_lines = 2;
    std::ranges::find(two_lines.columns, std::string{"title"}, &lists::ViewColumn::id)
        ->second_line = "%artist% · %album%";
    auto grouped = two_lines;
    grouped.id = "view-two-lines-albums";
    grouped.name = "Two lines, albums";
    grouped.grouping = lists::ViewGrouping::album;
    grouped.artwork = lists::ViewArtwork::beside;
    QVERIFY(saveCustomListViews({two_lines, grouped}).has_value());
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
    auto* model = qobject_cast<LocalListModel*>(view->model());
    LocalTrackRow first;
    first.raw_path = "/music/one.flac";
    first.title = "One";
    first.artist = "Air";
    first.album = "Moon Safari";
    first.probed = true;
    first.rating_hash = "rating-one";
    auto second = first;
    second.raw_path = "/music/two.flac";
    second.title = "Two";
    second.rating_hash = "rating-two";
    model->appendRows({first, second});
    window.chooseListView(QStringLiteral("view-two-lines"));
    const auto tall = ui::track_row_height + ui::trackSecondLineHeight(view->font());
    QTRY_COMPARE(view->rowHeight(1), tall);
    QCOMPARE(view->property(ui::track_row_lines_property).toInt(), 2);
    QVERIFY(qobject_cast<ui::TrackLinesDelegate*>(view->itemDelegate()) != nullptr);
    QTRY_COMPARE(model->index(0, local_title_column).data(ui::track_second_line_role).toString(),
                 QStringLiteral("Air · Moon Safari"));
    QCOMPARE(model->index(0, local_length_column).data(ui::track_second_line_role).toString(),
             QString{});
    // A larger font: the second line is measured, its descenders kept
    // inside the row rather than cut off at a fixed height.
    {
        auto large = view->font();
        large.setPointSizeF(24.0);
        view->setFont(large);
        window.chooseListView(QStringLiteral("view-two-lines-albums"));
        window.chooseListView(QStringLiteral("view-two-lines"));
        const QFontMetrics beneath{ui::trackSecondLineFont(large)};
        QTRY_VERIFY(view->rowHeight(1) >= ui::track_row_height + beneath.height());
        QVERIFY(view->rowHeight(1) > ui::track_row_height + 16);
        view->setFont(QFont{});
        window.chooseListView(QStringLiteral("view-two-lines-albums"));
        window.chooseListView(QStringLiteral("view-two-lines"));
        QTRY_COMPARE(view->rowHeight(1), tall);
    }
    // A rating coming later evaluates its row again, and the second lines of
    // built-in columns are drawn anew though the view has no columns of its
    // own.
    QSignalSpy redrawn{model, &QAbstractItemModel::dataChanged};
    model->applyRatings({{QStringLiteral("rating-one"), 8U}});
    QTRY_VERIFY(std::ranges::any_of(redrawn, [](const QList<QVariant>& signal) {
        return signal.at(0).toModelIndex().row() == 0 &&
               signal.at(0).toModelIndex().column() <= local_title_column &&
               signal.at(2).value<QList<int>>().contains(ui::track_second_line_role);
    }));
    QCOMPARE(model->index(0, local_title_column).data(ui::track_second_line_role).toString(),
             QStringLiteral("Air · Moon Safari"));
    // Grouped, the header above the first row, every row a line taller.
    window.chooseListView(QStringLiteral("view-two-lines-albums"));
    QTRY_VERIFY(view->albumGroupingEnabled());
    QTRY_COMPARE(view->rowHeight(1), tall);
    QVERIFY(view->rowHeight(0) >= tall + ui::QueueItemDelegate::album_header_height);
    // Covers beside the rows: the cover column is the view's artwork, shown
    // though this copy of plain columns had it hidden.
    QCOMPARE(view->albumArtworkColumn(), local_artwork_column);
    QVERIFY(!view->isColumnHidden(local_artwork_column));
    // And back to one line.
    window.chooseListView(QStringLiteral("plain-columns"));
    QTRY_COMPARE(view->rowHeight(1), ui::track_row_height);
    QVERIFY(window.close());
}

// ADR-0282: the editor shows an expression's error and will not save the
// view until it is right; a shipped view is edited as a copy, which the list
// it was opened from is then drawn by.
void BenchMainWindowTest::ListViews::listViewEditorChecksAndSavesACopy() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* model =
        qobject_cast<LocalListModel*>(static_cast<QTableView*>(tabs->currentWidget())->model());
    LocalTrackRow first;
    first.raw_path = "/music/one.flac";
    first.title = "La Femme d'Argent";
    first.artist = "Air";
    first.album = "Moon Safari";
    first.probed = true;
    auto second = first;
    second.raw_path = "/music/two.flac";
    second.title = "Sexy Boy";
    model->appendRows({first, second});
    window.chooseListView(QStringLiteral("plain-columns"));
    window.editListView(false);
    ListViewDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<ListViewDialog*>()) != nullptr);
    auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("bench-list-view-name"));
    QCOMPARE(name->text(), QStringLiteral("Plain columns (copy)"));
    auto* columns = dialog->findChild<QListWidget*>(QStringLiteral("bench-list-view-columns"));
    auto* add = dialog->findChild<QPushButton*>(QStringLiteral("bench-list-view-add-column"));
    auto* buttons = dialog->findChild<QDialogButtonBox*>();
    auto* save = buttons->button(QDialogButtonBox::Save);
    QVERIFY(save->isEnabled());
    // The title is chosen first: a built-in column, only its second line the
    // view's.
    QCOMPARE(columns->currentItem()->text(), QStringLiteral("Title"));
    const auto type = [](QLineEdit* field, const QString& value) {
        field->setText(value);
        emit field->textEdited(value);
    };
    const auto field = [dialog](const char* object) {
        return dialog->findChild<QLineEdit*>(QString::fromLatin1(object));
    };
    const auto note = [dialog](const char* object) {
        return dialog->findChild<QLabel*>(QString::fromLatin1(object) + QStringLiteral("-note"))
            ->text();
    };
    add->click();
    QCOMPARE(columns->currentRow(), columns->count() - 1);
    auto* display = field("bench-list-view-column-display");
    QVERIFY(display->isVisible());
    // Beneath an expression, what it gives for the list's first track.
    type(display, QStringLiteral("%artist% · %title%"));
    QCOMPARE(note("bench-list-view-column-display"), QStringLiteral("→ Air · La Femme d'Argent"));
    type(display, QStringLiteral("$if(%genre%"));
    QVERIFY(!save->isEnabled());
    QVERIFY(!note("bench-list-view-column-display").startsWith(QStringLiteral("→")));
    auto* problems = dialog->findChild<QLabel*>(QStringLiteral("bench-list-view-problems"));
    QVERIFY(problems->isVisible());
    QVERIFY(problems->text().contains(QStringLiteral("shows")));
    type(field("bench-list-view-column-title"), QStringLiteral("Codec"));
    QCOMPARE(columns->currentItem()->text(), QStringLiteral("Codec"));
    type(display, QStringLiteral("$info(codec)"));
    QVERIFY(save->isEnabled());
    QVERIFY(problems->isHidden());
    // The preview draws the view as edited.
    auto* preview = dialog->findChild<QTableView*>(QStringLiteral("bench-list-view-preview"));
    QTRY_COMPARE(preview->model()->columnCount(), local_column_count + 1);
    QCOMPARE(preview->model()->headerData(local_column_count, Qt::Horizontal).toString(),
             QStringLiteral("Codec"));
    QCOMPARE(preview->model()->rowCount(), 2);
    save->click();
    QTRY_VERIFY(window.findChild<ListViewDialog*>() == nullptr);
    const auto bound = window.currentListTab()->view_layout.view;
    QVERIFY(bound.startsWith(QStringLiteral("view-")));
    const auto copy = findListView(bound);
    QVERIFY(copy.has_value());
    QCOMPARE(copy->name, std::string{"Plain columns (copy)"});
    QCOMPARE(model->columnCount(), local_column_count + 1);
    QCOMPARE(model->headerData(local_column_count, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Codec"));
    QVERIFY(window.close());
}

// ADR-0285: a built-in column shows an expression of the view's -- plain
// columns' number, year and length are theirs -- and the editor changes one
// as it changes any column.
void BenchMainWindowTest::ListViews::builtinColumnsShowTheirExpressions() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* model =
        qobject_cast<LocalListModel*>(static_cast<QTableView*>(tabs->currentWidget())->model());
    LocalTrackRow row;
    row.raw_path = "/music/one.flac";
    row.title = "La Femme d'Argent";
    row.artist = "Air";
    row.album = "Moon Safari";
    row.track_number = "3";
    row.date = "1998-01-16";
    row.duration_ms = 429'000;
    row.probed = true;
    model->appendRows({row});
    window.chooseListView(QStringLiteral("plain-columns"));
    QCOMPARE(
        model->headerData(local_track_number_column, Qt::Horizontal, Qt::DisplayRole).toString(),
        QStringLiteral("#"));
    QTRY_COMPARE(model->index(0, local_track_number_column).data().toString(),
                 QStringLiteral("03"));
    QCOMPARE(model->index(0, local_date_column).data().toString(), QStringLiteral("1998"));
    QVERIFY(model->index(0, local_date_column)
                .data(Qt::TextAlignmentRole)
                .value<Qt::Alignment>()
                .testFlag(Qt::AlignRight));
    QCOMPARE(model->index(0, local_artist_column).data().toString(), QStringLiteral("Air"));

    window.editListView(false);
    ListViewDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<ListViewDialog*>()) != nullptr);
    auto* columns = dialog->findChild<QListWidget*>(QStringLiteral("bench-list-view-columns"));
    const auto artist = columns->findItems(QStringLiteral("Artist"), Qt::MatchExactly);
    QCOMPARE(artist.size(), 1);
    columns->setCurrentItem(artist.front());
    auto* shows = dialog->findChild<QLineEdit*>(QStringLiteral("bench-list-view-column-display"));
    // What it always showed, to change.
    QCOMPARE(shows->text(), QStringLiteral("%artist%"));
    QVERIFY(dialog->findChild<QPushButton*>(QStringLiteral("bench-list-view-column-reset"))
                ->isVisible());
    shows->setText(QStringLiteral("$upper(%artist%)"));
    emit shows->textEdited(shows->text());
    QCOMPARE(
        dialog->findChild<QLabel*>(QStringLiteral("bench-list-view-column-display-note"))->text(),
        QStringLiteral("→ AIR"));
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
    QTRY_VERIFY(window.findChild<ListViewDialog*>() == nullptr);
    const auto copy = findListView(window.currentListTab()->view_layout.view);
    QVERIFY(copy.has_value());
    QCOMPARE(
        std::ranges::find(copy->columns, std::string{"artist"}, &lists::ViewColumn::id)->display,
        std::string{"$upper(%artist%)"});
    // Left as it was, the title is no expression of the view's.
    QVERIFY(std::ranges::find(copy->columns, std::string{"title"}, &lists::ViewColumn::id)
                ->display.empty());
    QTRY_COMPARE(model->index(0, local_artist_column).data().toString(), QStringLiteral("AIR"));
    QVERIFY(window.close());
}

// ADR-0282: a layout saved before list views is drawn by its presentation's
// shipped view -- or, where its columns were arranged otherwise, by a copy
// arranged so, made once.
void BenchMainWindowTest::ListViews::listLayoutsFromBeforeViewsMigrate() {
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        QVERIFY(window.close());
    }
    auto state = TabStore::loadState();
    QCOMPARE(state.tabs.size(), 1U);
    QByteArray layout{R"({"schema":2,"presentation":"plain-columns","columns":[)"};
    QStringList columns;
    for (const auto& spec : track_column_specs) {
        // As plain columns are, but without the date.
        const auto shown = spec.logical != local_artwork_column &&
                           spec.logical != local_date_column && spec.logical < local_rating_column;
        columns << QStringLiteral(R"({"id":"%1","width":%2,"visible":%3})")
                       .arg(QString::fromLatin1(spec.id))
                       .arg(spec.default_width)
                       .arg(shown ? QStringLiteral("true") : QStringLiteral("false"));
    }
    layout += columns.join(QLatin1Char(',')).toUtf8() + "]}";
    state.tabs.front().layout = layout;
    TabStore::saveState(state);
    {
        BenchMainWindow migrated;
        migrated.show();
        auto* tabs = migrated.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<QTableView*>(tabs->currentWidget());
        const auto bound = migrated.currentListTab()->view_layout.view;
        QVERIFY(bound.startsWith(QStringLiteral("view-")));
        QCOMPARE(findListView(bound)->name, std::string{"Plain columns (customised)"});
        QVERIFY(view->isColumnHidden(local_date_column));
        QVERIFY(!view->isColumnHidden(local_title_column));
        QVERIFY(migrated.close());
    }
    const auto custom = customListViews();
    QVERIFY(custom && custom->size() == 1U);

    // A view since retired -- the header artwork -- opens as the one in its
    // place.
    state = TabStore::loadState();
    state.tabs.front().layout = QByteArray{
        R"({"schema":3,"presentation":"albums-header-artwork","view":"albums-header-artwork",)"
        R"("columns":[]})"};
    TabStore::saveState(state);
    {
        BenchMainWindow retired;
        retired.show();
        auto* tabs = retired.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QCOMPARE(retired.currentListTab()->view_layout.view, QStringLiteral("albums-side-artwork"));
        QCOMPARE(view->albumArtworkColumn(), local_artwork_column);
        QVERIFY(retired.close());
    }
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::ListViews)

#include "window_list_views_test.moc"
