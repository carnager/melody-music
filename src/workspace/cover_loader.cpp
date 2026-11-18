// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/cover_loader.hpp"

#include "uicommon/local_artwork.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace trackknife::bench {

CoverLoader::CoverLoader(const int kept, QObject* parent) : QObject(parent), cache_(kept) {
    pool_.setMaxThreadCount(1);
    connect(&watcher_, &QFutureWatcherBase::finished, this, &CoverLoader::finish);
}

CoverLoader::~CoverLoader() { stop(); }

std::optional<QImage> CoverLoader::cached(const QString& key) const {
    const auto* image = cache_.object(key);
    return image != nullptr ? std::optional{*image} : std::nullopt;
}

bool CoverLoader::pending(const QString& key) const {
    return queued_.contains(key) || (loading_ && !discard_ && loading_key_ == key);
}

void CoverLoader::request(const QString& key, Fetch fetch) {
    if (stopped_ || cache_.contains(key) || pending(key)) {
        return;
    }
    queued_.insert(key);
    queue_.push_back(Job{.key = key, .fetch = std::move(fetch)});
    pump();
}

void CoverLoader::keepOnly(const QSet<QString>& keys) {
    std::erase_if(queue_, [this, &keys](const Job& job) {
        if (keys.contains(job.key)) {
            return false;
        }
        queued_.remove(job.key);
        return true;
    });
    if (loading_ && !keys.contains(loading_key_)) {
        discard_ = true;
        cancellation_.request_cancellation();
    }
}

void CoverLoader::forget(const QString& key) {
    cache_.remove(key);
    if (queued_.remove(key)) {
        std::erase_if(queue_, [&key](const Job& job) { return job.key == key; });
    }
    if (loading_ && loading_key_ == key) {
        discard_ = true;
    }
}

void CoverLoader::clear() {
    cache_.clear();
    queue_.clear();
    queued_.clear();
    if (loading_) {
        discard_ = true;
        cancellation_.request_cancellation();
    }
}

void CoverLoader::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    clear();
    watcher_.waitForFinished();
    pool_.waitForDone();
}

void CoverLoader::pump() {
    if (stopped_ || loading_ || queue_.empty()) {
        return;
    }
    auto job = std::move(queue_.front());
    queue_.pop_front();
    queued_.remove(job.key);
    loading_ = true;
    loading_key_ = job.key;
    discard_ = false;
    cancellation_ = core::CancellationSource{};
    watcher_.setFuture(QtConcurrent::run(
        &pool_, [fetch = std::move(job.fetch), cancellation = cancellation_.token()] {
            if (cancellation.is_cancellation_requested()) {
                return Answer{.image = {}, .failed = true};
            }
            const auto bytes = fetch(cancellation);
            if (!bytes || cancellation.is_cancellation_requested()) {
                return Answer{.image = {}, .failed = true};
            }
            return Answer{.image = bytes->empty() ? QImage{} : ui::artworkThumbnail(*bytes),
                          .failed = false};
        }));
}

void CoverLoader::finish() {
    loading_ = false;
    if (stopped_) {
        return;
    }
    const auto answer = watcher_.result();
    if (!discard_ && !answer.failed) {
        cache_.insert(loading_key_, new QImage(answer.image));
        emit loaded(loading_key_, answer.image);
    }
    discard_ = false;
    pump();
}

} // namespace trackknife::bench
