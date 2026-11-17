// SPDX-License-Identifier: GPL-3.0-only

#include "bench/lyrics_panel.hpp"

#include "bench/lyrics_look_dialog.hpp"

#include <QActionGroup>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cstdlib>

namespace trackknife::bench {
namespace {

[[nodiscard]] QString source_text(const metadata::Lyrics& lyrics) {
    const auto kind = lyrics.synced ? QStringLiteral("Synced") : QStringLiteral("Plain");
    if (lyrics.source == "sidecar") {
        return kind + QStringLiteral(" · .lrc file");
    }
    if (lyrics.source == "sylt") {
        return kind + QStringLiteral(" · ID3 SYLT");
    }
    if (lyrics.source.starts_with("plugin:")) {
        return kind +
               QStringLiteral(" · from %1").arg(QString::fromStdString(lyrics.source.substr(7)));
    }
    if (lyrics.source.starts_with("tag:")) {
        return kind +
               QStringLiteral(" · %1 tag").arg(QString::fromStdString(lyrics.source.substr(4)));
    }
    return kind;
}

// The lines kept off the panel's edges, whatever a look's alignment: the
// margin is the list's own ground, as the lines are.
class LyricsLines final : public QListWidget {
  public:
    explicit LyricsLines(QWidget* parent) : QListWidget(parent) {
        setBackgroundRole(QPalette::Base);
        setAutoFillBackground(true);
        setViewportMargins(12, 0, 12, 0);
    }
};

// The cover small and soft: a few passes of a box blur over a thumbnail,
// drawn large it is a wash of the sleeve's colours.
[[nodiscard]] QImage blurred(const QImage& cover) {
    if (cover.isNull()) {
        return {};
    }
    auto image = cover.scaled(48, 48, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                     .convertToFormat(QImage::Format_ARGB32);
    const int width = image.width();
    const int height = image.height();
    constexpr int radius = 3;
    auto pass = [&](const bool across) {
        const QImage source = image.copy();
        const int length = across ? width : height;
        const int lines = across ? height : width;
        for (int line = 0; line < lines; ++line) {
            for (int at = 0; at < length; ++at) {
                int r = 0;
                int g = 0;
                int b = 0;
                int n = 0;
                for (int k = std::max(0, at - radius); k <= std::min(length - 1, at + radius);
                     ++k) {
                    const QRgb pixel = across ? source.pixel(k, line) : source.pixel(line, k);
                    r += qRed(pixel);
                    g += qGreen(pixel);
                    b += qBlue(pixel);
                    ++n;
                }
                const auto value = qRgb(r / n, g / n, b / n);
                if (across) {
                    image.setPixel(at, line, value);
                } else {
                    image.setPixel(line, at, value);
                }
            }
        }
    };
    for (int round = 0; round < 3; ++round) {
        pass(true);
        pass(false);
    }
    return image;
}

} // namespace

LyricsPanel::LyricsPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(12, 10, 6, 8);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(1);
    title_ = new QLabel(QStringLiteral("Lyrics"), this);
    auto font = title_->font();
    font.setWeight(QFont::DemiBold);
    font.setPointSizeF(font.pointSizeF() * 1.08);
    title_->setFont(font);
    titles->addWidget(title_);
    source_ = new QLabel(this);
    source_->setObjectName(QStringLiteral("lyrics-source"));
    source_->setForegroundRole(QPalette::PlaceholderText);
    titles->addWidget(source_);
    heading->addLayout(titles, 1);

    // ADR-0353: the looks, and the font and colours of the one chosen.
    look_ = readLyricsLook();
    look_settings_ = readLyricsLookSettings(look_);
    look_menu_ = new QMenu(this);
    look_menu_->setObjectName(QStringLiteral("lyrics-look-menu"));
    auto* looks = new QActionGroup(look_menu_);
    for (const auto look : all_lyrics_looks) {
        auto* action = look_menu_->addAction(lyricsLookName(look));
        action->setObjectName(QStringLiteral("lyrics-look-") + lyricsLookKey(look));
        action->setCheckable(true);
        action->setChecked(look == look_);
        looks->addAction(action);
        connect(action, &QAction::triggered, this, [this, look] { setLook(look); });
    }
    look_menu_->addSeparator();
    auto* adjust = look_menu_->addAction(QStringLiteral("Font and colours\u2026"));
    adjust->setObjectName(QStringLiteral("lyrics-look-adjust"));
    connect(adjust, &QAction::triggered, this, &LyricsPanel::openLookDialog);
    look_button_ = new QToolButton(this);
    look_button_->setObjectName(QStringLiteral("lyrics-look"));
    look_button_->setAutoRaise(true);
    look_button_->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
    if (look_button_->icon().isNull()) {
        look_button_->setText(QStringLiteral("Look"));
    }
    look_button_->setToolTip(QStringLiteral("Look"));
    look_button_->setAccessibleName(QStringLiteral("Lyrics look"));
    look_button_->setPopupMode(QToolButton::InstantPopup);
    look_button_->setMenu(look_menu_);
    heading->addWidget(look_button_, 0, Qt::AlignTop);
    layout->addLayout(heading);

