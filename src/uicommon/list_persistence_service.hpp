// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/persistence/list_repository.hpp"

#include <QByteArray>
#include <QObject>

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

class QThread;

namespace trackknife::engine {
class Workspace;
}

namespace trackknife::ui {

class ListPersistenceService final : public QObject {
    Q_OBJECT

  public:
    using CompletionCallback = std::function<void(QString)>;
    using UiStateCallback = std::function<void(QByteArray, QString)>;
    using TransformationChainsCallback =
        std::function<void(std::vector<persistence::SavedMetadataTransformationChain>, QString)>;
    using OutputProfilesCallback =
        std::function<void(std::vector<persistence::SavedOutputLayoutProfile>,
                           std::vector<persistence::SavedDestinationProfile>, QString)>;
    using EncoderPresetsCallback =
        std::function<void(std::vector<persistence::SavedEncoderPreset>, QString)>;

    explicit ListPersistenceService(std::filesystem::path database_path, QObject* parent = nullptr);
    ~ListPersistenceService() override;

    ListPersistenceService(const ListPersistenceService&) = delete;
    ListPersistenceService& operator=(const ListPersistenceService&) = delete;

    // Opens the database; the callback is told what failed, if anything.
    void initialize(CompletionCallback callback);
    void loadMetadataTransformationChains(TransformationChainsCallback callback);
    void saveMetadataTransformationChain(persistence::SavedMetadataTransformationChain chain,
                                         CompletionCallback callback = {});
    void removeMetadataTransformationChain(core::StableId id, CompletionCallback callback = {});
    void loadOutputProfiles(OutputProfilesCallback callback);
    void saveOutputLayoutProfile(persistence::SavedOutputLayoutProfile profile,
                                 CompletionCallback callback = {});
    void removeOutputLayoutProfile(core::StableId id, CompletionCallback callback = {});
    void saveDestinationProfile(persistence::SavedDestinationProfile profile,
                                CompletionCallback callback = {});
    void removeDestinationProfile(core::StableId id, CompletionCallback callback = {});
    void loadEncoderPresets(EncoderPresetsCallback callback);
    void saveEncoderPreset(persistence::SavedEncoderPreset preset,
                           CompletionCallback callback = {});
    void removeEncoderPreset(core::StableId id, CompletionCallback callback = {});
    void loadUiState(QString key, UiStateCallback callback);
    void saveUiState(QString key, QByteArray value, CompletionCallback callback = {});
    void backupDatabase(std::filesystem::path destination, CompletionCallback callback);

  private:
    struct State;
    QThread* thread_{nullptr};
    QObject* worker_{nullptr};
    std::shared_ptr<State> state_;
};

} // namespace trackknife::ui
