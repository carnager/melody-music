// SPDX-License-Identifier: GPL-3.0-only

#include "bench/lyrics_look_dialog.hpp"

#include "bench/lyrics_panel.hpp"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace trackknife::bench {
namespace {

[[nodiscard]] QIcon swatch(const QColor& color, const QPalette& palette) {
    QPixmap pixmap{16, 16};
    pixmap.fill(Qt::transparent);
    QPainter painter{&pixmap};
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(palette.color(QPalette::Mid));
    painter.setBrush(color.isValid() ? color : palette.color(QPalette::Base));
    painter.drawRoundedRect(QRectF{0.5, 0.5, 15, 15}, 3, 3);
    return QIcon{pixmap};
}

[[nodiscard]] QToolButton* reset_button(QWidget* parent, const QString& name) {
    auto* button = new QToolButton(parent);
    button->setObjectName(name);
    button->setText(QStringLiteral("Look’s own"));
    button->setToolTip(QStringLiteral("As the look draws it"));
    button->setAutoRaise(true);
    return button;
}

} // namespace

LyricsLookDialog::LyricsLookDialog(LyricsPanel* panel)
    : QDialog(panel), panel_(panel), original_(panel->lookSettings()),
      settings_(panel->lookSettings()) {
    setObjectName(QStringLiteral("lyrics-look-dialog"));
    setWindowTitle(QStringLiteral("%1 — Font and colours").arg(lyricsLookName(panel->look())));
    auto* layout = new QVBoxLayout(this);
    auto* note = new QLabel(
        QStringLiteral("For the %1 look; the others keep their own. The panel shows each change.")
            .arg(lyricsLookName(panel->look())),
        this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(note);
    auto* form = new QFormLayout;
    layout->addLayout(form);

    auto* font_row = new QHBoxLayout;
    font_ = new QFontComboBox(this);
    font_->setObjectName(QStringLiteral("lyrics-look-font"));
    font_reset_ = reset_button(this, QStringLiteral("lyrics-look-font-reset"));
    font_row->addWidget(font_, 1);
    font_row->addWidget(font_reset_);
    form->addRow(QStringLiteral("Font"), font_row);
    connect(font_, &QFontComboBox::currentFontChanged, this, [this](const QFont& font) {
        auto changed = settings_;
        changed.font_family = font.family();
        change(changed);
    });
    connect(font_reset_, &QToolButton::clicked, this, [this] {
        auto changed = settings_;
        changed.font_family.clear();
        change(changed);
    });

    size_ = new QSpinBox(this);
    size_->setObjectName(QStringLiteral("lyrics-look-size"));
    size_->setRange(lyrics_size_min, lyrics_size_max);
    size_->setSingleStep(10);
    size_->setSuffix(QStringLiteral(" %"));
    form->addRow(QStringLiteral("Size"), size_);
    connect(size_, &QSpinBox::valueChanged, this, [this](const int value) {
        auto changed = settings_;
        changed.size_percent = value;
        change(changed);
    });

    lines_ = colourRow(QStringLiteral("lines"), &LyricsLookSettings::lines, false);
    const auto add_colour_row = [form](const QString& label, const ColourRow& row) {
        auto* line = new QHBoxLayout;
        line->addWidget(row.pick, 1);
        line->addWidget(row.reset);
        form->addRow(label, line);
    };
    add_colour_row(QStringLiteral("Lines"), lines_);
    sung_ = colourRow(QStringLiteral("sung"), &LyricsLookSettings::sung, false);
    add_colour_row(QStringLiteral("Line sung"), sung_);
    background_ = colourRow(QStringLiteral("background"), &LyricsLookSettings::background, true);
    add_colour_row(QStringLiteral("Background"), background_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    buttons->button(QDialogButtonBox::RestoreDefaults)
        ->setText(QStringLiteral("All the look’s own"));
    connect(buttons, &QDialogButtonBox::accepted, this, &LyricsLookDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &LyricsLookDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
            [this] { change({}); });
    layout->addWidget(buttons);
    showSettings();
}

LyricsLookDialog::ColourRow LyricsLookDialog::colourRow(const QString& name,
                                                        QColor LyricsLookSettings::* member,
                                                        const bool alpha) {
    ColourRow row;
    row.pick = new QPushButton(this);
    row.pick->setObjectName(QStringLiteral("lyrics-look-") + name);
    row.reset =
        reset_button(this, QStringLiteral("lyrics-look-") + name + QStringLiteral("-reset"));
    connect(row.pick, &QPushButton::clicked, this,
            [this, member, alpha] { pickColour(member, alpha); });
    connect(row.reset, &QToolButton::clicked, this, [this, member] {
        auto changed = settings_;
        changed.*member = QColor{};
        change(changed);
    });
    return row;
}

void LyricsLookDialog::pickColour(QColor LyricsLookSettings::* member, const bool alpha) {
    // Each colour passed over is drawn in the panel; Cancel there goes back
    // to the one before.
    const auto before = settings_;
    const auto& style = panel_->style();
    QColor shown = settings_.*member;
    if (!shown.isValid()) {
        shown = member == &LyricsLookSettings::lines  ? style.lines
                : member == &LyricsLookSettings::sung ? style.sung
                                                      : style.background;
    }
    if (!shown.isValid()) {
        shown = palette().color(QPalette::Base);
    }
    auto* picker = new QColorDialog(shown, this);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    picker->setOption(QColorDialog::ShowAlphaChannel, alpha);
    connect(picker, &QColorDialog::currentColorChanged, this, [this, member](const QColor& color) {
        auto changed = settings_;
        changed.*member = color;
        change(changed);
    });
    connect(picker, &QColorDialog::colorSelected, this, [this, member](const QColor& color) {
        auto changed = settings_;
        changed.*member = color;
        change(changed);
    });
    connect(picker, &QDialog::rejected, this, [this, before] { change(before); });
    picker->open();
}

void LyricsLookDialog::change(const LyricsLookSettings& settings) {
    settings_ = settings;
    panel_->previewLookSettings(settings_);
    showSettings();
}

void LyricsLookDialog::showSettings() {
    const QSignalBlocker font_blocker{font_};
    const QSignalBlocker size_blocker{size_};
    const auto& style = panel_->style();
    font_->setCurrentFont(style.line_font);
    font_reset_->setEnabled(!settings_.font_family.isEmpty());
    size_->setValue(settings_.size_percent);
    const auto show = [this](const ColourRow& row, const QColor& own, const QColor& shown) {
        row.pick->setIcon(swatch(shown, palette()));
        row.pick->setText(own.isValid()
                              ? own.name(own.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb)
                              : QStringLiteral("The look’s"));
        row.reset->setEnabled(own.isValid());
    };
    show(lines_, settings_.lines, style.lines);
    show(sung_, settings_.sung, style.sung);
    show(background_, settings_.background, style.background);
}

void LyricsLookDialog::accept() {
    panel_->setLookSettings(settings_);
    QDialog::accept();
}

void LyricsLookDialog::reject() {
    panel_->previewLookSettings(original_);
    QDialog::reject();
}

} // namespace trackknife::bench