    lines_ = new LyricsLines(this);
    lines_->setObjectName(QStringLiteral("lyrics-lines"));
    lines_->setFrameShape(QFrame::NoFrame);
    lines_->setSelectionMode(QAbstractItemView::NoSelection);
    lines_->setFocusPolicy(Qt::NoFocus);
    lines_->setWordWrap(true);
    lines_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    lines_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    lines_->setSpacing(3);
    connect(lines_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const auto row = lines_->row(item);
        if (synced_ && row >= 0 && row < static_cast<int>(times_.size())) {
            emit seekRequested(times_[static_cast<std::size_t>(row)]);
        }
    });
    layout->addWidget(lines_, 1);
    scroll_ = new QVariantAnimation(this);
    scroll_->setDuration(450);
    scroll_->setEasingCurve(QEasingCurve::InOutCubic);
    connect(scroll_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        lines_->verticalScrollBar()->setValue(value.toInt());
    });
    lines_->viewport()->installEventFilter(this);
    showMessage(QStringLiteral("Nothing is playing"));
    restyle();
}

void LyricsPanel::showLyrics(const metadata::Lyrics& lyrics) {
    scroll_->stop();
    lines_->clear();
    times_.clear();
    current_ = -1;
    synced_ = lyrics.synced;
    message_ = false;
    source_->setText(source_text(lyrics));
    for (const auto& line : lyrics.lines) {
        // An empty synced line is a pause in the singing: kept, so the last
        // line sung stops being highlighted.
        new QListWidgetItem(QString::fromStdString(line.text), lines_);
        times_.push_back(line.time_ms.value_or(0));
    }
    restyleLines();
    lines_->scrollToTop();
    lines_->setCursor(synced_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (synced_ && reported_ms_) {
        setPosition(*reported_ms_, playing_);
    }
}

void LyricsPanel::showMessage(const QString& message) {
    scroll_->stop();
    lines_->clear();
    times_.clear();
    current_ = -1;
    synced_ = false;
    message_ = true;
    source_->clear();
    auto* item = new QListWidgetItem(message, lines_);
    item->setTextAlignment(Qt::AlignCenter);
    item->setForeground(style_.messageColor());
    lines_->setCursor(Qt::ArrowCursor);
}

void LyricsPanel::setPosition(const qint64 position_ms, const bool playing) {
    // Reports come twice a second and a moment late; the clock between them
    // is this one. A report moves it only when it is far off -- a seek made
    // elsewhere, a stall -- or when playing starts or stops.
    constexpr qint64 tolerance_ms = 400;
    const bool fresh = reported_ms_ != position_ms || playing_ != playing;
    const bool started_or_stopped = playing_ != playing;
    reported_ms_ = position_ms;
    if (fresh) {
        const auto predicted = position();
        playing_ = playing;
        if (seek_ms_) {
            // Until a report agrees with the seek, or a moment has passed,
            // the reports are from before it.
            const auto expected = *seek_ms_ + (playing ? seek_clock_.elapsed() : 0);
            if (std::llabs(position_ms - expected) <= 1'500) {
                seek_ms_.reset();
            } else if (seek_clock_.elapsed() < 2'000) {
                if (synced_) {
                    highlight(lineAt(position()));
                }
                return;
            } else {
                seek_ms_.reset();
            }
        }
        if (started_or_stopped || !anchor_clock_.isValid() ||
            std::llabs(position_ms - predicted) > tolerance_ms) {
            anchor_ms_ = position_ms;
            anchor_clock_.start();
        }
    }
    if (synced_) {
        highlight(lineAt(position()));
    }
}

void LyricsPanel::seekTo(const qint64 position_ms) {
    seek_ms_ = position_ms;
    seek_clock_.start();
    anchor_ms_ = position_ms;
    anchor_clock_.start();
    // Asked for here: the person wants to see it, not what they scrolled to.
    manual_scroll_.invalidate();
    if (synced_) {
        highlight(lineAt(position()));
    }
}

qint64 LyricsPanel::position() const {
    if (!anchor_clock_.isValid()) {
        return reported_ms_.value_or(0);
    }
    return anchor_ms_ + (playing_ ? anchor_clock_.elapsed() : 0);
}

int LyricsPanel::lineAt(const qint64 position_ms) const {
    // The last line begun by now.
    const auto after = std::upper_bound(times_.begin(), times_.end(), position_ms);
    return static_cast<int>(after - times_.begin()) - 1;
}

void LyricsPanel::highlight(const int line) {
    if (line == current_) {
        return;
    }
    current_ = line;
    restyleLines();
    follow(current_ < 0 ? 0 : current_);
}

void LyricsPanel::follow(const int line) {
    // Scrolled by hand a moment ago: the person is reading elsewhere.
    if (manual_scroll_.isValid() && manual_scroll_.elapsed() < 4'000) {
        return;
    }
    auto* item = lines_->item(line);
    if (item == nullptr) {
        return;
    }
    auto* bar = lines_->verticalScrollBar();
    const auto rect = lines_->visualItemRect(item);
    const auto target =
        std::clamp(bar->value() + rect.center().y() - lines_->viewport()->height() / 2,
                   bar->minimum(), bar->maximum());
    scroll_->stop();
    if (!isVisible() || std::abs(target - bar->value()) > lines_->viewport()->height() * 2 ||
        !QSettings{}.value(QStringLiteral("appearance/panel-animations"), true).toBool()) {
        // Far away -- a seek -- or not seen: there at once.
        bar->setValue(target);
        return;
    }
    scroll_->setStartValue(bar->value());
    scroll_->setEndValue(target);
    scroll_->start();
}

bool LyricsPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == lines_->viewport() && event->type() == QEvent::Wheel) {
        scroll_->stop();
        manual_scroll_.start();
    }
    return QWidget::eventFilter(watched, event);
}

