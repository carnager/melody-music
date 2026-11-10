// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/engine/job_registry.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>

namespace trackknife::engine {

class Catalogue;
class DynamicRules;
class Player;
class Workspace;

// ADR-0253: the rule a list continues with, as told: one of the engine's
// dynamic playlist rules (ADR-0349), named by its id.
struct ContinuationRule {
    std::string rule_id;
    std::string name;
    std::string query;
    // ADR-0258: a rule that picks groups -- an album, artists -- continues
    // with its selection; one that does not, with batch_size tracks.
    std::string group_by;
    std::size_t groups{0U};
    std::size_t per_group{0U};
    std::size_t limit{100U};
    bool shuffle{false};

    friend bool operator==(const ContinuationRule&, const ContinuationRule&) = default;
};

// Which lists continue, with which rule, and the continuing itself: as a
// track starts that its list ends after, a batch from the rule is appended to
// the queue -- not what is queued already, nor what played in the last 7
// days. A rule changed applies at once; a rule removed ends what continued
// with it.
class ListContinuation final {
  public:
    static constexpr std::size_t batch_size = 10U;
    static constexpr int recent_days = 7;

    ListContinuation(Workspace& workspace, const Catalogue& catalogue, DynamicRules& rules,
                     EventSink sink);
    ~ListContinuation();
    ListContinuation(const ListContinuation&) = delete;
    ListContinuation& operator=(const ListContinuation&) = delete;
    ListContinuation(ListContinuation&&) = delete;
    ListContinuation& operator=(ListContinuation&&) = delete;

    // List identity -> its rule, as it is now.
    [[nodiscard]] std::map<std::string, ContinuationRule> all() const;
    // Sets or (with nothing) ends a list's continuation; kept, and told to
    // every client. A rule the engine does not have, or one it cannot run
    // itself (Last.fm), is refused.
    [[nodiscard]] core::Result<void> set(const std::string& list,
                                         std::optional<std::string> rule_id);

    // Every tick of the playback watcher: continues once per track started.
    // Answers whether it appended.
    bool tick(Player& player);
    // Continues now if the queue's list has a rule and nothing would follow
    // what plays. Answers how many were appended.
    std::size_t continue_if_ending(Player& player);

  private:
    void load();
    void store_locked();
    void announce() const;
    // The rules changed: what continued with one gone ends.
    void rules_changed();
    [[nodiscard]] std::optional<ContinuationRule> rule_of(const std::string& rule_id) const;

    Workspace* workspace_;
    const Catalogue* catalogue_;
    DynamicRules* dynamic_rules_;
    std::size_t observer_{0U};
    EventSink sink_;
    mutable std::mutex mutex_;
    // List identity -> the id of its rule.
    std::map<std::string, std::string> lists_;
    // What was last seen playing, so a track's start is acted on once.
    std::string last_entry_;
    std::uint64_t last_instance_{0};
    std::mt19937 random_{std::random_device{}()};
};

// list.continuations -- every continued list and its rule -- and
// list.continuation.set {list, rule?: id}, which sets or, with no rule, ends
// one.
// Changes are told as the list.continuations event.
void register_list_continuation_methods(protocol::Dispatcher& dispatcher,
                                        ListContinuation& continuation);

} // namespace trackknife::engine
