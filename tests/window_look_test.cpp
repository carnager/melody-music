// SPDX-License-Identifier: GPL-3.0-only

// The window test: the window's look: palettes, colour schemes, size, the header.

#include "window_test.hpp"

#include "uicommon/library_tree_view.hpp"
#include "workspace/artwork_session.hpp"
#include "workspace/sources.hpp"

#include <QGroupBox>
#include <QMessageBox>
#include <QPainter>
#include <QStandardItemModel>
#include <sqlite3.h>

namespace trackknife::bench {

class BenchMainWindowTest::Look final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void transportIsOneRowWithCoverAndPills();
    void headerShowsThePlayingAlbumsCover();
    void activeTabAccentSurvivesThemeTextColor();
    void listTabsMeasureTheSameUnderEveryStyle();
    void aboutShowsTheMascotAndTheRelease();
    void paintedStatusIconsFollowThePalette();
    void colorSchemesAreChosenAndApplied();
    void interfaceSizeIsChosenForTheNextStart();
    void headerPillsFollowAPaletteChangedLater();
};

void BenchMainWindowTest::Look::headerShowsThePlayingAlbumsCover() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      media.filePath(QStringLiteral("art.flac"))));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    window.openLocalPaths({QFile::encodeName(media.path()).toStdString()});
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    // The row's album as its tags say, once they are read: taken before, it
    // was the key of a row with no tags yet, which no cover ever comes for.
    QTRY_VERIFY(tab->model->hasArtwork(tab->model->groupKey(0)));
    const auto group = tab->model->groupKey(0);

    // Nothing playing: a placeholder tile the size of a cover.
    window.refreshHeaderCover({});
    QVERIFY(window.header_cover_key_.isEmpty());
    QVERIFY(!window.now_playing_cover_->pixmap().isNull());

    // Playing: the cover of that entry's album, from the tab that has it.
    window.refreshHeaderCover(
        QString::fromStdString(tab->model->rows().front().entry_id.to_string()));
    QCOMPARE(window.header_cover_key_, group);
    QCOMPARE(window.now_playing_cover_->pixmap().deviceIndependentSize().toSize(), QSize(44, 44));
}

