// SPDX-License-Identifier: GPL-3.0-only

// The window test: the side panels, lyrics and the sources.

#include "window_test.hpp"

#include "trackknife/core/engine_state.hpp"

namespace trackknife::bench {

class BenchMainWindowTest::Panels final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void sourcePanelOpensOnALibrary();
    void narrowWindowKeepsListAndUpNextCompact();
    void theSourcesKeepTheirWidth();
    void sidePanelsShareTheirRoomAsTabs();
    void sidePanelsOpenBeforeAreTabsAgain();
    void lyricsPanelFollowsThePosition();
    void lyricsPanelLooksAreChosenAndAdjusted();
};

// ADR-0227: a remote tab's files are on the remote's machine and need not be
// reachable from this one, so its tags and covers come from that engine --
// however its rows got there.
void BenchMainWindowTest::Panels::narrowWindowKeepsListAndUpNextCompact() {
    BenchMainWindow window;
    window.resize(1'600, 1'000);
    window.show();
    QTRY_VERIFY(window.lists_restored_ && window.up_next_restored_);
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr);
    std::vector<LocalTrackRow> rows;
    for (int index = 0; index < 30; ++index) {
        LocalTrackRow row;
        row.raw_path = "/tmp/narrow-" + std::to_string(index) + ".flac";
        row.title = "Track " + std::to_string(index);
        row.artist = "Artist";
        row.album = "Album";
        rows.push_back(std::move(row));
    }
    tab->model->appendRows(rows);
    window.enqueueLocalRequests({rows[3], rows[7]});
    // Narrowed while Up Next slides open: the columns are too wide for a
    // moment, then fit again.
    window.findChild<QAction*>(QStringLiteral("action-show-up-next"))->trigger();
    window.resize(1'100, 720);
    QTRY_VERIFY(window.up_next_section_->isVisible());
    QTest::qWait(600);

    // Up Next: a stack of two-line rows, no table headers, under the list
    // and low enough to leave it its room (ADR-0294).
    QVERIFY(dynamic_cast<UpNextDelegate*>(window.up_next_view_->itemDelegate()) != nullptr);
    QVERIFY(window.up_next_view_->horizontalHeader()->isHidden());
    QVERIFY(window.up_next_view_->verticalHeader()->isHidden());
    QCOMPARE(window.up_next_view_->rowHeight(0), UpNextDelegate::row_height);
    QTRY_VERIFY(window.up_next_section_->height() <= window.tabs_->height());

    // The list's columns fit, so no scroll bar is left standing under it.
    QTRY_COMPARE(tab->view->horizontalHeader()->length(), tab->view->viewport()->width());
    QTRY_VERIFY(!tab->view->horizontalScrollBar()->isVisible());
}

void BenchMainWindowTest::Panels::sourcePanelOpensOnALibrary() {
    QSettings{}.remove(QStringLiteral("local-library/view"));
    {
        // Nothing chosen yet: the library -- the first tab, Playlists last.
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QCOMPARE(window.local_source_tabs_->currentIndex(), 0);
        QCOMPARE(window.local_source_tabs_->tabText(0), QStringLiteral("Library"));
        QCOMPARE(window.playlists_tab_, window.local_source_tabs_->count() - 1);
        // Clicking Playlists is a choice, and is kept.
        emit window.local_source_tabs_->tabBarClicked(window.playlists_tab_);
        window.local_source_tabs_->setCurrentIndex(window.playlists_tab_);
    }
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QCOMPARE(window.local_source_tabs_->currentIndex(), window.playlists_tab_);
        emit window.local_source_tabs_->tabBarClicked(0);
    }
    // Folders, as kept from before they went, is the library.
    QSettings{}.setValue(QStringLiteral("local-library/view"), QStringLiteral("folders"));
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QCOMPARE(window.local_source_tabs_->currentIndex(), 0);
    }

    // This computer's library hidden: the remote's opens in its place.
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_show_local_key), false);
    QTemporaryDir remote_state;
    QVERIFY(remote_state.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_COMPARE(
        window.local_source_tabs_->tabData(window.local_source_tabs_->currentIndex()).toString(),
        window.remoteEngine()->key.text());
    QSettings{}.remove(QLatin1String(SettingsDialog::library_show_local_key));
}

