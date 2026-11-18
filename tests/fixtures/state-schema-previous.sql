-- SPDX-License-Identifier: GPL-3.0-only
-- The state schema the upgrade step starts from (version 57); replaced with
-- each schema change (src/persistence/src/current_schema.hpp, ADR-0354).
CREATE TABLE list_documents (id TEXT PRIMARY KEY NOT NULL, kind INTEGER NOT NULL, name BLOB NOT NULL, pinned INTEGER NOT NULL CHECK(pinned IN (0,1)), dirty INTEGER NOT NULL CHECK(dirty IN (0,1)), position INTEGER NOT NULL UNIQUE, remote INTEGER NOT NULL DEFAULT 0, engine TEXT NOT NULL DEFAULT '');
CREATE TABLE list_items (document_id TEXT NOT NULL REFERENCES list_documents(id) ON DELETE CASCADE, position INTEGER NOT NULL, source INTEGER NOT NULL, profile_id TEXT, source_reference BLOB NOT NULL, duration_ms INTEGER, logical_reference BLOB, segment_start_sample INTEGER, segment_end_sample INTEGER, selected_audio_stream INTEGER, codec_subsong_index INTEGER, observed_device BLOB, observed_inode BLOB, observed_size BLOB, observed_mtime_seconds BLOB, observed_mtime_nanoseconds BLOB, entry_id TEXT, PRIMARY KEY(document_id, position));
CREATE TABLE list_item_fields (document_id TEXT NOT NULL, item_position INTEGER NOT NULL, position INTEGER NOT NULL, name BLOB NOT NULL, value BLOB NOT NULL, native_name BLOB NOT NULL DEFAULT X'', provenance INTEGER NOT NULL DEFAULT 0 CHECK(provenance BETWEEN 0 AND 5), language BLOB, description BLOB, PRIMARY KEY(document_id, item_position, position), FOREIGN KEY(document_id, item_position) REFERENCES list_items(document_id, position) ON DELETE CASCADE);
CREATE TABLE connection_profiles (id TEXT PRIMARY KEY NOT NULL, name BLOB NOT NULL, host BLOB NOT NULL, port INTEGER NOT NULL CHECK(port BETWEEN 1 AND 65535), local_music_root BLOB, auto_connect INTEGER NOT NULL CHECK(auto_connect IN (0,1)), position INTEGER NOT NULL UNIQUE);
CREATE TABLE track_view_presets (binding BLOB PRIMARY KEY NOT NULL, header_state BLOB NOT NULL, position INTEGER NOT NULL UNIQUE);
CREATE TABLE operation_journal_occurrences (journal_id TEXT NOT NULL REFERENCES operation_journal(id) ON DELETE CASCADE, position INTEGER NOT NULL, item_index INTEGER NOT NULL, PRIMARY KEY(journal_id, position));
CREATE TABLE operation_journal_changes (journal_id TEXT NOT NULL REFERENCES operation_journal(id) ON DELETE CASCADE, position INTEGER NOT NULL, field_index INTEGER NOT NULL, canonical_name BLOB NOT NULL, property_name BLOB NOT NULL, original_present INTEGER NOT NULL CHECK(original_present IN (0,1)), patch_kind INTEGER NOT NULL CHECK(patch_kind BETWEEN 0 AND 1), exact_native_name BLOB, PRIMARY KEY(journal_id, position));
CREATE TABLE operation_journal_values (journal_id TEXT NOT NULL, change_position INTEGER NOT NULL, value_kind INTEGER NOT NULL CHECK(value_kind BETWEEN 0 AND 1), position INTEGER NOT NULL, value BLOB NOT NULL, PRIMARY KEY(journal_id, change_position, value_kind, position), FOREIGN KEY(journal_id, change_position) REFERENCES operation_journal_changes(journal_id, position) ON DELETE CASCADE);
CREATE TABLE operation_journal_intents (journal_id TEXT NOT NULL, change_position INTEGER NOT NULL, position INTEGER NOT NULL, item_index INTEGER NOT NULL, PRIMARY KEY(journal_id, change_position, position), FOREIGN KEY(journal_id, change_position) REFERENCES operation_journal_changes(journal_id, position) ON DELETE CASCADE);
CREATE TABLE local_metadata_cache (source_reference BLOB PRIMARY KEY NOT NULL, previous_device BLOB NOT NULL, previous_inode BLOB NOT NULL, previous_size BLOB NOT NULL, previous_mtime_seconds BLOB NOT NULL, previous_mtime_nanoseconds BLOB NOT NULL, published_device BLOB NOT NULL, published_inode BLOB NOT NULL, published_size BLOB NOT NULL, published_mtime_seconds BLOB NOT NULL, published_mtime_nanoseconds BLOB NOT NULL);
CREATE TABLE local_metadata_cache_fields (source_reference BLOB NOT NULL REFERENCES local_metadata_cache(source_reference) ON DELETE CASCADE, position INTEGER NOT NULL, name BLOB NOT NULL, value BLOB NOT NULL, native_name BLOB NOT NULL, provenance INTEGER NOT NULL CHECK(provenance BETWEEN 0 AND 5), language BLOB, description BLOB, PRIMARY KEY(source_reference, position));
CREATE TABLE local_metadata_refreshes (operation_id TEXT PRIMARY KEY NOT NULL, source_reference BLOB NOT NULL, previous_device BLOB NOT NULL, previous_inode BLOB NOT NULL, previous_size BLOB NOT NULL, previous_mtime_seconds BLOB NOT NULL, previous_mtime_nanoseconds BLOB NOT NULL, published_device BLOB NOT NULL, published_inode BLOB NOT NULL, published_size BLOB NOT NULL, published_mtime_seconds BLOB NOT NULL, published_mtime_nanoseconds BLOB NOT NULL, affected_occurrences INTEGER NOT NULL CHECK(affected_occurrences > 0));
CREATE TABLE metadata_operation_backups (journal_id TEXT PRIMARY KEY NOT NULL REFERENCES operation_journal(id) ON DELETE CASCADE, state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4), undo_id TEXT, completed_at_unix_seconds INTEGER NOT NULL, updated_at_unix_seconds INTEGER NOT NULL, error_code INTEGER, error_message BLOB);
CREATE TABLE metadata_transformation_chains (id TEXT PRIMARY KEY NOT NULL, schema_version INTEGER NOT NULL CHECK(schema_version = 1), name BLOB NOT NULL UNIQUE, automatic INTEGER NOT NULL DEFAULT 0 CHECK(automatic IN (0,1)));
CREATE TABLE output_layout_profiles (id TEXT PRIMARY KEY NOT NULL, schema_version INTEGER NOT NULL CHECK(schema_version = 1), name BLOB NOT NULL UNIQUE, dialect BLOB NOT NULL, dialect_version INTEGER NOT NULL, compiler_schema INTEGER NOT NULL, relative_directory_expression BLOB NOT NULL, basename_expression BLOB NOT NULL, sanitization_policy BLOB NOT NULL, sanitization_version INTEGER NOT NULL);
CREATE TABLE destination_profiles (id TEXT PRIMARY KEY NOT NULL, schema_version INTEGER NOT NULL CHECK(schema_version = 1), name BLOB NOT NULL UNIQUE, root_raw_path BLOB NOT NULL, containment_policy BLOB NOT NULL, containment_version INTEGER NOT NULL);
CREATE TABLE file_publication_journal (id TEXT PRIMARY KEY NOT NULL, state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 7), publication_kind INTEGER NOT NULL CHECK(publication_kind BETWEEN 1 AND 2), source_path BLOB NOT NULL, target_path BLOB NOT NULL, prepared_path BLOB NOT NULL, expected_device BLOB NOT NULL, expected_inode BLOB NOT NULL, expected_size BLOB NOT NULL, expected_mtime_seconds BLOB NOT NULL, expected_mtime_nanoseconds BLOB NOT NULL, prepared_device BLOB, prepared_inode BLOB, prepared_size BLOB, prepared_mtime_seconds BLOB, prepared_mtime_nanoseconds BLOB, target_device BLOB, target_inode BLOB, target_size BLOB, target_mtime_seconds BLOB, target_mtime_nanoseconds BLOB, error_code INTEGER, error_message BLOB, reverses_id TEXT REFERENCES file_publication_journal(id), content_kind INTEGER NOT NULL DEFAULT 0 CHECK(content_kind BETWEEN 0 AND 1));
CREATE TABLE file_publication_journal_occurrences (journal_id TEXT NOT NULL REFERENCES file_publication_journal(id) ON DELETE CASCADE, position INTEGER NOT NULL, item_index INTEGER NOT NULL, PRIMARY KEY(journal_id, position));
CREATE TABLE file_publication_journal_directories (journal_id TEXT NOT NULL REFERENCES file_publication_journal(id) ON DELETE CASCADE, position INTEGER NOT NULL, raw_path BLOB NOT NULL, PRIMARY KEY(journal_id, position));
CREATE TABLE local_source_relocations (sequence INTEGER PRIMARY KEY AUTOINCREMENT, operation_id TEXT NOT NULL UNIQUE, source_reference BLOB NOT NULL, target_reference BLOB NOT NULL, previous_device BLOB NOT NULL, previous_inode BLOB NOT NULL, previous_size BLOB NOT NULL, previous_mtime_seconds BLOB NOT NULL, previous_mtime_nanoseconds BLOB NOT NULL, published_device BLOB NOT NULL, published_inode BLOB NOT NULL, published_size BLOB NOT NULL, published_mtime_seconds BLOB NOT NULL, published_mtime_nanoseconds BLOB NOT NULL, affected_occurrences INTEGER NOT NULL CHECK(affected_occurrences > 0), cache_rekeyed INTEGER NOT NULL CHECK(cache_rekeyed IN (0,1)), metadata_refreshed INTEGER NOT NULL DEFAULT 0 CHECK(metadata_refreshed IN (0,1)), CHECK(source_reference != target_reference));
CREATE TABLE musicbrainz_response_cache (url BLOB PRIMARY KEY NOT NULL, body BLOB NOT NULL, fetched_at_unix_seconds INTEGER NOT NULL CHECK(fetched_at_unix_seconds >= 0));
CREATE TABLE encoder_presets (id TEXT PRIMARY KEY NOT NULL, preset_id BLOB NOT NULL, preset_version INTEGER NOT NULL CHECK(preset_version >= 1), display_name BLOB NOT NULL UNIQUE, codec_name BLOB NOT NULL, container_name BLOB NOT NULL, file_extension BLOB NOT NULL, lossless INTEGER NOT NULL CHECK(lossless IN (0, 1)), bit_rate INTEGER, vbr_quality INTEGER, sample_format_hint BLOB NOT NULL);
CREATE TABLE local_library_roots (raw_path BLOB PRIMARY KEY NOT NULL, available INTEGER NOT NULL DEFAULT 0 CHECK(available IN (0,1)), error TEXT NOT NULL DEFAULT '', scan_token TEXT NOT NULL DEFAULT '');
CREATE TABLE local_library_tracks (raw_path BLOB PRIMARY KEY NOT NULL, root BLOB NOT NULL REFERENCES local_library_roots(raw_path) ON DELETE CASCADE, revision TEXT NOT NULL, title TEXT NOT NULL, artist TEXT NOT NULL, album TEXT NOT NULL, album_key TEXT NOT NULL, release_id TEXT NOT NULL, date TEXT NOT NULL, disc INTEGER NOT NULL, track INTEGER NOT NULL, search_track TEXT NOT NULL, search_album TEXT NOT NULL, available INTEGER NOT NULL CHECK(available IN (0,1)), seen TEXT NOT NULL, codec_name TEXT NOT NULL DEFAULT '', sample_rate INTEGER NOT NULL DEFAULT 0, bits INTEGER NOT NULL DEFAULT 0, channels INTEGER NOT NULL DEFAULT 0, duration_ms INTEGER NOT NULL DEFAULT -1, field_index_complete INTEGER NOT NULL DEFAULT 0 CHECK(field_index_complete IN (0,1)), rating_hash TEXT NOT NULL DEFAULT '', album_rating_hash TEXT NOT NULL DEFAULT '', added INTEGER NOT NULL DEFAULT 0);
CREATE TABLE local_library_fields (raw_path BLOB NOT NULL REFERENCES local_library_tracks(raw_path) ON DELETE CASCADE, canonical_name TEXT NOT NULL, position INTEGER NOT NULL, value BLOB NOT NULL, value_lower BLOB NOT NULL, PRIMARY KEY (raw_path, canonical_name, position));
CREATE TABLE operation_journal_artwork (
    journal_id TEXT PRIMARY KEY NOT NULL
        REFERENCES operation_journal(id) ON DELETE CASCADE,
    intent_kind INTEGER NOT NULL CHECK(intent_kind BETWEEN 0 AND 3),
    target_ordinal INTEGER NOT NULL CHECK(target_ordinal >= 0),
    original_item_count INTEGER NOT NULL CHECK(original_item_count >= 0),
    planned_item_count INTEGER NOT NULL CHECK(planned_item_count >= 0),
    original_target_fingerprint BLOB
        CHECK(original_target_fingerprint IS NULL OR
              length(original_target_fingerprint) = 32),
    replacement_fingerprint BLOB
        CHECK(replacement_fingerprint IS NULL OR length(replacement_fingerprint) = 32),
    original_inventory_fingerprint BLOB NOT NULL
        CHECK(length(original_inventory_fingerprint) = 32),
    planned_inventory_fingerprint BLOB NOT NULL
        CHECK(length(planned_inventory_fingerprint) = 32),
    CHECK((intent_kind = 0 AND original_target_fingerprint IS NOT NULL AND
           replacement_fingerprint IS NOT NULL AND
           planned_item_count = original_item_count AND
           target_ordinal < original_item_count) OR
          (intent_kind = 1 AND original_target_fingerprint IS NOT NULL AND
           replacement_fingerprint IS NULL AND
           planned_item_count + 1 = original_item_count AND
           target_ordinal < original_item_count) OR
          (intent_kind = 2 AND original_target_fingerprint IS NULL AND
           replacement_fingerprint IS NOT NULL AND
           target_ordinal = original_item_count AND
           planned_item_count = original_item_count + 1) OR
          (intent_kind = 3 AND target_ordinal = 0 AND original_target_fingerprint IS NULL AND replacement_fingerprint IS NULL))
);
CREATE TABLE local_ratings (
    hash TEXT PRIMARY KEY,
    type TEXT NOT NULL CHECK(type IN ('track','album')),
    rating INTEGER NOT NULL CHECK(rating BETWEEN 1 AND 10),
    updated_at TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE TABLE saved_searches (
    id TEXT PRIMARY KEY NOT NULL,
    name BLOB NOT NULL UNIQUE CHECK(length(name) BETWEEN 1 AND 256),
    expression BLOB NOT NULL CHECK(length(expression) BETWEEN 1 AND 4096),
    dialect TEXT NOT NULL CHECK(dialect IN ('tkq-1', 'words-1')),
    scope INTEGER NOT NULL CHECK(scope IN (0, 1, 2)),
    revision INTEGER NOT NULL CHECK(revision > 0)
);
CREATE TABLE IF NOT EXISTS "operation_journal" (id TEXT PRIMARY KEY NOT NULL, kind INTEGER NOT NULL CHECK(kind = 0), state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 5), source_path BLOB NOT NULL, prepared_path BLOB NOT NULL, backup_path BLOB NOT NULL, expected_device BLOB NOT NULL, expected_inode BLOB NOT NULL, expected_size BLOB NOT NULL, expected_mtime_seconds BLOB NOT NULL, expected_mtime_nanoseconds BLOB NOT NULL, prepared_device BLOB, prepared_inode BLOB, prepared_size BLOB, prepared_mtime_seconds BLOB, prepared_mtime_nanoseconds BLOB, published_device BLOB, published_inode BLOB, published_size BLOB, published_mtime_seconds BLOB, published_mtime_nanoseconds BLOB, error_code INTEGER, error_message BLOB, content_kind INTEGER NOT NULL DEFAULT 0 CHECK(content_kind BETWEEN 0 AND 4), backup_device BLOB, backup_inode BLOB, backup_size BLOB, backup_mtime_seconds BLOB, backup_mtime_nanoseconds BLOB, in_place INTEGER NOT NULL DEFAULT 0
    CHECK(in_place IN (0, 1)));
