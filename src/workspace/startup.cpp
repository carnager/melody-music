// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/startup.hpp"

#include "bench/settings_keys.hpp"
#include "workspace/tab_store.hpp"

#include "trackknife/core/engine_state.hpp"
#include "trackknife/persistence/workspace_backup.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <filesystem>

namespace trackknife::bench {

namespace {

// The same place under Trackknife's name (ADR-0287): each of the path's last
// two components that is the application's name now, the old one.
[[nodiscard]] QString underOldName(const QString& path) {
    const auto name = QCoreApplication::applicationName();
    auto parts = QDir::cleanPath(path).split(QLatin1Char('/'));
    for (auto index = parts.size() - 1; index >= 0 && index >= parts.size() - 2; --index) {
        if (parts[index] == name) {
            parts[index] = QStringLiteral("trackknife");
        } else if (parts[index].startsWith(name + QLatin1Char('.'))) {
            // The settings file, melody.conf or however its format names it.
            parts[index] = QStringLiteral("trackknife") + parts[index].sliced(name.size());
        }
    }
    return parts.join(QLatin1Char('/'));
}

} // namespace

void adoptTrackknifeNames() {
    // ADR-0287: settings, the window's data and its cache move from
    // Trackknife's names, once. The data folder was the engine's too: only
    // the window's own entries move here, the engine moves its own.
    const auto new_settings = QSettings{}.fileName();
    if (const auto old_settings = underOldName(new_settings); old_settings != new_settings &&
                                                              !QFile::exists(new_settings) &&
                                                              QFile::exists(old_settings)) {
        QDir{}.mkpath(QFileInfo{new_settings}.absolutePath());
        if (QFile::rename(old_settings, new_settings)) {
            // The rating backup copy kept under the name it had by default
            // then, so no file is written again for the new default.
            QSettings settings;
            if (settings.value(QLatin1String(SettingsKeys::rating_backup_key), false).toBool() &&
                !settings.contains(QLatin1String(SettingsKeys::rating_backup_tag_key))) {
                settings.setValue(QLatin1String(SettingsKeys::rating_backup_tag_key),
                                  QStringLiteral("TRACKKNIFE_RATING"));
            }
        }
    }
    const auto new_data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (const QDir old_data{underOldName(new_data)};
        old_data.path() != new_data && old_data.exists()) {
        QDir{}.mkpath(new_data);
        for (const auto& name : old_data.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
            const bool windows =
                name == QStringLiteral("tabs") || name == QStringLiteral("lastfm-v1.json") ||
                name == QStringLiteral("melodyd.log") || name.startsWith(QStringLiteral("before-"));
            if (windows && !QFileInfo::exists(new_data + QLatin1Char('/') + name)) {
                QDir{}.rename(old_data.filePath(name), new_data + QLatin1Char('/') + name);
            }
        }
    }
    const auto new_cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (const auto old_cache = underOldName(new_cache);
        old_cache != new_cache && !QDir{new_cache}.exists() && QDir{old_cache}.exists()) {
        QDir{}.mkpath(QFileInfo{new_cache}.absolutePath());
        QDir{}.rename(old_cache, new_cache);
    }
}

void isolateTestEngineState() {
    if (!QStandardPaths::isTestModeEnabled()) {
        return;
    }
    const auto folder =
        QFile::encodeName(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    if (qEnvironmentVariableIsEmpty("MELODY_STATE_DIR")) {
        qputenv("MELODY_STATE_DIR", folder);
    }
    // ADR-0339: and Melody's presets with the test's own data, not shared
    // by every test that runs in the same home.
    if (qEnvironmentVariableIsEmpty("MELODY_DATA_DIR")) {
        qputenv("MELODY_DATA_DIR", folder);
    }
}

namespace {

constexpr auto restore_from_key = "recovery/restore-from";
constexpr auto restore_parts_key = "recovery/restore-parts";
// The open tabs, in the settings; restored with the tabs, not the settings.
constexpr auto tabs_key = "tabs/v1";

[[nodiscard]] QString settingsBeside(const QString& from) {
    return from + QStringLiteral(".settings.ini");
}

[[nodiscard]] QString tabsBeside(const QString& from) { return from + QStringLiteral(".tabs"); }

[[nodiscard]] std::filesystem::path rawPath(const QString& path) {
    const auto encoded = QFile::encodeName(path);
    return std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

[[nodiscard]] QString shownPath(const std::filesystem::path& path) {
    return QFile::decodeName(QByteArray::fromStdString(path.native()));
}

// The settings as they are now, kept where `presets` keeps its database.
[[nodiscard]] bool keepSettings(const QSettings& current, const QString& destination) {
    QSettings kept{destination, QSettings::IniFormat};
    kept.setValue(QStringLiteral("backup/format"), 1);
    for (const auto& key : current.allKeys()) {
        kept.setValue(QStringLiteral("values/") + key, current.value(key));
    }
    kept.sync();
    return kept.status() == QSettings::NoError;
}

} // namespace

WorkspaceRestore workspaceBackupParts(const QString& from) {
    QSettings imported{settingsBeside(from), QSettings::IniFormat};
    const auto settings_valid = QFile::exists(settingsBeside(from)) &&
                                imported.status() == QSettings::NoError &&
                                imported.value(QStringLiteral("backup/format")).toInt() == 1;
    return {.from = from,
            .presets = QFile::exists(from),
            .settings = settings_valid,
            // Which tabs are open is in the settings, their rows beside them.
            .tabs = settings_valid && QDir{tabsBeside(from)}.exists()};
}

void scheduleWorkspaceRestore(const WorkspaceRestore& restore) {
    QStringList parts;
    if (restore.presets) {
        parts << QStringLiteral("presets");
    }
    if (restore.settings) {
        parts << QStringLiteral("settings");
    }
    if (restore.tabs) {
        parts << QStringLiteral("tabs");
    }
    QSettings settings;
    settings.setValue(QLatin1String(restore_from_key), restore.from);
    settings.setValue(QLatin1String(restore_parts_key), parts);
    settings.sync();
}

QString applyPendingWorkspaceRestore() {
    QSettings settings;
    const auto from = settings.value(QLatin1String(restore_from_key)).toString();
    const auto parts = settings.value(QLatin1String(restore_parts_key)).toStringList();
    settings.remove(QLatin1String(restore_from_key));
    settings.remove(QLatin1String(restore_parts_key));
    // Scheduled before: it put the backup over the engine's database. Never
    // applied.
    settings.remove(QStringLiteral("recovery/pending-workspace-restore"));
    settings.remove(QStringLiteral("recovery/pending-settings-restore"));
    settings.sync();
    if (from.isEmpty() || parts.isEmpty()) {
        return {};
    }

    const auto data = core::melody_data_directory();
    std::error_code ignored;
    std::filesystem::create_directories(data, ignored);
    const auto stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
    QStringList restored;
    QStringList failed;

    if (parts.contains(QStringLiteral("presets"))) {
        const auto rollback =
            data / rawPath(QStringLiteral("presets-before-restore-%1.sqlite").arg(stamp));
        if (auto done = persistence::restore_workspace_database_backup(
                rawPath(from), core::presets_database_path(), rollback)) {
            restored << QStringLiteral("presets");
        } else {
            failed
                << QStringLiteral("presets: %1").arg(QString::fromStdString(done.error().message));
        }
    }

    const auto want_settings = parts.contains(QStringLiteral("settings"));
    const auto want_tabs = parts.contains(QStringLiteral("tabs"));
    if (want_settings || want_tabs) {
        QSettings imported{settingsBeside(from), QSettings::IniFormat};
        const auto rollback =
            shownPath(data) + QStringLiteral("/settings-before-restore-%1.ini").arg(stamp);
        if (!QFile::exists(settingsBeside(from)) || imported.status() != QSettings::NoError ||
            imported.value(QStringLiteral("backup/format")).toInt() != 1) {
            failed << QStringLiteral("settings: the backup's settings could not be read");
        } else if (!keepSettings(settings, rollback)) {
            failed << QStringLiteral("settings: the current ones could not be kept, so nothing "
                                     "was replaced");
        } else {
            const auto backed_up_tabs = QStringLiteral("values/") + QLatin1String(tabs_key);
            if (want_settings) {
                const auto open_tabs = settings.value(QLatin1String(tabs_key));
                settings.clear();
                for (const auto& key : imported.allKeys()) {
                    if (key.startsWith(QStringLiteral("values/")) && key != backed_up_tabs &&
                        !key.startsWith(QStringLiteral("values/recovery/"))) {
                        settings.setValue(key.sliced(7), imported.value(key));
                    }
                }
                if (open_tabs.isValid()) {
                    settings.setValue(QLatin1String(tabs_key), open_tabs);
                }
                restored << QStringLiteral("settings");
            }
            if (want_tabs) {
                if (TabStore::restoreFrom(tabsBeside(from))) {
                    if (imported.contains(backed_up_tabs)) {
                        settings.setValue(QLatin1String(tabs_key), imported.value(backed_up_tabs));
                    }
                    restored << QStringLiteral("tabs");
                } else {
                    failed << QStringLiteral("tabs: they could not be copied back");
                }
            }
            settings.sync();
        }
    }

    auto notice = restored.isEmpty()
                      ? QStringLiteral("Workspace restore failed")
                      : QStringLiteral("Restored %1 from %2; what they replaced is kept in %3")
                            .arg(restored.join(QStringLiteral(", ")), from, shownPath(data));
    if (!failed.isEmpty()) {
        notice +=
            (restored.isEmpty() ? QStringLiteral(": %1") : QStringLiteral("; not restored: %1"))
                .arg(failed.join(QStringLiteral("; ")));
    }
    return notice;
}

} // namespace trackknife::bench
