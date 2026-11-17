// SPDX-License-Identifier: GPL-3.0-only

#include "bench/lyrics_look.hpp"

#include <QPalette>
#include <QSettings>

#include <algorithm>

namespace trackknife::bench {
namespace {

[[nodiscard]] QString group(const LyricsLook look) {
    return QStringLiteral("lyrics/looks/") + lyricsLookKey(look) + QLatin1Char('/');
}

[[nodiscard]] QColor read_color(const QSettings& settings, const QString& key) {
    const auto text = settings.value(key).toString();
    const QColor color = QColor::fromString(text);
    return text.isEmpty() || !color.isValid() ? QColor{} : color;
}

void write_color(QSettings& settings, const QString& key, const QColor& color) {
    if (color.isValid()) {
        settings.setValue(key, color.name(QColor::HexArgb));
    } else {
        settings.remove(key);
    }
}

[[nodiscard]] QFont scaled(QFont font, const double factor) {
    font.setPointSizeF(font.pointSizeF() * factor);
    return font;
}

} // namespace

QString lyricsLookName(const LyricsLook look) {
    switch (look) {
    case LyricsLook::page:
        return QStringLiteral("Page");
    case LyricsLook::stage:
        return QStringLiteral("Stage");
    case LyricsLook::sleeve:
        return QStringLiteral("Sleeve");
    case LyricsLook::reading:
        return QStringLiteral("Reading");
    }
    return {};
}

QString lyricsLookKey(const LyricsLook look) { return lyricsLookName(look).toLower(); }

LyricsLook lyricsLookFromKey(const QString& key) {
    for (const auto look : all_lyrics_looks) {
        if (lyricsLookKey(look) == key) {
            return look;
        }
    }
    return LyricsLook::page;
}

LyricsLook readLyricsLook() {
    return lyricsLookFromKey(QSettings{}.value(QStringLiteral("lyrics/look")).toString());
}

void writeLyricsLook(const LyricsLook look) {
    QSettings{}.setValue(QStringLiteral("lyrics/look"), lyricsLookKey(look));
}

LyricsLookSettings readLyricsLookSettings(const LyricsLook look) {
    const QSettings settings;
    const auto at = group(look);
    LyricsLookSettings read;
    read.font_family = settings.value(at + QStringLiteral("font")).toString();
    read.size_percent = std::clamp(settings.value(at + QStringLiteral("size"), 100).toInt(),
                                   lyrics_size_min, lyrics_size_max);
    read.lines = read_color(settings, at + QStringLiteral("lines"));
    read.sung = read_color(settings, at + QStringLiteral("sung"));
    read.background = read_color(settings, at + QStringLiteral("background"));
    return read;
}

void writeLyricsLookSettings(const LyricsLook look, const LyricsLookSettings& written) {
    QSettings settings;
    const auto at = group(look);
    if (written.font_family.isEmpty()) {
        settings.remove(at + QStringLiteral("font"));
    } else {
        settings.setValue(at + QStringLiteral("font"), written.font_family);
    }
    if (written.size_percent == 100) {
        settings.remove(at + QStringLiteral("size"));
    } else {
        settings.setValue(at + QStringLiteral("size"),
                          std::clamp(written.size_percent, lyrics_size_min, lyrics_size_max));
    }
    write_color(settings, at + QStringLiteral("lines"), written.lines);
    write_color(settings, at + QStringLiteral("sung"), written.sung);
    write_color(settings, at + QStringLiteral("background"), written.background);
}

QColor LyricsStyle::lineColor(const int distance) const {
    if (distance == 0) {
        return sung;
    }
    const int away = std::abs(distance);
    double fade = 1.0;
    switch (look) {
    case LyricsLook::page:
        fade = 0.5;
        break;
    case LyricsLook::stage:
        // Lines fall away from the one sung, a few kept in sight.
        fade = std::max(0.14, 0.62 - 0.12 * (away - 1));
        break;
    case LyricsLook::sleeve:
        fade = std::max(0.36, 0.6 - 0.08 * (away - 1));
        break;
    case LyricsLook::reading:
        fade = 1.0;
        break;
    }
    auto color = lines;
    color.setAlphaF(static_cast<float>(static_cast<double>(color.alphaF()) * fade));
    return color;
}

QColor LyricsStyle::messageColor() const {
    auto color = lines;
    color.setAlphaF(color.alphaF() * 0.55F);
    return color;
}

LyricsStyle lyricsStyle(const LyricsLook look, const LyricsLookSettings& settings,
                        const QPalette& palette, const QFont& font) {
    LyricsStyle style;
    style.look = look;
    auto base = font;
    if (!settings.font_family.isEmpty()) {
        base.setFamily(settings.font_family);
    }
    base =
        scaled(base, std::clamp(settings.size_percent, lyrics_size_min, lyrics_size_max) / 100.0);
    const auto text = palette.color(QPalette::Text);
    switch (look) {
    case LyricsLook::page:
        // As it always was: centred, the line sung in bold.
        style.alignment = Qt::AlignHCenter;
        style.line_font = base;
        style.sung_font = base;
        style.sung_font.setBold(true);
        style.lines = text;
        style.sung = text;
        style.spacing = 3;
        break;
    case LyricsLook::stage:
        // Large, the line sung a little larger still, the rest receding.
        style.alignment = Qt::AlignHCenter;
        style.line_font = scaled(base, 1.3);
        style.sung_font = scaled(base, 1.45);
        style.sung_font.setWeight(QFont::DemiBold);
        style.lines = text;
        style.sung = text;
        style.spacing = 9;
        break;
    case LyricsLook::sleeve:
        // The album's cover, blurred and veiled, the words light on it.
        style.alignment = Qt::AlignHCenter;
        style.line_font = scaled(base, 1.12);
        style.sung_font = style.line_font;
        style.sung_font.setWeight(QFont::DemiBold);
        style.lines = QColor{0xf4, 0xef, 0xe6};
        style.sung = QColor{0xff, 0xfb, 0xf2};
        style.background = QColor{0x14, 0x12, 0x10, 0xa8};
        style.cover = true;
        style.spacing = 6;
        break;
    case LyricsLook::reading:
        // A page to read: left, close, the line sung only in the accent.
        style.alignment = Qt::AlignLeft;
        style.line_font = base;
        style.sung_font = base;
        style.lines = text;
        style.sung = palette.color(QPalette::Highlight);
        style.spacing = 2;
        break;
    }
    if (settings.lines.isValid()) {
        style.lines = settings.lines;
    }
    if (settings.sung.isValid()) {
        style.sung = settings.sung;
    }
    if (settings.background.isValid()) {
        style.background = settings.background;
        if (style.cover) {
            // Over the cover, a chosen colour is a veil, never a wall.
            style.background.setAlpha(std::min(style.background.alpha(), 0xa8));
        }
    }
    return style;
}

} // namespace trackknife::bench