CREATE TABLE local_listening_history (
    track_hash TEXT PRIMARY KEY NOT NULL,
    play_count INTEGER NOT NULL DEFAULT 0 CHECK(play_count >= 0),
    last_played_ms INTEGER NOT NULL DEFAULT 0 CHECK(last_played_ms >= 0),
    resume_position_ms INTEGER NOT NULL DEFAULT 0 CHECK(resume_position_ms >= 0),
    resume_updated_at_ms INTEGER NOT NULL DEFAULT 0 CHECK(resume_updated_at_ms >= 0)
);
CREATE TABLE local_listening_sources (
    observation_hash TEXT PRIMARY KEY NOT NULL,
    source_id TEXT NOT NULL
);
CREATE TABLE local_listening_occurrences (
    occurrence_id TEXT PRIMARY KEY NOT NULL,
    track_hash TEXT NOT NULL,
    played_at_ms INTEGER NOT NULL CHECK(played_at_ms > 0)
);
CREATE TABLE engine_state (
    key TEXT PRIMARY KEY NOT NULL,
    value TEXT NOT NULL,
    updated_at_ms INTEGER NOT NULL
);
CREATE TABLE local_library_revision (
    id TEXT NOT NULL,
    revision INTEGER NOT NULL
);
INSERT INTO local_library_revision(id, revision) VALUES (lower(hex(randomblob(8))), 1);
CREATE TABLE engine_lists (
    id TEXT PRIMARY KEY NOT NULL,
    name BLOB NOT NULL,
    -- 0 a working list, closed and gone with its tab; 1 saved, kept until
    -- deleted. Saving a working list is changing this.
    kind INTEGER NOT NULL CHECK(kind IN (0, 1)),
    revision INTEGER NOT NULL CHECK(revision > 0),
    created_ms INTEGER NOT NULL,
    modified_ms INTEGER NOT NULL
, draft_of TEXT, draft_base INTEGER);
CREATE TABLE engine_list_items (
    list_id TEXT NOT NULL REFERENCES engine_lists(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    entry_id TEXT NOT NULL,
    raw_path BLOB NOT NULL,
    logical_reference BLOB,
    segment_start_sample INTEGER,
    segment_end_sample INTEGER,
    audio_stream_index INTEGER,
    subsong_index INTEGER,
    duration_ms INTEGER,
    title BLOB NOT NULL DEFAULT X'',
    artist BLOB NOT NULL DEFAULT X'',
    album BLOB NOT NULL DEFAULT X'', album_artist BLOB NOT NULL DEFAULT X'', date BLOB NOT NULL DEFAULT X'', track_gain_db REAL, track_peak REAL, album_gain_db REAL, album_peak REAL,
    PRIMARY KEY(list_id, position),
    UNIQUE(list_id, entry_id)
);
CREATE TABLE metadata_transformation_actions (
    chain_id TEXT NOT NULL REFERENCES metadata_transformation_chains(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 20),
    target_field BLOB NOT NULL,
    argument BLOB,
    dialect BLOB,
    dialect_version INTEGER,
    compiler_schema INTEGER,
    integer_argument INTEGER,
    integer_argument_2 INTEGER,
    PRIMARY KEY(chain_id, position)
);
CREATE TABLE metadata_transformation_action_values (
    chain_id TEXT NOT NULL,
    action_position INTEGER NOT NULL,
    position INTEGER NOT NULL,
    value BLOB NOT NULL,
    PRIMARY KEY(chain_id, action_position, position),
    FOREIGN KEY(chain_id, action_position)
        REFERENCES metadata_transformation_actions(chain_id, position) ON DELETE CASCADE
);
CREATE TABLE file_publication_backups (
    journal_id TEXT PRIMARY KEY NOT NULL
        REFERENCES file_publication_journal(id) ON DELETE CASCADE,
    state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),
    undo_id TEXT,
    completed_at_unix_seconds INTEGER NOT NULL,
    updated_at_unix_seconds INTEGER NOT NULL,
    error_code INTEGER,
    error_message BLOB
, kept_path BLOB, kept_device BLOB, kept_inode BLOB, kept_size BLOB, kept_mtime_seconds BLOB, kept_mtime_nanoseconds BLOB);
CREATE TABLE retired_engine_lists(
  id TEXT,
  name,
  kind INT,
  revision INT,
  created_ms INT,
  modified_ms INT,
  draft_of TEXT,
  draft_base INT
);
CREATE TABLE retired_engine_list_items(
  list_id TEXT,
  position INT,
  entry_id TEXT,
  raw_path,
  logical_reference,
  segment_start_sample INT,
  segment_end_sample INT,
  audio_stream_index INT,
  subsong_index INT,
  duration_ms INT,
  title,
  artist,
  album,
  album_artist,
  date,
  track_gain_db REAL,
  track_peak REAL,
  album_gain_db REAL,
  album_peak REAL
);
CREATE TABLE local_library_mounts (mount_point BLOB PRIMARY KEY NOT NULL,
    filesystem TEXT NOT NULL, source BLOB NOT NULL);
