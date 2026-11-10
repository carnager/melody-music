// SPDX-License-Identifier: GPL-3.0-only

// ADR-0349: the dynamic playlist rules are the engine's -- listed, saved,
// edited, removed and handed over through the protocol, kept across a
// restart, and played by the engine itself.

#include "recording_audition.hpp"

#include "trackknife/core/stable_id.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/dynamic_rules.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/dispatch.hpp"
#include "trackknife/protocol/message.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
          std::pair{"tagged-tone-opus", ".opus"}}) {
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

    std::string saved_id;
    {
        engine::DynamicRules rules{*workspace, sink};
        protocol::Dispatcher dispatcher;
        engine::register_dynamic_rule_methods(dispatcher, rules, catalogue, *player);

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

        // A Last.fm rule is kept, but not played by the engine.
        const Json loved{
            {"id", "loved"}, {"name", "Loved"}, {"source", "loved"}, {"user", "someone"}};
        require(answer(dispatcher, "dynamic.save", Json{{"rule", loved}}).contains("rule"),
                "a Last.fm rule is kept");
        require(!call(dispatcher, "dynamic.play", Json{{"id", "loved"}}).result,
                "and not played by the engine");

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
