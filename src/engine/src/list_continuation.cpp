// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/list_continuation.hpp"

#include "trackknife/core/stable_id.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/dynamic_rules.hpp"
#include "trackknife/engine/dynamic_selection.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/message.hpp"
#include "trackknife/query/tkq.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <utility>
#include <vector>

namespace trackknife::engine {
namespace {

using protocol::Json;

constexpr std::string_view store_key = "list-continuation.v1";

[[nodiscard]] std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The rule, without what played in the last `days` days: a track never
// played has no age, and a missing age matches no comparison -- hence both.
// The rule's own query when it cannot be narrowed so.
[[nodiscard]] std::string without_recent(const std::string& query, const int days) {
    auto narrowed = query::narrow_tkq_source(
        query, "HISTORY(dayssinceplayed) MISSING OR HISTORY(dayssinceplayed) GREATER " +
                   std::to_string(days - 1));
    if (narrowed) {
        return std::move(*narrowed);
    }
    return query;
}

// A rule as it is told: {rule, name, query, ...}.
[[nodiscard]] Json rule_json(const ContinuationRule& rule) {
    return Json{{"rule", rule.rule_id},  {"name", rule.name},
                {"query", rule.query},   {"group_by", rule.group_by},
                {"groups", rule.groups}, {"per_group", rule.per_group},
                {"limit", rule.limit},   {"shuffle", rule.shuffle}};
}

[[nodiscard]] ContinuationRule continuation_of(const DynamicRule& rule) {
    const auto selection = rule.selection();
    return ContinuationRule{.rule_id = rule.id,
                            .name = rule.name,
                            .query = rule.query,
                            .group_by = selection.group_by,
                            .groups = selection.groups,
                            .per_group = selection.per_group,
                            .limit = selection.limit,
                            .shuffle = rule.shuffle};
}

// A copy kept before ADR-0349 (list-continuation.v1), as a rule.
[[nodiscard]] DynamicRule copied_rule(const Json& value) {
    auto rule = dynamic_rule_from_json(value);
    rule.id = value.value("rule", std::string{});
    rule.source = "rules";
    return rule;
}

} // namespace

ListContinuation::ListContinuation(Workspace& workspace, const Catalogue& catalogue,
                                   DynamicRules& rules, EventSink sink)
    : workspace_(&workspace), catalogue_(&catalogue), dynamic_rules_(&rules),
      sink_(std::move(sink)) {
    load();
    observer_ = dynamic_rules_->on_change([this] { rules_changed(); });
}

ListContinuation::~ListContinuation() { dynamic_rules_->forget(observer_); }

void ListContinuation::load() {
    auto stored = workspace_->load_engine_state(store_key);
    if (!stored || !*stored) {
        return;
    }
    const auto document = Json::parse(**stored, nullptr, false);
    const auto lists = document.is_object() ? document.find("lists") : document.end();
    if (lists == document.end() || !lists->is_object()) {
        return;
    }
    std::vector<DynamicRule> copies;
    for (const auto& [list, value] : lists->items()) {
        if (!core::StableId::parse(list)) {
            continue;
        }
        if (value.is_string()) {
            lists_.emplace(list, value.get<std::string>());
        } else if (value.is_object()) {
            // ADR-0349: a copy kept by an engine before rules were its own
            // becomes one of its rules, unless it has that rule already.
            auto rule = copied_rule(value);
            if (rule.id.empty()) {
                continue;
            }
            lists_.emplace(list, rule.id);
            copies.push_back(std::move(rule));
        }
    }
    if (!copies.empty()) {
        static_cast<void>(dynamic_rules_->adopt(std::move(copies)));
        std::erase_if(lists_, [this](const auto& entry) { return !rule_of(entry.second); });
        store_locked();
    }
}

void ListContinuation::store_locked() {
    Json lists = Json::object();
    for (const auto& [list, rule_id] : lists_) {
        lists[list] = rule_id;
    }
    const Json document{{"version", 2}, {"lists", std::move(lists)}};
    static_cast<void>(workspace_->save_engine_state(store_key, document.dump(), now_ms()));
}

std::optional<ContinuationRule> ListContinuation::rule_of(const std::string& rule_id) const {
    auto rule = dynamic_rules_->find(rule_id);
    if (!rule || !rule->from_library()) {
        return std::nullopt;
    }
    return continuation_of(*rule);
}

void ListContinuation::announce() const {
    if (!sink_) {
        return;
    }
    auto lists = Json::array();
    for (const auto& [list, rule] : all()) {
        auto told = rule_json(rule);
        told["list"] = list;
        lists.push_back(std::move(told));
    }
    sink_(protocol::Event{.name = "list.continuations",
                          .data = Json{{"continuations", std::move(lists)}}});
}

void ListContinuation::rules_changed() {
    {
        const std::lock_guard guard{mutex_};
        if (lists_.empty()) {
            return;
        }
        if (std::erase_if(lists_, [this](const auto& entry) { return !rule_of(entry.second); }) >
            0U) {
            store_locked();
        }
    }
    // Told even with nothing ended: a rule's name or query may have changed.
    announce();
}

std::map<std::string, ContinuationRule> ListContinuation::all() const {
    std::map<std::string, std::string> lists;
    {
        const std::lock_guard guard{mutex_};
        lists = lists_;
    }
    std::map<std::string, ContinuationRule> rules;
    for (const auto& [list, rule_id] : lists) {
        if (auto rule = rule_of(rule_id)) {
            rules.emplace(list, std::move(*rule));
        }
    }
    return rules;
}

core::Result<void> ListContinuation::set(const std::string& list,
                                         std::optional<std::string> rule_id) {
    if (!core::StableId::parse(list)) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "a list is named by its identity",
                                           .context = {{.key = "list", .value = list}}});
    }
    if (rule_id && !rule_of(*rule_id)) {
        return std::unexpected(core::Error{.code = core::ErrorCode::not_found,
                                           .message = "there is no such rule this engine can run",
                                           .context = {{.key = "rule", .value = *rule_id}}});
    }
    {
        const std::lock_guard guard{mutex_};
        if (rule_id) {
            lists_.insert_or_assign(list, std::move(*rule_id));
        } else {
            lists_.erase(list);
        }
        // Looked at again at the next tick: a list whose last track already
        // plays continues as soon as it is told to.
        last_entry_.clear();
        store_locked();
    }
    announce();
    return {};
}