void BenchMainWindowTest::Look::transportIsOneRowWithCoverAndPills() {
    BenchMainWindow window;
    window.show();
    QCoreApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);

    auto* now_playing = window.findChild<QLabel*>(QStringLiteral("bench-now-playing"));
    auto* now_playing_context =
        window.findChild<QLabel*>(QStringLiteral("bench-now-playing-context"));
    auto* seek = window.findChild<QSlider*>(QStringLiteral("bench-seek"));
    auto* volume = window.findChild<QSlider*>(QStringLiteral("bench-volume"));
    auto* device = window.findChild<QToolButton*>(QStringLiteral("bench-device"));
    auto* header = window.findChild<QWidget*>(QStringLiteral("bench-player-header"));
    auto* transport = window.findChild<QWidget*>(QStringLiteral("bench-transport-buttons"));

    QVERIFY(now_playing != nullptr);
    QVERIFY(now_playing_context != nullptr);
    QVERIFY(seek != nullptr);
    QVERIFY(volume != nullptr);
    QVERIFY(device != nullptr);
    QVERIFY(header != nullptr);
    QVERIFY(transport != nullptr);
    QVERIFY(window.findChild<QComboBox*>(QStringLiteral("bench-device")) == nullptr);
    QCOMPARE(now_playing->accessibleName(), QStringLiteral("Current artist and title"));
    QCOMPARE(now_playing_context->accessibleName(), QStringLiteral("Current album and date"));
    QCOMPARE(seek->accessibleName(), QStringLiteral("Playback position"));
    QVERIFY(dynamic_cast<ui::LineSlider*>(seek) != nullptr);
    QVERIFY(dynamic_cast<ui::LineSlider*>(volume) != nullptr);
    QCOMPARE(device->accessibleName(), QStringLiteral("Audio output device"));
    // The regular menu bar is the application menu — no hamburger button.
    QVERIFY(window.findChild<QToolButton*>(QStringLiteral("bench-main-menu")) == nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-backup-workspace")) != nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-restore-workspace")) != nullptr);
    QVERIFY(!window.menuBar()->isHidden());
    // File, Edit, Workspace, Playback, and Help with the language references.
    QCOMPARE(window.menuBar()->actions().size(), 5);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-reference-tkfmt")) != nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-reference-scripts")) != nullptr);
    QCOMPARE(device->toolButtonStyle(), Qt::ToolButtonIconOnly);
    // Nothing to name, no chevron squeezed beside the icon.
    QVERIFY(device->findChild<QLabel*>(QStringLiteral("bench-device-chevron"))->isHidden());
    QVERIFY(!device->icon().isNull());
    QVERIFY(device->menu() != nullptr);
    QCOMPARE(device->menu()->objectName(), QStringLiteral("bench-device-menu"));
    QVERIFY(!device->menu()->actions().empty());
    QCOMPARE(device->menu()->actions().front()->text(), QStringLiteral("System default"));
    QVERIFY(window.property("trackknife-player-output-available").isValid());
    QVERIFY(window.property("trackknife-player-output-suspended").isValid());
    QVERIFY(window.property("trackknife-player-default-output").isValid());
    // Previous, play and next; stop lives in the Playback menu.
    QCOMPARE(transport->findChildren<QToolButton*>(QString{}, Qt::FindDirectChildrenOnly).size(),
             3);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-show-up-next")) != nullptr);

    // One row: cover, then what is playing, then the controls and position,
    // then where it plays and what waits -- all on one line.
    auto* cover = window.findChild<QLabel*>(QStringLiteral("bench-now-playing-cover"));
    auto* up_next = window.findChild<QToolButton*>(QStringLiteral("action-up-next"));
    auto* count = window.findChild<QLabel*>(QStringLiteral("bench-up-next-count"));
    QVERIFY(cover != nullptr && up_next != nullptr && count != nullptr);
    const auto at = [&window](const QWidget* widget) {
        return QRect{widget->mapTo(&window, QPoint{}), widget->size()};
    };
    QTRY_VERIFY(!cover->pixmap().isNull());
    QVERIFY(at(now_playing).bottom() <= at(now_playing_context).top());
    QVERIFY(at(cover).right() < at(now_playing).left());
    QVERIFY(at(now_playing).right() < at(transport).left());
    QVERIFY(at(transport).right() < at(seek).left());
    QVERIFY(at(seek).right() < at(volume).left());
    QVERIFY(at(volume).right() < at(device).left());
    QVERIFY(at(device).right() < at(up_next).left());
    for (const auto* widget :
         std::initializer_list<const QWidget*>{cover, transport, volume, device, up_next}) {
        QVERIFY(std::abs(at(widget).center().y() - at(seek).center().y()) <= 1);
    }
    QVERIFY(header->height() <= 64);
    // The count shows only when something waits.
    QVERIFY(count->isHidden());
    QCOMPARE(up_next->accessibleName(), QStringLiteral("Up Next"));
}

void BenchMainWindowTest::Look::activeTabAccentSurvivesThemeTextColor() {
    class WhiteLabelStyle final : public QProxyStyle {
      public:
        WhiteLabelStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}
        void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                         const QWidget* widget = nullptr) const override {
            if (element == CE_TabBarTabShape) {
                painter->fillRect(option->rect, Qt::black);
            } else if (element == CE_TabBarTabLabel) {
                painter->setPen(Qt::white);
                painter->drawText(option->rect, Qt::AlignCenter, QStringLiteral("Theme label"));
            } else {
                QProxyStyle::drawControl(element, option, painter, widget);
            }
        }
    };
    PlaybackTabBar bar;
    auto* style = new WhiteLabelStyle;
    style->setParent(&bar);
    bar.setStyle(style);
    auto palette = bar.palette();
    const QColor accent(240, 30, 160);
    palette.setColor(QPalette::Highlight, accent);
    bar.setPalette(palette);
    bar.addTab(QStringLiteral("Album"));
    bar.addTab(QStringLiteral("Browsing"));
    bar.resize(360, 40);
    bar.setTabIcon(0, playbackSpeakerIcon(palette));
    bar.setTabData(0, true);
    const auto colored_pixels = [&] {
        const auto image = bar.grab().toImage();
        int count = 0;
        const auto rect = bar.tabRect(0).adjusted(0, 0, 0, -4);
        for (int y = rect.top(); y <= rect.bottom(); ++y)
            for (int x = rect.left(); x <= rect.right(); ++x)
                if (const auto pixel = image.pixelColor(x, y);
                    pixel.red() > 80 && pixel.blue() > 40 && pixel.green() < pixel.red() / 3)
                    ++count;
        return count;
    };
    QVERIFY(colored_pixels() > 10);
    bar.setCurrentIndex(1);
    QVERIFY(colored_pixels() > 10);
    // The tab being browsed is filled with the list's ground; the others
    // are not, whichever is playing.
    const auto image = bar.grab().toImage();
    const auto ground = bar.palette().color(QPalette::Base);
    QCOMPARE(image.pixelColor(bar.tabRect(1).center().x(), bar.tabRect(1).bottom() - 1), ground);
    QVERIFY(image.pixelColor(bar.tabRect(0).center().x(), bar.tabRect(0).bottom() - 1) != ground);
    bar.setTabData(0, false);
    bar.setTabIcon(0, QIcon{});
    QCOMPARE(colored_pixels(), 0);
}