// ADR-0292: side panels open together share their room as tabs, and one
// closed leaves the others as they were.
namespace {

// A plugin of one side panel, for side panels to share the room with Lyrics.
void sidePanelPlugin(const QString& folder) {
    QVERIFY(QDir{}.mkpath(folder));
    QFile manifest{folder + QStringLiteral("/plugin.json")};
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(R"({"schema": 1, "name": "Side", "panels": [{"id": "p",
                      "label": "Side", "run": ["true"]}]})");
}

} // namespace

// ADR-0292: side panels open together share their room as tabs, and one
// closed leaves the others as they were; ADR-0294: Panels hides them all and
// brings the same back.
void BenchMainWindowTest::Panels::sidePanelsShareTheirRoomAsTabs() {
    const auto folder = pluginFolders().value(0) + QStringLiteral("/side");
    sidePanelPlugin(folder);
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("side")), true);
    BenchMainWindow window;
    window.resize(1200, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* lyrics = window.findChild<QDockWidget*>(QStringLiteral("bench-lyrics"));
    auto* side = window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-side-p"));
    QVERIFY(lyrics != nullptr && side != nullptr);
    auto* show_lyrics = window.findChild<QAction*>(QStringLiteral("action-show-lyrics"));
    auto* show_side = window.findChild<QAction*>(QStringLiteral("action-show-plugin-panel-side-p"));
    QVERIFY(show_lyrics != nullptr && show_side != nullptr);
    show_side->trigger();
    QTRY_VERIFY(side->isVisible() && side->width() >= 260);
    show_lyrics->trigger();
    QTRY_VERIFY(lyrics->isVisible());
    QVERIFY(window.tabifiedDockWidgets(lyrics).contains(side));
    // One area, two tabs, the one just opened in front.
    const auto tab_bar = [&window, lyrics]() -> QTabBar* {
        for (auto* bar : window.findChildren<QTabBar*>()) {
            if (bar->count() == 2 && bar->isVisible() &&
                (bar->tabText(0) == lyrics->windowTitle() ||
                 bar->tabText(1) == lyrics->windowTitle())) {
                return bar;
            }
        }
        return nullptr;
    };
    QTRY_VERIFY(tab_bar() != nullptr);
    auto* tabs = tab_bar();
    QCOMPARE(tabs->tabText(tabs->currentIndex()), lyrics->windowTitle());

    // Panels: all hidden at once, and the same back.
    auto* panels = window.findChild<QToolButton*>(QStringLiteral("action-panels"));
    QVERIFY(panels != nullptr);
    QTRY_VERIFY(panels->isChecked());
    panels->click();
    QTRY_VERIFY(!lyrics->isVisible() && !side->isVisible());
    QTRY_VERIFY(!panels->isChecked());
    panels->click();
    QTRY_VERIFY(lyrics->isVisible() && side->isVisible());
    QTRY_VERIFY(panels->isChecked());
    QTRY_VERIFY(window.tabifiedDockWidgets(lyrics).contains(side) ||
                window.tabifiedDockWidgets(side).contains(lyrics));

    // Closed, the other stays, at a width of its own -- not squeezed.
    show_lyrics->trigger();
    QTRY_VERIFY(!lyrics->isVisible());
    QVERIFY(side->isVisible());
    QTRY_VERIFY(side->width() >= 260);
    show_side->trigger();
    QSettings{}.remove(QStringLiteral("plugins"));
    QSettings{}.remove(QStringLiteral("plugin-panels"));
    QSettings{}.remove(QStringLiteral("panels"));
    QSettings{}.remove(QStringLiteral("lyrics"));
    QDir{folder}.removeRecursively();
}

