// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/formats/cue_sheet.hpp"
#include "trackknife/loudness/replaygain.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/loudness_sidecar.hpp"
#include "uicommon/local_artwork.hpp"
#include "workspace/workspace_view.hpp"

#include <QSettings>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(MELODY_THREAD_SANITIZER)
extern "C" void __tsan_acquire(void* address);
extern "C" void __tsan_release(void* address);
#endif

namespace trackknife::bench {
namespace {

constexpr std::size_t discovery_row_limit = 100'000U;

// What a row showed before its file was read stays where the file has no
// tag for it: a queue entry another client gave its title and album, say,
// for a file without tags. A file's own tags always win.
void keep_what_the_file_lacks(LocalTrackRow& filled, const LocalTrackRow& had) {
    const auto keep = [&filled](std::string& value, const std::string& before,
                                const std::initializer_list<std::string_view> names) {
        const bool tagged = std::ranges::any_of(names, [&filled](const std::string_view name) {
            return filled.metadata.first_effective_value(name).has_value();
        });
        if (!tagged && !before.empty()) {
            value = before;
        }
    };
    keep(filled.title, had.title, {"title"});
    keep(filled.artist, had.artist, {"artist"});
    keep(filled.album, had.album, {"album"});
    keep(filled.album_artist, had.album_artist, {"albumartist"});
    keep(filled.date, had.date, {"date"});
    keep(filled.track_number, had.track_number, {"tracknumber"});
}

void append_metadata_value(metadata::MetadataDocument& document, std::string native_name,
                           std::string value, const metadata::FieldProvenance provenance) {
    if (value.empty()) {
        return;
    }
    const auto canonical_name =
        metadata::resolve_text_property_identity(native_name).canonical_name;
    if (canonical_name.empty()) {
        return;
    }
    document.fields.push_back(metadata::MetadataField{
        .canonical_name = canonical_name,
        .native_name = std::move(native_name),
        .values = {std::move(value)},
        .qualifier = {},
        .provenance = provenance,
    });
}

// Folder expansion only ingests plausible audio and external cue sheets;
// explicitly opened files always pass regardless (core discovery contract).
// The probe/resolver still gates everything this list lets through.
constexpr std::array<std::string_view, 28> audio_extensions{
    "flac", "mp3", "ogg",  "oga", "opus", "m4a", "mp4", "aac", "wv",  "wav",
    "rf64", "w64", "aiff", "aif", "aifc", "ape", "mpc", "tta", "spx", "mka",
    "wma",  "dsf", "dff",  "cue", "mod",  "xm",  "s3m", "it"};

[[nodiscard]] bool is_cue_path(const std::string& raw_path) {
    const auto slash = raw_path.find_last_of('/');
    const auto dot = raw_path.find_last_of('.');
    return dot != std::string::npos && (slash == std::string::npos || dot > slash) &&
           lowercased_ascii(raw_path.substr(dot + 1U)) == "cue";
}

[[nodiscard]] std::string cue_remark(const formats::CueLogicalTrack& track,
                                     const std::string_view name) {
    const auto found = std::ranges::find_if(
        track.remarks.rbegin(), track.remarks.rend(),
        [name](const formats::CueMetadataField& field) { return field.name == name; });
    return found == track.remarks.rend() ? std::string{} : found->value;
}

[[nodiscard]] std::string cue_logical_reference(const std::string& raw_cue_path,
                                                const formats::CueLogicalTrack& track) {
    return cue_track_logical_reference(raw_cue_path, track.file_index, track.track_index);
}

[[nodiscard]] LocalTrackRow
cue_row(const formats::ResolvedCueSheet& sheet, const formats::ResolvedCueTrack& resolved,
        const metadata::MetadataDocument& embedded_document,
        const std::optional<core::LocalSourceRevision>& source_revision) {
    const auto& cue = resolved.cue;
    LocalTrackRow row;
    row.raw_path = resolved.raw_source_path;
    row.logical_reference = cue_logical_reference(sheet.raw_cue_path, cue);
    row.segment = resolved.sample_range;
    row.metadata = embedded_document;
    row.source_revision = source_revision;
    for (const auto& remark : cue.remarks) {
        append_metadata_value(row.metadata, remark.name, remark.value,
                              metadata::FieldProvenance::segment);
    }
    append_metadata_value(row.metadata, "TITLE", cue.title.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "ARTIST", cue.performer.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "ALBUM", cue.album_title.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "ALBUMARTIST", cue.album_performer.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "SONGWRITER", cue.songwriter.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "ISRC", cue.isrc.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    // REM values retain source order at segment scope. The established CUE
    // policy selects the last DATE as the effective sidecar projection.
    append_metadata_value(row.metadata, "DATE", cue_remark(cue, "DATE"),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "TRACKNUMBER", std::to_string(cue.track_number),
                          metadata::FieldProvenance::sidecar);
    row.duration_ms = resolved.duration_ms;
    project_display_metadata(row);
    row.probed = true;
    return row;
}

} // namespace

// ADR-0355: a tab's rows that are bare paths -- dragged in, opened, taken
// over from a queue, or followed to where a file moved -- are filled in by
// the engine the tab is of, as every tab is: from its index, or from the
// file when the index does not have it. A file it cannot read keeps the row
// it had and is not asked about again: left unanswered, it would be asked
// on every look at the tab, ahead of everything else on the connection.
void Workspace::enqueueUnprobedRows(ListTab& tab) {
    // Asked of the engine the tab is of -- not reached, nothing to ask.
    auto* source = catalogueOf(EngineKey::of(tab.document));
    if (source == nullptr) {
        return;
    }
    std::vector<std::string> paths;
    std::unordered_set<std::string> asked;
    for (const auto& row : tab.model->rows()) {
        // A CUE track's title is its own, not the file's.
        if (!row.probed && !row.segment && asked.insert(row.raw_path).second) {
            paths.push_back(row.raw_path);
        }
    }
    if (paths.empty()) {
        return;
    }
    // Opened here, used there: it holds its own reference to the connection,
    // and connects there, not on this thread.
    std::shared_ptr<engine::Catalogue> catalogue{source->openDeferred()};
    using Found = std::vector<persistence::LibraryTrackSnapshot>;
    auto* watcher = new QFutureWatcher<Found>(this);
    const auto id = QString::fromStdString(tab.document.id.to_string());
    connect(watcher, &QFutureWatcher<Found>::finished, this,
            [this, watcher, id, asked = std::move(asked)] {
                watcher->deleteLater();
                auto found = watcher->result();
                auto* target = tabForDocument(id);
                if (target == nullptr) {
                    return;
                }
                std::unordered_map<std::string, LocalTrackRow> by_path;
                for (auto& snapshot : found) {
                    auto row = cached_library_row(std::move(snapshot));
                    auto path = row.raw_path;
                    by_path.emplace(std::move(path), std::move(row));
                }
                bool applied = false;
                for (int row = 0; row < target->model->rowCount(); ++row) {
                    const auto& current = target->model->rows()[static_cast<std::size_t>(row)];
                    if (current.probed || current.segment || !asked.contains(current.raw_path)) {
                        continue;
                    }
                    const auto match = by_path.find(current.raw_path);
                    auto filled = match != by_path.end() ? match->second : current;
                    keep_what_the_file_lacks(filled, current);
                    filled.selection = current.selection;
                    std::vector<LocalTrackRow> one;
                    one.push_back(std::move(filled));
                    applied =
                        target->model->applyProbeRows(current.raw_path, row, std::move(one)) ||
                        applied;
                }
                if (applied) {
                    schedulePersist();
                    syncArtwork(*target);
                }
            });
    watcher->setFuture(QtConcurrent::run([catalogue, paths = std::move(paths)] {
        auto read = catalogue->read_tracks(paths, {});
        return read ? std::move(*read) : Found{};
    }));
}

void Workspace::insertRemotePaths(ListTab& tab, std::vector<std::string> raw_paths,
                                  const int insertion_row) {
    std::vector<LocalTrackRow> rows;
    rows.reserve(raw_paths.size());
    for (auto& raw_path : raw_paths) {
        LocalTrackRow row;
        row.raw_path = std::move(raw_path);
        row.title = core::display_raw_path(row.raw_path.substr(row.raw_path.find_last_of('/') + 1));
        rows.push_back(std::move(row));
    }
    if (rows.empty()) {
        return;
    }
    tab.model->appendRows(std::move(rows), insertion_row);
    markTabDirty(tab);
    enqueueUnprobedRows(tab);
}

void Workspace::syncArtwork(ListTab& tab) {
    // A remote tab's files are on the remote's machine (ADR-0227), and
    // nothing of it need be mounted here: its covers come from its engine.
    std::shared_ptr<engine::Catalogue> engine;
    if (const auto key = EngineKey::of(tab.document); !key.isLocal()) {
        auto* source = catalogueOf(key);
        if (source == nullptr) {
            return;
        }
        engine = source->openDeferred();
    }
    const auto& rows = tab.model->rows();
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const auto& track = rows[static_cast<std::size_t>(row)];
        if (track.album.empty() && track.artist.empty() && track.album_artist.empty()) {
            continue;
        }
        const auto key = tab.model->groupKey(row);
        if (const auto cached = artwork_cache_.constFind(key);
            cached != artwork_cache_.constEnd()) {
            if (!cached->isNull() && !tab.model->hasArtwork(key)) {
                tab.model->setArtwork(key, *cached);
            }
            continue;
        }
        if (!artwork_pending_.contains(key)) {
            artwork_pending_.insert(key);
            artwork_queue_.push_back(
                ArtworkJob{.key = key, .raw_path = track.raw_path, .engine = engine});
        }
    }
    pumpArtworkQueue();
}

QImage Workspace::coverFor(const LocalTrackRow& track, const EngineKey& engine_key) {
    if (track.album.empty() && track.artist.empty() && track.album_artist.empty()) {
        return {};
    }
    const auto key = LocalListModel::groupKeyOf(track);
    for (const auto& tab : list_tabs_) {
        if (tab->model->hasArtwork(key)) {
            return tab->model->artwork(key);
        }
    }
    if (const auto cached = artwork_cache_.constFind(key); cached != artwork_cache_.constEnd()) {
        return *cached;
    }
    // Not in any list: fetched as a tab's would be, from the engine that has
    // the file when it is another's.
    std::shared_ptr<engine::Catalogue> engine;
    if (!engine_key.isLocal()) {
        auto* catalogue = catalogueOf(engine_key);
        if (catalogue == nullptr) {
            return {};
        }
        engine = catalogue->openDeferred();
    }
    if (!artwork_pending_.contains(key)) {
        artwork_pending_.insert(key);
        artwork_queue_.push_back(
            ArtworkJob{.key = key, .raw_path = track.raw_path, .engine = engine});
        pumpArtworkQueue();
    }
    return {};
}

void Workspace::invalidateArtwork(const std::string& raw_path) {
    invalidateArtwork(std::set<std::string>{raw_path});
}

void Workspace::invalidateArtwork(const std::set<std::string>& raw_paths) {
    if (raw_paths.empty()) {
        return;
    }
    QSet<QString> keys;
    for (const auto& tab : list_tabs_) {
        const auto& rows = tab->model->rows();
        for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
            if (raw_paths.contains(rows[static_cast<std::size_t>(row)].raw_path)) {
                keys.insert(tab->model->groupKey(row));
            }
        }
    }
    for (const auto& key : keys) {
        artwork_cache_.remove(key);
        artwork_pending_.remove(key);
        std::erase_if(artwork_queue_, [&key](const ArtworkJob& job) { return job.key == key; });
        if (artwork_running_ && artwork_outcome_ && artwork_outcome_->key == key) {
            artwork_invalidated_while_loading_.insert(key);
        }
        for (const auto& tab : list_tabs_) {
            tab->model->setArtwork(key, {});
        }
    }
    for (const auto& tab : list_tabs_) {
        syncArtwork(*tab);
    }
}

