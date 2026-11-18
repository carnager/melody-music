// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QColor>

namespace trackknife::ui {

// `from` moved `share` of the way to `to` -- 0 is `from`, 1 is `to` -- in
// every channel, alpha too. Melody's colours are mixed from the palette's
// this one way, never written out.
[[nodiscard]] inline QColor mix(const QColor& from, const QColor& to, const qreal share) {
    const auto toward = [share](const float a, const float b) {
        return static_cast<float>(static_cast<qreal>(a) +
                                  ((static_cast<qreal>(b) - static_cast<qreal>(a)) * share));
    };
    return QColor::fromRgbF(toward(from.redF(), to.redF()), toward(from.greenF(), to.greenF()),
                            toward(from.blueF(), to.blueF()), toward(from.alphaF(), to.alphaF()));
}

} // namespace trackknife::ui
