// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Settings for the engines elsewhere a test window or catalogue connects to,
// as Settings › Engines keeps them (ADR-0234, ADR-0286).

#include "bench/remote_engines.hpp"
#include "bench/settings_keys.hpp"

#include <QSettings>
#include <QString>

namespace trackknife::bench::testing {

// `address` as the one engine elsewhere, with its own password if given.
inline void configureRemoteEngine(const QString& address, const QString& password = {}) {
    RemoteEngineSetting engine;
    engine.address = address;
    engine.password = password;
    saveRemoteEngines({engine});
}

// The first engine elsewhere holds the library, as for someone whose music
// is all there; or, again, this computer.
inline void holdLibraryOnTheRemote() {
    const auto engines = loadRemoteEngines();
    QSettings{}.setValue(QLatin1String(SettingsKeys::library_engine_key),
                         engines.empty() ? QString{} : engines.front().address);
}
inline void holdLibraryHere() {
    QSettings{}.remove(QLatin1String(SettingsKeys::library_engine_key));
}

// The id kept for the first engine elsewhere, empty until it said one.
[[nodiscard]] inline QString rememberedRemoteId() {
    const auto engines = loadRemoteEngines();
    return engines.empty() ? QString{} : engines.front().id;
}

} // namespace trackknife::bench::testing