void Workspace::pumpArtworkQueue() {
    if (artwork_running_ || artwork_queue_.empty()) {
        return;
    }
    auto job = std::move(artwork_queue_.front());
    artwork_queue_.pop_front();
    artwork_running_ = true;
    connect(&artwork_watcher_, &QFutureWatcher<void>::finished, this, &Workspace::finishArtworkLoad,
            Qt::SingleShotConnection);
    artwork_outcome_ =
        std::make_shared<ArtworkOutcome>(ArtworkOutcome{.key = std::move(job.key), .image = {}});
    artwork_watcher_.setFuture(
        QtConcurrent::run([raw_path = std::move(job.raw_path), engine = std::move(job.engine),
                           outcome = artwork_outcome_, cancellation = work_cancellation_.token()] {
            if (engine) {
                const auto bytes = engine->artwork(raw_path, cancellation);
                outcome->image = bytes ? ui::artworkThumbnail(*bytes) : QImage{};
                outcome->failed = !bytes;
            } else {
                outcome->image = ui::loadLocalArtwork(raw_path, cancellation);
            }
#if defined(MELODY_THREAD_SANITIZER)
            __tsan_release(outcome.get());
#endif
        }));
}

void Workspace::finishArtworkLoad() {
    artwork_running_ = false;
    auto outcome = artwork_outcome_;
    if (!outcome) {
        pumpArtworkQueue();
        return;
    }
#if defined(MELODY_THREAD_SANITIZER)
    __tsan_acquire(outcome.get());
#endif
    if (artwork_invalidated_while_loading_.remove(outcome->key)) {
        // Invalidation already queued a fresh read and its pending marker now
        // owns this key. Discard only the stale in-flight result.
        pumpArtworkQueue();
        return;
    }
    if (outcome->failed) {
        // Not known to have no cover: asked again when the tab next looks.
        artwork_pending_.remove(outcome->key);
        pumpArtworkQueue();
        return;
    }
    // Failed lookups are cached as null so a missing cover is asked once, not
    // on every metadata refresh.
    artwork_cache_.insert(outcome->key, outcome->image);
    if (!outcome->image.isNull()) {
        for (const auto& tab : list_tabs_) {
            if (!tab->model->hasArtwork(outcome->key)) {
                tab->model->setArtwork(outcome->key, outcome->image);
            }
        }
        // Up Next and the header may have asked for it without a tab.
        if (up_next_local_model_ != nullptr) {
            for (int row = 0; row < up_next_local_model_->rowCount(); ++row) {
                if (up_next_local_model_->groupKey(row) == outcome->key) {
                    up_next_local_model_->setArtwork(outcome->key, outcome->image);
                    break;
                }
            }
        }
        view_->artworkLoaded(outcome->key);
    }
    // The cover a notification waits for: it goes now, with it.
    if (outcome->key == desktop_cover_waiting_) {
        desktop_cover_waiting_.clear();
        publishDesktopState();
    }
    pumpArtworkQueue();
}

