// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/dynamic_rules.hpp"

#include "trackknife/core/stable_id.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/message.hpp"
#include "trackknife/query/tkq.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#include <utility>

namespace trackknife::engine {
namespace {

using protocol::Json;

constexpr std::string_view store_key = "dynamic-rules.v1";
constexpr std::array<std::string_view, 5> sources{"rules", "similar", "loved", "top", "tag"};

[[nodiscard]] std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] core::Error invalid(std::string message, std::string key = {}) {
    core::Error error{
        .code = core::ErrorCode::invalid_argument, .message = std::move(message), .context = {}};
    if (!key.empty()) {
        error.context.push_back({.key = "param", .value = std::move(key)});
    }
    return error;
}

[[nodiscard]] std::string first_value(const persistence::TkqRowFacts& facts,
                                      const std::string& field) {
    const auto found = facts.fields.find(field);
    return found == facts.fields.end() || found->second.empty() ? std::string{}
                                                                : found->second.front().first;
}

[[nodiscard]] DynamicRule shipped(const char* id, const char* name, const char* query,
                                  const std::size_t limit, const bool shuffle,
                                  const char* group_by = "", const std::size_t groups = 0U,
                                  const std::size_t per_group = 0U) {
    return DynamicRule{.id = std::string{"shipped:"} + id,
                       .name = name,
                       .source = "rules",
                       .query = query,
                       .artist = {},
                       .track = {},
                       .user = {},
                       .tag = {},
                       .limit = limit,
                       .shuffle = shuffle,
                       .group_by = group_by,
                       .groups = groups,
                       .per_group = per_group};
}

} // namespace

DynamicSelection DynamicRule::selection() const {
    const bool grouped = groups > 0U && !group_by.empty();
    return DynamicSelection{.query = query,
                            .limit = std::clamp<std::size_t>(limit, 1U, dynamic_selection_limit),
                            .shuffle = shuffle,
                            .group_by = grouped ? group_by : std::string{},
                            .groups = grouped ? groups : 0U,
                            .per_group = grouped ? per_group : 0U};
}

std::vector<DynamicRule> shipped_dynamic_rules() {
    return {
        shipped("random-tracks", "Random tracks", "ALL", 50U, true),
        shipped("random-album", "Random album", "ALL", 500U, false,
                "%albumartist% — %album% %date%", 1U, 0U),
        shipped("random-artists", "Random artists", "ALL", 30U, true, "%albumartist%", 10U, 3U),
        shipped("rated-8", "Rated 8 and higher", "rating GREATER 7", 100U, true),
        shipped("unrated", "Unrated", "rating MISSING", 100U, true),
        shipped("recently-added", "Recently added",
                "dayssinceadded LESS 31 SORT BY $num($info(dayssinceadded),6)", 100U, false),
        shipped("not-heard-in-a-year", "Not heard in a year",
                "HISTORY(dayssinceplayed) GREATER 365 OR HISTORY(dayssinceplayed) MISSING", 100U,
                true),
    };
}

