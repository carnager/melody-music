// SPDX-License-Identifier: GPL-3.0-only

// ADR-0253: a list continues with a dynamic playlist's rule. ADR-0349: the
// rules are the engine's; the window chooses one by its id, and hands each
// engine the rules it kept itself before, once.

#include "workspace/workspace.hpp"

#include "bench/dynamic_playlist_service.hpp"
#include "workspace/workspace_view.hpp"

#include <QSettings>

namespace trackknife::bench {
namespace {

// A list can continue only with a tkq-1 rule, which the engine runs itself:
// a Last.fm source would stop with the window.
[[nodiscard]] bool continuable(const DynamicPlaylistDefinition& definition) {
    return definition.source == QStringLiteral("rules") && !definition.id.isEmpty() &&
           !definition.query.trimmed().isEmpty();
}

[[nodiscard]] QString handedKey(const EngineKey& engine) {
    return QStringLiteral("dynamic-playlists/handed-v1/") + engine.text();
}

} // namespace

std::vector<Workspace::ContinuationChoice>
Workspace::continuationChoices(const ListTab& tab) const {
    std::vector<ContinuationChoice> choices;
    const auto* playback = playbackOf(EngineKey::of(tab.document));
    if (playback == nullptr) {
        return choices;
    }
    for (const auto& definition : playback->dynamicRules()) {
        if (continuable(definition)) {
            choices.push_back({.rule_id = definition.id,
                               .name = definition.name,
                               .shipped = definition.shipped()});
        }
    }
    return choices;
}

std::optional<EnginePlayback::Continuation> Workspace::continuationOf(const ListTab& tab) const {
    const auto* playback = playbackOf(EngineKey::of(tab.document));
    if (playback == nullptr) {
        return std::nullopt;
    }
    const auto found = playback->continuations().constFind(document_text(tab.document.id));
    if (found == playback->continuations().constEnd()) {
        return std::nullopt;
    }
    return *found;
}

void Workspace::setContinuation(const ListTab& tab, const QString& rule_id) {
    auto* playback = playbackOf(EngineKey::of(tab.document));
    if (playback == nullptr || !playback->active()) {
        view_->showMessage(tr("The list's engine is not connected"), 5'000);
        return;
    }
    if (rule_id.isEmpty()) {
        playback->setContinuation(document_text(tab.document.id), std::nullopt);
        return;
    }
    const auto& rules = playback->dynamicRules();
    if (std::ranges::none_of(rules, [&rule_id](const auto& definition) {
            return definition.id == rule_id && continuable(definition);
        })) {
        view_->showMessage(tr("That dynamic playlist is gone"), 5'000);
        return;
    }
    playback->setContinuation(document_text(tab.document.id), rule_id);
}

void Workspace::followContinuations(EngineLink& engine) {
    connect(engine.playback, &EnginePlayback::continuationsChanged, this, [this, link = &engine] {
        for (const auto& tab : list_tabs_) {
            if (EngineKey::of(tab->document) == link->key) {
                view_->refreshTabChrome(*tab);
            }
        }
    });
    connect(engine.playback, &EnginePlayback::dynamicRulesChanged, this,
            [this, link = &engine] { handOldDynamicRules(*link); });
}

void Workspace::handOldDynamicRules(EngineLink& engine) {
    // ADR-0349: the rules the window kept before go to each engine once, so
    // every engine starts with the same set. The settings stay, for an
    // engine reached later.
    if (engine.playback == nullptr || !engine.playback->dynamicRulesKnown() ||
        QSettings{}.value(handedKey(engine.key)).toBool() ||
        handing_rules_.contains(engine.key.text())) {
        return;
    }
    const auto old = loadDynamicPlaylists(QStringLiteral("local"));
    if (!old || old->isEmpty()) {
        QSettings{}.setValue(handedKey(engine.key), true);
        return;
    }
    handing_rules_.insert(engine.key.text());
    engine.playback->handDynamicRules(*old,
                                      [this, key = engine.key](const core::Result<int>& adopted) {
                                          handing_rules_.remove(key.text());
                                          if (adopted) {
                                              QSettings{}.setValue(handedKey(key), true);
                                          }
                                      });
}

} // namespace trackknife::bench