void Workspace::startDiscovery(std::vector<std::string> raw_paths, QString target_document_id,
                               const int insertion_row, const bool replace_and_play) {
    if (discovery_running_) {
        view_->showMessage(QStringLiteral("A folder scan is already running"), 3'000);
        return;
    }
    discovery_running_ = true;
    discovery_target_document_ = std::move(target_document_id);
    discovery_insertion_row_ = insertion_row;
    const auto* target = tabForDocument(discovery_target_document_);
    discovery_insertion_anchor_ = target ? target->model->index(insertion_row, 0) : QModelIndex{};
    discovery_anchored_ = discovery_insertion_anchor_.isValid();
    discovery_replace_and_play_ = replace_and_play;
    connect(&discovery_watcher_, &QFutureWatcher<DiscoveryOutcome>::finished, this,
            &Workspace::finishDiscovery, Qt::SingleShotConnection);
    discovery_watcher_.setFuture(QtConcurrent::run([paths = std::move(raw_paths),
                                                    cancellation = work_cancellation_.token()] {
        auto discovered =
            core::discover_local_sources(std::span{paths.data(), paths.size()}, cancellation,
                                         discovery_row_limit, std::span{audio_extensions});
        DiscoveryOutcome outcome{.rows = {},
                                 .issues = std::move(discovered.issues),
                                 .cancelled = discovered.cancelled,
                                 .truncated = discovered.truncated};
        std::vector<std::optional<formats::ResolvedCueSheet>> resolved_cues(
            discovered.raw_files.size());
        std::unordered_set<std::string> cue_sources;
        std::unordered_map<std::string, metadata::LocalMetadataRead> cue_metadata;

        // Resolve cues first so an audio file referenced by a successfully
        // expanded sheet is not also inserted as one duplicate whole-file
        // row when both came from the same intake batch.
        for (std::size_t index = 0U; index < discovered.raw_files.size(); ++index) {
            if (cancellation.is_cancellation_requested()) {
                outcome.cancelled = true;
                break;
            }
            const auto& raw_path = discovered.raw_files[index];
            if (!is_cue_path(raw_path)) {
                continue;
            }
            auto resolved = formats::resolve_external_cue_sheet(raw_path, cancellation);
            if (!resolved) {
                outcome.issues.push_back(core::LocalSourceIssue{
                    .raw_path = raw_path, .error = std::move(resolved.error())});
                continue;
            }
            for (const auto& physical : resolved->physical_sources) {
                cue_sources.insert(physical);
                if (!cue_metadata.contains(physical)) {
                    if (auto read = metadata::read_local_metadata(physical, cancellation); read) {
                        cue_metadata.emplace(physical, std::move(*read));
                    }
                }
            }
            resolved_cues[index] = std::move(*resolved);
        }

        bool row_limit_reached = false;
        for (std::size_t index = 0U;
             index < discovered.raw_files.size() && !outcome.cancelled && !row_limit_reached;
             ++index) {
            const auto& raw_path = discovered.raw_files[index];
            if (is_cue_path(raw_path)) {
                if (!resolved_cues[index]) {
                    continue;
                }
                for (const auto& resolved_track : resolved_cues[index]->tracks) {
                    if (outcome.rows.size() == discovery_row_limit) {
                        outcome.truncated = true;
                        row_limit_reached = true;
                        outcome.issues.push_back(core::LocalSourceIssue{
                            .raw_path = raw_path,
                            .error =
                                core::Error{
                                    .code = core::ErrorCode::limit_exceeded,
                                    .message = "CUE expansion reached the local row limit",
                                    .context = {},
                                },
                        });
                        break;
                    }
                    const auto embedded = cue_metadata.find(resolved_track.raw_source_path);
                    if (embedded == cue_metadata.end()) {
                        outcome.rows.push_back(
                            cue_row(*resolved_cues[index], resolved_track, {}, std::nullopt));
                    } else {
                        outcome.rows.push_back(cue_row(*resolved_cues[index], resolved_track,
                                                       embedded->second.document,
                                                       embedded->second.source_revision));
                    }
                }
                continue;
            }

            std::error_code canonical_error;
            const auto canonical =
                std::filesystem::canonical(std::filesystem::path{raw_path}, canonical_error);
            if (!canonical_error && cue_sources.contains(canonical.native())) {
                continue;
            }
            if (outcome.rows.size() == discovery_row_limit) {
                outcome.truncated = true;
                row_limit_reached = true;
                outcome.issues.push_back(core::LocalSourceIssue{
                    .raw_path = raw_path,
                    .error =
                        core::Error{
                            .code = core::ErrorCode::limit_exceeded,
                            .message = "Local intake reached the row limit",
                            .context = {},
                        },
                });
                continue;
            }
            LocalTrackRow row;
            row.raw_path = raw_path;
            outcome.rows.push_back(std::move(row));
        }
        return outcome;
    }));
}

void Workspace::finishDiscovery() {
    discovery_running_ = false;
    auto result = discovery_watcher_.result();
    auto* tab = tabForDocument(discovery_target_document_);
    if (tab == nullptr || (discovery_anchored_ && !discovery_insertion_anchor_.isValid())) {
        return;
    }
    // This computer's files found for another engine's list: each as that
    // engine reaches it -- the library's music mounted here, say. The rest
    // can play only here: into a list of this computer's, not left out.
    if (const auto into = EngineKey::of(tab->document); !into.isLocal() && !result.rows.empty()) {
        std::vector<LocalTrackRow> reached;
        std::vector<LocalTrackRow> here;
        for (auto& row : result.rows) {
            if (auto translated = crossEnginePath(row.raw_path, EngineKey::local(), into)) {
                row.raw_path = std::move(*translated);
                // Known of the file here says nothing of it there.
                row.source_revision.reset();
                reached.push_back(std::move(row));
            } else {
                here.push_back(std::move(row));
            }
        }
        result.rows = std::move(reached);
        if (!here.empty()) {
            const auto count = static_cast<int>(here.size());
            auto* local =
                addList(persistence::ListDocument{.id = core::StableId::random(),
                                                  .kind = persistence::ListKind::scratch,
                                                  .name = utf8Bytes(tr("On this computer")),
                                                  .pinned = false,
                                                  .dirty = false,
                                                  .items = {},
                                                  .engine = {}},
                        result.rows.empty());
            local->model->appendRows(std::move(here), -1);
            markTabDirty(*local);
            enqueueUnprobedRows(*local);
            syncArtwork(*local);
            schedulePersist();
            view_->showMessage(tr("%n track(s) not in %1's library went into a list of this "
                                  "computer's, played here",
                                  nullptr, count)
                                   .arg(engineName(into)),
                               8'000);
        }
    }
    if (!result.rows.empty() && (!discovery_replace_and_play_ || !result.cancelled)) {
        if (discovery_replace_and_play_) {
            tab->model->replaceRows(std::move(result.rows), true);
            playRow(*tab, 0);
        } else {
            tab->model->appendRows(std::move(result.rows), discovery_anchored_
                                                               ? discovery_insertion_anchor_.row()
                                                               : discovery_insertion_row_);
        }
        markTabDirty(*tab);
        enqueueUnprobedRows(*tab);
        syncArtwork(*tab);
    }
    if (!result.issues.empty()) {
        view_->showMessage(
            QStringLiteral("%1 entr%2 could not be opened")
                .arg(result.issues.size())
                .arg(result.issues.size() == 1U ? QStringLiteral("y") : QStringLiteral("ies")),
            5'000);
    }
    if (result.truncated) {
        view_->showMessage(QStringLiteral("Folder scan hit the file limit"), 5'000);
    }
}

} // namespace trackknife::bench
