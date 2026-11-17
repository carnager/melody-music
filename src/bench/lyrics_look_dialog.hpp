// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/lyrics_look.hpp"

#include <QDialog>

class QFontComboBox;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace trackknife::bench {

class LyricsPanel;

// ADR-0353: the font and colours of the look the Lyrics panel shows. Each
// change is drawn in the panel at once; OK keeps them, Cancel goes back.
class LyricsLookDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit LyricsLookDialog(LyricsPanel* panel);

    [[nodiscard]] const LyricsLookSettings& settings() const noexcept { return settings_; }
    // Changed as the dialog's own controls change it, for tests.
    void change(const LyricsLookSettings& settings);

    void accept() override;
    void reject() override;

  private:
    struct ColourRow {
        QPushButton* pick{nullptr};
        QToolButton* reset{nullptr};
    };
    [[nodiscard]] ColourRow colourRow(const QString& name, QColor LyricsLookSettings::* member,
                                      bool alpha);
    void pickColour(QColor LyricsLookSettings::* member, bool alpha);
    // The controls as the settings stand, without telling anyone.
    void showSettings();

    LyricsPanel* panel_;
    LyricsLookSettings original_;
    LyricsLookSettings settings_;
    QFontComboBox* font_{nullptr};
    QToolButton* font_reset_{nullptr};
    QSpinBox* size_{nullptr};
    ColourRow lines_;
    ColourRow sung_;
    ColourRow background_;
};

} // namespace trackknife::bench