// The list tabs are sized and their close buttons placed by the bar, not the
// style: the same under any, each tab with room for its whole text and the
// button on its right, after it.
void BenchMainWindowTest::Look::listTabsMeasureTheSameUnderEveryStyle() {
    const auto previous = QApplication::style()->name();
    const auto measure = [](const QString& style) {
        QApplication::setStyle(QStyleFactory::create(style));
        PlaybackTabWidget tabs;
        tabs.setDocumentMode(true);
        tabs.addTab(new QWidget, QStringLiteral("Local *"));
        const auto playing = tabs.addTab(new QWidget, playbackSpeakerIcon(tabs.palette()),
                                         QStringLiteral("gemenon *"));
        tabs.tabBar()->setTabData(playing, true);
        tabs.setTabText(playing, tabs.tabText(playing));
        tabs.resize(700, 60);
        tabs.show();
        auto* bar = tabs.tabBar();
        std::vector<std::pair<QRect, QRect>> found;
        for (int index = 0; index < bar->count(); ++index) {
            auto* close = bar->tabButton(index, QTabBar::RightSide);
            const auto tab = bar->tabRect(index);
            if (close == nullptr || bar->tabButton(index, QTabBar::LeftSide) != nullptr) {
                return std::vector<std::pair<QRect, QRect>>{};
            }
            // Room for the text, and the button after it.
            const auto text = bar->fontMetrics().horizontalAdvance(bar->tabText(index));
            if (close->geometry().left() < tab.left() + text + 12 ||
                close->geometry().right() > tab.right()) {
                return std::vector<std::pair<QRect, QRect>>{};
            }
            found.emplace_back(tab, close->geometry());
        }
        return found;
    };
    const auto fusion = measure(QStringLiteral("Fusion"));
    const auto windows = measure(QStringLiteral("Windows"));
    QApplication::setStyle(QStyleFactory::create(previous));
    QCOMPARE(fusion.size(), std::size_t{2});
    QVERIFY(fusion == windows);
}

// ADR-0287: Help › About Melody shows the mascot, the release and the
// licence.
void BenchMainWindowTest::Look::aboutShowsTheMascotAndTheRelease() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QCOMPARE(window.windowTitle(), QStringLiteral("Melody"));
    auto* about = window.findChild<QAction*>(QStringLiteral("action-about"));
    QVERIFY(about != nullptr);
    QCOMPARE(about->text(), QStringLiteral("About Melody"));
    about->trigger();
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<QDialog*>(QStringLiteral("bench-about"))) != nullptr);
    const auto* mascot = dialog->findChild<QLabel*>(QStringLiteral("bench-about-mascot"));
    QVERIFY(mascot != nullptr && !mascot->pixmap().isNull());
    // Drawn, not an empty square: the SVG reached the label.
    const auto image = mascot->pixmap().toImage();
    int drawn = 0;
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4) {
            drawn += qAlpha(image.pixel(x, y)) > 0 ? 1 : 0;
        }
    }
    QVERIFY2(drawn > 100, qPrintable(QString::number(drawn)));
    const auto* version = dialog->findChild<QLabel*>(QStringLiteral("bench-about-version"));
    QVERIFY(version != nullptr && version->text().startsWith(QStringLiteral("Version ")));
    dialog->close();
}

