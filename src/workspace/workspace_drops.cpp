// SPDX-License-Identifier: GPL-3.0-only

// Drag and drop (ADR-0233): what a drop on a list, a new list, Up Next or
// a list in the lists pane does.

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "workspace/library_browser.hpp"
#include "workspace/workspace_view.hpp"

#include <QFile>
#include <QFileInfo>
#include <QPersistentModelIndex>
#include <QPointer>

#include <algorithm>
#include <utility>

namespace trackknife::bench {
namespace {

void resolved(const Workspace::Dragged& dragged,
              std::function<void(std::vector<std::string>)> done) {
    if (!dragged.paths.empty()) {
        done(dragged.paths);
    } else if (dragged.resolve) {
        dragged.resolve(std::move(done));
    }
}

} // namespace

void Workspace::addDroppedPaths(const QString& id, const EngineKey& files_engine,
                                std::vector<std::string> paths, const int row) {
    auto* destination = tabForDocument(id);
    if (destination == nullptr) {
        return;
    }
    const auto into = EngineKey::of(destination->document);
    // From another engine's library: as this list's engine sees those files,
    // where it can.
    // This computer's -- from a file manager, say: found here, folders and
    // CUE sheets opened, then each as the list's engine reaches it
    // (finishDiscovery).
    if (files_engine.isLocal()) {
        startDiscovery(std::move(paths), id, row);
        return;
    }
    // From another engine's library: as this list's engine sees those files,
    // where it can.
    if (files_engine != into) {
        paths = crossEnginePaths(std::move(paths), files_engine, into);
    }
    if (paths.empty()) {
        return;
    }
    // ADR-0227: a remote's paths are its machine's; looking for them here,
    // as discovery does, would find nothing without a mount.
    if (!into.isLocal()) {
        insertRemotePaths(*destination, std::move(paths), row);
    } else {
        startDiscovery(std::move(paths), id, row);
    }
}

bool Workspace::dropOnList(Dragged dragged, ListTab& target, const int insertion_row,
                           const bool copy) {
    const auto id = document_text(target.document.id);
    if (dragged.isRows()) {
        if (dragged.from_model == target.model && !copy) {
            target.model->reorderRows(std::move(dragged.rows), insertion_row);
            markTabDirty(target);
            return true;
        }
        return transferRows(dragged.from_tab, dragged.from_model, dragged.engine, dragged.dynamic,
                            dragged.rows, id, !copy && !dragged.dynamic, insertion_row);
    }
    if (!dragged.isFiles() || discovery_running_) {
        return false;
    }
    // The row dropped before, kept while the files are found.
    const QPersistentModelIndex anchor{target.model->index(insertion_row, 0)};
    const bool anchored = anchor.isValid();
    const QPointer self{this};
    resolved(dragged, [self, id, insertion_row, anchor, anchored,
                       engine = dragged.engine](std::vector<std::string> paths) {
        if (!self || (anchored && !anchor.isValid())) {
            return;
        }
        self->addDroppedPaths(id, engine, std::move(paths),
                              anchored ? anchor.row() : insertion_row);
    });
    return true;
}

bool Workspace::dropOnUpNext(Dragged dragged, const int position) {
    if (dragged.isRows()) {
        std::ranges::sort(dragged.rows);
        std::vector<LocalTrackRow> tracks;
        for (const auto row : dragged.rows) {
            if (row >= 0 && row < dragged.from_model->rowCount()) {
                tracks.push_back(dragged.from_model->rows()[static_cast<std::size_t>(row)]);
            }
        }
        enqueueLocalRequests(std::move(tracks), position, dragged.engine);
        view_->refreshUpNext();
        return true;
    }
    // A library's albums and tracks; not folders, nor files from elsewhere.
    if (dragged.library == nullptr || dragged.entries.empty()) {
        return false;
    }
    const QPointer self{this};
    dragged.library->resolveEntryRows(
        std::move(dragged.entries),
        [self, position, engine = dragged.engine](std::vector<LocalTrackRow> rows) {
            if (self) {
                self->enqueueLocalRequests(std::move(rows), position, engine);
                self->view_->refreshUpNext();
            }
        });
    return true;
}

} // namespace trackknife::bench