CREATE TRIGGER local_library_revision_insert AFTER INSERT ON local_library_tracks
BEGIN
    UPDATE local_library_revision SET revision = revision + 1;
END;
CREATE TRIGGER local_library_revision_delete AFTER DELETE ON local_library_tracks
BEGIN
    UPDATE local_library_revision SET revision = revision + 1;
END;
CREATE TRIGGER local_library_revision_update AFTER UPDATE ON local_library_tracks
WHEN OLD.title IS NOT NEW.title OR OLD.artist IS NOT NEW.artist OR OLD.album IS NOT NEW.album
    OR OLD.album_key IS NOT NEW.album_key OR OLD.date IS NOT NEW.date OR OLD.disc IS NOT NEW.disc
    OR OLD.track IS NOT NEW.track OR OLD.available IS NOT NEW.available
    OR OLD.duration_ms IS NOT NEW.duration_ms
BEGIN
    UPDATE local_library_revision SET revision = revision + 1;
END;
CREATE INDEX list_item_fields_item ON list_item_fields(document_id, item_position, position);
CREATE INDEX metadata_operation_backups_state_time ON metadata_operation_backups(state, completed_at_unix_seconds DESC);
CREATE INDEX file_publication_journal_state ON file_publication_journal(state);
CREATE INDEX local_source_relocations_source ON local_source_relocations(source_reference, sequence);
CREATE INDEX file_publication_journal_reverses ON file_publication_journal(reverses_id);
CREATE INDEX musicbrainz_response_cache_age ON musicbrainz_response_cache(fetched_at_unix_seconds);
CREATE INDEX local_library_artist ON local_library_tracks(artist,album_key,disc,track);
CREATE INDEX local_library_album ON local_library_tracks(album_key,disc,track);
CREATE INDEX local_library_root ON local_library_tracks(root,seen);
CREATE INDEX local_library_fields_lookup ON local_library_fields(canonical_name, value_lower);
CREATE INDEX local_library_incomplete_fields ON local_library_tracks(raw_path) WHERE available=1 AND field_index_complete=0;
CREATE INDEX local_library_rating_hash ON local_library_tracks(rating_hash);
CREATE INDEX local_library_album_rating_hash ON local_library_tracks(album_rating_hash);
CREATE INDEX operation_journal_state ON operation_journal(state);
CREATE INDEX local_listening_history_last_played
    ON local_listening_history(last_played_ms DESC);
CREATE UNIQUE INDEX list_items_entry ON list_items(document_id, entry_id);
CREATE INDEX local_library_added ON local_library_tracks(added);
CREATE INDEX engine_list_items_path ON engine_list_items(raw_path);
CREATE UNIQUE INDEX engine_lists_draft ON engine_lists(draft_of) WHERE draft_of IS NOT NULL;
CREATE INDEX file_publication_backups_state_time
    ON file_publication_backups(state, completed_at_unix_seconds DESC);
CREATE INDEX local_library_album_date ON local_library_tracks(album_key, date);
CREATE INDEX retired_engine_list_items_list ON retired_engine_list_items(list_id, position);
UPDATE schema_version SET version = 57;