// ADR-0292: panels open before stay tabs when the window opens again, and
// a plugin's made again when Settings are saved joins them; Up Next, under
// the lists, is none of them (ADR-0294).
void BenchMainWindowTest::Panels::sidePanelsOpenBeforeAreTabsAgain() {
    const auto folder = pluginFolders().value(0) + QStringLiteral("/side");
    sidePanelPlugin(folder);
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("side")), true);
    QSettings{}.setValue(QStringLiteral("lyrics/visible"), true);
    QSettings{}.setValue(QStringLiteral("up-next/visible"), true);
    QSettings{}.setValue(QStringLiteral("plugin-panels/side-p/visible"), true);
    {
        BenchMainWindow window;
        window.resize(1200, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* lyrics = window.findChild<QDockWidget*>(QStringLiteral("bench-lyrics"));
        QVERIFY(lyrics != nullptr);
        QVERIFY(window.findChild<QDockWidget*>(QStringLiteral("bench-up-next")) == nullptr);
        const auto side = [&window] {
            return window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-side-p"));
        };
        QVERIFY(side() != nullptr);
        const auto tabbed = [&window, lyrics](QDockWidget* other) {
            return window.tabifiedDockWidgets(lyrics).contains(other) ||
                   window.tabifiedDockWidgets(other).contains(lyrics);
        };
        QTRY_VERIFY(lyrics->isVisible() && side()->isVisible() &&
                    window.up_next_section_->isVisible());
        QTRY_VERIFY(tabbed(side()));
        // Made again, as when Settings are saved: a tab still.
        window.buildPluginPanels();
        QTRY_VERIFY(side() != nullptr && side()->isVisible() && tabbed(side()));
    }
    for (const auto* key : {"lyrics/visible", "up-next/visible", "plugin-panels"}) {
        QSettings{}.remove(QString::fromLatin1(key));
    }
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
}

// ADR-0292: one layout -- the sources left, the lists right -- nothing to
// edit or rearrange, and the divider kept where it was left.
void BenchMainWindowTest::Panels::theSourcesKeepTheirWidth() {
    QSettings{}.remove(QStringLiteral("workspace/sources-width"));
    {
        BenchMainWindow window;
        window.resize(1200, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* split = qobject_cast<QSplitter*>(window.centralWidget());
        QVERIFY(split != nullptr);
        QCOMPARE(split->objectName(), QStringLiteral("bench-panel-split"));
        QCOMPARE(split->count(), 2);
        QCOMPARE(split->widget(0)->objectName(), QStringLiteral("bench-panel-sources"));
        QCOMPARE(split->widget(1)->objectName(), QStringLiteral("bench-track-area"));
        for (const auto* gone :
             {"action-edit-panel-layout", "action-layout-side-by-side", "action-layout-top-bottom",
              "action-layout-tabbed", "action-layout-swap-panels", "action-reset-panel-layout"}) {
            QVERIFY2(window.findChild<QAction*>(QString::fromLatin1(gone)) == nullptr, gone);
        }
        // A quarter for the sources at first.
        QTRY_VERIFY(split->sizes().value(0) > 0);
        QVERIFY(std::abs(split->sizes().value(0) - split->width() / 4) < 40);
        // Moved by hand, kept.
        split->setSizes({360, split->width() - 360});
        emit split->splitterMoved(360, 1);
    }
    BenchMainWindow again;
    again.resize(1200, 700);
    again.show();
    QVERIFY(QTest::qWaitForWindowExposed(&again));
    auto* split = qobject_cast<QSplitter*>(again.centralWidget());
    QVERIFY(split != nullptr);
    QTRY_VERIFY(std::abs(split->sizes().value(0) - 360) < 4);
    // Sized by the window manager after it opened -- as a tiling one does --
    // and made narrow and wide again: the sources as they were.
    BenchMainWindow tiled;
    tiled.resize(640, 480);
    tiled.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tiled));
    auto* tiled_split = qobject_cast<QSplitter*>(tiled.centralWidget());
    QVERIFY(tiled_split != nullptr);
    for (const int width : {1800, 900, 1600}) {
        tiled.resize(width, 900);
        QTRY_COMPARE(tiled_split->width(), tiled.centralWidget()->width());
        QTRY_VERIFY2(std::abs(tiled_split->sizes().value(0) - 360) < 4,
                     qPrintable(QString::number(tiled_split->sizes().value(0))));
    }
    QSettings{}.remove(QStringLiteral("workspace/sources-width"));
}

