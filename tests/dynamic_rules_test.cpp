// SPDX-License-Identifier: GPL-3.0-only

// ADR-0349: the dynamic playlist rules are the engine's -- listed, saved,
// edited, removed and handed over through the protocol, kept across a
// restart, and played by the engine itself.
// ADR-0350: a Last.fm rule too -- what Last.fm suggests, matched in the
// library, those not picked the last time first.
// ADR-0351: the queue says which rule's selection it is, until it is
// replaced, edited or the rule removed.

#include "recording_audition.hpp"

#include "trackknife/core/stable_id.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/dynamic_rules.hpp"
#include "trackknife/engine/job_methods.hpp"
#include "trackknife/engine/job_registry.hpp"
#include "trackknife/engine/playback_methods.hpp"
#include "trackknife/engine/playback_store.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/dispatch.hpp"
#include "trackknife/protocol/message.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace core = trackknife::core;
namespace engine = trackknife::engine;
namespace protocol = trackknife::protocol;
using protocol::Json;
using trackknife::testing::RecordingAudition;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

[[nodiscard]] std::filesystem::path materialize(const std::filesystem::path& fixtures,
                                                const std::string& name,
                                                const std::filesystem::path& target) {
    std::ifstream input{fixtures / (name + ".b64")};
    std::string base64((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::erase(base64, '\n');
    const auto decoded = protocol::decode_raw_path(base64);
    require(decoded.has_value(), "a fixture decodes");
    std::ofstream output{target, std::ios::binary};
    output.write(decoded->data(), static_cast<std::streamsize>(decoded->size()));
    return target;
}

[[nodiscard]] protocol::Response call(const protocol::Dispatcher& dispatcher, std::string method,
                                      Json params = Json::object()) {
    return dispatcher.dispatch(
        protocol::Request{.id = 1, .method = std::move(method), .params = std::move(params)});
}

[[nodiscard]] Json answer(const protocol::Dispatcher& dispatcher, std::string method,
                          Json params = Json::object()) {
    const auto response = call(dispatcher, method, std::move(params));
    if (!response.result) {
        std::cerr << method << ": " << (response.error ? response.error->message : "") << '\n';
    }
    require(response.result.has_value(), "the call is answered");
    return *response.result;
}

} // namespace

int main(const int argc, char** argv) {
    require(argc == 2, "the fixtures folder is given");
    const std::filesystem::path fixtures{argv[1]};
    const auto directory = std::filesystem::temp_directory_path() /
                           ("melody-dynamic-rules-" + core::StableId::random().to_string());
    const auto music = directory / "music";
    std::filesystem::create_directories(music);
    for (const auto& [name, extension] :
         {std::pair{"tagged-tone-flac", ".flac"}, std::pair{"tagged-tone-mp3", ".mp3"},
          std::pair{"loudness-tone-opus", ".opus"}, std::pair{"rf64-tone-wav", ".wav"}}) {
        static_cast<void>(materialize(fixtures, name, music / (std::string{name} + extension)));
    }
    const auto database = directory / "engine.sqlite3";
    engine::LocalCatalogue catalogue{database};
    require(catalogue.prepare().has_value(), "a library");
    require(catalogue.add_root(music.string()).has_value(), "its folder is added");
    trackknife::persistence::LibraryScanProgress progress;
    require(catalogue.scan({}, progress).has_value(), "scanned");
    auto workspace = engine::Workspace::open(database);
    require(workspace.has_value(), "the engine's store");
    auto player = engine::Player::create_without_audio();
    RecordingAudition audition;
    require(player->set_output(&audition).has_value(), "the player plays on the test's output");

    std::vector<Json> told;
    const auto sink = [&told](const protocol::Event& event) {
        if (event.name == "dynamic.rules") {
            told.push_back(event.data);
        }
    };
    const auto shipped_count = engine::shipped_dynamic_rules().size();

    // What Last.fm suggests: the library's three songs -- the FLAC and the
    // MP3 are one, by their tags -- as Last.fm spells them (in capitals),
    // and one the library does not have; the first twice.
    std::vector<engine::LastFmSuggestion> suggested;
    std::set<std::string> library_paths;
    {
        const auto all = catalogue.select(engine::DynamicSelection{.query = "ALL",
                                                                   .limit = 500U,
                                                                   .shuffle = false,
                                                                   .group_by = {},
                                                                   .groups = 0U,
                                                                   .per_group = 0U});
        require(all && all->paths.size() == 4U, "four files");
        const auto tracks = catalogue.cached_tracks(all->paths);
        require(tracks.has_value(), "their tags");
        for (const auto& track : *tracks) {
            require(!track.facts.artist.empty() && !track.facts.title.empty(), "tagged");
            auto upper = [](std::string text) {
                for (auto& character : text) {
                    if (character >= 'a' && character <= 'z') {
                        character = static_cast<char>(character - 'a' + 'A');
                    }
                }
                return text;
            };
            const engine::LastFmSuggestion song{upper(track.facts.artist),
                                                upper(track.facts.title)};
            if (std::ranges::find(suggested, song) == suggested.end()) {
                suggested.push_back(song);
            }
            library_paths.insert(track.raw_path);
        }
        require(suggested.size() == 3U, "three songs");
        suggested.insert(suggested.begin() + 1, {"Nobody", "Not here"});
        suggested.push_back(suggested.front());
    }
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> asked;
    const engine::DynamicPicker::Suggest suggest =
        [&](const std::string& method,
            const std::vector<std::pair<std::string, std::string>>& params,
            const std::size_t limit) -> core::Result<std::vector<engine::LastFmSuggestion>> {
        require(limit == 500U, "as many as a rule can pick");
        asked.emplace_back(method, params);
        return suggested;
    };

    std::string saved_id;
    {
        engine::DynamicRules rules{*workspace, sink};
        engine::DynamicPicker picker{*workspace, catalogue, suggest};
        engine::JobRegistry jobs{[](const protocol::Event&) {}};
        protocol::Dispatcher dispatcher;
        engine::register_dynamic_rule_methods(dispatcher, rules, picker, catalogue, *player, jobs);

        // The shipped rules, first, marked, and fixed.
        auto listed = answer(dispatcher, "dynamic.rules").at("rules");
        require(listed.size() == shipped_count, "only the shipped rules at first");
        require(listed.front().at("id") == "shipped:random-tracks" &&
                    listed.front().at("shipped") == true,
                "first the shipped ones, marked");
        for (const auto& rule : engine::shipped_dynamic_rules()) {
            require(engine::validate_dynamic_rule(rule).has_value(), "a shipped rule is valid");
        }
        auto changed_shipped = listed.at(1);
        changed_shipped["per_group"] = 2;
        require(!call(dispatcher, "dynamic.save", Json{{"rule", changed_shipped}}).result,
                "a shipped rule is not changed");
        require(!call(dispatcher, "dynamic.delete", Json{{"id", "shipped:random-album"}}).result,
                "nor removed");

        // A new rule is named by the engine.
        const Json rock{
            {"name", "Everything"}, {"query", "codec PRESENT"}, {"limit", 2}, {"shuffle", true}};
        const auto saved = answer(dispatcher, "dynamic.save", Json{{"rule", rock}}).at("rule");
        saved_id = saved.at("id").get<std::string>();
        require(!saved_id.empty() && saved.at("shipped") == false, "a new rule gets an id");
        require(!told.empty() && told.back().at("rules").size() == shipped_count + 1U,
                "and every client is told");

        // What is not a rule is refused.
        for (const auto& bad : {Json{{"name", ""}, {"query", "ALL"}},
                                Json{{"name", "Broken"}, {"query", "codec ((("}},
                                Json{{"name", "None"}, {"query", "ALL"}, {"limit", 0}},
                                Json{{"name", "Big"}, {"query", "ALL"}, {"limit", 501}},
                                Json{{"name", "Groups"}, {"query", "ALL"}, {"groups", 2}},
                                Json{{"name", "Where"}, {"source", "spotify"}}}) {
            require(!call(dispatcher, "dynamic.save", Json{{"rule", bad}}).result,
                    "an invalid rule is refused");
        }

        // Edited by its id.
        auto edited = saved;
        edited["name"] = "All of it";
        require(answer(dispatcher, "dynamic.save", Json{{"rule", edited}}).at("rule").at("name") ==
                    "All of it",
                "a rule is edited");
        require(answer(dispatcher, "dynamic.rules").at("rules").size() == shipped_count + 1U,
                "in place");

        // A Last.fm rule is kept, and played by the engine, as a job: Last.fm
        // takes its time.
        const Json loved{
            {"id", "loved"}, {"name", "Loved"}, {"source", "loved"}, {"user", "someone"}};
        require(answer(dispatcher, "dynamic.save", Json{{"rule", loved}}).contains("rule"),
                "a Last.fm rule is kept");
        const auto job = answer(dispatcher, "dynamic.play", Json{{"id", "loved"}});
        require(job.contains("job_id"), "a Last.fm rule plays as a job");
        jobs.wait_all();
        require(!asked.empty() && asked.back().first == "user.getLovedTracks" &&
                    asked.back().second ==
                        std::vector<std::pair<std::string, std::string>>{{"user", "someone"}},
                "the user's loved tracks are asked for");
        require(player->queue().size() == 3U, "what the library has of them is queued");
        require(player->queue_list().empty(), "of no list");
        require(player->state().queue_rule == "loved", "the rule's selection, once it is queued");
        Json no_user = loved;
        no_user["id"] = "no-user";
        no_user["user"] = "";
        require(answer(dispatcher, "dynamic.save", Json{{"rule", no_user}}).contains("rule"),
                "a rule still being written is kept");
        require(!call(dispatcher, "dynamic.play", Json{{"id", "no-user"}}).result,
                "but not played without a user");
        require(answer(dispatcher, "dynamic.delete", Json{{"id", "no-user"}}).contains("id"),
                "and removed");

        // Handed over: only what the engine does not know.
        const auto adopted = answer(
            dispatcher, "dynamic.adopt",
            Json{{"rules",
                  Json::array({Json{{"id", "loved"}, {"name", "Other"}, {"source", "loved"}},
                               Json{{"id", "new"}, {"name", "New"}, {"query", "ALL"}},
                               Json{{"id", "shipped:unrated"}, {"name", "Mine"}, {"query", "ALL"}},
                               Json{{"id", "broken"}, {"name", "Broken"}, {"query", "((("}}})}});
        require(adopted.at("adopted") == 1, "one new rule taken");
        require(rules.find("loved")->name == "Loved", "a known rule is left as it is");
        require(rules.find("shipped:unrated")->name == "Unrated", "a shipped one too");
        require(!rules.find("broken"), "an invalid one is not taken");
        require(answer(dispatcher, "dynamic.delete", Json{{"id", "new"}}).at("id") == "new",
                "a rule is removed");
        require(!call(dispatcher, "dynamic.delete", Json{{"id", "new"}}).result, "and is gone");

        // Played: the queue is the rule's selection, playing.
        const auto played = answer(dispatcher, "dynamic.play", Json{{"id", saved_id}});
        require(played.at("tracks") == 2, "the rule's limit");
        require(player->queue().size() == 2U, "the queue is the selection");
        require(player->queue_list().empty(), "of no list");
        require(player->state().entry.to_string() == played.at("playing").get<std::string>(),
                "and it plays from its first track");
        require(!player->queue().front().title.empty(), "named from the library");
        require(answer(dispatcher, "dynamic.play", Json{{"id", "shipped:random-album"}})
                        .at("tracks")
                        .get<int>() >= 1,
                "a shipped rule plays too");
        auto nothing = saved;
        nothing["id"] = "nothing";
        nothing["query"] = "title HAS zzqqxxnothingatall";
        require(answer(dispatcher, "dynamic.save", Json{{"rule", nothing}}).contains("rule"),
                "a rule that finds nothing");
        require(!call(dispatcher, "dynamic.play", Json{{"id", "nothing"}}).result, "plays nothing");
        require(!call(dispatcher, "dynamic.play", Json{{"id", "missing"}}).result,
                "nor does a rule there is not");

        // ADR-0351: which rule's selection the queue is.
        engine::register_playback_methods(dispatcher, *player, &catalogue);
        engine::follow_queue_rule(rules, *player);
        const auto origin = [&dispatcher] {
            return answer(dispatcher, "playback.state").at("dynamic_rule");
        };
        require(answer(dispatcher, "dynamic.play", Json{{"id", saved_id}}).at("tracks") == 2,
                "a rule played");
        require(origin() == saved_id, "the state names its rule");
        require(answer(dispatcher, "playback.queue").at("dynamic_rule") == saved_id,
                "and so does the queue");
        // Playback keeps it: pausing, resuming, seeking, moving on.
        static_cast<void>(answer(dispatcher, "playback.pause"));
        static_cast<void>(answer(dispatcher, "playback.resume"));
        static_cast<void>(answer(dispatcher, "playback.seek", Json{{"position_ms", 100}}));
        static_cast<void>(answer(dispatcher, "playback.next"));
        require(origin() == saved_id, "playing on keeps it");
        // So does renaming the rule: the id is what is matched.
        auto renamed = engine::dynamic_rule_json(*rules.find(saved_id));
        renamed["name"] = "Renamed";
        static_cast<void>(answer(dispatcher, "dynamic.save", Json{{"rule", renamed}}));
        require(origin() == saved_id, "renaming keeps it");
        renamed["name"] = "All of it";
        static_cast<void>(answer(dispatcher, "dynamic.save", Json{{"rule", renamed}}));
        // A rule that fails leaves the queue and its rule as they were.
        require(!call(dispatcher, "dynamic.play", Json{{"id", "nothing"}}).result, "fails");
        require(origin() == saved_id, "and the queue is still the earlier rule's");
        // Kept across a restart, with the queue.
        require(player->persisted().queue_rule == saved_id, "remembered");
        // Edited, it is not the rule's selection any more.
        const auto first = player->queue().front().entry_id;
        require(player
                    ->edit_queue(
                        {},
                        [&first](std::vector<engine::QueueEntry>& queue) -> core::Result<void> {
                            std::erase_if(queue, [&first](const auto& entry) {
                                return entry.entry_id != first;
                            });
                            return {};
                        })
                    .value_or(false),
                "the queue edited");
        require(origin().is_null(), "an edited queue is no rule's");
        // Replaced by another client, likewise.
        static_cast<void>(answer(dispatcher, "dynamic.play", Json{{"id", saved_id}}));
        require(origin() == saved_id, "played again");
        static_cast<void>(
            answer(dispatcher, "playback.replace_queue",
                   Json{{"entries", Json::array({engine::to_json(player->queue().front())})}}));
        require(origin().is_null(), "a replaced queue is no rule's");
        // And a rule removed takes its name off the queue.
        auto brief = saved;
        brief.erase("id");
        brief["name"] = "Brief";
        const auto brief_id = answer(dispatcher, "dynamic.save", Json{{"rule", brief}})
                                  .at("rule")
                                  .at("id")
                                  .get<std::string>();
        static_cast<void>(answer(dispatcher, "dynamic.play", Json{{"id", brief_id}}));
        require(origin() == brief_id, "another rule played");
        static_cast<void>(answer(dispatcher, "dynamic.delete", Json{{"id", brief_id}}));
        require(origin().is_null(), "its rule removed, the queue is no rule's");
        static_cast<void>(answer(dispatcher, "dynamic.play", Json{{"id", saved_id}}));
        static_cast<void>(answer(dispatcher, "playback.pause"));
    }

    // ADR-0351: a restart brings the queue back with its rule -- unless the
    // rule went meanwhile.
    {
        {
            engine::PlaybackStore store{*player, *workspace};
            store.persist();
        }
        auto restarted = engine::Player::create_without_audio();
        engine::PlaybackStore store{*restarted, *workspace};
        require(store.restore(), "the queue comes back");
        require(restarted->state().queue_rule == saved_id, "as the rule's selection");
        engine::DynamicRules rules{*workspace, {}};
        engine::follow_queue_rule(rules, *restarted);
        require(restarted->queue_rule() == saved_id, "its rule is still there");

        auto gone = engine::Player::create_without_audio();
        auto persisted = player->persisted();
        persisted.queue_rule = "removed-meanwhile";
        static_cast<void>(gone->restore(std::move(persisted)));
        require(gone->queue_rule() == "removed-meanwhile", "restored as it was kept");
        engine::follow_queue_rule(rules, *gone);
        require(gone->queue_rule().empty(), "but a rule that is gone is not claimed");
    }

    // ADR-0350: what a Last.fm source picks.
    {
        engine::DynamicPicker picker{*workspace, catalogue, suggest};
        engine::DynamicRule similar{.id = {},
                                    .name = "Like it",
                                    .source = "similar",
                                    .query = {},
                                    .artist = "Seed",
                                    .track = "Song",
                                    .user = {},
                                    .tag = {},
                                    .limit = 2U,
                                    .shuffle = false,
                                    .group_by = {},
                                    .groups = 0U,
                                    .per_group = 0U};
        std::vector<std::pair<std::size_t, std::size_t>> reported;
        const auto first = picker.pick(similar, {}, [&](std::size_t done, std::size_t of) {
            reported.emplace_back(done, of);
        });
        require(first.has_value(), "picked");
        require(asked.back().first == "track.getSimilar" &&
                    asked.back().second ==
                        std::vector<std::pair<std::string, std::string>>{{"artist", "Seed"},
                                                                         {"track", "Song"}},
                "the seed's similar tracks are asked for");
        require(first->matched == 3U, "every suggestion the library has, once");
        require(first->unmatched == 1U, "and one it does not");
        require(first->paths.size() == 2U, "as many as the rule wants");
        require(!reported.empty() && reported.back() == std::pair<std::size_t, std::size_t>{5U, 5U},
                "matching reports its progress");
        // Picked again: the one not picked the last time comes first, and
        // a new start of the engine still knows.
        engine::DynamicPicker restarted{*workspace, catalogue, suggest};
        const auto second = restarted.pick(similar);
        require(second && second->paths.size() == 2U, "picked again");
        const std::set<std::string> before{first->paths.begin(), first->paths.end()};
        std::size_t fresh = 0U;
        for (const auto& path : second->paths) {
            require(library_paths.contains(path), "a library file");
            fresh += before.contains(path) ? 0U : 1U;
        }
        require(fresh == 1U, "the one left out the last time is picked");
        // A different seed is its own source.
        auto other = similar;
        other.track = "Other song";
        other.limit = 3U;
        require(restarted.pick(other)->paths.size() == 3U, "every match");

        // What the window asks: a rule, saved or not, as a job.
        engine::JobCatalog catalog;
        engine::register_dynamic_rule_jobs(catalog, picker);
        auto unnamed = engine::dynamic_rule_json(similar);
        unnamed["name"] = "";
        auto work = catalog.build("dynamic.pick", Json{{"rule", unnamed}});
        require(work.has_value(), "a rule being written is picked");
        std::vector<Json> job_progress;
        const auto outcome = (*work)({}, [&job_progress](Json reported_progress) {
            job_progress.push_back(reported_progress);
        });
        require(outcome.at("paths").size() == 2U && outcome.at("matched") == 3 &&
                    outcome.at("unmatched") == 1,
                "the job answers what was picked");
        require(protocol::decode_raw_path(outcome.at("paths").front().get<std::string>()) &&
                    library_paths.contains(
                        *protocol::decode_raw_path(outcome.at("paths").front().get<std::string>())),
                "as files");
        require(!job_progress.empty() && job_progress.back().at("of") == 5, "with its progress");
        auto library_rule = engine::dynamic_rule_json(engine::shipped_dynamic_rules().front());
        const auto library_outcome =
            (*catalog.build("dynamic.pick", Json{{"rule", library_rule}}))({}, [](Json) {});
        require(library_outcome.at("paths").size() == 4U, "a tkq-1 rule is picked the same way");
        require(!catalog.build("dynamic.pick", Json{{"rule", Json{{"query", "((("}}}}),
                "a broken rule is refused at once");
        core::CancellationSource stop;
        stop.request_cancellation();
        require((*work)(stop.token(), [](Json) {}).value("cancelled", false),
                "a cancelled pick says so");
        auto tagless = similar;
        tagless.source = "tag";
        require(!picker.pick(tagless), "a tag rule needs its tag");
        engine::DynamicPicker without_lastfm{*workspace, catalogue, {}};
        require(!without_lastfm.pick(similar), "an engine without Last.fm cannot pick one");
        // Last.fm failing is said.
        engine::DynamicPicker failing{
            *workspace, catalogue,
            [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
               std::size_t) -> core::Result<std::vector<engine::LastFmSuggestion>> {
                return std::unexpected(core::Error{.code = core::ErrorCode::io,
                                                   .message = "Last.fm could not be reached",
                                                   .context = {}});
            }};
        const auto failed = failing.pick(similar);
        require(!failed && failed.error().message == "Last.fm could not be reached",
                "Last.fm failing is said");
    }

    // Kept: another start has them.
    {
        engine::DynamicRules rules{*workspace, {}};
        const auto all = rules.all();
        require(all.size() == shipped_count + 3U, "the saved rules are kept");
        require(rules.find(saved_id)->name == "All of it", "as edited");
        require(rules.find(saved_id)->shuffle, "with every field");
        require(rules.find("loved")->user == "someone", "a Last.fm rule's too");
    }

    std::filesystem::remove_all(directory);
    return EXIT_SUCCESS;
}
