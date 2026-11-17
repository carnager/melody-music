// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QColor>
#include <QFont>
#include <QString>

#include <array>

class QPalette;

namespace trackknife::bench {

// ADR-0353: how the Lyrics panel draws. Each look is Melody's own; what the
// person changes -- a font, a size, colours -- is kept for each look apart.
enum class LyricsLook { page, stage, sleeve, reading };

inline constexpr std::array all_lyrics_looks{LyricsLook::page, LyricsLook::stage,
                                             LyricsLook::sleeve, LyricsLook::reading};

// What the person changed of a look; empty or invalid, the look's own.
struct LyricsLookSettings {
    QString font_family;
    int size_percent{100};
    QColor lines;
    QColor sung;
    QColor background;

    friend bool operator==(const LyricsLookSettings&, const LyricsLookSettings&) = default;
};

[[nodiscard]] QString lyricsLookName(LyricsLook look);
[[nodiscard]] QString lyricsLookKey(LyricsLook look);
[[nodiscard]] LyricsLook lyricsLookFromKey(const QString& key);

// Kept under lyrics/look and lyrics/looks/<look>/... in Melody's settings.
[[nodiscard]] LyricsLook readLyricsLook();
void writeLyricsLook(LyricsLook look);
[[nodiscard]] LyricsLookSettings readLyricsLookSettings(LyricsLook look);
void writeLyricsLookSettings(LyricsLook look, const LyricsLookSettings& settings);

inline constexpr int lyrics_size_min = 50;
inline constexpr int lyrics_size_max = 300;

// A look made concrete under a colour scheme and a font: what the panel
// draws with.
struct LyricsStyle {
    Qt::Alignment alignment{Qt::AlignHCenter};
    QFont line_font;
    QFont sung_font;
    QColor lines;
    QColor sung;
    // Invalid: the list's own ground. Over a cover, the veil laid on it.
    QColor background;
    // The playing album's cover behind the lyrics, blurred.
    bool cover{false};
    int spacing{3};

    // A synced line `distance` lines from the one sung (0: that one).
    [[nodiscard]] QColor lineColor(int distance) const;
    // Lines of plain lyrics, and a word in place of lyrics.
    [[nodiscard]] QColor plainColor() const { return lines; }
    [[nodiscard]] QColor messageColor() const;

    LyricsLook look{LyricsLook::page};
};

[[nodiscard]] LyricsStyle lyricsStyle(LyricsLook look, const LyricsLookSettings& settings,
                                      const QPalette& palette, const QFont& font);

} // namespace trackknife::bench