// ADR-0353: the panel draws in a look chosen from its heading, each look
// with a font, size and colours of its own, previewed as they change.
void BenchMainWindowTest::Panels::lyricsPanelLooksAreChosenAndAdjusted() {
    QSettings{}.remove(QStringLiteral("lyrics/look"));
    QSettings{}.remove(QStringLiteral("lyrics/looks"));
    LyricsPanel panel;
    panel.resize(300, 400);
    panel.show();
    auto* lines = panel.findChild<QListWidget*>(QStringLiteral("lyrics-lines"));
    QVERIFY(lines != nullptr);
    QCOMPARE(panel.look(), LyricsLook::page);
    metadata::Lyrics synced{
        .source = "sidecar", .synced = true, .lines = {}, .declared_length_ms = {}};
    for (int line = 0; line < 8; ++line) {
        synced.lines.push_back({.time_ms = line * 1'000, .text = "Line " + std::to_string(line)});
    }
    panel.showLyrics(synced);
    panel.setPosition(2'100, false);
    QCOMPARE(panel.currentLine(), 2);

    // Page, as it always was: centred, the line sung bold, the rest dimmed alike.
    QVERIFY(lines->item(2)->font().bold() && !lines->item(3)->font().bold());
    QVERIFY((lines->item(2)->textAlignment() & Qt::AlignHCenter) != 0);
    QCOMPARE(lines->item(3)->foreground().color(), lines->item(6)->foreground().color());
    const auto page_size = lines->item(3)->font().pointSizeF();

    // Chosen from the heading's menu, and kept.
    auto* stage = panel.findChild<QAction*>(QStringLiteral("lyrics-look-stage"));
    QVERIFY(stage != nullptr && panel.lookMenu()->actions().contains(stage));
    stage->trigger();
    QCOMPARE(panel.look(), LyricsLook::stage);
    QCOMPARE(readLyricsLook(), LyricsLook::stage);
    QVERIFY(lines->item(3)->font().pointSizeF() > page_size);
    QVERIFY(lines->item(2)->font().pointSizeF() > lines->item(3)->font().pointSizeF());
    // Lines recede the further they are from the one sung.
    QVERIFY(lines->item(3)->foreground().color().alphaF() >
            lines->item(6)->foreground().color().alphaF());
    QCOMPARE(lines->item(1)->foreground().color().alphaF(),
             lines->item(3)->foreground().color().alphaF());

    // Reading: left, the line sung only in the accent.
    panel.setLook(LyricsLook::reading);
    QVERIFY((lines->item(2)->textAlignment() & Qt::AlignLeft) != 0);
    QVERIFY(!lines->item(2)->font().bold());
    QCOMPARE(lines->item(2)->foreground().color(), panel.palette().color(QPalette::Highlight));
    QCOMPARE(lines->item(5)->foreground().color(), panel.palette().color(QPalette::Text));

    // A look's own colours and size, kept for it alone.
    LyricsLookSettings mine;
    mine.sung = QColor{0xc0, 0x40, 0x20};
    mine.lines = QColor{0x20, 0x40, 0x60};
    mine.size_percent = 150;
    panel.setLookSettings(mine);
    QCOMPARE(lines->item(2)->foreground().color(), mine.sung);
    QCOMPARE(lines->item(5)->foreground().color(), mine.lines);
    QCOMPARE(readLyricsLookSettings(LyricsLook::reading), mine);
    QCOMPARE(readLyricsLookSettings(LyricsLook::page), LyricsLookSettings{});
    panel.setLook(LyricsLook::page);
    QCOMPARE(panel.lookSettings(), LyricsLookSettings{});
    QCOMPARE(lines->item(3)->font().pointSizeF(), page_size);
    panel.setLook(LyricsLook::reading);
    QCOMPARE(panel.lookSettings(), mine);

    // The dialog draws each change at once; Cancel goes back, OK keeps.
    {
        LyricsLookDialog dialog{&panel};
        auto changed = mine;
        changed.sung = QColor{0x10, 0xa0, 0x10};
        dialog.change(changed);
        QCOMPARE(lines->item(2)->foreground().color(), changed.sung);
        QCOMPARE(readLyricsLookSettings(LyricsLook::reading), mine);
        dialog.reject();
        QCOMPARE(lines->item(2)->foreground().color(), mine.sung);
    }
    {
        LyricsLookDialog dialog{&panel};
        auto changed = mine;
        changed.sung = QColor{};
        changed.font_family = QStringLiteral("Serif");
        dialog.change(changed);
        dialog.accept();
        QCOMPARE(readLyricsLookSettings(LyricsLook::reading), changed);
        QCOMPARE(lines->item(2)->foreground().color(), panel.palette().color(QPalette::Highlight));
    }

    // Sleeve: the playing album's cover behind the words, light on it.
    panel.setLook(LyricsLook::sleeve);
    QVERIFY(!panel.showsCover());
    QImage cover{64, 64, QImage::Format_RGB32};
    cover.fill(QColor{0x20, 0x60, 0xe0});
    panel.setCover(cover);
    QVERIFY(panel.showsCover());
    const auto shot = panel.grab().toImage();
    const auto ground = shot.pixelColor(shot.width() / 2, shot.height() - 4);
    QVERIFY2(ground.blue() > ground.red() + 20, qPrintable(ground.name()));
    QVERIFY(lines->item(5)->foreground().color().lightnessF() > 0.8F);
    panel.setCover({});
    QVERIFY(!panel.showsCover());

    QSettings{}.remove(QStringLiteral("lyrics/look"));
    QSettings{}.remove(QStringLiteral("lyrics/looks"));
}

// ADR-0276: the line being sung is highlighted, carried on between the
// engine's reports while it plays, and a click on a line seeks there.
void BenchMainWindowTest::Panels::lyricsPanelFollowsThePosition() {
    LyricsPanel panel;
    panel.resize(300, 400);
    panel.show();
    auto* lines = panel.findChild<QListWidget*>(QStringLiteral("lyrics-lines"));
    auto* source = panel.findChild<QLabel*>(QStringLiteral("lyrics-source"));
    QVERIFY(lines != nullptr && source != nullptr);
    QCOMPARE(lines->count(), 1); // "Nothing is playing"

    metadata::Lyrics synced{
        .source = "sidecar", .synced = true, .lines = {}, .declared_length_ms = {}};
    for (const auto& [time, text] : {std::pair{1'000, "One"}, std::pair{2'000, "Two"},
                                     std::pair{3'000, ""}, std::pair{60'000, "Late"}}) {
        synced.lines.push_back({.time_ms = time, .text = text});
    }
    panel.showLyrics(synced);
    QCOMPARE(lines->count(), 4);
    QCOMPARE(source->text(), QStringLiteral("Synced · .lrc file"));
    panel.setPosition(500, false);
    QCOMPARE(panel.currentLine(), -1);
    panel.setPosition(1'200, false);
    QCOMPARE(panel.currentLine(), 0);
    QVERIFY(lines->item(0)->font().bold() && !lines->item(1)->font().bold());
    // Playing, the same report moves on by itself, refreshed as the
    // window's transport timer does.
    QTRY_VERIFY_WITH_TIMEOUT((panel.setPosition(1'900, true), panel.currentLine() == 1), 2'000);
    QVERIFY(!lines->item(0)->font().bold() && lines->item(1)->font().bold());
    // Paused, it stays; a seek far on jumps there.
    panel.setPosition(2'500, false);
    QCOMPARE(panel.currentLine(), 1);
    panel.setPosition(3'100, false);
    QCOMPARE(panel.currentLine(), 2);
    panel.setPosition(61'000, false);
    QCOMPARE(panel.currentLine(), 3);

    // A seek asked for here moves the highlight at once; reports from
    // before it, still on their way, are not believed; one that agrees is.
    panel.seekTo(1'500);
    QCOMPARE(panel.currentLine(), 0);
    panel.setPosition(61'000, false);
    QCOMPARE(panel.currentLine(), 0);
    panel.setPosition(1'550, false);
    QCOMPARE(panel.currentLine(), 0);
    // A seek made elsewhere -- far from what the clock says -- is followed.
    panel.setPosition(2'200, false);
    QCOMPARE(panel.currentLine(), 1);
    // Playing, a report a little behind the clock does not pull it back.
    panel.setPosition(2'000, true);
    QTRY_VERIFY_WITH_TIMEOUT((panel.setPosition(2'000, true), panel.position() >= 2'250), 2'000);
    const auto before_late_report = panel.position();
    panel.setPosition(before_late_report - 200, true);
    QVERIFY(panel.position() >= before_late_report);
    panel.setPosition(0, false);

    QSignalSpy seeks{&panel, &LyricsPanel::seekRequested};
    QTest::mouseClick(lines->viewport(), Qt::LeftButton, {},
                      lines->visualItemRect(lines->item(1)).center());
    QCOMPARE(seeks.size(), 1);
    QCOMPARE(seeks.front().front().toLongLong(), qint64{2'000});

    // Plain lyrics: text, nothing highlighted, a click seeks nowhere.
    panel.showLyrics({.source = "tag:LYRICS",
                      .synced = false,
                      .lines = {{.time_ms = std::nullopt, .text = "Words"}},
                      .declared_length_ms = {}});
    QCOMPARE(source->text(), QStringLiteral("Plain · LYRICS tag"));
    panel.setPosition(5'000, true);
    QCOMPARE(panel.currentLine(), -1);
    QTest::mouseClick(lines->viewport(), Qt::LeftButton, {},
                      lines->visualItemRect(lines->item(0)).center());
    QCOMPARE(seeks.size(), 1);

    // Long lyrics scroll the line sung to the middle, gliding there; a
    // scroll by hand is left alone for a moment.
    metadata::Lyrics song{
        .source = "sidecar", .synced = true, .lines = {}, .declared_length_ms = {}};
    for (int line = 0; line < 60; ++line) {
        song.lines.push_back({.time_ms = line * 1'000, .text = "Line " + std::to_string(line)});
    }
    panel.showLyrics(song);
    panel.setPosition(30'200, false);
    QCOMPARE(panel.currentLine(), 30);
    const auto centred = [&] {
        const auto rect = lines->visualItemRect(lines->item(panel.currentLine()));
        return std::abs(rect.center().y() - lines->viewport()->height() / 2) <= rect.height();
    };
    QTRY_VERIFY_WITH_TIMEOUT(centred(), 2'000);
    QWheelEvent wheel{QPointF{10, 10},   lines->viewport()->mapToGlobal(QPointF{10, 10}),
                      QPoint{},          QPoint{0, 120},
                      Qt::NoButton,      Qt::NoModifier,
                      Qt::NoScrollPhase, false};
    QApplication::sendEvent(lines->viewport(), &wheel);
    const auto scrolled_to = lines->verticalScrollBar()->value();
    panel.setPosition(33'200, false);
    QCOMPARE(panel.currentLine(), 33);
    QTest::qWait(600);
    QCOMPARE(lines->verticalScrollBar()->value(), scrolled_to);
    // A seek from here shows the line again.
    panel.seekTo(40'100);
    QTRY_VERIFY_WITH_TIMEOUT(centred(), 2'000);

    panel.showMessage(QStringLiteral("No lyrics"));
    QCOMPARE(lines->count(), 1);
    QCOMPARE(lines->item(0)->text(), QStringLiteral("No lyrics"));
    QVERIFY(source->text().isEmpty());

    // In the window: a dock, shown with the other side panels -- Panels,
    // Ctrl+Shift+L, shows and hides them together (ADR-0299).
    QSettings{}.remove(QStringLiteral("panels/shown"));
    QSettings{}.setValue(QStringLiteral("lyrics/visible"), false);
    BenchMainWindow window;
    window.show();
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-lyrics"));
    auto* panels = window.findChild<QAction*>(QStringLiteral("action-toggle-panels"));
    QVERIFY(dock != nullptr && panels != nullptr);
    QCOMPARE(panels->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+L")));
    QVERIFY(dock->isHidden());
    panels->trigger();
    QTRY_VERIFY(dock->isVisible());
    QVERIFY(QSettings{}.value(QStringLiteral("panels/shown")).toBool());
    panels->trigger();
    QTRY_VERIFY(dock->isHidden());
    QVERIFY(!QSettings{}.value(QStringLiteral("panels/shown")).toBool());
    QSettings{}.remove(QStringLiteral("panels/shown"));
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::Panels)

#include "window_panels_test.moc"
