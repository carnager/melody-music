// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/protocol/message.hpp"

#include <QString>

namespace trackknife::bench {

// A dynamic playlist's rule as the window shows and edits it. ADR-0349: the
// rules are the engine's; this is one as an engine told it.
struct DynamicPlaylistDefinition {
    QString id{};
    QString name{};
    // Before ADR-0349, which settings the window kept it under.
    QString profile{};
    QString source{QStringLiteral("rules")};
    QString query{QStringLiteral("rating GREATER 6")};
    QString artist{};
    QString track{};
    QString user{};
    QString tag{};
    int limit{100};
    bool shuffle{false};
    // ADR-0258: rules may pick `groups` groups of their matches by this
    // tkfmt-1 expression, `per_group` tracks from each (0: all).
    QString group_by{};
    int groups{0};
    int per_group{0};

    // ADR-0258: shipped with Melody, used and copied but not changed.
    [[nodiscard]] bool shipped() const { return id.startsWith(QStringLiteral("shipped:")); }
};

// ADR-0349: a rule as the engine's dynamic.* methods tell and take it.
[[nodiscard]] DynamicPlaylistDefinition dynamicRuleOf(const protocol::Json& value);
[[nodiscard]] protocol::Json dynamicRuleJson(const DynamicPlaylistDefinition& definition);

} // namespace trackknife::bench