bool ListContinuation::tick(Player& player) {
    const auto state = player.state();
    const auto key = state.entry.to_string() + "/" + std::to_string(state.queue_revision);
    {
        const std::lock_guard guard{mutex_};
        if (state.entry.is_nil() || (key == last_entry_ && state.instance == last_instance_)) {
            return false;
        }
        last_entry_ = key;
        last_instance_ = state.instance;
    }
    return continue_if_ending(player) > 0U;
}

std::size_t ListContinuation::continue_if_ending(Player& player) {
    const auto list = player.queue_list();
    std::string rule_id;
    {
        const std::lock_guard guard{mutex_};
        const auto found = lists_.find(list);
        if (list.empty() || found == lists_.end()) {
            return 0U;
        }
        rule_id = found->second;
    }
    const auto found_rule = rule_of(rule_id);
    if (!found_rule) {
        return 0U;
    }
    const auto& rule = *found_rule;
    if (!player.ends_after_current()) {
        return 0U;
    }
    std::set<std::string> queued;
    for (const auto& entry : player.queue()) {
        queued.insert(entry.source.raw_path);
    }
    // Not what is queued; preferably not what played lately either, but
    // rather that than nothing, should the rule be narrow. A rule that picks
    // groups continues with its selection -- the next album, whole -- and one
    // that does not with batch_size tracks at random (ADR-0258).
    const bool grouped = rule.groups > 0U && !rule.group_by.empty();
    std::vector<std::string> candidates;
    for (const auto& source : {without_recent(rule.query, recent_days), rule.query}) {
        const DynamicSelection selection{.query = source,
                                         .limit = grouped ? rule.limit : batch_size,
                                         .shuffle = grouped ? rule.shuffle : true,
                                         .group_by = grouped ? rule.group_by : std::string{},
                                         .groups = grouped ? rule.groups : 0U,
                                         .per_group = grouped ? rule.per_group : 0U};
        core::Result<DynamicSelected> selected = std::unexpected(core::Error{});
        {
            std::mt19937 random;
            {
                const std::lock_guard guard{mutex_};
                random.seed(random_());
            }
            selected = select_dynamic(*catalogue_, selection, queued, random);
        }
        if (!selected) {
            std::cerr << "melodyd: continuing " << list << " (" << rule.name
                      << ") failed: " << selected.error().message << "\n";
            return 0U;
        }
        if (!selected->paths.empty()) {
            candidates = std::move(selected->paths);
            break;
        }
    }
    if (candidates.empty()) {
        return 0U;
    }
    auto batch = queue_entries_of(*catalogue_, candidates);
    if (!batch) {
        std::cerr << "melodyd: continuing " << list << " failed: " << batch.error().message << "\n";
        return 0U;
    }
    const auto appended = batch->size();
    player.append_to_queue(std::move(*batch));
    return appended;
}

void register_list_continuation_methods(protocol::Dispatcher& dispatcher,
                                        ListContinuation& continuation) {
    dispatcher.on("list.continuations", [&continuation](const Json&) -> core::Result<Json> {
        auto lists = Json::array();
        for (const auto& [list, rule] : continuation.all()) {
            auto told = rule_json(rule);
            told["list"] = list;
            lists.push_back(std::move(told));
        }
        return Json{{"continuations", std::move(lists)}};
    });
    dispatcher.on("list.continuation.set",
                  [&continuation](const Json& params) -> core::Result<Json> {
                      const auto list = params.value("list", std::string{});
                      std::optional<std::string> rule;
                      if (const auto given = params.find("rule");
                          given != params.end() && given->is_string()) {
                          rule = given->get<std::string>();
                      }
                      if (auto set = continuation.set(list, std::move(rule)); !set) {
                          return std::unexpected(std::move(set.error()));
                      }
                      return Json{{"list", list}};
                  });
}

} // namespace trackknife::engine
