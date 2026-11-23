// SPDX-License-Identifier: GPL-3.0-only

#include "bench/trackknife_style.hpp"

#include <QApplication>
#include <QIconEngine>
#include <QImage>
#include <QPainter>
#include <QPixmapCache>
#include <QStyleOptionToolButton>
#include <QtTest>

#include <memory>
#include <utility>

namespace {
struct Rendered {
    int calls{};
    int original_calls{};
    int live_copies{};
    QIcon::Mode mode{QIcon::Normal};
    QIcon::State state{QIcon::Off};
    qreal dpr{};
    QSize size;
};

// Models a theme engine with state retained by its owner. The source must
// never be rendered into: only a temporary clone may accumulate entries.
class ProbeEngine final : public QIconEngine {
  public:
    explicit ProbeEngine(std::shared_ptr<Rendered> rendered, bool copy = false)
        : rendered_(std::move(rendered)), copy_(copy) {
        if (copy_) {
            ++rendered_->live_copies;
        }
    }
    ~ProbeEngine() override {
        if (copy_) {
            --rendered_->live_copies;
        }
    }
    QIconEngine* clone() const override { return new ProbeEngine(rendered_, true); }
    bool isNull() override { return false; }
    void paint(QPainter*, const QRect&, QIcon::Mode, QIcon::State) override {}
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal dpr) override {
        ++rendered_->calls;
        if (!copy_) {
            ++rendered_->original_calls;
        }
        rendered_->mode = mode;
        rendered_->state = state;
        rendered_->dpr = dpr;
        rendered_->size = size;
        QPixmap image(size * dpr);
        image.setDevicePixelRatio(dpr);
        image.fill(QPalette{}.color(QPalette::Text));
        return image;
    }

  private:
    std::shared_ptr<Rendered> rendered_;
    bool copy_;
};

class StyleIconCacheTest final : public QObject {
    Q_OBJECT
  private slots:
    void repeatedPaintsReusePixels();
    void paletteChangesDoNotAccumulateInTheSource();
    void modeStateAndScaleRemainDistinct();
    void themeChangesInvalidatePixels();
};

QImage paint(trackknife::bench::TrackknifeStyle& style, QStyleOptionToolButton option,
             qreal dpr = 1.0) {
    QImage image(QSize(32, 32) * dpr, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        style.drawControl(QStyle::CE_ToolButtonLabel, &option, &painter);
    }
    return image;
}

QStyleOptionToolButton button(const std::shared_ptr<Rendered>& rendered) {
    QStyleOptionToolButton option;
    option.rect = QRect(0, 0, 32, 32);
    option.iconSize = QSize(18, 18);
    option.toolButtonStyle = Qt::ToolButtonIconOnly;
    option.state = QStyle::State_Enabled;
    option.icon = QIcon(new ProbeEngine(rendered));
    return option;
}

void StyleIconCacheTest::repeatedPaintsReusePixels() {
#ifndef Q_OS_MACOS
    QSKIP("Workaround for Qt's macOS native icon engine");
#endif
    QPixmapCache::clear();
    trackknife::bench::TrackknifeStyle style;
    auto rendered = std::make_shared<Rendered>();
    const auto option = button(rendered);
    for (int i = 0; i < 100; ++i) {
        paint(style, option);
    }
    QCOMPARE(rendered->calls, 1);
    QCOMPARE(rendered->original_calls, 0);
    QCOMPARE(rendered->live_copies, 0);
    QPixmapCache::clear();
    paint(style, option);
    QCOMPARE(rendered->calls, 2); // eviction is safe: render again
}

void StyleIconCacheTest::paletteChangesDoNotAccumulateInTheSource() {
#ifndef Q_OS_MACOS
    QSKIP("Workaround for Qt's macOS native icon engine");
#endif
    QPixmapCache::clear();
    const auto old_palette = QApplication::palette();
    const auto old_limit = QPixmapCache::cacheLimit();
    QPixmapCache::setCacheLimit(64);
    trackknife::bench::TrackknifeStyle style;
    auto rendered = std::make_shared<Rendered>();
    const auto option = button(rendered);
    for (int i = 0; i < 512; ++i) {
        auto palette = old_palette;
        palette.setColor(QPalette::Text, QColor(i % 256, i / 256, 0));
        QApplication::setPalette(palette);
        paint(style, option);
    }
    QApplication::setPalette(old_palette);
    QPixmapCache::setCacheLimit(old_limit);
    QCOMPARE(rendered->calls, 512);
    QCOMPARE(rendered->original_calls, 0);
    QCOMPARE(rendered->live_copies, 0);
}

void StyleIconCacheTest::modeStateAndScaleRemainDistinct() {
#ifndef Q_OS_MACOS
    QSKIP("Workaround for Qt's macOS native icon engine");
#endif
    QPixmapCache::clear();
    trackknife::bench::TrackknifeStyle style;
    auto rendered = std::make_shared<Rendered>();
    auto option = button(rendered);
    QCOMPARE(paint(style, option).pixelColor(16, 16), QPalette{}.color(QPalette::Text));
    QCOMPARE(rendered->mode, QIcon::Normal);
    option.state = {};
    QCOMPARE(paint(style, option).pixelColor(16, 16), QPalette{}.color(QPalette::Text));
    QCOMPARE(rendered->mode, QIcon::Disabled);
    option.state = QStyle::State_Enabled | QStyle::State_On;
    paint(style, option);
    QCOMPARE(rendered->state, QIcon::On);
    option.state = QStyle::State_Enabled | QStyle::State_MouseOver | QStyle::State_AutoRaise;
    paint(style, option, 2.0);
    QCOMPARE(rendered->mode, QIcon::Active);
    QCOMPARE(rendered->state, QIcon::Off);
    QCOMPARE(rendered->dpr, 2.0);
    QCOMPARE(rendered->calls, 4);
    paint(style, option, 1.0);
    QCOMPARE(rendered->dpr, 1.0);
    QCOMPARE(rendered->calls, 5);
    option.iconSize = QSize(24, 24);
    paint(style, option);
    QCOMPARE(rendered->size, QSize(24, 24));
    option.rect = QRect(0, 0, 12, 14);
    paint(style, option);
    QCOMPARE(rendered->size, QSize(12, 14));
}

void StyleIconCacheTest::themeChangesInvalidatePixels() {
#ifndef Q_OS_MACOS
    QSKIP("Workaround for Qt's macOS native icon engine");
#endif
    QPixmapCache::clear();
    const auto old_theme = QIcon::themeName();
    trackknife::bench::TrackknifeStyle style;
    auto rendered = std::make_shared<Rendered>();
    const auto option = button(rendered);
    paint(style, option);
    QIcon::setThemeName(QStringLiteral("melody-icon-cache-test-theme"));
    paint(style, option);
    QIcon::setThemeName(old_theme);
    QCOMPARE(rendered->calls, 2);
}
} // namespace

QTEST_MAIN(StyleIconCacheTest)
#include "style_icon_cache_test.moc"