core::Result<void> validate_dynamic_rule(const DynamicRule& rule) {
    if (rule.name.empty()) {
        return std::unexpected(invalid("a rule needs a name", "name"));
    }
    if (std::ranges::find(sources, rule.source) == sources.end()) {
        return std::unexpected(invalid("unknown source: " + rule.source, "source"));
    }
    if (rule.limit < 1U || rule.limit > dynamic_selection_limit) {
        return std::unexpected(invalid("limit is 1 to 500", "limit"));
    }
    if (rule.groups > dynamic_selection_limit || rule.per_group > dynamic_selection_limit) {
        return std::unexpected(invalid("groups and per_group are 0 to 500", "groups"));
    }
    if (rule.groups > 0U && rule.group_by.empty()) {
        return std::unexpected(invalid("groups need an expression to group by", "group_by"));
    }
    if (rule.from_library()) {
        if (auto compiled = query::compile_tkq(rule.query); !compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
    }
    return {};
}

Json dynamic_rule_json(const DynamicRule& rule) {
    return Json{{"id", rule.id},         {"name", rule.name},          {"shipped", rule.shipped()},
                {"source", rule.source}, {"query", rule.query},        {"artist", rule.artist},
                {"track", rule.track},   {"user", rule.user},          {"tag", rule.tag},
                {"limit", rule.limit},   {"shuffle", rule.shuffle},    {"group_by", rule.group_by},
                {"groups", rule.groups}, {"per_group", rule.per_group}};
}

DynamicRule dynamic_rule_from_json(const Json& value) {
    if (!value.is_object()) {
        return {};
    }
    const auto text = [&value](const char* key, std::string fallback = {}) {
        const auto found = value.find(key);
        return found != value.end() && found->is_string() ? found->get<std::string>()
                                                          : std::move(fallback);
    };
    // Out of range is kept as such, for validation to refuse.
    const auto count = [&value](const char* key, const std::size_t fallback) {
        const auto found = value.find(key);
        if (found == value.end() || !found->is_number_integer()) {
            return fallback;
        }
        const auto number = found->get<std::int64_t>();
        return number < 0 ? dynamic_selection_limit + 1U : static_cast<std::size_t>(number);
    };
    const auto shuffle = value.find("shuffle");
    return DynamicRule{.id = text("id"),
                       .name = text("name"),
                       .source = text("source", "rules"),
                       .query = text("query"),
                       .artist = text("artist"),
                       .track = text("track"),
                       .user = text("user"),
                       .tag = text("tag"),
                       .limit = count("limit", 100U),
                       .shuffle =
                           shuffle != value.end() && shuffle->is_boolean() && shuffle->get<bool>(),
                       .group_by = text("group_by"),
                       .groups = count("groups", 0U),
                       .per_group = count("per_group", 0U)};
}

core::Result<std::vector<QueueEntry>> queue_entries_of(const Catalogue& catalogue,
                                                       const std::vector<std::string>& paths) {
    auto tracks = catalogue.cached_tracks(paths);
    if (!tracks) {
        return std::unexpected(std::move(tracks.error()));
    }
    std::vector<QueueEntry> entries;
    entries.reserve(tracks->size());
    for (const auto& track : *tracks) {
        QueueEntry entry;
        entry.source.raw_path = track.raw_path;
        if (track.facts.duration_ms >= 0) {
            entry.duration_ms = track.facts.duration_ms;
        }
        entry.title = track.facts.title;
        const auto album_artist = first_value(track.facts, "albumartist");
        entry.group.artist = album_artist.empty() ? track.facts.artist : album_artist;
        entry.group.album = track.facts.album;
        entries.push_back(std::move(entry));
    }
    return entries;
}

DynamicRules::DynamicRules(Workspace& workspace, EventSink sink)
    : workspace_(&workspace), sink_(std::move(sink)) {
    load();
}

void DynamicRules::load() {
    auto stored = workspace_->load_engine_state(store_key);
    if (!stored || !*stored) {
        return;
    }
    const auto document = Json::parse(**stored, nullptr, false);
    const auto rules = document.is_object() ? document.find("rules") : document.end();
    if (rules == document.end() || !rules->is_array()) {
        return;
    }
    for (const auto& value : *rules) {
        auto rule = dynamic_rule_from_json(value);
        if (!rule.id.empty() && !rule.shipped() && validate_dynamic_rule(rule)) {
            saved_.push_back(std::move(rule));
        }
    }
}

void DynamicRules::store_locked() {
    auto rules = Json::array();
    for (const auto& rule : saved_) {
        auto stored = dynamic_rule_json(rule);
        stored.erase("shipped");
        rules.push_back(std::move(stored));
    }
    const Json document{{"version", 1}, {"rules", std::move(rules)}};
    static_cast<void>(workspace_->save_engine_state(store_key, document.dump(), now_ms()));
}

void DynamicRules::changed() {
    std::vector<std::function<void()>> observers;
    {
        const std::lock_guard guard{mutex_};
        for (const auto& [number, observer] : observers_) {
            observers.push_back(observer);
        }
    }
    if (sink_) {
        auto rules = Json::array();
        for (const auto& rule : all()) {
            rules.push_back(dynamic_rule_json(rule));
        }
        sink_(protocol::Event{.name = "dynamic.rules", .data = Json{{"rules", std::move(rules)}}});
    }
    for (const auto& observer : observers) {
        observer();
    }
}

std::vector<DynamicRule> DynamicRules::all() const {
    auto rules = shipped_dynamic_rules();
    const std::lock_guard guard{mutex_};
    rules.insert(rules.end(), saved_.begin(), saved_.end());
    return rules;
}

std::optional<DynamicRule> DynamicRules::find(const std::string& id) const {
    if (id.starts_with("shipped:")) {
        for (auto& rule : shipped_dynamic_rules()) {
            if (rule.id == id) {
                return std::move(rule);
            }
        }
        return std::nullopt;
    }
    const std::lock_guard guard{mutex_};
    const auto found = std::ranges::find(saved_, id, &DynamicRule::id);
    return found == saved_.end() ? std::nullopt : std::optional{*found};
}

core::Result<DynamicRule> DynamicRules::save(DynamicRule rule) {
    if (rule.shipped()) {
        return std::unexpected(invalid("a rule that ships is not changed; save a copy", "id"));
    }
    if (auto valid = validate_dynamic_rule(rule); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    {
        const std::lock_guard guard{mutex_};
        if (rule.id.empty()) {
            rule.id = core::StableId::random().to_string();
        }
        const auto found = std::ranges::find(saved_, rule.id, &DynamicRule::id);
        if (found == saved_.end()) {
            saved_.push_back(rule);
        } else if (*found == rule) {
            return rule;
        } else {
            *found = rule;
        }
        store_locked();
    }
    changed();
    return rule;
}

core::Result<void> DynamicRules::remove(const std::string& id) {
    if (id.starts_with("shipped:")) {
        return std::unexpected(invalid("a rule that ships is not removed", "id"));
    }
    {
        const std::lock_guard guard{mutex_};
        const auto removed =
            std::erase_if(saved_, [&id](const auto& rule) { return rule.id == id; });
        if (removed == 0U) {
            return std::unexpected(core::Error{.code = core::ErrorCode::not_found,
                                               .message = "there is no such rule",
                                               .context = {{.key = "id", .value = id}}});
        }
        store_locked();
    }
    changed();
    return {};
}

std::size_t DynamicRules::adopt(std::vector<DynamicRule> rules) {
    std::size_t adopted = 0U;
    {
        const std::lock_guard guard{mutex_};
        for (auto& rule : rules) {
            if (rule.id.empty() || rule.shipped() || !validate_dynamic_rule(rule) ||
                std::ranges::find(saved_, rule.id, &DynamicRule::id) != saved_.end()) {
                continue;
            }
            saved_.push_back(std::move(rule));
            ++adopted;
        }
        if (adopted > 0U) {
            store_locked();
        }
    }
    if (adopted > 0U) {
        changed();
    }
    return adopted;
}

std::size_t DynamicRules::on_change(std::function<void()> observer) {
    const std::lock_guard guard{mutex_};
    const auto number = next_observer_++;
    observers_.emplace(number, std::move(observer));
    return number;
}

void DynamicRules::forget(const std::size_t observer) {
    const std::lock_guard guard{mutex_};
    observers_.erase(observer);
}

void register_dynamic_rule_methods(protocol::Dispatcher& dispatcher, DynamicRules& rules,
                                   const Catalogue& catalogue, Player& player) {
    dispatcher.on("dynamic.rules", [&rules](const Json&) -> core::Result<Json> {
        auto all = Json::array();
        for (const auto& rule : rules.all()) {
            all.push_back(dynamic_rule_json(rule));
        }
        return Json{{"rules", std::move(all)}};
    });
    dispatcher.on("dynamic.save", [&rules](const Json& params) -> core::Result<Json> {
        const auto given = params.find("rule");
        if (given == params.end() || !given->is_object()) {
            return std::unexpected(invalid("a rule is required", "rule"));
        }
        auto saved = rules.save(dynamic_rule_from_json(*given));
        if (!saved) {
            return std::unexpected(std::move(saved.error()));
        }
        return Json{{"rule", dynamic_rule_json(*saved)}};
    });
    dispatcher.on("dynamic.delete", [&rules](const Json& params) -> core::Result<Json> {
        const auto id = params.value("id", std::string{});
        if (auto removed = rules.remove(id); !removed) {
            return std::unexpected(std::move(removed.error()));
        }
        return Json{{"id", id}};
    });
    dispatcher.on("dynamic.adopt", [&rules](const Json& params) -> core::Result<Json> {
        const auto given = params.find("rules");
        if (given == params.end() || !given->is_array()) {
            return std::unexpected(invalid("a list of rules is required", "rules"));
        }
        std::vector<DynamicRule> adopted;
        adopted.reserve(given->size());
        for (const auto& value : *given) {
            adopted.push_back(dynamic_rule_from_json(value));
        }
        return Json{{"adopted", rules.adopt(std::move(adopted))}};
    });
    dispatcher.on(
        "dynamic.play", [&rules, &catalogue, &player](const Json& params) -> core::Result<Json> {
            const auto id = params.value("id", std::string{});
            const auto rule = rules.find(id);
            if (!rule) {
                return std::unexpected(core::Error{.code = core::ErrorCode::not_found,
                                                   .message = "there is no such rule",
                                                   .context = {{.key = "id", .value = id}}});
            }
            if (!rule->from_library()) {
                return std::unexpected(
                    invalid("a Last.fm rule is played from Melody, not by the engine", "id"));
            }
            std::mt19937 random{std::random_device{}()};
            auto selected = select_dynamic(catalogue, rule->selection(), {}, random);
            if (!selected) {
                return std::unexpected(std::move(selected.error()));
            }
            if (selected->paths.empty()) {
                return std::unexpected(core::Error{.code = core::ErrorCode::not_found,
                                                   .message = "the rule finds nothing",
                                                   .context = {{.key = "id", .value = id}}});
            }
            auto entries = queue_entries_of(catalogue, selected->paths);
            if (!entries) {
                return std::unexpected(std::move(entries.error()));
            }
            if (entries->empty()) {
                return std::unexpected(core::Error{.code = core::ErrorCode::not_found,
                                                   .message = "the rule finds nothing",
                                                   .context = {{.key = "id", .value = id}}});
            }
            const auto start = entries->front().entry_id;
            const auto count = entries->size();
            player.replace_queue(std::move(*entries));
            if (auto played = player.play_entry(start); !played) {
                return std::unexpected(std::move(played.error()));
            }
            return Json{{"playing", start.to_string()}, {"tracks", count}};
        });
}

} // namespace trackknife::engine
