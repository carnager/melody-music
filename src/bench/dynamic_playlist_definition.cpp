// SPDX-License-Identifier: GPL-3.0-only
#include "bench/dynamic_playlist_definition.hpp"

#include <algorithm>

namespace trackknife::bench {

engine::DynamicSelection DynamicPlaylistDefinition::selection() const {
    const bool grouped = groups > 0 && !group_by.trimmed().isEmpty();
    return engine::DynamicSelection{
        .query = query.toStdString(),
        .limit = static_cast<std::size_t>(std::clamp(limit, 1, 500)),
        .shuffle = shuffle,
        .group_by = grouped ? group_by.toStdString() : std::string{},
        .groups = grouped ? static_cast<std::size_t>(groups) : 0U,
        .per_group = grouped ? static_cast<std::size_t>(std::max(per_group, 0)) : 0U};
}

DynamicPlaylistDefinition dynamicRuleOf(const protocol::Json& value) {
    if (!value.is_object()) {
        return {};
    }
    const auto text = [&value](const char* key, const char* fallback = "") {
        const auto found = value.find(key);
        return QString::fromStdString(found != value.end() && found->is_string()
                                          ? found->get<std::string>()
                                          : std::string{fallback});
    };
    const auto count = [&value](const char* key, const int fallback) {
        const auto found = value.find(key);
        return found != value.end() && found->is_number_integer()
                   ? static_cast<int>(std::clamp<std::int64_t>(found->get<std::int64_t>(), 0, 500))
                   : fallback;
    };
    const auto shuffle = value.find("shuffle");
    return DynamicPlaylistDefinition{.id = text("id"),
                                     .name = text("name"),
                                     .profile = {},
                                     .source = text("source", "rules"),
                                     .query = text("query"),
                                     .artist = text("artist"),
                                     .track = text("track"),
                                     .user = text("user"),
                                     .tag = text("tag"),
                                     .limit = count("limit", 100),
                                     .shuffle = shuffle != value.end() && shuffle->is_boolean() &&
                                                shuffle->get<bool>(),
                                     .group_by = text("group_by"),
                                     .groups = count("groups", 0),
                                     .per_group = count("per_group", 0)};
}

protocol::Json dynamicRuleJson(const DynamicPlaylistDefinition& definition) {
    protocol::Json rule{{"name", definition.name.toStdString()},
                        {"source", definition.source.toStdString()},
                        {"query", definition.query.toStdString()},
                        {"artist", definition.artist.toStdString()},
                        {"track", definition.track.toStdString()},
                        {"user", definition.user.toStdString()},
                        {"tag", definition.tag.toStdString()},
                        {"limit", definition.limit},
                        {"shuffle", definition.shuffle},
                        {"group_by", definition.group_by.toStdString()},
                        {"groups", definition.groups},
                        {"per_group", definition.per_group}};
    if (!definition.id.isEmpty()) {
        rule["id"] = definition.id.toStdString();
    }
    return rule;
}

} // namespace trackknife::bench
