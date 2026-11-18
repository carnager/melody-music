// SPDX-License-Identifier: GPL-3.0-only

#include "bench/remote_engines.hpp"

#include "bench/settings_keys.hpp"

#include <QSettings>

namespace trackknife::bench {
namespace {

constexpr auto array_key = "engines/remote";

[[nodiscard]] QString text(const QSettings& settings, const char* key) {
    return settings.value(QLatin1String(key), QString{}).toString().trimmed();
}

} // namespace

QString RemoteEngineSetting::effectivePassword() const {
    return password.isEmpty() ? text(QSettings{}, SettingsKeys::engine_password_key) : password;
}

QString RemoteEngineSetting::label() const { return name.isEmpty() ? address : name; }

RemoteMount RemoteEngineSetting::mount() const {
    return RemoteMount{.remote_folder = music_folder.toStdString(),
                       .local_folder = reachable_at.toStdString()};
}

std::vector<RemoteEngineSetting> loadRemoteEngines() {
    QSettings settings;
    std::vector<RemoteEngineSetting> engines;
    const auto count = settings.beginReadArray(QLatin1String(array_key));
    engines.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        RemoteEngineSetting engine{
            .address = settings.value("address").toString().trimmed(),
            .password = settings.value("password").toString().trimmed(),
            .music_folder = settings.value("music-folder").toString().trimmed(),
            .reachable_at = settings.value("reachable-at").toString().trimmed(),
            .id = settings.value("id").toString().trimmed(),
            .stream_kbps = settings.value("stream-kbps", -1).toInt(),
            .name = settings.value("name").toString().trimmed()};
        if (!engine.address.isEmpty()) {
            engines.push_back(std::move(engine));
        }
    }
    settings.endArray();
    return engines;
}

void saveRemoteEngines(const std::vector<RemoteEngineSetting>& engines) {
    QSettings settings;
    settings.remove(QLatin1String(array_key));
    settings.beginWriteArray(QLatin1String(array_key), static_cast<int>(engines.size()));
    for (int index = 0; index < static_cast<int>(engines.size()); ++index) {
        const auto& engine = engines[static_cast<std::size_t>(index)];
        settings.setArrayIndex(index);
        settings.setValue("address", engine.address);
        settings.setValue("password", engine.password);
        settings.setValue("music-folder", engine.music_folder);
        settings.setValue("reachable-at", engine.reachable_at);
        settings.setValue("id", engine.id);
        settings.setValue("stream-kbps", engine.stream_kbps);
        settings.setValue("name", engine.name);
    }
    settings.endArray();
}

void rememberEngineId(const QString& address, const QString& id) {
    auto engines = loadRemoteEngines();
    bool changed = false;
    for (auto& engine : engines) {
        if (engine.address == address && engine.id != id) {
            engine.id = id;
            changed = true;
        }
    }
    if (changed) {
        saveRemoteEngines(engines);
    }
}

void rememberEngineName(const QString& address, const QString& name) {
    if (name.isEmpty()) {
        return;
    }
    auto engines = loadRemoteEngines();
    bool changed = false;
    for (auto& engine : engines) {
        if (engine.address == address && engine.name != name) {
            engine.name = name;
            changed = true;
        }
    }
    if (changed) {
        saveRemoteEngines(engines);
    }
}

} // namespace trackknife::bench
