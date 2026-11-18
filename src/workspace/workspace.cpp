// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/workspace.hpp"

#include "trackknife/core/engine_state.hpp"
#include "trackknife/core/environment.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/dynamic_playlist_service.hpp"
#include "bench/remote_engines.hpp"
#include "workspace/startup.hpp"
#include "workspace/tab_store.hpp"
#include "workspace/workspace_view.hpp"

#include <QDir>
#include <QFutureWatcher>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace trackknife::bench {
namespace {

constexpr int persist_debounce_ms = 1'000;

} // namespace

Workspace::Workspace(QObject* parent) : QObject(parent) {
    catalogue_work_.setMaxThreadCount(1);
    // ADR-0234: this computer's engine is always the first link, connected
    // or not; remotes follow when configured.
    engines_.push_back(std::make_unique<EngineLink>());
    engines_.front()->key = EngineKey::local();
    // Up Next's rows, as every window shows them.
    up_next_local_model_ = new LocalListModel(this);
    // The playback modes and ReplayGain as they were left, whichever window.
    loadLocalPlaybackModes();
}

Workspace::~Workspace() {
    // The connections before the catalogues they were made from: one waits
    // for its work in hand, which may revive its engine through them.
    for (const auto& engine : engines_) {
        delete engine->playback;
        engine->playback = nullptr;
    }
}

void Workspace::start() {
    isolateTestEngineState();
    // ADR-0339: Melody's presets are its own -- not the engine's database,
    // which this window no longer opens -- shared with melody-cli.
    database_path_ = core::presets_database_path();
    std::error_code ignored;
    std::filesystem::create_directories(database_path_.parent_path(), ignored);
    connectLocalEngine();
    persistence_ = new ui::ListPersistenceService(database_path_, this);
    persistence_timer_ = new QTimer(this);
    persistence_timer_->setSingleShot(true);
    persistence_timer_->setInterval(persist_debounce_ms);
    connect(persistence_timer_, &QTimer::timeout, this, [this] {
        persistNow(false);
        refreshNewRatings();
    });
    tab_store_ = std::make_unique<TabStore>();
    // ADR-0259: the window keeps its tabs itself (TabStore).
    persistence_->initialize([this](const QString& error) {
        if (!error.isEmpty()) {
            view_->showMessage(QStringLiteral("List restore failed: %1").arg(error), 5'000);
        }
        restoreFromTabStore();
    });
}

void Workspace::restoreFromTabStore() {
    auto state = TabStore::loadState();
    restored_track_view_layouts_.clear();
    for (const auto& tab : state.tabs) {
        if (!tab.layout.isEmpty()) {
            restored_track_view_layouts_.insert(
                QStringLiteral("local:%1").arg(QString::fromStdString(tab.id.to_string())),
                tab.layout);
        }
    }
    restored_active_ = state.active;
    auto* preparing = new QFutureWatcher<std::vector<PreparedList>>(this);
    connect(preparing, &QFutureWatcherBase::finished, this, [this, preparing] {
        preparing->deleteLater();
        restoreLists(preparing->future().takeResult());
        // ADR-0286: a tab's rows that could not be read are kept aside, and
        // said: for a tab that is no saved playlist they are its only copy.
        if (const auto unreadable = tab_store_->takeUnreadable(); !unreadable.empty()) {
            view_->showMessage(
                QStringLiteral("%1 tab(s) could not be read and open empty; the files are kept "
                               "as %2")
                    .arg(unreadable.size())
                    .arg(QStringList{unreadable.begin(), unreadable.end()}.join(
                        QStringLiteral(", "))),
                0);
        }
        restoredWorkspace({});
    });
    preparing->setFuture(
        QtConcurrent::run([store = tab_store_.get(), tabs = std::move(state.tabs)] {
            std::vector<persistence::ListDocument> documents;
            documents.reserve(tabs.size());
            for (const auto& tab : tabs) {
                auto cached = store->readRows(tab.id);
                // Its rows gone, a tab is empty -- a saved playlist until its
                // engine answers -- and not edited: an empty list kept over
                // the engine's would lose it all.
                auto document = cached.value_or(persistence::ListDocument{.id = tab.id,
                                                                          .kind = tab.kind,
                                                                          .name = tab.name,
                                                                          .pinned = false,
                                                                          .dirty = false,
                                                                          .items = {},
                                                                          .engine = {}});
                // The window's own say: which engine, and how it is shown.
                document.engine = tab.engine;
                document.pinned = tab.pinned;
                document.dirty = cached.has_value() && tab.dirty;
                documents.push_back(std::move(document));
            }
            return prepareLists(std::move(documents));
        }));
}

void Workspace::restoredWorkspace(const QString& error) {
    restoreUpNext();
    // After the lists, because the entry the engine names is looked for in
    // them before a tab is invented for it.
    reattachToEngine();
    // ADR-0227: the remote engines too, after the lists for the same reason
    // -- their tabs may already be among them.
    for (const auto& setting : loadRemoteEngines()) {
        connectRemoteEngine(setting);
    }
    view_->workspaceRestored(error.isEmpty());
}

Workspace::EngineLink* Workspace::link(const EngineKey& key) const {
    const auto found = std::ranges::find(engines_, key, [](const auto& each) { return each->key; });
    return found != engines_.end() ? found->get() : nullptr;
}

EnginePlayback* Workspace::playbackOf(const EngineKey& key) const {
    const auto* engine = link(key);
    return engine != nullptr ? engine->playback : nullptr;
}

CatalogueSource* Workspace::catalogueOf(const EngineKey& key) const {
    const auto* engine = link(key);
    return engine != nullptr ? engine->catalogue.get() : nullptr;
}

Workspace::ListTab* Workspace::tabForDocument(const core::StableId& document_id) {
    if (document_id.is_nil()) {
        return nullptr;
    }
    return tabForDocument(QString::fromStdString(document_id.to_string()));
}

Workspace::ListTab* Workspace::tabForDocument(const QString& document_id) {
    for (const auto& tab : list_tabs_) {
        if (QString::fromStdString(tab->document.id.to_string()) == document_id) {
            return tab.get();
        }
    }
    if (detached_playback_ &&
        QString::fromStdString(detached_playback_->document.id.to_string()) == document_id) {
        return &*detached_playback_;
    }
    return nullptr;
}

void Workspace::schedulePersist() {
    if (persistence_timer_ != nullptr) {
        persistence_timer_->start();
    }
}

} // namespace trackknife::bench
