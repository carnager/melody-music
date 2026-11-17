// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/lyrics_look.hpp"
#include "trackknife/metadata/lyrics.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QWidget>

#include <optional>
#include <vector>

class QLabel;
class QListWidget;
class QMenu;
class QToolButton;
class QVariantAnimation;

namespace trackknife::bench {

// ADR-0276: the playing track's lyrics. Synced lyrics highlight the line
// being sung and keep it in the middle; clicking a line asks to seek there.
// Plain lyrics are text to scroll. How it draws is a look, chosen from its
// heading (ADR-0353).
class LyricsPanel final : public QWidget {
    Q_OBJECT

  public:
    explicit LyricsPanel(QWidget* parent = nullptr);

    void showLyrics(const metadata::Lyrics& lyrics);
    // A word in place of lyrics: none, on their way, or why not.
    void showMessage(const QString& message);
    // The engine's position, a moment old, as each refresh has it: carried
    // on here while it plays, so the highlight moves with the music between
    // reports. Small differences from a report are let be; a large one is a
    // seek made elsewhere.
    void setPosition(qint64 position_ms, bool playing);
    // A seek this window asked for: the highlight goes there now, and the
    // reports still on their way from before it are not believed.
    void seekTo(qint64 position_ms);
    // Where the panel takes the music to be now.
    [[nodiscard]] qint64 position() const;

    // The highlighted line; -1, none.
    [[nodiscard]] int currentLine() const noexcept { return current_; }
    [[nodiscard]] bool synced() const noexcept { return synced_; }

    // ADR-0353: the look it draws in, kept in the settings when chosen.
    [[nodiscard]] LyricsLook look() const noexcept { return look_; }
    void setLook(LyricsLook look);
    // What the person changed of the look shown: previewed, drawn and not
    // kept; set, kept too.
    [[nodiscard]] const LyricsLookSettings& lookSettings() const noexcept { return look_settings_; }
    void previewLookSettings(const LyricsLookSettings& settings);
    void setLookSettings(const LyricsLookSettings& settings);
    [[nodiscard]] const LyricsStyle& style() const noexcept { return style_; }
    // The playing album's cover, for a look drawn over it; null, none.
    void setCover(const QImage& cover);
    [[nodiscard]] bool showsCover() const noexcept {
        return style_.cover && !blurred_cover_.isNull();
    }
    // Its look's menu, as the heading's button and a right click open it.
    [[nodiscard]] QMenu* lookMenu() const noexcept { return look_menu_; }

  signals:
    void seekRequested(qint64 position_ms);

  protected:
    void changeEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void highlight(int line);
    // The look made concrete again -- chosen, changed, or a colour scheme
    // changed under it -- and every line drawn in it.
    void restyle();
    // Each line in the font and colour its place gives it.
    void restyleLines();
    void openLookDialog();
    [[nodiscard]] int lineAt(qint64 position_ms) const;

    QLabel* title_{nullptr};
    QLabel* source_{nullptr};
    QToolButton* look_button_{nullptr};
    QMenu* look_menu_{nullptr};
    LyricsLook look_{LyricsLook::page};
    LyricsLookSettings look_settings_;
    LyricsStyle style_;
    QImage cover_;
    // The cover small and soft, drawn large behind the lines.
    QImage blurred_cover_;
    QListWidget* lines_{nullptr};
    std::vector<qint64> times_;
    bool synced_{false};
    bool message_{false};
    int current_{-1};
    void follow(int line);

    // The last report seen, and the clock the highlight runs on: a position
    // at a moment, carried on while playing.
    std::optional<qint64> reported_ms_;
    bool playing_{false};
    qint64 anchor_ms_{0};
    QElapsedTimer anchor_clock_;
    // A seek asked for here, and when: until a report agrees, older ones
    // are not believed.
    std::optional<qint64> seek_ms_;
    QElapsedTimer seek_clock_;
    // Scrolling to the line sung, and a pause in it after the person
    // scrolls by hand.
    QVariantAnimation* scroll_{nullptr};
    QElapsedTimer manual_scroll_;
};

} // namespace trackknife::bench