void BenchMainWindowTest::Look::paintedStatusIconsFollowThePalette() {
    const auto before = QGuiApplication::palette();
    BenchMainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* album_shuffle = window.findChild<QAction*>(QStringLiteral("action-local-album-random"));
    QVERIFY(album_shuffle != nullptr);
    // The colour of the icon's strokes: its most opaque pixel.
    const auto ink = [album_shuffle] {
        const auto image = album_shuffle->icon().pixmap(24, 24).toImage();
        QColor strongest{Qt::transparent};
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const auto pixel = image.pixelColor(x, y);
                if (pixel.alpha() > strongest.alpha()) {
                    strongest = pixel;
                }
            }
        }
        return strongest;
    };
    auto later = before;
    later.setColor(QPalette::Window, QColor{0x1e, 0x1f, 0x22});
    later.setColor(QPalette::ButtonText, QColor{0xff, 0x00, 0x00});
    QGuiApplication::setPalette(later);
    QTRY_VERIFY2(ink().red() > 200 && ink().green() < 60 && ink().blue() < 60,
                 qPrintable(ink().name(QColor::HexArgb)));
    QGuiApplication::setPalette(before);
}

// ADR-0247: Trackknife's own light and dark schemes, chosen in Settings,
// applied at once -- with Fusion, which paints with the palette -- and the
// system's colours back again.
// ADR-0251: the interface size is offered in Settings, stored, and read back
// before the application exists -- through the settings file alone, as the
// next start reads it -- for QT_SCALE_FACTOR; anything unusable is 100%.
void BenchMainWindowTest::Look::interfaceSizeIsChosenForTheNextStart() {
    QSettings{}.remove(QLatin1String(interface_scale_key));
    QCOMPARE(chosenInterfaceScale(), 1.0);
    BenchMainWindow window;
    window.show();
    window.findChild<QAction*>(QStringLiteral("action-settings"))->trigger();
    auto* dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog != nullptr);
    auto* combo = dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-interface-scale"));
    QVERIFY(combo != nullptr);
    QCOMPARE(combo->count(), static_cast<int>(interfaceScales().size()));
    QCOMPARE(combo->currentData().toString(), QStringLiteral("1"));
    combo->setCurrentIndex(combo->findData(QStringLiteral("1.5")));
    QPointer<SettingsDialog> lifetime = dialog;
    dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    QTRY_VERIFY(lifetime.isNull());
    QSettings{}.sync();
    QCOMPARE(chosenInterfaceScale(), 1.5);
    // Opened again, Settings shows the size chosen, not "As the system".
    window.findChild<QAction*>(QStringLiteral("action-settings"))->trigger();
    QTRY_VERIFY(window.findChild<SettingsDialog*>() != nullptr);
    dialog = window.findChild<SettingsDialog*>();
    combo = dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-interface-scale"));
    QVERIFY(combo != nullptr);
    QCOMPARE(combo->currentData().toString(), QStringLiteral("1.5"));
    dialog->reject();
    QSettings{}.setValue(QLatin1String(interface_scale_key), QStringLiteral("7"));
    QSettings{}.sync();
    QCOMPARE(chosenInterfaceScale(), 1.0);
    QSettings{}.remove(QLatin1String(interface_scale_key));
}

// The header's pills are mixed from the palette; a palette that changes after
// the window was built -- the desktop's theme arriving or turning light, not
// a scheme chosen here -- recolours them rather than leaving dark pills on a
// light window.
void BenchMainWindowTest::Look::headerPillsFollowAPaletteChangedLater() {
    followColorSchemes();
    QApplication::setPalette(darkPalette());
    BenchMainWindow window;
    window.resize(1000, 600);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* up_next = window.findChild<QToolButton*>(QStringLiteral("action-up-next"));
    QVERIFY(up_next != nullptr);
    const auto ground_of = [up_next] {
        const auto image = up_next->grab().toImage();
        return image.pixelColor(image.width() / 2, 3);
    };
    QVERIFY(ground_of().lightness() < 128);
    QApplication::setPalette(lightPalette());
    QCoreApplication::processEvents();
    QTRY_VERIFY(ground_of().lightness() > 128);
    ColorSchemes::instance().applyChosen();
}