void LyricsPanel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange) {
        restyle();
    }
}

void LyricsPanel::contextMenuEvent(QContextMenuEvent* event) {
    look_menu_->popup(event->globalPos());
    event->accept();
}

void LyricsPanel::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    if (!style_.cover && !style_.background.isValid()) {
        return;
    }
    QPainter painter{this};
    if (style_.cover && !blurred_cover_.isNull()) {
        // The sleeve fills the panel, cropped to it, never stretched.
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QSizeF size = QSizeF{blurred_cover_.size()}.scaled(QSizeF{rect().size()},
                                                                 Qt::KeepAspectRatioByExpanding);
        const QRectF target{QPointF{(width() - size.width()) / 2, (height() - size.height()) / 2},
                            size};
        painter.drawImage(target, blurred_cover_);
        painter.fillRect(rect(), style_.background);
        return;
    }
    auto ground = style_.background;
    if (style_.cover) {
        // No cover: the veil alone, opaque.
        ground.setAlpha(255);
    }
    painter.fillRect(rect(), ground);
}

void LyricsPanel::setLook(const LyricsLook look) {
    look_ = look;
    writeLyricsLook(look);
    look_settings_ = readLyricsLookSettings(look);
    for (auto* action : look_menu_->actions()) {
        if (action->objectName() == QStringLiteral("lyrics-look-") + lyricsLookKey(look)) {
            action->setChecked(true);
        }
    }
    restyle();
}

void LyricsPanel::previewLookSettings(const LyricsLookSettings& settings) {
    look_settings_ = settings;
    restyle();
}

void LyricsPanel::setLookSettings(const LyricsLookSettings& settings) {
    writeLyricsLookSettings(look_, settings);
    previewLookSettings(settings);
}

void LyricsPanel::setCover(const QImage& cover) {
    if (cover.cacheKey() == cover_.cacheKey() && cover.isNull() == cover_.isNull()) {
        return;
    }
    cover_ = cover;
    blurred_cover_ = style_.cover ? blurred(cover_) : QImage{};
    update();
}

void LyricsPanel::openLookDialog() {
    auto* dialog = new LyricsLookDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void LyricsPanel::restyle() {
    style_ = lyricsStyle(look_, look_settings_, palette(), font());
    if (style_.cover && blurred_cover_.isNull()) {
        blurred_cover_ = blurred(cover_);
    }
    // Drawn on the panel's own ground when the look has one: the list lets
    // it through.
    const bool own_ground = style_.cover || style_.background.isValid();
    auto list_palette = palette();
    if (own_ground) {
        list_palette.setColor(QPalette::Base, Qt::transparent);
    }
    lines_->setPalette(list_palette);
    lines_->viewport()->setAutoFillBackground(!own_ground);
    lines_->setAutoFillBackground(!own_ground);
    lines_->setSpacing(style_.spacing);
    auto heading = title_->palette();
    heading.setColor(QPalette::WindowText,
                     own_ground ? style_.sung : palette().color(QPalette::WindowText));
    title_->setPalette(heading);
    auto source = source_->palette();
    source.setColor(QPalette::WindowText, own_ground ? style_.messageColor()
                                                     : palette().color(QPalette::PlaceholderText));
    source_->setPalette(source);
    source_->setForegroundRole(QPalette::WindowText);
    restyleLines();
    update();
}

void LyricsPanel::restyleLines() {
    if (message_) {
        if (auto* item = lines_->item(0)) {
            item->setForeground(style_.messageColor());
            item->setFont(style_.line_font);
        }
        lines_->doItemsLayout();
        return;
    }
    for (int row = 0; row < lines_->count(); ++row) {
        auto* item = lines_->item(row);
        const bool sung = synced_ && row == current_;
        item->setTextAlignment(style_.alignment | Qt::AlignVCenter);
        item->setFont(sung ? style_.sung_font : style_.line_font);
        if (!synced_) {
            item->setForeground(style_.plainColor());
        } else if (current_ < 0) {
            // Not begun: every line waits alike.
            item->setForeground(style_.lineColor(1));
        } else {
            item->setForeground(style_.lineColor(row - current_));
        }
    }
    lines_->doItemsLayout();
}

} // namespace trackknife::bench
