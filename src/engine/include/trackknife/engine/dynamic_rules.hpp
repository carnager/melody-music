// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/engine/dynamic_selection.hpp"
#include "trackknife/engine/job_methods.hpp"
#include "trackknife/engine/job_registry.hpp"
#include "trackknife/engine/lastfm.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::engine {

class Catalogue;
class Workspace;

// ADR-0349: a dynamic playlist's rule -- a tkq-1 query and how to pick from
// what it matches (ADR-0258), or a Last.fm source the window fetches.
struct DynamicRule {
    std::string id;
    std::string name;
    // "rules", or a Last.fm source: "similar", "loved", "top", "tag".
    std::string source{"rules"};
    std::string query;
    // For the Last.fm sources: the seed track, the user, the tag.
    std::string artist;
    std::string track;
    std::string user;
    std::string tag;
    std::size_t limit{100U};
    bool shuffle{false};
    std::string group_by;
    std::size_t groups{0U};
    std::size_t per_group{0U};

    [[nodiscard]] bool shipped() const { return id.starts_with("shipped:"); }
    // Run by the engine itself, as a tkq-1 rule is; not a Last.fm source.
    [[nodiscard]] bool from_library() const { return source == "rules"; }
    [[nodiscard]] DynamicSelection selection() const;

    friend bool operator==(const DynamicRule&, const DynamicRule&) = default;
};

// The rules that ship (ADR-0258), always there, in this order.
[[nodiscard]] std::vector<DynamicRule> shipped_dynamic_rules();

// Why a rule cannot be kept, or nothing: a name, a known source, counts in
// range, groups with an expression to group by, a query that compiles.
[[nodiscard]] core::Result<void> validate_dynamic_rule(const DynamicRule& rule);

[[nodiscard]] protocol::Json dynamic_rule_json(const DynamicRule& rule);
[[nodiscard]] DynamicRule dynamic_rule_from_json(const protocol::Json& value);

// The queue entries for files of the library, named from it, in order.
[[nodiscard]] core::Result<std::vector<QueueEntry>>
queue_entries_of(const Catalogue& catalogue, const std::vector<std::string>& paths);

// The engine's dynamic playlist rules: those that ship, then those saved,
// kept in the engine's store and told to every client as they change.
class DynamicRules final {
  public:
    DynamicRules(Workspace& workspace, EventSink sink);
    DynamicRules(const DynamicRules&) = delete;
    DynamicRules& operator=(const DynamicRules&) = delete;

    // The shipped rules, then those saved, in the order saved.
    [[nodiscard]] std::vector<DynamicRule> all() const;
    [[nodiscard]] std::optional<DynamicRule> find(const std::string& id) const;
    // Kept: a new rule (no id) is named here, one with an id replaces it.
    // A shipped rule is not changed.
    [[nodiscard]] core::Result<DynamicRule> save(DynamicRule rule);
    [[nodiscard]] core::Result<void> remove(const std::string& id);
    // Keeps each valid rule whose id is not known yet; answers how many.
    std::size_t adopt(std::vector<DynamicRule> rules);

    // Called, without any lock held, after the rules changed, until
    // forgotten by the number this answers.
    std::size_t on_change(std::function<void()> observer);
    void forget(std::size_t observer);

  private:
    void load();
    void store_locked();
    void changed();

    Workspace* workspace_;
    EventSink sink_;
    mutable std::mutex mutex_;
    std::vector<DynamicRule> saved_;
    std::map<std::size_t, std::function<void()>> observers_;
    std::size_t next_observer_{0U};
};

// ADR-0350: what a rule picks from this engine's library. A tkq-1 rule, its
// selection (ADR-0258). A Last.fm source, the tracks it suggests that the
// library has -- each the one file of its artist and title -- chosen as
// the window chose them: those not picked the last time first, then in
// Last.fm's order or shuffled. What was picked is kept, per source, in the
// engine's store.
class DynamicPicker final {
  public:
    // Asks Last.fm: a method, its parameters, and how many at most.
    using Suggest = std::function<core::Result<std::vector<LastFmSuggestion>>(
        const std::string& method, const std::vector<std::pair<std::string, std::string>>& params,
        std::size_t limit)>;
    // While matching: how many suggestions are looked up, of how many.
    using Progress = std::function<void(std::size_t done, std::size_t of)>;
    struct Picked {
        std::vector<std::string> paths;
        // How many tracks there were to choose from.
        std::size_t matched{0U};
        // Suggestions the library does not have.
        std::size_t unmatched{0U};
    };

    DynamicPicker(Workspace& workspace, const Catalogue& catalogue, Suggest suggest);
    DynamicPicker(const DynamicPicker&) = delete;
    DynamicPicker& operator=(const DynamicPicker&) = delete;

    [[nodiscard]] core::Result<Picked> pick(const DynamicRule& rule,
                                            const core::CancellationToken& cancellation = {},
                                            const Progress& progress = {});

  private:
    [[nodiscard]] core::Result<Picked> suggested(const DynamicRule& rule,
                                                 const core::CancellationToken& cancellation,
                                                 const Progress& progress);

    Workspace* workspace_;
    const Catalogue* catalogue_;
    Suggest suggest_;
    std::mutex mutex_;
    std::mt19937 random_{std::random_device{}()};
};

// dynamic.rules, dynamic.save {rule}, dynamic.delete {id}, dynamic.adopt
// {rules}, and dynamic.play {id}: the queue replaced with what the rule
// picks, playing -- answered at once for a tkq-1 rule; a Last.fm rule's
// waits on Last.fm, so it is a job, answered with its job_id. Changes are
// told as dynamic.rules.
void register_dynamic_rule_methods(protocol::Dispatcher& dispatcher, DynamicRules& rules,
                                   DynamicPicker& picker, const Catalogue& catalogue,
                                   Player& player, JobRegistry& jobs);

// The dynamic.pick job {rule}: what a rule -- saved or not -- picks, as
// {paths, matched, unmatched}, with progress {done, of} while matching.
void register_dynamic_rule_jobs(JobCatalog& jobs, DynamicPicker& picker);

} // namespace trackknife::engine