void BenchMainWindowTest::Look::colorSchemesAreChosenAndApplied() {
    // Complete schemes: an unfocused window keeps its accent, disabled text
    // reads as disabled.
    for (const auto& palette : {lightPalette(), darkPalette()}) {
        QCOMPARE(palette.color(QPalette::Inactive, QPalette::Highlight),
                 palette.color(QPalette::Active, QPalette::Highlight));
        QVERIFY(palette.color(QPalette::Disabled, QPalette::Text) !=
                palette.color(QPalette::Active, QPalette::Text));
    }
    QVERIFY(darkPalette().color(QPalette::Window).lightness() <
            lightPalette().color(QPalette::Window).lightness());

    // Icons in the variant the ground needs, found among those installed.
    {
        QTemporaryDir icons;
        QVERIFY(icons.isValid());
        for (const auto* name : {"Glyphs-Dark", "Glyphs-Light", "breezy", "breezy-dark"}) {
            QVERIFY(QDir{icons.path()}.mkpath(QString::fromLatin1(name)));
            QFile index{icons.path() + QLatin1Char('/') + QString::fromLatin1(name) +
                        QStringLiteral("/index.theme")};
            QVERIFY(index.open(QIODevice::WriteOnly));
            index.write("[Icon Theme]\nName=x\n");
        }
        const auto paths = QIcon::themeSearchPaths();
        QIcon::setThemeSearchPaths({icons.path()});
        QCOMPARE(iconThemeVariant(QStringLiteral("Glyphs-Dark"), false),
                 QStringLiteral("Glyphs-Light"));
        QCOMPARE(iconThemeVariant(QStringLiteral("Glyphs-Light"), true),
                 QStringLiteral("Glyphs-Dark"));
        QCOMPARE(iconThemeVariant(QStringLiteral("breezy-dark"), false), QStringLiteral("breezy"));
        QCOMPARE(iconThemeVariant(QStringLiteral("breezy"), true), QStringLiteral("breezy-dark"));
        QCOMPARE(iconThemeVariant(QStringLiteral("Lonely"), true), QStringLiteral("Lonely"));
        QIcon::setThemeSearchPaths(paths);
    }

    followColorSchemes();
    BenchMainWindow window;
    window.show();
    const auto choose = [&window](const QString& scheme) {
        window.findChild<QAction*>(QStringLiteral("action-settings"))->trigger();
        auto* dialog = window.findChild<SettingsDialog*>();
        QVERIFY(dialog != nullptr);
        auto* combo = dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-color-scheme"));
        QVERIFY(combo != nullptr);
        combo->setCurrentIndex(combo->findData(scheme));
        QPointer<SettingsDialog> lifetime = dialog;
        dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
            ->button(QDialogButtonBox::Save)
            ->click();
        QTRY_VERIFY(lifetime.isNull());
    };
    choose(QStringLiteral("dark"));
    QCOMPARE(QApplication::palette().color(QPalette::Window),
             darkPalette().color(QPalette::Window));
    // ADR-0250: Trackknife's own style under every scheme, as Quick's.
    QVERIFY(qobject_cast<TrackknifeStyle*>(QApplication::style()) != nullptr);
    QVERIFY(ColorSchemes::instance().ownPalette());
    QCOMPARE(window.palette().color(QPalette::Base), darkPalette().color(QPalette::Base));

    choose(QStringLiteral("light"));
    QCOMPARE(QApplication::palette().color(QPalette::Window),
             lightPalette().color(QPalette::Window));

    choose(QStringLiteral("system"));
    QVERIFY(!ColorSchemes::instance().ownPalette());
    QVERIFY(qobject_cast<TrackknifeStyle*>(QApplication::style()) != nullptr);
    QVERIFY(QApplication::palette().color(QPalette::Window) !=
            darkPalette().color(QPalette::Window));
    QSettings{}.remove(QLatin1String(color_scheme_key));
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::Look)

#include "window_look_test.moc"
