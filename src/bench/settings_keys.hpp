// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/metadata/artwork_write_plan.hpp"

#include <QString>

#include <string>

namespace trackknife::bench {

// The QSettings keys the Settings dialog writes and the rest of the
// workspace reads. Kept apart from the dialog so that what reads them --
// the engine launcher, the remote engines, the window -- needs no widgets.
struct SettingsKeys {
    // How covers are stored, as Settings › Covers says (ADR-0091).
    [[nodiscard]] static metadata::ArtworkStoragePolicy artworkPolicy();

    static constexpr auto acoustid_client_key = "musicbrainz/acoustid-client-key";
    // ADR-0360: Identify also asks Discogs where a token is set.
    static constexpr auto discogs_token_key = "musicbrainz/discogs-token";
    // ADR-0237 stage 2: engines also write track ratings into the files.
    static constexpr auto ratings_in_tags_key = "library/ratings-in-tags";
    // The scale other players' plain RATING tags are read on: off, 5, 10, 100.
    static constexpr auto rating_tag_scale_key = "library/rating-tag-scale";
    // ADR-0245: ratings also copied, as their 0-10 number, into a tag of the
    // user's naming -- when they are written at all.
    static constexpr auto rating_backup_key = "library/rating-backup";
    static constexpr auto rating_backup_tag_key = "library/rating-backup-tag";
    // The tag the engines copy ratings into: empty when off, or when the name
    // chosen cannot hold it.
    [[nodiscard]] static std::string ratingBackupTag();
    // ADR-0227: this computer's engine, when it is one already running rather
    // than the one the workspace starts. Not shown in Settings: it is for
    // tests and for developing the engine, which run their own.
    static constexpr auto library_local_engine_socket_key = "library/local-engine-socket";
    // ADR-0286: the engine holding the library -- "local", or an engine
    // elsewhere by its address (one of loadRemoteEngines()). Unset, or an
    // engine since removed: this computer's.
    static constexpr auto library_engine_key = "library/engine";
    static constexpr auto library_engine_local = "local";
    // ADR-0235: this computer's engine finds UPnP renderers and plays on them.
    static constexpr auto engine_upnp_key = "engine/upnp";
    static constexpr auto engine_share_key = "engine/share";
    static constexpr auto engine_listen_key = "engine/listen";
    static constexpr auto engine_listen_default = "0.0.0.0:6600";
    static constexpr auto engine_stream_port_key = "engine/stream-port";
    static constexpr int engine_stream_port_default = 6601;
    // The password of the engines on this network: this computer's, and an
    // engine elsewhere's unless it has its own (RemoteEngineSetting).
    static constexpr auto engine_password_key = "engine/password";
    static constexpr auto engine_music_root_key = "engine/music-root";
    // ADR-0228: the remote engine may play on this computer's speakers
    // through this computer's engine -- no melody-agent needed here.
    static constexpr auto engine_play_for_remote_key = "engine/play-for-remote";
    // ADR-0239: what engines elsewhere stream to those speakers, in kbps of
    // Opus, 0 the original files: when nearby, and when reached through a
    // VPN or a router.
    static constexpr auto engine_stream_nearby_key = "engine/stream-nearby-kbps";
    static constexpr int engine_stream_nearby_default = 0;
    static constexpr auto engine_stream_away_key = "engine/stream-away-kbps";
    static constexpr int engine_stream_away_default = 128;
    // ADR-0286: the settings above are in this computer's engine's
    // melodyd.conf, handed over once; after that each change is.
    static constexpr auto engine_settings_in_config_key = "engine/settings-in-config";
    static constexpr auto replaygain_sidecar_only_key = "replaygain/sidecar-only";
    static constexpr auto replaygain_true_peak_key = "replaygain/true-peak";
    static constexpr auto artwork_embed_key = "artwork/embed";
    static constexpr auto artwork_folder_image_key = "artwork/write-folder-image";
    static constexpr auto artwork_folder_image_name_key = "artwork/folder-image-name";
    static constexpr auto artwork_fetch_source_key = "artwork/fetch-source";
    // Longest edge of a newly written cover, in pixels; 0 is no limit.
    static constexpr auto artwork_max_embedded_edge_key = "artwork/max-embedded-edge";
    static constexpr auto artwork_max_folder_edge_key = "artwork/max-folder-edge";
    // ADR-0317: covers written at most this large unless set otherwise.
    static constexpr int artwork_default_max_edge = 1000;
};

} // namespace trackknife::bench
