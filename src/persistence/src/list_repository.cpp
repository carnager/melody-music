// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/list_repository.hpp"

#include "current_schema.hpp"

#include "trackknife/core/sha256.hpp"
#include "trackknife/core/unicode.hpp"
#include "trackknife/persistence/list_edits.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "trackknife/query/tkq.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace trackknife::persistence {
namespace {

using detail::current_schema_version;
constexpr std::size_t maximum_items_per_document = 1'000'000U;
constexpr std::size_t maximum_source_relocations = 1'000'000U;
constexpr std::size_t maximum_metadata_transformation_chains = 256U;
constexpr std::size_t maximum_output_layout_profiles = 256U;
constexpr std::size_t maximum_encoder_presets = 256U;
constexpr std::size_t maximum_destination_profiles = 256U;

struct StatementDeleter {
    void operator()(sqlite3_stmt* statement) const noexcept {
        if (statement != nullptr) {
            sqlite3_finalize(statement);
        }
    }
};

using Statement = std::unique_ptr<sqlite3_stmt, StatementDeleter>;

[[nodiscard]] core::Error database_error(sqlite3* database, std::string message) {
    if (database != nullptr) {
        message += ": ";
        message += sqlite3_errmsg(database);
    }
    return core::Error{
        .code = core::ErrorCode::database, .message = std::move(message), .context = {}};
}

[[nodiscard]] core::Result<void> execute(sqlite3* database, const char* sql) {
    char* message = nullptr;
    const auto result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    if (result == SQLITE_OK) {
        return {};
    }
    auto error = database_error(database, message == nullptr ? "SQLite statement failed"
                                                             : std::string{message});
    sqlite3_free(message);
    return std::unexpected(std::move(error));
}

[[nodiscard]] core::Result<Statement> prepare(sqlite3* database, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK) {
        return std::unexpected(database_error(database, "Could not prepare SQLite statement"));
    }
    return Statement{raw};
}

[[nodiscard]] bool bind_blob(sqlite3_stmt* statement, const int index,
                             const std::string_view value) {
    return sqlite3_bind_blob64(statement, index, value.data(), value.size(), SQLITE_TRANSIENT) ==
           SQLITE_OK;
}

[[nodiscard]] bool bind_text(sqlite3_stmt* statement, const int index,
                             const std::string_view value) {
    return sqlite3_bind_text64(statement, index, value.data(), value.size(), SQLITE_TRANSIENT,
                               SQLITE_UTF8) == SQLITE_OK;
}

[[nodiscard]] std::string column_blob(sqlite3_stmt* statement, const int column) {
    const auto size = sqlite3_column_bytes(statement, column);
    const auto* data = static_cast<const char*>(sqlite3_column_blob(statement, column));
    return data == nullptr || size <= 0 ? std::string{} : std::string{data, data + size};
}

[[nodiscard]] std::string column_text(sqlite3_stmt* statement, const int column) {
    const auto size = sqlite3_column_bytes(statement, column);
    const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
    return data == nullptr || size <= 0 ? std::string{} : std::string{data, data + size};
}

std::string listening_observation(const std::string_view path,
                                  const core::LocalSourceRevision& revision) {
    // The path is length-prefixed raw bytes, never decoded or normalized.
    return core::sha256_hex(std::to_string(path.size()) + ":" + std::string{path} + ":" +
                            std::to_string(revision.device) + ":" + std::to_string(revision.inode) +
                            ":" + std::to_string(revision.size) + ":" +
                            std::to_string(revision.modification_time_seconds) + ":" +
                            std::to_string(revision.modification_time_nanoseconds));
}

bool valid_listening_source(const ListItem& source) {
    return source.source == ListSource::local && !source.source_reference.empty() &&
           source.source_reference.find('\0') == std::string::npos && source.source_revision &&
           source.source_revision->inode != 0 &&
           (!source.segment || (source.segment->start_sample >= 0 &&
                                (!source.segment->end_sample ||
                                 *source.segment->end_sample > source.segment->start_sample))) &&
           (!source.source_selection ||
            (source.source_selection->audio_stream_index.value_or(0) >= 0 &&
             source.source_selection->subsong_index.value_or(0) >= 0));
}

std::string listening_track_key(const std::string& id, const ListItem& source) {
    const auto number = [](const auto& value) {
        return value ? std::to_string(*value) : std::string{"default"};
    };
    const auto selection = source.source_selection.value_or(ListItemSourceSelection{});
    const auto range = source.segment ? std::to_string(source.segment->start_sample) + ":" +
                                            number(source.segment->end_sample)
                                      : std::string{"whole"};
    return core::sha256_hex("local-listen-1:" + id + ":" + number(selection.audio_stream_index) +
                            ":" + number(selection.subsong_index) + ":" + range);
}

core::Result<std::string> listening_source(sqlite3* database, const std::string& observation,
                                           const std::optional<std::string>& desired = {}) {
    auto insert =
        prepare(database, "INSERT INTO local_listening_sources(observation_hash,source_id) "
                          "VALUES(?,?) ON CONFLICT(observation_hash) DO NOTHING");
    const auto id = desired ? *desired : core::StableId::random().to_string();
    if (!insert || !bind_text(insert->get(), 1, observation) || !bind_text(insert->get(), 2, id) ||
        sqlite3_step(insert->get()) != SQLITE_DONE)
        return std::unexpected(database_error(database, "Could not store listening identity"));
    auto query =
        prepare(database, "SELECT source_id FROM local_listening_sources WHERE observation_hash=?");
    if (!query || !bind_text(query->get(), 1, observation) ||
        sqlite3_step(query->get()) != SQLITE_ROW)
        return std::unexpected(database_error(database, "Could not resolve listening identity"));
    auto resolved = column_text(query->get(), 0);
    if (desired && resolved != *desired)
        return std::unexpected(core::Error{.code = core::ErrorCode::conflict,
                                           .message = "Conflicting listening source identity",
                                           .context = {}});
    return resolved;
}

core::Result<void> link_listening_source(sqlite3* database, const std::string_view previous_path,
                                         const core::LocalSourceRevision& previous,
                                         const std::string_view published_path,
                                         const core::LocalSourceRevision& published) {
    auto id = listening_source(database, listening_observation(previous_path, previous));
    if (!id)
        return std::unexpected(std::move(id.error()));
    auto linked = listening_source(database, listening_observation(published_path, published), *id);
    if (!linked)
        return std::unexpected(std::move(linked.error()));
    return {};
}

[[nodiscard]] std::string encode_unsigned(const std::uint64_t value) {
    std::string bytes(8U, '\0');
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
        bytes[index] = static_cast<char>((value >> shift) & 0xFFU);
    }
    return bytes;
}

[[nodiscard]] std::optional<std::uint64_t> decode_unsigned(const std::string_view bytes) {
    if (bytes.size() != 8U) {
        return std::nullopt;
    }
    std::uint64_t value = 0U;
    for (const auto character : bytes) {
        value = (value << 8U) | static_cast<unsigned char>(character);
    }
    return value;
}

[[nodiscard]] std::string encode_signed(const std::int64_t value) {
    return encode_unsigned(std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] std::optional<std::int64_t> decode_signed(const std::string_view bytes) {
    const auto value = decode_unsigned(bytes);
    return value ? std::optional{std::bit_cast<std::int64_t>(*value)} : std::nullopt;
}

[[nodiscard]] bool bind_revision(sqlite3_stmt* statement, const int first,
                                 const core::LocalSourceRevision& revision) {
    const std::array values{
        encode_unsigned(revision.device),
        encode_unsigned(revision.inode),
        encode_unsigned(revision.size),
        encode_signed(revision.modification_time_seconds),
        encode_signed(revision.modification_time_nanoseconds),
    };
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (!bind_blob(statement, first + static_cast<int>(index), values[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] core::Result<core::LocalSourceRevision>
read_revision(sqlite3_stmt* statement, const int first, const std::string_view description) {
    const auto device = decode_unsigned(column_blob(statement, first));
    const auto inode = decode_unsigned(column_blob(statement, first + 1));
    const auto size = decode_unsigned(column_blob(statement, first + 2));
    const auto seconds = decode_signed(column_blob(statement, first + 3));
    const auto nanoseconds = decode_signed(column_blob(statement, first + 4));
    if (!device || !inode || !size || !seconds || !nanoseconds) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::database,
            .message = std::string{description} + " contains an invalid source revision",
            .context = {},
        });
    }
    return core::LocalSourceRevision{
        .device = *device,
        .inode = *inode,
        .size = *size,
        .modification_time_seconds = *seconds,
        .modification_time_nanoseconds = *nanoseconds,
    };
}

[[nodiscard]] std::optional<std::string> optional_blob(sqlite3_stmt* statement, const int column) {
    return sqlite3_column_type(statement, column) == SQLITE_NULL
               ? std::nullopt
               : std::optional{column_blob(statement, column)};
}

[[nodiscard]] core::Result<void> step_done(sqlite3* database, sqlite3_stmt* statement,
                                           std::string_view operation) {
    if (sqlite3_step(statement) != SQLITE_DONE) {
        return std::unexpected(database_error(database, std::string{operation}));
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return {};
}

// The schema version as it stands, read without a lock, or nothing when there
// is no schema yet.
[[nodiscard]] std::optional<sqlite3_int64> current_version(sqlite3* database) {
    auto query = prepare(database, "SELECT version FROM schema_version LIMIT 1");
    if (!query || sqlite3_step(query->get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    return sqlite3_column_int64(query->get(), 0);
}

[[nodiscard]] core::Result<void> migrate(sqlite3* database) {
    // The engine opens the database for every library request. A database
    // already at this version needs no write lock to say so -- taking one
    // made every read wait behind a scan or a tag write, and fail after the
    // busy timeout.
    if (current_version(database) == sqlite3_int64{current_schema_version}) {
        return {};
    }
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    if (auto result = execute(database, "CREATE TABLE IF NOT EXISTS schema_version ("
                                        "version INTEGER NOT NULL CHECK(version >= 0))");
        !result) {
        rollback();
        return result;
    }
    if (auto result = execute(database, "INSERT INTO schema_version(version) "
                                        "SELECT 0 WHERE NOT EXISTS (SELECT 1 FROM schema_version)");
        !result) {
        rollback();
        return result;
    }
    auto query = prepare(database, "SELECT version FROM schema_version LIMIT 1");
    if (!query) {
        rollback();
        return std::unexpected(std::move(query.error()));
    }
    if (sqlite3_step(query->get()) != SQLITE_ROW) {
        auto error = database_error(database, "Could not read SQLite schema version");
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto version = sqlite3_column_int64(query->get(), 0);
    query->reset();
    if (version > current_schema_version) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported,
            .message = "Melody state database was created by a newer application version",
            .context = {{"schema_version", std::to_string(version)}},
        });
    }
    if (version == current_schema_version) {
        // Nothing to change, so nothing to check. The engine opens the
        // database for every library request, and the foreign-key check below
        // reads all of it: on a library of 66,000 tracks that was half a
        // second per request, spent confirming that no migration had run.
        return execute(database, "COMMIT");
    }
    if (version != 0) {
        // ADR-0354: no release ever wrote a database older than this one,
        // so there is nothing to migrate from; one is made anew.
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported,
            .message = "Melody state database is from before schema " +
                       std::to_string(current_schema_version) + "; move it aside and start afresh",
            .context = {{"schema_version", std::to_string(version)}},
        });
    }
    if (auto result = execute(database, detail::current_schema_sql); !result) {
        rollback();
        return result;
    }
    // Foreign keys are off while the schema changes (see open()); a rebuild
    // that left a reference dangling is refused here rather than committed.
    auto dangling = prepare(database, "PRAGMA foreign_key_check");
    if (!dangling) {
        rollback();
        return std::unexpected(std::move(dangling.error()));
    }
    if (sqlite3_step(dangling->get()) == SQLITE_ROW) {
        auto error = database_error(database, "A schema migration left a dangling reference");
        dangling->reset();
        rollback();
        return std::unexpected(std::move(error));
    }
    dangling->reset();
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }

    return {};
}

class LocalSourceRelocationResolver final {
  public:
    LocalSourceRelocationResolver(sqlite3* database, Statement next,
                                  std::unordered_set<std::string> source_references)
        : database_{database}, next_{std::move(next)},
          source_references_{std::move(source_references)} {}

    [[nodiscard]] core::Result<void>
    resolve(std::string& source_reference,
            std::optional<core::LocalSourceRevision>& source_revision) {
        if (!source_revision || !source_references_.contains(source_reference)) {
            return {};
        }
        sqlite3_int64 preceding_sequence = 0;
        for (std::size_t step_count = 0U; step_count < maximum_source_relocations; ++step_count) {
            auto* statement = next_.get();
            if (!bind_blob(statement, 1, source_reference) ||
                !bind_revision(statement, 2, *source_revision) ||
                sqlite3_bind_int64(statement, 7, preceding_sequence) != SQLITE_OK) {
                return std::unexpected(
                    database_error(database_, "Could not bind local-source relocation"));
            }
            const auto step = sqlite3_step(statement);
            if (step == SQLITE_DONE) {
                sqlite3_reset(statement);
                sqlite3_clear_bindings(statement);
                return {};
            }
            if (step != SQLITE_ROW) {
                auto error = database_error(database_, "Could not resolve local-source relocation");
                sqlite3_reset(statement);
                sqlite3_clear_bindings(statement);
                return std::unexpected(std::move(error));
            }
            const auto sequence = sqlite3_column_int64(statement, 0);
            auto target_reference = column_blob(statement, 1);
            auto published_revision = read_revision(statement, 2, "Local-source relocation");
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            if (!published_revision) {
                return std::unexpected(std::move(published_revision.error()));
            }
            if (sequence <= preceding_sequence || target_reference.empty() ||
                target_reference == source_reference || published_revision->inode == 0U) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Local-source relocation history is invalid",
                    .context = {},
                });
            }
            preceding_sequence = sequence;
            source_reference = std::move(target_reference);
            source_revision = *published_revision;
            if (!source_references_.contains(source_reference)) {
                return {};
            }
        }
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "Local-source relocation history exceeds the replay limit",
            .context = {},
        });
    }

  private:
    sqlite3* database_{nullptr};
    Statement next_;
    std::unordered_set<std::string> source_references_;
};

[[nodiscard]] core::Result<LocalSourceRelocationResolver>
local_source_relocation_resolver(sqlite3* database) {
    auto sources = prepare(database, "SELECT DISTINCT source_reference "
                                     "FROM local_source_relocations");
    if (!sources) {
        return std::unexpected(std::move(sources.error()));
    }
    std::unordered_set<std::string> source_references;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(sources->get())) == SQLITE_ROW) {
        auto source_reference = column_blob(sources->get(), 0);
        if (source_reference.empty() || source_references.size() >= maximum_source_relocations ||
            !source_references.insert(std::move(source_reference)).second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Local-source relocation index is invalid",
                .context = {},
            });
        }
    }
    if (step != SQLITE_DONE) {
        return std::unexpected(
            database_error(database, "Could not load local-source relocation index"));
    }
    auto next = prepare(
        database,
        "SELECT sequence, target_reference, published_device, published_inode, published_size, "
        "published_mtime_seconds, published_mtime_nanoseconds "
        "FROM local_source_relocations WHERE source_reference = ? AND previous_device = ? "
        "AND previous_inode = ? AND previous_size = ? AND previous_mtime_seconds = ? "
        "AND previous_mtime_nanoseconds = ? AND sequence > ? ORDER BY sequence LIMIT 1");
    if (!next) {
        return std::unexpected(std::move(next.error()));
    }
    return LocalSourceRelocationResolver{database, std::move(*next), std::move(source_references)};
}

struct SerializedTransformationAction {
    int kind{0};
    std::string_view target_field;
    std::optional<std::string_view> argument;
    std::optional<std::string_view> dialect;
    std::optional<std::uint32_t> dialect_version;
    std::optional<std::uint32_t> compiler_schema;
    std::optional<std::uint32_t> integer_argument;
    std::optional<std::uint32_t> integer_argument_2;
    const std::vector<std::string>* values{nullptr};
};

[[nodiscard]] SerializedTransformationAction
serialize_transformation_action(const metadata::MetadataTransformationAction& action) {
    return std::visit(
        [](const auto& typed) -> SerializedTransformationAction {
            using Action = std::decay_t<decltype(typed)>;
            const std::string_view target = [&]() -> std::string_view {
                if constexpr (std::is_same_v<Action, metadata::MetadataCaptureValuesAction>) {
                    return typed.source;
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataBlocklistFieldsAction> ||
                                     std::is_same_v<Action,
                                                    metadata::MetadataAllowlistFieldsAction>) {
                    return "*";
                } else {
                    return typed.target_field;
                }
            }();
            SerializedTransformationAction serialized{
                .kind = 0,
                .target_field = target,
                .argument = std::nullopt,
                .dialect = std::nullopt,
                .dialect_version = std::nullopt,
                .compiler_schema = std::nullopt,
                .integer_argument = std::nullopt,
                .integer_argument_2 = std::nullopt,
                .values = nullptr,
            };
            if constexpr (std::is_same_v<Action, metadata::MetadataSetValuesAction>) {
                serialized.kind = 0;
                serialized.values = &typed.values;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataAddValuesAction>) {
                serialized.kind = 1;
                serialized.values = &typed.values;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataRemoveFieldAction>) {
                serialized.kind = 2;
                if (typed.match_mode == metadata::MetadataFieldMatchMode::exact_native) {
                    serialized.integer_argument = 1U;
                }
            } else if constexpr (std::is_same_v<Action, metadata::MetadataRemoveFieldIfAction>) {
                serialized.kind = 15;
                serialized.argument = typed.condition;
                serialized.dialect = typed.dialect.dialect;
                serialized.dialect_version = typed.dialect.dialect_version;
                serialized.compiler_schema = typed.dialect.compiler_schema;
                if (typed.match_mode == metadata::MetadataFieldMatchMode::exact_native) {
                    serialized.integer_argument = 1U;
                }
            } else if constexpr (std::is_same_v<Action, metadata::MetadataTransformValuesAction>) {
                switch (typed.transform) {
                case metadata::MetadataValueTransformKind::trim_ascii:
                    serialized.kind = 3;
                    break;
                case metadata::MetadataValueTransformKind::lowercase:
                    serialized.kind = 4;
                    break;
                case metadata::MetadataValueTransformKind::uppercase:
                    serialized.kind = 5;
                    break;
                case metadata::MetadataValueTransformKind::capitalize_first:
                    serialized.kind = 10;
                    break;
                }
            } else if constexpr (std::is_same_v<Action, metadata::MetadataCopyFieldAction>) {
                serialized.kind = 6;
                serialized.argument = typed.source_field;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataSplitValuesAction>) {
                serialized.kind = 7;
                serialized.argument = typed.separator;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataJoinValuesAction>) {
                serialized.kind = 8;
                serialized.argument = typed.separator;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataFormatValueAction>) {
                serialized.kind = 9;
                serialized.argument = typed.source;
                serialized.dialect = typed.dialect.dialect;
                serialized.dialect_version = typed.dialect.dialect_version;
                serialized.compiler_schema = typed.dialect.compiler_schema;
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataRemoveMatchingValuesAction>) {
                serialized.kind = 11;
                serialized.argument = typed.match;
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataReplaceMatchingValuesAction>) {
                serialized.kind = 12;
                serialized.argument = typed.match;
                serialized.values = &typed.replacement_values;
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataNumberSelectedItemsAction>) {
                serialized.kind = 13;
                serialized.integer_argument = typed.start;
                serialized.integer_argument_2 = typed.padding;
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataNumberGroupedItemsAction>) {
                serialized.kind = 17;
                serialized.argument = typed.group_expression;
                serialized.dialect = typed.dialect.dialect;
                serialized.dialect_version = typed.dialect.dialect_version;
                serialized.compiler_schema = typed.dialect.compiler_schema;
                serialized.integer_argument = typed.start;
                serialized.integer_argument_2 = typed.padding;
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataKeepFirstCharactersAction>) {
                serialized.kind = 14;
                serialized.integer_argument = typed.character_count;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataCaptureValuesAction>) {
                serialized.kind = 16;
                serialized.argument = typed.pattern;
                serialized.dialect = typed.dialect.dialect;
                serialized.dialect_version = typed.dialect.dialect_version;
                serialized.compiler_schema = typed.dialect.compiler_schema;
                serialized.integer_argument = static_cast<std::uint32_t>(typed.source_kind);
            } else if constexpr (std::is_same_v<Action, metadata::MetadataBlocklistFieldsAction>) {
                serialized.kind = 18;
                serialized.values = &typed.fields;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataAllowlistFieldsAction>) {
                serialized.kind = 19;
                serialized.values = &typed.fields;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataConvertRatingAction>) {
                // ADR-0244: the source field, and the scale by its top.
                serialized.kind = 20;
                serialized.argument = typed.source_field;
                serialized.integer_argument = typed.scale == metadata::PlainRatingScale::hundred
                                                  ? 100U
                                              : typed.scale == metadata::PlainRatingScale::ten ? 10U
                                                                                               : 5U;
            }
            return serialized;
        },
        action);
}

[[nodiscard]] core::Result<void>
validate_saved_transformation_chain(const SavedMetadataTransformationChain& saved_chain) {
    if (saved_chain.id.is_nil() || saved_chain.chain.name.empty()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "Saved metadata transformations require a non-nil ID and non-empty name",
            .context = {},
        });
    }
    return metadata::validate_metadata_transformation_chain(saved_chain.chain);
}

[[nodiscard]] core::Result<void>
validate_saved_output_layout_profile(const SavedOutputLayoutProfile& saved_profile) {
    if (saved_profile.id.is_nil()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "Saved output layouts require a non-nil ID",
            .context = {},
        });
    }
    return operations::validate_output_layout_profile(saved_profile.profile);
}

[[nodiscard]] core::Result<void>
validate_saved_destination_profile(const SavedDestinationProfile& saved_profile) {
    if (saved_profile.id.is_nil()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "Saved destinations require a non-nil ID",
            .context = {},
        });
    }
    return operations::validate_destination_profile(saved_profile.profile);
}

[[nodiscard]] core::Result<void>
validate_saved_encoder_preset(const SavedEncoderPreset& saved_preset) {
    const auto invalid = [](std::string message) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = std::move(message),
                                           .context = {}});
    };
    if (saved_preset.id.is_nil()) {
        return invalid("Saved encoder presets require a non-nil ID");
    }
    const auto& preset = saved_preset.preset;
    if (preset.id.empty() || preset.id.size() > 128U) {
        return invalid("Encoder preset identifiers must be 1-128 bytes");
    }
    if (preset.display_name.empty() || preset.display_name.size() > 1'024U) {
        return invalid("Encoder preset names must be 1-1024 bytes");
    }
    if (preset.codec_name.empty() || preset.codec_name.size() > 128U ||
        preset.container_name.empty() || preset.container_name.size() > 128U) {
        return invalid("Encoder preset codec and container names must be 1-128 bytes");
    }
    if (preset.file_extension.empty() || preset.file_extension.size() > 32U ||
        preset.file_extension.find('/') != std::string::npos ||
        preset.file_extension.find('.') != std::string::npos) {
        return invalid("Encoder preset extensions must be 1-32 bytes without separators");
    }
    if (preset.version < 1) {
        return invalid("Encoder preset versions start at 1");
    }
    if (preset.bit_rate && preset.vbr_quality) {
        return invalid("Encoder presets choose bit rate or VBR quality, never both");
    }
    if (preset.bit_rate && (*preset.bit_rate < 8'000 || *preset.bit_rate > 2'000'000)) {
        return invalid("Encoder preset bit rates must be 8-2000 kbps");
    }
    if (preset.vbr_quality && (*preset.vbr_quality < -2 || *preset.vbr_quality > 12)) {
        return invalid("Encoder preset VBR qualities must be between -2 and 12");
    }
    if (preset.sample_format_hint.size() > 32U) {
        return invalid("Encoder preset sample-format hints must be at most 32 bytes");
    }
    return {};
}

} // namespace

struct ListRepository::Impl {
    sqlite3* database{nullptr};

    ~Impl() {
        if (database != nullptr) {
            sqlite3_close(database);
        }
    }
};

ListRepository::ListRepository(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

ListRepository::ListRepository(ListRepository&&) noexcept = default;
ListRepository& ListRepository::operator=(ListRepository&&) noexcept = default;
ListRepository::~ListRepository() = default;

core::Result<ListRepository> ListRepository::open(const std::filesystem::path& path) {
    auto implementation = std::make_unique<Impl>();
    const auto& encoded = path.native();
    if (sqlite3_open_v2(encoded.c_str(), &implementation->database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        return std::unexpected(
            database_error(implementation->database, "Could not open Melody state database"));
    }
    sqlite3_busy_timeout(implementation->database, 2'000);
    if (auto result = execute(implementation->database, "PRAGMA journal_mode = WAL"); !result) {
        return std::unexpected(std::move(result.error()));
    }
    // Migrations run with foreign keys off. Rebuilding a table (create the
    // new shape, copy, drop the old, rename) is how SQLite changes a CHECK,
    // and with foreign keys on, the drop is a DELETE of every row that
    // cascades into every child table. Schemas 29 and 38 did exactly that
    // to the operation journal's evidence and its backup records. The pragma
    // is a no-op inside a transaction, so it is set before migrate() begins.
    if (auto result = execute(implementation->database, "PRAGMA foreign_keys = OFF"); !result) {
        return std::unexpected(std::move(result.error()));
    }
    if (auto result = migrate(implementation->database); !result) {
        return std::unexpected(std::move(result.error()));
    }
    if (auto result = execute(implementation->database, "PRAGMA foreign_keys = ON"); !result) {
        return std::unexpected(std::move(result.error()));
    }
    return ListRepository{std::move(implementation)};
}

core::Result<unsigned> ListRepository::schema_version() const {
    auto statement = prepare(implementation_->database, "SELECT version FROM schema_version");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    if (sqlite3_step(statement->get()) != SQLITE_ROW) {
        return std::unexpected(
            database_error(implementation_->database, "Could not read schema version"));
    }
    const auto version = sqlite3_column_int64(statement->get(), 0);
    if (version < 0 || version > std::numeric_limits<unsigned>::max()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                           .message = "Invalid SQLite schema version",
                                           .context = {}});
    }
    return static_cast<unsigned>(version);
}

core::Result<LocalSourceRelocationResult>
ListRepository::relocate_local_source(const LocalSourceRelocation& relocation) {
    if (relocation.operation_id.is_nil() || relocation.source_reference.empty() ||
        relocation.target_reference.empty() ||
        relocation.source_reference.find('\0') != std::string::npos ||
        relocation.target_reference.find('\0') != std::string::npos ||
        relocation.source_reference == relocation.target_reference ||
        relocation.previous_revision.inode == 0U || relocation.published_revision.inode == 0U) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "Local-source relocation requires distinct raw paths and valid evidence",
            .context = {},
        });
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return std::unexpected(std::move(result.error()));
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    const auto operation_id = relocation.operation_id.to_string();

    auto replay =
        prepare(database, "SELECT source_reference, target_reference, previous_device, "
                          "previous_inode, previous_size, previous_mtime_seconds, "
                          "previous_mtime_nanoseconds, published_device, published_inode, "
                          "published_size, published_mtime_seconds, published_mtime_nanoseconds "
                          "FROM local_source_relocations WHERE operation_id = ?");
    if (!replay || !bind_text(replay->get(), 1, operation_id)) {
        auto error = replay ? database_error(database, "Could not bind relocation replay")
                            : std::move(replay.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto replay_step = sqlite3_step(replay->get());
    if (replay_step == SQLITE_ROW) {
        auto previous_revision = read_revision(replay->get(), 2, "Local-source relocation");
        auto published_revision = read_revision(replay->get(), 7, "Local-source relocation");
        if (!previous_revision || !published_revision) {
            auto error = !previous_revision ? std::move(previous_revision.error())
                                            : std::move(published_revision.error());
            rollback();
            return std::unexpected(std::move(error));
        }
        if (column_blob(replay->get(), 0) != relocation.source_reference ||
            column_blob(replay->get(), 1) != relocation.target_reference ||
            *previous_revision != relocation.previous_revision ||
            *published_revision != relocation.published_revision) {
            rollback();
            return std::unexpected(core::Error{
                .code = core::ErrorCode::conflict,
                .message = "Local-source relocation identity was replayed with different content",
                .context = {{"operation_id", operation_id}},
            });
        }
        rollback();
        return LocalSourceRelocationResult{.already_applied = true};
    }
    if (replay_step != SQLITE_DONE) {
        auto error = database_error(database, "Could not inspect relocation replay");
        rollback();
        return std::unexpected(std::move(error));
    }

    // ADR-0286: every move is recorded, so that a client which saw the file at
    // its old path -- a tab kept while its window was closed -- finds where it
    // went (catalogue.resolve_moves), and a replay of the move finds it done.
    auto insert_relocation = prepare(
        database,
        "INSERT INTO local_source_relocations(operation_id, source_reference, target_reference, "
        "previous_device, previous_inode, previous_size, previous_mtime_seconds, "
        "previous_mtime_nanoseconds, published_device, published_inode, published_size, "
        "published_mtime_seconds, published_mtime_nanoseconds) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!insert_relocation || !bind_text(insert_relocation->get(), 1, operation_id) ||
        !bind_blob(insert_relocation->get(), 2, relocation.source_reference) ||
        !bind_blob(insert_relocation->get(), 3, relocation.target_reference) ||
        !bind_revision(insert_relocation->get(), 4, relocation.previous_revision) ||
        !bind_revision(insert_relocation->get(), 9, relocation.published_revision)) {
        auto error = insert_relocation
                         ? database_error(database, "Could not bind local-source relocation")
                         : std::move(insert_relocation.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto stored = step_done(database, insert_relocation->get(),
                                "Could not store local-source relocation");
        !stored) {
        rollback();
        return std::unexpected(std::move(stored.error()));
    }
    if (auto result = refresh_library_source(
            database, relocation.source_reference, relocation.target_reference,
            relocation.published_document ? &*relocation.published_document : nullptr);
        !result) {
        rollback();
        return std::unexpected(std::move(result.error()));
    }
    if (auto linked = link_listening_source(
            database, relocation.source_reference, relocation.previous_revision,
            relocation.target_reference, relocation.published_revision);
        !linked) {
        rollback();
        return std::unexpected(std::move(linked.error()));
    }
    if (auto committed = execute(database, "COMMIT"); !committed) {
        rollback();
        return std::unexpected(std::move(committed.error()));
    }
    return LocalSourceRelocationResult{.already_applied = false};
}

core::Result<std::vector<SavedMetadataTransformationChain>>
ListRepository::load_metadata_transformation_chains() const {
    auto* database = implementation_->database;
    auto chains_query = prepare(
        database, "SELECT id, schema_version, name, automatic FROM metadata_transformation_chains "
                  "ORDER BY name, id");
    if (!chains_query) {
        return std::unexpected(std::move(chains_query.error()));
    }
    std::vector<SavedMetadataTransformationChain> chains;
    std::unordered_map<std::string, std::size_t> chain_indices;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(chains_query->get())) == SQLITE_ROW) {
        const auto id_text = column_text(chains_query->get(), 0);
        auto id = core::StableId::parse(id_text);
        const auto schema_version = sqlite3_column_int64(chains_query->get(), 1);
        auto name = column_blob(chains_query->get(), 2);
        const auto automatic = sqlite3_column_int(chains_query->get(), 3);
        if (!id || id->is_nil() || schema_version != 1 || name.empty() ||
            (automatic != 0 && automatic != 1) ||
            chains.size() >= maximum_metadata_transformation_chains ||
            !chain_indices.emplace(id_text, chains.size()).second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Invalid persisted metadata transformation chain",
                .context = {{"chain_id", id_text}},
            });
        }
        chains.push_back(SavedMetadataTransformationChain{
            .id = *id,
            .chain =
                metadata::MetadataTransformationChain{
                    .schema_version = static_cast<std::uint32_t>(schema_version),
                    .name = std::move(name),
                    .actions = {},
                },
            .automatic = automatic != 0,
        });
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(
            database_error(database, "Could not load metadata transformation chains"));
    }

    auto actions_query = prepare(
        database,
        "SELECT chain_id, position, kind, target_field, argument, dialect, dialect_version, "
        "compiler_schema, integer_argument, integer_argument_2 "
        "FROM metadata_transformation_actions ORDER BY chain_id, position");
    if (!actions_query) {
        return std::unexpected(std::move(actions_query.error()));
    }
    while ((result = sqlite3_step(actions_query->get())) == SQLITE_ROW) {
        const auto chain_id = column_text(actions_query->get(), 0);
        const auto found = chain_indices.find(chain_id);
        const auto position = sqlite3_column_int64(actions_query->get(), 1);
        const auto kind = sqlite3_column_int(actions_query->get(), 2);
        if (found == chain_indices.end() || position < 0 ||
            static_cast<std::size_t>(position) != chains[found->second].chain.actions.size() ||
            kind < 0 || kind > 20) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Invalid persisted metadata transformation action order",
                .context = {{"chain_id", chain_id}},
            });
        }
        const auto target = column_blob(actions_query->get(), 3);
        const auto argument = optional_blob(actions_query->get(), 4);
        const auto dialect = optional_blob(actions_query->get(), 5);
        const bool has_dialect_version =
            sqlite3_column_type(actions_query->get(), 6) != SQLITE_NULL;
        const bool has_compiler_schema =
            sqlite3_column_type(actions_query->get(), 7) != SQLITE_NULL;
        const auto dialect_version = sqlite3_column_int64(actions_query->get(), 6);
        const auto compiler_schema = sqlite3_column_int64(actions_query->get(), 7);
        const bool has_integer_argument =
            sqlite3_column_type(actions_query->get(), 8) != SQLITE_NULL;
        const bool has_integer_argument_2 =
            sqlite3_column_type(actions_query->get(), 9) != SQLITE_NULL;
        const auto integer_argument = sqlite3_column_int64(actions_query->get(), 8);
        const auto integer_argument_2 = sqlite3_column_int64(actions_query->get(), 9);
        const bool has_any_dialect = dialect || has_dialect_version || has_compiler_schema;
        const bool has_complete_dialect = dialect && has_dialect_version && has_compiler_schema;
        const bool has_any_integer = has_integer_argument || has_integer_argument_2;
        metadata::MetadataTransformationAction action;
        switch (kind) {
        case 0:
            if (argument || has_any_dialect || has_any_integer) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted set-values action contains unexpected data",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataSetValuesAction{.target_field = target, .values = {}};
            break;
        case 1:
            if (argument || has_any_dialect || has_any_integer) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted add-values action contains unexpected data",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataAddValuesAction{.target_field = target, .values = {}};
            break;
        case 3:
        case 4:
        case 5:
        case 10: {
            if (argument || has_any_dialect || has_any_integer) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted metadata transformation action contains unexpected data",
                    .context = {{"chain_id", chain_id}},
                });
            }
            const auto transform = kind == 3   ? metadata::MetadataValueTransformKind::trim_ascii
                                   : kind == 4 ? metadata::MetadataValueTransformKind::lowercase
                                   : kind == 5
                                       ? metadata::MetadataValueTransformKind::uppercase
                                       : metadata::MetadataValueTransformKind::capitalize_first;
            action = metadata::MetadataTransformValuesAction{
                .target_field = target,
                .transform = transform,
            };
            break;
        }
        case 2:
            if (argument || has_any_dialect || has_integer_argument_2 ||
                (has_integer_argument && integer_argument != 1)) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted remove-field action has an invalid match mode",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataRemoveFieldAction{
                .target_field = target,
                .match_mode = has_integer_argument ? metadata::MetadataFieldMatchMode::exact_native
                                                   : metadata::MetadataFieldMatchMode::logical,
            };
            break;
        case 6:
        case 7:
        case 8:
            if (!argument || has_any_dialect || has_any_integer) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted exact-value action is missing its argument",
                    .context = {{"chain_id", chain_id}},
                });
            }
            if (kind == 6) {
                action = metadata::MetadataCopyFieldAction{.target_field = target,
                                                           .source_field = *argument};
            } else if (kind == 7) {
                action = metadata::MetadataSplitValuesAction{.target_field = target,
                                                             .separator = *argument};
            } else {
                action = metadata::MetadataJoinValuesAction{.target_field = target,
                                                            .separator = *argument};
            }
            break;
        case 9:
            if (!argument || !has_complete_dialect || dialect_version < 0 || compiler_schema < 0 ||
                dialect_version > std::numeric_limits<std::uint32_t>::max() ||
                compiler_schema > std::numeric_limits<std::uint32_t>::max() || has_any_integer) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted formatting action is missing its dialect",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataFormatValueAction{
                .target_field = target,
                .dialect =
                    titleformat::DialectVersion{
                        .dialect = *dialect,
                        .dialect_version = static_cast<std::uint32_t>(dialect_version),
                        .compiler_schema = static_cast<std::uint32_t>(compiler_schema),
                    },
                .source = *argument,
            };
            break;
        case 11:
        case 12:
            if (!argument || has_any_dialect || has_any_integer) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted matching action is missing its exact value",
                    .context = {{"chain_id", chain_id}},
                });
            }
            if (kind == 11) {
                action = metadata::MetadataRemoveMatchingValuesAction{
                    .target_field = target,
                    .match = *argument,
                };
            } else {
                action = metadata::MetadataReplaceMatchingValuesAction{
                    .target_field = target,
                    .match = *argument,
                    .replacement_values = {},
                };
            }
            break;
        case 13:
            if (argument || has_any_dialect || !has_integer_argument || !has_integer_argument_2 ||
                integer_argument < 0 || integer_argument_2 < 0 ||
                integer_argument > std::numeric_limits<std::uint32_t>::max() ||
                integer_argument_2 > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted numbering action has invalid numeric arguments",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataNumberSelectedItemsAction{
                .target_field = target,
                .start = static_cast<std::uint32_t>(integer_argument),
                .padding = static_cast<std::uint32_t>(integer_argument_2),
            };
            break;
        case 17:
            if (!argument || !has_complete_dialect || dialect_version < 0 || compiler_schema < 0 ||
                dialect_version > std::numeric_limits<std::uint32_t>::max() ||
                compiler_schema > std::numeric_limits<std::uint32_t>::max() ||
                !has_integer_argument || !has_integer_argument_2 || integer_argument < 0 ||
                integer_argument_2 < 0 ||
                integer_argument > std::numeric_limits<std::uint32_t>::max() ||
                integer_argument_2 > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted grouped numbering action has invalid arguments",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataNumberGroupedItemsAction{
                .target_field = target,
                .dialect =
                    titleformat::DialectVersion{
                        .dialect = *dialect,
                        .dialect_version = static_cast<std::uint32_t>(dialect_version),
                        .compiler_schema = static_cast<std::uint32_t>(compiler_schema),
                    },
                .group_expression = *argument,
                .start = static_cast<std::uint32_t>(integer_argument),
                .padding = static_cast<std::uint32_t>(integer_argument_2),
            };
            break;
        case 14:
            if (argument || has_any_dialect || !has_integer_argument || has_integer_argument_2 ||
                integer_argument <= 0 ||
                integer_argument > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted keep-first action has an invalid character count",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataKeepFirstCharactersAction{
                .target_field = target,
                .character_count = static_cast<std::uint32_t>(integer_argument),
            };
            break;
        case 15:
            if (!argument || !has_complete_dialect || dialect_version < 0 || compiler_schema < 0 ||
                dialect_version > std::numeric_limits<std::uint32_t>::max() ||
                compiler_schema > std::numeric_limits<std::uint32_t>::max() ||
                has_integer_argument_2 || (has_integer_argument && integer_argument != 1)) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted conditional-remove action is missing its dialect",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataRemoveFieldIfAction{
                .target_field = target,
                .dialect =
                    titleformat::DialectVersion{
                        .dialect = *dialect,
                        .dialect_version = static_cast<std::uint32_t>(dialect_version),
                        .compiler_schema = static_cast<std::uint32_t>(compiler_schema),
                    },
                .condition = *argument,
                .match_mode = has_integer_argument ? metadata::MetadataFieldMatchMode::exact_native
                                                   : metadata::MetadataFieldMatchMode::logical,
            };
            break;
        case 16:
            if (!argument || !has_complete_dialect || dialect_version < 0 || compiler_schema < 0 ||
                dialect_version > std::numeric_limits<std::uint32_t>::max() ||
                compiler_schema > std::numeric_limits<std::uint32_t>::max() ||
                !has_integer_argument || has_integer_argument_2 || integer_argument < 0 ||
                integer_argument > 3) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted capture action has invalid dialect or source data",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataCaptureValuesAction{
                .dialect =
                    metadata::CapturePatternDialectVersion{
                        .dialect = *dialect,
                        .dialect_version = static_cast<std::uint32_t>(dialect_version),
                        .compiler_schema = static_cast<std::uint32_t>(compiler_schema),
                    },
                .source_kind = static_cast<metadata::MetadataCaptureSourceKind>(integer_argument),
                .source = target,
                .pattern = *argument,
            };
            break;
        case 18:
        case 19:
            if (target != "*" || argument || has_any_dialect || has_any_integer) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted field-filter action contains unexpected data",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action =
                kind == 18
                    ? metadata::
                          MetadataTransformationAction{metadata::MetadataBlocklistFieldsAction{}}
                    : metadata::MetadataTransformationAction{
                          metadata::MetadataAllowlistFieldsAction{}};
            break;
        case 20: {
            const auto scale = integer_argument == 5     ? metadata::PlainRatingScale::five
                               : integer_argument == 10  ? metadata::PlainRatingScale::ten
                               : integer_argument == 100 ? metadata::PlainRatingScale::hundred
                                                         : metadata::PlainRatingScale::off;
            if (!argument || has_any_dialect || !has_integer_argument || has_integer_argument_2 ||
                scale == metadata::PlainRatingScale::off) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted rating conversion is missing its field or scale",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataConvertRatingAction{
                .target_field = target, .source_field = *argument, .scale = scale};
            break;
        }
        default:
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Unknown persisted metadata transformation action",
                .context = {{"chain_id", chain_id}},
            });
        }
        chains[found->second].chain.actions.push_back(std::move(action));
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(
            database_error(database, "Could not load metadata transformation actions"));
    }

    auto values_query = prepare(
        database,
        "SELECT chain_id, action_position, position, value FROM "
        "metadata_transformation_action_values ORDER BY chain_id, action_position, position");
    if (!values_query) {
        return std::unexpected(std::move(values_query.error()));
    }
    while ((result = sqlite3_step(values_query->get())) == SQLITE_ROW) {
        const auto chain_id = column_text(values_query->get(), 0);
        const auto found = chain_indices.find(chain_id);
        const auto action_position = sqlite3_column_int64(values_query->get(), 1);
        const auto value_position = sqlite3_column_int64(values_query->get(), 2);
        if (found == chain_indices.end() || action_position < 0 || value_position < 0 ||
            static_cast<std::size_t>(action_position) >=
                chains[found->second].chain.actions.size()) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Invalid persisted metadata transformation value order",
                .context = {{"chain_id", chain_id}},
            });
        }
        auto& action =
            chains[found->second].chain.actions[static_cast<std::size_t>(action_position)];
        auto* values = [&]() -> std::vector<std::string>* {
            if (auto* set = std::get_if<metadata::MetadataSetValuesAction>(&action)) {
                return &set->values;
            }
            if (auto* add = std::get_if<metadata::MetadataAddValuesAction>(&action)) {
                return &add->values;
            }
            if (auto* replace =
                    std::get_if<metadata::MetadataReplaceMatchingValuesAction>(&action)) {
                return &replace->replacement_values;
            }
            if (auto* blocklist = std::get_if<metadata::MetadataBlocklistFieldsAction>(&action)) {
                return &blocklist->fields;
            }
            if (auto* allowlist = std::get_if<metadata::MetadataAllowlistFieldsAction>(&action)) {
                return &allowlist->fields;
            }
            return nullptr;
        }();
        if (values == nullptr || static_cast<std::size_t>(value_position) != values->size()) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Invalid persisted metadata transformation literal value",
                .context = {{"chain_id", chain_id}},
            });
        }
        values->push_back(column_blob(values_query->get(), 3));
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(
            database_error(database, "Could not load metadata transformation literal values"));
    }
    for (const auto& chain : chains) {
        if (auto valid = validate_saved_transformation_chain(chain); !valid) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Persisted metadata transformation chain failed validation: " +
                           valid.error().message,
                .context = {{"chain_id", chain.id.to_string()}},
            });
        }
    }
    return chains;
}

core::Result<void> ListRepository::upsert_metadata_transformation_chain(
    const SavedMetadataTransformationChain& saved_chain) {
    if (auto valid = validate_saved_transformation_chain(saved_chain); !valid) {
        return valid;
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    const auto id = saved_chain.id.to_string();
    auto count_query = prepare(
        database,
        "SELECT COUNT(*), EXISTS(SELECT 1 FROM metadata_transformation_chains WHERE id = ?) "
        "FROM metadata_transformation_chains");
    if (!count_query || !bind_text(count_query->get(), 1, id) ||
        sqlite3_step(count_query->get()) != SQLITE_ROW) {
        auto error = count_query ? database_error(database, "Could not count saved transformations")
                                 : std::move(count_query.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto count = sqlite3_column_int64(count_query->get(), 0);
    const bool already_exists = sqlite3_column_int(count_query->get(), 1) != 0;
    sqlite3_reset(count_query->get());
    if (!already_exists &&
        count >= static_cast<sqlite3_int64>(maximum_metadata_transformation_chains)) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "At most 256 metadata transformation chains can be saved",
            .context = {},
        });
    }
    auto name_query =
        prepare(database, "SELECT id FROM metadata_transformation_chains WHERE name = ?");
    if (!name_query || !bind_blob(name_query->get(), 1, saved_chain.chain.name)) {
        auto error = name_query ? database_error(database, "Could not bind transformation name")
                                : std::move(name_query.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto name_result = sqlite3_step(name_query->get());
    const auto name_owner =
        name_result == SQLITE_ROW ? std::optional{column_text(name_query->get(), 0)} : std::nullopt;
    sqlite3_reset(name_query->get());
    if (name_owner && *name_owner != id) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "A saved metadata transformation already uses that exact name",
            .context = {{"name", saved_chain.chain.name}},
        });
    }
    if (name_result != SQLITE_ROW && name_result != SQLITE_DONE) {
        auto error = database_error(database, "Could not check transformation name");
        rollback();
        return std::unexpected(std::move(error));
    }

    auto chain_statement = prepare(
        database, "INSERT INTO metadata_transformation_chains(id, schema_version, name, automatic) "
                  "VALUES(?,?,?,?) "
                  "ON CONFLICT(id) DO UPDATE SET schema_version = excluded.schema_version, "
                  "name = excluded.name, automatic = excluded.automatic");
    if (!chain_statement || !bind_text(chain_statement->get(), 1, id) ||
        sqlite3_bind_int64(chain_statement->get(), 2, saved_chain.chain.schema_version) !=
            SQLITE_OK ||
        !bind_blob(chain_statement->get(), 3, saved_chain.chain.name) ||
        sqlite3_bind_int(chain_statement->get(), 4, saved_chain.automatic ? 1 : 0) != SQLITE_OK) {
        auto error = chain_statement
                         ? database_error(database, "Could not bind metadata transformation chain")
                         : std::move(chain_statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, chain_statement->get(),
                                "Could not store metadata transformation chain");
        !result) {
        rollback();
        return result;
    }
    auto delete_actions =
        prepare(database, "DELETE FROM metadata_transformation_actions WHERE chain_id = ?");
    if (!delete_actions || !bind_text(delete_actions->get(), 1, id)) {
        auto error = delete_actions
                         ? database_error(database, "Could not bind transformation replacement")
                         : std::move(delete_actions.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, delete_actions->get(),
                                "Could not replace metadata transformation actions");
        !result) {
        rollback();
        return result;
    }
    auto action_statement = prepare(
        database,
        "INSERT INTO metadata_transformation_actions(chain_id, position, kind, target_field, "
        "argument, dialect, dialect_version, compiler_schema, integer_argument, "
        "integer_argument_2) VALUES(?,?,?,?,?,?,?,?,?,?)");
    auto value_statement = prepare(
        database,
        "INSERT INTO metadata_transformation_action_values(chain_id, action_position, position, "
        "value) VALUES(?,?,?,?)");
    if (!action_statement || !value_statement) {
        auto error = !action_statement ? std::move(action_statement.error())
                                       : std::move(value_statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto bind_optional_view = [](sqlite3_stmt* statement, const int index,
                                       const std::optional<std::string_view>& value) {
        return value ? bind_blob(statement, index, *value)
                     : sqlite3_bind_null(statement, index) == SQLITE_OK;
    };
    const auto bind_optional_number = [](sqlite3_stmt* statement, const int index,
                                         const std::optional<std::uint32_t> value) {
        return value ? sqlite3_bind_int64(statement, index, *value) == SQLITE_OK
                     : sqlite3_bind_null(statement, index) == SQLITE_OK;
    };
    for (std::size_t position = 0U; position < saved_chain.chain.actions.size(); ++position) {
        const auto serialized =
            serialize_transformation_action(saved_chain.chain.actions[position]);
        auto* statement = action_statement->get();
        if (!bind_text(statement, 1, id) ||
            sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(position)) != SQLITE_OK ||
            sqlite3_bind_int(statement, 3, serialized.kind) != SQLITE_OK ||
            !bind_blob(statement, 4, serialized.target_field) ||
            !bind_optional_view(statement, 5, serialized.argument) ||
            !bind_optional_view(statement, 6, serialized.dialect) ||
            !bind_optional_number(statement, 7, serialized.dialect_version) ||
            !bind_optional_number(statement, 8, serialized.compiler_schema) ||
            !bind_optional_number(statement, 9, serialized.integer_argument) ||
            !bind_optional_number(statement, 10, serialized.integer_argument_2)) {
            auto error = database_error(database, "Could not bind metadata transformation action");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto result =
                step_done(database, statement, "Could not store metadata transformation action");
            !result) {
            rollback();
            return result;
        }
        if (serialized.values == nullptr) {
            continue;
        }
        for (std::size_t value_position = 0U; value_position < serialized.values->size();
             ++value_position) {
            auto* value = value_statement->get();
            if (!bind_text(value, 1, id) ||
                sqlite3_bind_int64(value, 2, static_cast<sqlite3_int64>(position)) != SQLITE_OK ||
                sqlite3_bind_int64(value, 3, static_cast<sqlite3_int64>(value_position)) !=
                    SQLITE_OK ||
                !bind_blob(value, 4, (*serialized.values)[value_position])) {
                auto error =
                    database_error(database, "Could not bind transformation literal value");
                rollback();
                return std::unexpected(std::move(error));
            }
            if (auto result =
                    step_done(database, value, "Could not store transformation literal value");
                !result) {
                rollback();
                return result;
            }
        }
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<void> ListRepository::remove_metadata_transformation_chain(const core::StableId& id) {
    if (id.is_nil()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "A saved metadata transformation ID cannot be nil",
            .context = {},
        });
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    auto statement = prepare(database, "DELETE FROM metadata_transformation_chains WHERE id = ?");
    const auto encoded = id.to_string();
    if (!statement || !bind_text(statement->get(), 1, encoded)) {
        auto error = statement ? database_error(database, "Could not bind transformation deletion")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result =
            step_done(database, statement->get(), "Could not remove metadata transformation chain");
        !result) {
        rollback();
        return result;
    }
    if (sqlite3_changes(database) != 1) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::not_found,
            .message = "Saved metadata transformation was not found",
            .context = {{"chain_id", encoded}},
        });
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<std::vector<SavedOutputLayoutProfile>>
ListRepository::load_output_layout_profiles() const {
    auto* database = implementation_->database;
    auto statement = prepare(
        database, "SELECT id, schema_version, name, dialect, dialect_version, compiler_schema, "
                  "relative_directory_expression, basename_expression, sanitization_policy, "
                  "sanitization_version FROM output_layout_profiles ORDER BY name, id");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<SavedOutputLayoutProfile> profiles;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        const auto id_text = column_text(statement->get(), 0);
        auto id = core::StableId::parse(id_text);
        const auto schema_version = sqlite3_column_int64(statement->get(), 1);
        const auto dialect_version = sqlite3_column_int64(statement->get(), 4);
        const auto compiler_schema = sqlite3_column_int64(statement->get(), 5);
        const auto sanitization_version = sqlite3_column_int64(statement->get(), 9);
        if (!id || id->is_nil() || schema_version < 0 || dialect_version < 0 ||
            compiler_schema < 0 || sanitization_version < 0 ||
            schema_version > std::numeric_limits<std::uint32_t>::max() ||
            dialect_version > std::numeric_limits<std::uint32_t>::max() ||
            compiler_schema > std::numeric_limits<std::uint32_t>::max() ||
            sanitization_version > std::numeric_limits<std::uint32_t>::max() ||
            profiles.size() >= maximum_output_layout_profiles) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Invalid persisted output layout profile",
                .context = {{"profile_id", id_text}},
            });
        }
        SavedOutputLayoutProfile profile{
            .id = *id,
            .profile =
                operations::OutputLayoutProfile{
                    .schema_version = static_cast<std::uint32_t>(schema_version),
                    .name = column_blob(statement->get(), 2),
                    .dialect =
                        titleformat::DialectVersion{
                            .dialect = column_blob(statement->get(), 3),
                            .dialect_version = static_cast<std::uint32_t>(dialect_version),
                            .compiler_schema = static_cast<std::uint32_t>(compiler_schema),
                        },
                    .relative_directory_expression = column_blob(statement->get(), 6),
                    .basename_expression = column_blob(statement->get(), 7),
                    .sanitization_policy =
                        operations::PolicyVersion{
                            .name = column_blob(statement->get(), 8),
                            .version = static_cast<std::uint32_t>(sanitization_version),
                        },
                },
        };
        if (auto valid = validate_saved_output_layout_profile(profile); !valid) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Persisted output layout failed validation: " + valid.error().message,
                .context = {{"profile_id", id_text}},
            });
        }
        profiles.push_back(std::move(profile));
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not load output layout profiles"));
    }
    return profiles;
}

core::Result<void>
ListRepository::upsert_output_layout_profile(const SavedOutputLayoutProfile& saved_profile) {
    if (auto valid = validate_saved_output_layout_profile(saved_profile); !valid) {
        return valid;
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    const auto id = saved_profile.id.to_string();
    auto count = prepare(
        database, "SELECT COUNT(*), EXISTS(SELECT 1 FROM output_layout_profiles WHERE id = ?) "
                  "FROM output_layout_profiles");
    if (!count || !bind_text(count->get(), 1, id) || sqlite3_step(count->get()) != SQLITE_ROW) {
        auto error = count ? database_error(database, "Could not count output layouts")
                           : std::move(count.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto profile_count = sqlite3_column_int64(count->get(), 0);
    const auto already_exists = sqlite3_column_int(count->get(), 1) != 0;
    count->reset();
    if (!already_exists &&
        profile_count >= static_cast<sqlite3_int64>(maximum_output_layout_profiles)) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "At most 256 output layout profiles can be saved",
            .context = {},
        });
    }
    auto name = prepare(database, "SELECT id FROM output_layout_profiles WHERE name = ?");
    if (!name || !bind_blob(name->get(), 1, saved_profile.profile.name)) {
        auto error = name ? database_error(database, "Could not bind output layout name")
                          : std::move(name.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto name_result = sqlite3_step(name->get());
    const auto name_owner =
        name_result == SQLITE_ROW ? std::optional{column_text(name->get(), 0)} : std::nullopt;
    name->reset();
    if (name_owner && *name_owner != id) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "A saved output layout already uses that exact name",
            .context = {{"name", saved_profile.profile.name}},
        });
    }
    if (name_result != SQLITE_ROW && name_result != SQLITE_DONE) {
        auto error = database_error(database, "Could not check output layout name");
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto& profile = saved_profile.profile;
    auto statement = prepare(
        database,
        "INSERT INTO output_layout_profiles(id, schema_version, name, dialect, dialect_version, "
        "compiler_schema, relative_directory_expression, basename_expression, "
        "sanitization_policy, sanitization_version) VALUES(?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET schema_version=excluded.schema_version, "
        "name=excluded.name, dialect=excluded.dialect, "
        "dialect_version=excluded.dialect_version, compiler_schema=excluded.compiler_schema, "
        "relative_directory_expression=excluded.relative_directory_expression, "
        "basename_expression=excluded.basename_expression, "
        "sanitization_policy=excluded.sanitization_policy, "
        "sanitization_version=excluded.sanitization_version");
    if (!statement || !bind_text(statement->get(), 1, id) ||
        sqlite3_bind_int64(statement->get(), 2, profile.schema_version) != SQLITE_OK ||
        !bind_blob(statement->get(), 3, profile.name) ||
        !bind_blob(statement->get(), 4, profile.dialect.dialect) ||
        sqlite3_bind_int64(statement->get(), 5, profile.dialect.dialect_version) != SQLITE_OK ||
        sqlite3_bind_int64(statement->get(), 6, profile.dialect.compiler_schema) != SQLITE_OK ||
        !bind_blob(statement->get(), 7, profile.relative_directory_expression) ||
        !bind_blob(statement->get(), 8, profile.basename_expression) ||
        !bind_blob(statement->get(), 9, profile.sanitization_policy.name) ||
        sqlite3_bind_int64(statement->get(), 10, profile.sanitization_policy.version) !=
            SQLITE_OK) {
        auto error = statement ? database_error(database, "Could not bind output layout profile")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result =
            step_done(database, statement->get(), "Could not store output layout profile");
        !result) {
        rollback();
        return result;
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<void> ListRepository::remove_output_layout_profile(const core::StableId& id) {
    if (id.is_nil()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "A saved output layout ID cannot be nil",
                                           .context = {}});
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    auto statement = prepare(database, "DELETE FROM output_layout_profiles WHERE id = ?");
    const auto encoded = id.to_string();
    if (!statement || !bind_text(statement->get(), 1, encoded)) {
        auto error = statement ? database_error(database, "Could not bind output layout deletion")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, statement->get(), "Could not remove output layout");
        !result) {
        rollback();
        return result;
    }
    if (sqlite3_changes(database) != 1) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::not_found,
            .message = "Saved output layout was not found",
            .context = {{"profile_id", encoded}},
        });
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<std::vector<SavedDestinationProfile>>
ListRepository::load_destination_profiles() const {
    auto* database = implementation_->database;
    auto statement =
        prepare(database, "SELECT id, schema_version, name, root_raw_path, containment_policy, "
                          "containment_version FROM destination_profiles ORDER BY name, id");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<SavedDestinationProfile> profiles;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        const auto id_text = column_text(statement->get(), 0);
        auto id = core::StableId::parse(id_text);
        const auto schema_version = sqlite3_column_int64(statement->get(), 1);
        const auto containment_version = sqlite3_column_int64(statement->get(), 5);
        if (!id || id->is_nil() || schema_version < 0 || containment_version < 0 ||
            schema_version > std::numeric_limits<std::uint32_t>::max() ||
            containment_version > std::numeric_limits<std::uint32_t>::max() ||
            profiles.size() >= maximum_destination_profiles) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Invalid persisted destination profile",
                .context = {{"profile_id", id_text}},
            });
        }
        SavedDestinationProfile profile{
            .id = *id,
            .profile =
                operations::DestinationProfile{
                    .schema_version = static_cast<std::uint32_t>(schema_version),
                    .name = column_blob(statement->get(), 2),
                    .root_raw_path = column_blob(statement->get(), 3),
                    .containment_policy =
                        operations::PolicyVersion{
                            .name = column_blob(statement->get(), 4),
                            .version = static_cast<std::uint32_t>(containment_version),
                        },
                },
        };
        if (auto valid = validate_saved_destination_profile(profile); !valid) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Persisted destination failed validation: " + valid.error().message,
                .context = {{"profile_id", id_text}},
            });
        }
        profiles.push_back(std::move(profile));
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not load destination profiles"));
    }
    return profiles;
}

core::Result<void>
ListRepository::upsert_destination_profile(const SavedDestinationProfile& saved_profile) {
    if (auto valid = validate_saved_destination_profile(saved_profile); !valid) {
        return valid;
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    const auto id = saved_profile.id.to_string();
    auto count = prepare(database,
                         "SELECT COUNT(*), EXISTS(SELECT 1 FROM destination_profiles WHERE id = ?) "
                         "FROM destination_profiles");
    if (!count || !bind_text(count->get(), 1, id) || sqlite3_step(count->get()) != SQLITE_ROW) {
        auto error = count ? database_error(database, "Could not count destinations")
                           : std::move(count.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto profile_count = sqlite3_column_int64(count->get(), 0);
    const auto already_exists = sqlite3_column_int(count->get(), 1) != 0;
    count->reset();
    if (!already_exists &&
        profile_count >= static_cast<sqlite3_int64>(maximum_destination_profiles)) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "At most 256 destination profiles can be saved",
            .context = {},
        });
    }
    auto name = prepare(database, "SELECT id FROM destination_profiles WHERE name = ?");
    if (!name || !bind_blob(name->get(), 1, saved_profile.profile.name)) {
        auto error = name ? database_error(database, "Could not bind destination name")
                          : std::move(name.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto name_result = sqlite3_step(name->get());
    const auto name_owner =
        name_result == SQLITE_ROW ? std::optional{column_text(name->get(), 0)} : std::nullopt;
    name->reset();
    if (name_owner && *name_owner != id) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "A saved destination already uses that exact name",
            .context = {{"name", saved_profile.profile.name}},
        });
    }
    if (name_result != SQLITE_ROW && name_result != SQLITE_DONE) {
        auto error = database_error(database, "Could not check destination name");
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto& profile = saved_profile.profile;
    auto statement = prepare(
        database, "INSERT INTO destination_profiles(id, schema_version, name, root_raw_path, "
                  "containment_policy, containment_version) VALUES(?,?,?,?,?,?) "
                  "ON CONFLICT(id) DO UPDATE SET schema_version=excluded.schema_version, "
                  "name=excluded.name, root_raw_path=excluded.root_raw_path, "
                  "containment_policy=excluded.containment_policy, "
                  "containment_version=excluded.containment_version");
    if (!statement || !bind_text(statement->get(), 1, id) ||
        sqlite3_bind_int64(statement->get(), 2, profile.schema_version) != SQLITE_OK ||
        !bind_blob(statement->get(), 3, profile.name) ||
        !bind_blob(statement->get(), 4, profile.root_raw_path) ||
        !bind_blob(statement->get(), 5, profile.containment_policy.name) ||
        sqlite3_bind_int64(statement->get(), 6, profile.containment_policy.version) != SQLITE_OK) {
        auto error = statement ? database_error(database, "Could not bind destination profile")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, statement->get(), "Could not store destination profile");
        !result) {
        rollback();
        return result;
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<void> ListRepository::remove_destination_profile(const core::StableId& id) {
    if (id.is_nil()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "A saved destination ID cannot be nil",
                                           .context = {}});
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    auto statement = prepare(database, "DELETE FROM destination_profiles WHERE id = ?");
    const auto encoded = id.to_string();
    if (!statement || !bind_text(statement->get(), 1, encoded)) {
        auto error = statement ? database_error(database, "Could not bind destination deletion")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, statement->get(), "Could not remove destination");
        !result) {
        rollback();
        return result;
    }
    if (sqlite3_changes(database) != 1) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::not_found,
            .message = "Saved destination was not found",
            .context = {{"profile_id", encoded}},
        });
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<std::vector<SavedEncoderPreset>> ListRepository::load_encoder_presets() const {
    auto* database = implementation_->database;
    auto statement =
        prepare(database, "SELECT id, preset_id, preset_version, display_name, codec_name, "
                          "container_name, file_extension, lossless, bit_rate, vbr_quality, "
                          "sample_format_hint FROM encoder_presets ORDER BY display_name, id");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<SavedEncoderPreset> presets;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        const auto id_text = column_text(statement->get(), 0);
        auto id = core::StableId::parse(id_text);
        const auto preset_version = sqlite3_column_int64(statement->get(), 2);
        const auto lossless = sqlite3_column_int64(statement->get(), 7);
        if (!id || id->is_nil() || preset_version < 1 ||
            preset_version > std::numeric_limits<int>::max() || (lossless != 0 && lossless != 1) ||
            presets.size() >= maximum_encoder_presets) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Invalid persisted encoder preset",
                .context = {{"preset_id", id_text}},
            });
        }
        SavedEncoderPreset preset{
            .id = *id,
            .preset =
                convert::EncoderPreset{
                    .id = column_blob(statement->get(), 1),
                    .version = static_cast<int>(preset_version),
                    .display_name = column_blob(statement->get(), 3),
                    .codec_name = column_blob(statement->get(), 4),
                    .container_name = column_blob(statement->get(), 5),
                    .file_extension = column_blob(statement->get(), 6),
                    .lossless = lossless == 1,
                    .bit_rate = sqlite3_column_type(statement->get(), 8) == SQLITE_NULL
                                    ? std::nullopt
                                    : std::optional{static_cast<std::int64_t>(
                                          sqlite3_column_int64(statement->get(), 8))},
                    .vbr_quality = sqlite3_column_type(statement->get(), 9) == SQLITE_NULL
                                       ? std::nullopt
                                       : std::optional{static_cast<int>(
                                             sqlite3_column_int64(statement->get(), 9))},
                    .sample_format_hint = column_blob(statement->get(), 10),
                },
        };
        if (auto valid = validate_saved_encoder_preset(preset); !valid) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Persisted encoder preset failed validation: " + valid.error().message,
                .context = {{"preset_id", id_text}},
            });
        }
        presets.push_back(std::move(preset));
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not load encoder presets"));
    }
    return presets;
}

core::Result<void> ListRepository::upsert_encoder_preset(const SavedEncoderPreset& saved_preset) {
    if (auto valid = validate_saved_encoder_preset(saved_preset); !valid) {
        return valid;
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    const auto id = saved_preset.id.to_string();
    auto count =
        prepare(database, "SELECT COUNT(*), EXISTS(SELECT 1 FROM encoder_presets WHERE id = ?) "
                          "FROM encoder_presets");
    if (!count || !bind_text(count->get(), 1, id) || sqlite3_step(count->get()) != SQLITE_ROW) {
        auto error = count ? database_error(database, "Could not count encoder presets")
                           : std::move(count.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto preset_count = sqlite3_column_int64(count->get(), 0);
    const auto already_exists = sqlite3_column_int(count->get(), 1) != 0;
    count->reset();
    if (!already_exists && preset_count >= static_cast<sqlite3_int64>(maximum_encoder_presets)) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "At most 256 encoder presets can be saved",
            .context = {},
        });
    }
    auto name = prepare(database, "SELECT id FROM encoder_presets WHERE display_name = ?");
    if (!name || !bind_blob(name->get(), 1, saved_preset.preset.display_name)) {
        auto error = name ? database_error(database, "Could not bind encoder preset name")
                          : std::move(name.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto name_result = sqlite3_step(name->get());
    const auto name_owner =
        name_result == SQLITE_ROW ? std::optional{column_text(name->get(), 0)} : std::nullopt;
    name->reset();
    if (name_owner && *name_owner != id) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "A saved encoder preset already uses that exact name",
            .context = {{"name", saved_preset.preset.display_name}},
        });
    }
    if (name_result != SQLITE_ROW && name_result != SQLITE_DONE) {
        auto error = database_error(database, "Could not check encoder preset name");
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto& preset = saved_preset.preset;
    auto statement = prepare(
        database,
        "INSERT INTO encoder_presets(id, preset_id, preset_version, display_name, codec_name, "
        "container_name, file_extension, lossless, bit_rate, vbr_quality, sample_format_hint) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET preset_id=excluded.preset_id, "
        "preset_version=excluded.preset_version, display_name=excluded.display_name, "
        "codec_name=excluded.codec_name, container_name=excluded.container_name, "
        "file_extension=excluded.file_extension, lossless=excluded.lossless, "
        "bit_rate=excluded.bit_rate, vbr_quality=excluded.vbr_quality, "
        "sample_format_hint=excluded.sample_format_hint");
    const auto bind_optional_int = [](sqlite3_stmt* target, const int column,
                                      const std::optional<std::int64_t> value) {
        return value ? sqlite3_bind_int64(target, column, *value) == SQLITE_OK
                     : sqlite3_bind_null(target, column) == SQLITE_OK;
    };
    if (!statement || !bind_text(statement->get(), 1, id) ||
        !bind_blob(statement->get(), 2, preset.id) ||
        sqlite3_bind_int64(statement->get(), 3, preset.version) != SQLITE_OK ||
        !bind_blob(statement->get(), 4, preset.display_name) ||
        !bind_blob(statement->get(), 5, preset.codec_name) ||
        !bind_blob(statement->get(), 6, preset.container_name) ||
        !bind_blob(statement->get(), 7, preset.file_extension) ||
        sqlite3_bind_int64(statement->get(), 8, preset.lossless ? 1 : 0) != SQLITE_OK ||
        !bind_optional_int(statement->get(), 9, preset.bit_rate) ||
        !bind_optional_int(statement->get(), 10,
                           preset.vbr_quality
                               ? std::optional{static_cast<std::int64_t>(*preset.vbr_quality)}
                               : std::nullopt) ||
        !bind_blob(statement->get(), 11, preset.sample_format_hint)) {
        auto error = statement ? database_error(database, "Could not bind encoder preset")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, statement->get(), "Could not store encoder preset");
        !result) {
        rollback();
        return result;
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<void> ListRepository::remove_encoder_preset(const core::StableId& id) {
    if (id.is_nil()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "A saved encoder preset ID cannot be nil",
                                           .context = {}});
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    auto statement = prepare(database, "DELETE FROM encoder_presets WHERE id = ?");
    const auto encoded = id.to_string();
    if (!statement || !bind_text(statement->get(), 1, encoded)) {
        auto error = statement ? database_error(database, "Could not bind encoder preset deletion")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, statement->get(), "Could not remove encoder preset");
        !result) {
        rollback();
        return result;
    }
    if (sqlite3_changes(database) != 1) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::not_found,
            .message = "Saved encoder preset was not found",
            .context = {{"preset_id", encoded}},
        });
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

namespace {
core::Result<void> validate_search(const SavedSearch& search) {
    if (search.id.is_nil() || search.name.empty() || search.name.size() > 256U ||
        search.name.find('\0') != std::string::npos || search.expression.empty() ||
        search.expression.size() > 4096U ||
        (search.dialect != "tkq-1" && search.dialect != "words-1") ||
        (search.scope != SavedSearchScope::library &&
         search.scope != SavedSearchScope::current_tab &&
         search.scope != SavedSearchScope::server) ||
        search.revision >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "Invalid saved search (name: 1–256 bytes; query: 1–4096 bytes)",
                        .context = {}});
    }
    if (!core::unicodeCodePointCount(search.name) ||
        search.expression.find('\0') != std::string::npos ||
        search.name.find_first_not_of(" \t\r\n") == std::string::npos) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "Saved searches require a valid name and query",
                        .context = {}});
    }
    const auto compiled = search.dialect == "tkq-1"
                              ? query::compile_tkq(search.expression)
                              : query::compile_tkq_word_search(search.expression);
    if (!compiled) {
        return std::unexpected(compiled.error());
    }
    return {};
}
} // namespace

core::Result<std::vector<SavedSearch>> ListRepository::load_saved_searches() const {
    auto* database = implementation_->database;
    auto statement = prepare(database, "SELECT id, name, expression, dialect, scope, revision FROM "
                                       "saved_searches ORDER BY name, id");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    std::vector<SavedSearch> searches;
    int result;
    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        const auto id = core::StableId::parse(column_text(statement->get(), 0));
        const auto revision = sqlite3_column_int64(statement->get(), 5);
        const auto scope = sqlite3_column_int(statement->get(), 4);
        if (!id || revision < 1 || scope < 0 || scope > 1 || searches.size() >= 256U) {
            return std::unexpected(database_error(database, "Invalid saved search record"));
        }
        SavedSearch search{.id = *id,
                           .name = column_blob(statement->get(), 1),
                           .expression = column_blob(statement->get(), 2),
                           .dialect = column_text(statement->get(), 3),
                           .scope = static_cast<SavedSearchScope>(scope),
                           .revision = static_cast<std::uint64_t>(revision)};
        if (auto valid = validate_search(search); !valid) {
            return std::unexpected(valid.error());
        }
        searches.push_back(std::move(search));
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not load saved searches"));
    }
    return searches;
}

core::Result<void> ListRepository::save_search(const SavedSearch& search) {
    if (auto valid = validate_search(search); !valid) {
        return valid;
    }
    auto* database = implementation_->database;
    auto statement = prepare(
        database, search.revision == 0
                      ? "INSERT INTO saved_searches(id,name,expression,dialect,scope,revision) "
                        "SELECT ?1,?2,?3,?4,?5,1 WHERE (SELECT count(*) FROM saved_searches) < 256"
                      : "UPDATE saved_searches SET name=?2, expression=?3, dialect=?4, scope=?5, "
                        "revision=revision+1 WHERE id=?1 AND revision=?6");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    auto* stmt = statement->get();
    if (!bind_text(stmt, 1, search.id.to_string()) || !bind_blob(stmt, 2, search.name) ||
        !bind_blob(stmt, 3, search.expression) || !bind_text(stmt, 4, search.dialect) ||
        sqlite3_bind_int(stmt, 5, static_cast<int>(search.scope)) != SQLITE_OK ||
        (search.revision != 0 &&
         sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(search.revision)) != SQLITE_OK)) {
        return std::unexpected(database_error(database, "Could not bind saved search"));
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        if (sqlite3_errcode(database) == SQLITE_CONSTRAINT) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::conflict,
                            .message = "A saved search with this name or identity already exists",
                            .context = {}});
        }
        return std::unexpected(database_error(database, "Could not save search"));
    }
    if (sqlite3_changes(database) != 1) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = search.revision == 0
                           ? "At most 256 searches can be saved"
                           : "Saved search changed or was deleted; reopen Search to reload it",
            .context = {}});
    }
    return {};
}

core::Result<void> ListRepository::remove_search(const SavedSearch& expected) {
    auto* database = implementation_->database;
    auto statement = prepare(database, "DELETE FROM saved_searches WHERE id=?1 AND revision=?2");
    if (!statement || !bind_text(statement->get(), 1, expected.id.to_string()) ||
        expected.revision == 0 ||
        expected.revision > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()) ||
        sqlite3_bind_int64(statement->get(), 2, static_cast<sqlite3_int64>(expected.revision)) !=
            SQLITE_OK) {
        return std::unexpected(database_error(database, "Invalid saved search deletion"));
    }
    if (auto done = step_done(database, statement->get(), "Could not delete saved search"); !done) {
        return done;
    }
    if (sqlite3_changes(database) != 1) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "Saved search changed or was deleted; reopen Search to reload it",
            .context = {}});
    }
    return {};
}

core::Result<std::string> ListRepository::local_listening_key(const ListItem& source) {
    if (!valid_listening_source(source)) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "Listening history requires a qualified local source",
                        .context = {}});
    }
    auto id =
        listening_source(implementation_->database,
                         listening_observation(source.source_reference, *source.source_revision));
    if (!id)
        return std::unexpected(std::move(id.error()));
    // Identity follows playable content, not a CUE filename or list occurrence.
    return listening_track_key(*id, source);
}

core::Result<std::string> local_listening_observation(const ListItem& source) {
    if (!valid_listening_source(source))
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "Listening history requires a qualified local source",
                        .context = {}});
    return listening_observation(source.source_reference, *source.source_revision);
}

std::string local_listening_track_hash(const std::string& source_id, const ListItem& source) {
    return listening_track_key(source_id, source);
}

core::Result<std::optional<LocalListeningHistory>>
ListRepository::lookup_local_listening_history(const ListItem& source) const {
    if (!valid_listening_source(source))
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "Listening history requires a qualified local source",
                        .context = {}});
    auto* database = implementation_->database;
    auto query =
        prepare(database, "SELECT source_id FROM local_listening_sources WHERE observation_hash=?");
    if (!query ||
        !bind_text(query->get(), 1,
                   listening_observation(source.source_reference, *source.source_revision)))
        return std::unexpected(database_error(database, "Could not query listening identity"));
    const auto step = sqlite3_step(query->get());
    if (step == SQLITE_DONE)
        return std::optional<LocalListeningHistory>{};
    if (step != SQLITE_ROW)
        return std::unexpected(database_error(database, "Could not read listening identity"));
    return load_local_listening_history(listening_track_key(column_text(query->get(), 0), source));
}

core::Result<void> ListRepository::record_local_listen(const ListItem& source,
                                                       const core::StableId occurrence_id,
                                                       const std::int64_t played_at_ms) {
    if (occurrence_id.is_nil() || played_at_ms <= 0)
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "A listen requires an occurrence and timestamp",
                        .context = {}});
    auto* database = implementation_->database;
    if (auto begun = execute(database, "BEGIN IMMEDIATE"); !begun)
        return begun;
    const auto finish = [database](core::Result<void> result) {
        if (result)
            result = execute(database, "COMMIT");
        if (!result)
            static_cast<void>(execute(database, "ROLLBACK"));
        return result;
    };
    auto key = local_listening_key(source);
    if (!key)
        return finish(std::unexpected(std::move(key.error())));
    auto event = prepare(
        database, "INSERT INTO local_listening_occurrences(occurrence_id,track_hash,played_at_ms) "
                  "VALUES(?,?,?) ON CONFLICT(occurrence_id) DO NOTHING");
    if (!event || !bind_text(event->get(), 1, occurrence_id.to_string()) ||
        !bind_text(event->get(), 2, *key) ||
        sqlite3_bind_int64(event->get(), 3, played_at_ms) != SQLITE_OK ||
        sqlite3_step(event->get()) != SQLITE_DONE)
        return finish(
            std::unexpected(database_error(database, "Could not store listening occurrence")));
    if (sqlite3_changes(database) != 0) {
        // Qualification can occur halfway through a track. It is not completion
        // and must not clear a saved resume position.
        auto increment = prepare(
            database,
            "INSERT INTO local_listening_history(track_hash,play_count,last_played_ms) "
            "VALUES(?,1,?) ON CONFLICT(track_hash) DO UPDATE SET "
            "play_count=play_count+1,last_played_ms=max(last_played_ms,excluded.last_played_ms)");
        if (!increment || !bind_text(increment->get(), 1, *key) ||
            sqlite3_bind_int64(increment->get(), 2, played_at_ms) != SQLITE_OK)
            return finish(
                std::unexpected(database_error(database, "Could not bind qualified listen")));
        return finish(step_done(database, increment->get(), "Could not count qualified listen"));
    }
    auto existing =
        prepare(database, "SELECT track_hash,played_at_ms FROM local_listening_occurrences "
                          "WHERE occurrence_id=?");
    if (!existing || !bind_text(existing->get(), 1, occurrence_id.to_string()) ||
        sqlite3_step(existing->get()) != SQLITE_ROW)
        return finish(
            std::unexpected(database_error(database, "Could not read listening occurrence")));
    if (column_text(existing->get(), 0) != *key ||
        sqlite3_column_int64(existing->get(), 1) != played_at_ms)
        return finish(std::unexpected(core::Error{.code = core::ErrorCode::conflict,
                                                  .message = "Listening occurrence replay differs",
                                                  .context = {}}));
    return finish({});
}

core::Result<std::optional<LocalListeningHistory>>
ListRepository::load_local_listening_history(const std::string_view track_hash) const {
    if (track_hash.size() != 64U ||
        track_hash.find_first_not_of("0123456789abcdef") != std::string_view::npos) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "Invalid listening-history track identity",
                                           .context = {}});
    }
    auto* database = implementation_->database;
    auto statement = prepare(database, "SELECT play_count,last_played_ms,resume_position_ms,"
                                       "resume_updated_at_ms FROM local_listening_history "
                                       "WHERE track_hash=?");
    if (!statement || !bind_text(statement->get(), 1, track_hash)) {
        return std::unexpected(statement
                                   ? database_error(database, "Could not bind listening history")
                                   : std::move(statement.error()));
    }
    const auto result = sqlite3_step(statement->get());
    if (result == SQLITE_DONE)
        return std::optional<LocalListeningHistory>{};
    if (result != SQLITE_ROW)
        return std::unexpected(database_error(database, "Could not load listening history"));
    const auto count = sqlite3_column_int64(statement->get(), 0);
    const auto played = sqlite3_column_int64(statement->get(), 1);
    const auto position = sqlite3_column_int64(statement->get(), 2);
    const auto updated = sqlite3_column_int64(statement->get(), 3);
    if (count < 0 || played < 0 || position < 0 || updated < 0) {
        return std::unexpected(database_error(database, "Invalid listening history record"));
    }
    return std::optional{LocalListeningHistory{
        .track_hash = std::string{track_hash},
        .play_count = static_cast<std::uint64_t>(count),
        .last_played_ms = played,
        .resume_position_ms = position,
        .updated_at_ms = updated,
    }};
}

core::Result<void> ListRepository::record_local_play(const std::string_view track_hash,
                                                     const std::int64_t played_at_ms) {
    if (played_at_ms <= 0) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "A completed play requires a timestamp",
                                           .context = {}});
    }
    if (auto loaded = load_local_listening_history(track_hash); !loaded)
        return std::unexpected(std::move(loaded.error()));
    auto* database = implementation_->database;
    auto statement =
        prepare(database,
                "INSERT INTO local_listening_history(track_hash,play_count,last_played_ms,"
                "resume_position_ms,resume_updated_at_ms) VALUES(?,1,?,0,?) "
                "ON CONFLICT(track_hash) DO UPDATE SET play_count=play_count+1,"
                "last_played_ms=max(last_played_ms,excluded.last_played_ms),"
                "resume_position_ms=CASE WHEN excluded.resume_updated_at_ms>=resume_updated_at_ms "
                "THEN 0 ELSE resume_position_ms END,"
                "resume_updated_at_ms=max(resume_updated_at_ms,excluded.resume_updated_at_ms)");
    if (!statement || !bind_text(statement->get(), 1, track_hash) ||
        sqlite3_bind_int64(statement->get(), 2, played_at_ms) != SQLITE_OK ||
        sqlite3_bind_int64(statement->get(), 3, played_at_ms) != SQLITE_OK) {
        return std::unexpected(statement ? database_error(database, "Could not bind completed play")
                                         : std::move(statement.error()));
    }
    return step_done(database, statement->get(), "Could not record completed play");
}

core::Result<void> ListRepository::save_engine_state(const std::string_view key,
                                                     const std::string_view value,
                                                     const std::int64_t updated_at_ms) {
    if (key.empty() || updated_at_ms <= 0) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "Invalid engine state write",
                                           .context = {}});
    }
    auto* database = implementation_->database;
    auto statement =
        prepare(database, "INSERT INTO engine_state(key,value,updated_at_ms) VALUES(?,?,?) "
                          "ON CONFLICT(key) DO UPDATE SET value=excluded.value,"
                          "updated_at_ms=excluded.updated_at_ms");
    if (!statement || !bind_text(statement->get(), 1, key) ||
        !bind_text(statement->get(), 2, value) ||
        sqlite3_bind_int64(statement->get(), 3, updated_at_ms) != SQLITE_OK) {
        return std::unexpected(statement ? database_error(database, "Could not bind engine state")
                                         : std::move(statement.error()));
    }
    return step_done(database, statement->get(), "Could not save engine state");
}

core::Result<std::optional<std::string>>
ListRepository::load_engine_state(const std::string_view key) const {
    auto* database = implementation_->database;
    auto statement = prepare(database, "SELECT value FROM engine_state WHERE key = ?");
    if (!statement || !bind_text(statement->get(), 1, key)) {
        return std::unexpected(statement ? database_error(database, "Could not bind engine state")
                                         : std::move(statement.error()));
    }
    const auto stepped = sqlite3_step(statement->get());
    if (stepped == SQLITE_ROW) {
        return std::optional{column_text(statement->get(), 0)};
    }
    if (stepped != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not read engine state"));
    }
    return std::optional<std::string>{};
}

core::Result<void> ListRepository::save_local_resume(const std::string_view track_hash,
                                                     const std::int64_t position_ms,
                                                     const std::int64_t updated_at_ms) {
    if (position_ms < 0 || updated_at_ms <= 0) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "Invalid local resume observation",
                                           .context = {}});
    }
    if (auto loaded = load_local_listening_history(track_hash); !loaded)
        return std::unexpected(std::move(loaded.error()));
    auto* database = implementation_->database;
    auto statement = prepare(
        database, "INSERT INTO local_listening_history(track_hash,resume_position_ms,"
                  "resume_updated_at_ms) VALUES(?,?,?) ON CONFLICT(track_hash) DO UPDATE SET "
                  "resume_position_ms=excluded.resume_position_ms,"
                  "resume_updated_at_ms=excluded.resume_updated_at_ms "
                  "WHERE excluded.resume_updated_at_ms>resume_updated_at_ms");
    if (!statement || !bind_text(statement->get(), 1, track_hash) ||
        sqlite3_bind_int64(statement->get(), 2, position_ms) != SQLITE_OK ||
        sqlite3_bind_int64(statement->get(), 3, updated_at_ms) != SQLITE_OK) {
        return std::unexpected(statement ? database_error(database, "Could not bind local resume")
                                         : std::move(statement.error()));
    }
    return step_done(database, statement->get(), "Could not save local resume");
}

namespace {

// ADR-0233: bounds, so a confused client cannot grow the engine's database
// without limit.
constexpr std::size_t maximum_engine_lists = 4'096U;
constexpr std::size_t maximum_engine_list_name_bytes = 1'024U;
constexpr std::size_t maximum_engine_list_text_bytes = 4'096U;

[[nodiscard]] core::Error engine_list_conflict(std::string message) {
    return core::Error{
        .code = core::ErrorCode::conflict, .message = std::move(message), .context = {}};
}

[[nodiscard]] core::Result<void> validate_engine_list(const std::string_view name,
                                                      const std::vector<EngineListItem>& items) {
    const auto invalid = [](std::string message) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = std::move(message),
                                           .context = {}});
    };
    if (name.empty() || name.size() > maximum_engine_list_name_bytes) {
        return invalid("A list needs a name of at most 1024 bytes");
    }
    if (items.size() > maximum_items_per_document) {
        return invalid("A list holds at most 1000000 entries");
    }
    std::unordered_set<std::string> entries;
    entries.reserve(items.size());
    for (const auto& item : items) {
        if (item.raw_path.empty() || item.entry_id.is_nil() ||
            !entries.insert(item.entry_id.to_string()).second) {
            return invalid("Every list entry needs a path and an identity of its own");
        }
        if (item.title.size() > maximum_engine_list_text_bytes ||
            item.artist.size() > maximum_engine_list_text_bytes ||
            item.album.size() > maximum_engine_list_text_bytes ||
            item.album_artist.size() > maximum_engine_list_text_bytes ||
            item.date.size() > maximum_engine_list_text_bytes) {
            return invalid("A list entry's tags are at most 4096 bytes each");
        }
    }
    return {};
}

// The stored revision, or nothing for a list that does not exist.
[[nodiscard]] core::Result<std::optional<std::uint64_t>>
engine_list_revision(sqlite3* database, const core::StableId& id) {
    auto statement = prepare(database, "SELECT revision FROM engine_lists WHERE id=?1");
    if (!statement || !bind_text(statement->get(), 1, id.to_string())) {
        return std::unexpected(database_error(database, "Could not read a list's revision"));
    }
    const auto step = sqlite3_step(statement->get());
    if (step == SQLITE_DONE) {
        return std::optional<std::uint64_t>{};
    }
    if (step != SQLITE_ROW) {
        return std::unexpected(database_error(database, "Could not read a list's revision"));
    }
    return std::optional{static_cast<std::uint64_t>(sqlite3_column_int64(statement->get(), 0))};
}

// Refuses a write made against a revision that is no longer the stored one.
[[nodiscard]] core::Result<void> check_revision(const std::optional<std::uint64_t>& stored,
                                                const std::optional<std::uint64_t>& expected) {
    if (!expected) {
        return {};
    }
    if (*expected == 0U && stored) {
        return std::unexpected(engine_list_conflict("A list with this identity already exists"));
    }
    if (*expected != 0U && !stored) {
        return std::unexpected(engine_list_conflict("The list was deleted meanwhile"));
    }
    if (*expected != 0U && *stored != *expected) {
        return std::unexpected(engine_list_conflict("The list was changed meanwhile"));
    }
    return {};
}

[[nodiscard]] core::Result<std::vector<EngineListSummary>>
select_engine_list_summaries(sqlite3* database, const std::optional<core::StableId>& only) {
    const std::string sql =
        std::string{"SELECT id, name, revision, modified_ms, (SELECT count(*) FROM "
                    "engine_list_items i WHERE i.list_id=p.id), kind, draft_of, draft_base "
                    "FROM engine_lists"} +
        (only ? " p WHERE id=?1" : " p ORDER BY name, id");
    auto statement = prepare(database, sql.c_str());
    if (!statement || (only && !bind_text(statement->get(), 1, only->to_string()))) {
        return std::unexpected(database_error(database, "Could not list lists"));
    }
    std::vector<EngineListSummary> summaries;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement->get())) == SQLITE_ROW) {
        auto* row = statement->get();
        const auto id = core::StableId::parse(column_text(row, 0));
        const auto revision = sqlite3_column_int64(row, 2);
        const auto kind = sqlite3_column_int(row, 5);
        if (!id || revision < 1 || kind < 0 || kind > 1 ||
            summaries.size() >= maximum_engine_lists) {
            return std::unexpected(database_error(database, "Invalid stored list"));
        }
        std::optional<core::StableId> draft_of;
        if (sqlite3_column_type(row, 6) != SQLITE_NULL) {
            const auto drafted = core::StableId::parse(column_text(row, 6));
            if (!drafted) {
                return std::unexpected(database_error(database, "Invalid stored list"));
            }
            draft_of = *drafted;
        }
        summaries.push_back(
            EngineListSummary{.id = *id,
                              .name = column_blob(row, 1),
                              .kind = static_cast<EngineListKind>(kind),
                              .revision = static_cast<std::uint64_t>(revision),
                              .tracks = static_cast<std::size_t>(sqlite3_column_int64(row, 4)),
                              .modified_ms = sqlite3_column_int64(row, 3),
                              .draft_of = draft_of,
                              .draft_base = static_cast<std::uint64_t>(
                                  std::max<sqlite3_int64>(0, sqlite3_column_int64(row, 7)))});
    }
    if (step != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not list lists"));
    }
    return summaries;
}

[[nodiscard]] bool bind_optional_int(sqlite3_stmt* statement, const int index,
                                     const std::optional<std::int64_t>& value) {
    return (value ? sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(*value))
                  : sqlite3_bind_null(statement, index)) == SQLITE_OK;
}

[[nodiscard]] bool bind_optional_real(sqlite3_stmt* statement, const int index,
                                      const std::optional<double>& value) {
    return (value ? sqlite3_bind_double(statement, index, *value)
                  : sqlite3_bind_null(statement, index)) == SQLITE_OK;
}

[[nodiscard]] std::optional<double> column_optional_real(sqlite3_stmt* statement,
                                                         const int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_double(statement, column);
}

[[nodiscard]] std::optional<std::int64_t> column_optional_int(sqlite3_stmt* statement,
                                                              const int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(statement, column);
}

[[nodiscard]] core::Result<void> insert_engine_list_items(sqlite3* database,
                                                          const core::StableId& id,
                                                          std::span<const EngineListItem> items,
                                                          const std::int64_t first_position = 0) {
    auto statement = prepare(
        database, "INSERT INTO engine_list_items(list_id, position, entry_id, raw_path, "
                  "logical_reference, segment_start_sample, segment_end_sample, "
                  "audio_stream_index, subsong_index, duration_ms, title, artist, album, "
                  "album_artist, date, track_gain_db, track_peak, album_gain_db, album_peak) "
                  "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19)");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    const auto list = id.to_string();
    for (std::size_t position = 0; position < items.size(); ++position) {
        const auto& item = items[position];
        auto* raw = statement->get();
        const auto selection = item.source_selection.value_or(ListItemSourceSelection{});
        const auto gain = item.replay_gain.value_or(ListItemReplayGain{});
        const auto stream = selection.audio_stream_index
                                ? std::optional<std::int64_t>{*selection.audio_stream_index}
                                : std::nullopt;
        const auto subsong = selection.subsong_index
                                 ? std::optional<std::int64_t>{*selection.subsong_index}
                                 : std::nullopt;
        const bool bound =
            bind_text(raw, 1, list) &&
            sqlite3_bind_int64(raw, 2,
                               static_cast<sqlite3_int64>(first_position) +
                                   static_cast<sqlite3_int64>(position)) == SQLITE_OK &&
            bind_text(raw, 3, item.entry_id.to_string()) && bind_blob(raw, 4, item.raw_path) &&
            (item.logical_reference ? bind_blob(raw, 5, *item.logical_reference)
                                    : sqlite3_bind_null(raw, 5) == SQLITE_OK) &&
            bind_optional_int(
                raw, 6, item.segment ? std::optional{item.segment->start_sample} : std::nullopt) &&
            bind_optional_int(raw, 7, item.segment ? item.segment->end_sample : std::nullopt) &&
            bind_optional_int(raw, 8, stream) && bind_optional_int(raw, 9, subsong) &&
            bind_optional_int(raw, 10, item.duration_ms) && bind_blob(raw, 11, item.title) &&
            bind_blob(raw, 12, item.artist) && bind_blob(raw, 13, item.album) &&
            bind_blob(raw, 14, item.album_artist) && bind_blob(raw, 15, item.date) &&
            bind_optional_real(raw, 16, gain.track_gain_db) &&
            bind_optional_real(raw, 17, gain.track_peak) &&
            bind_optional_real(raw, 18, gain.album_gain_db) &&
            bind_optional_real(raw, 19, gain.album_peak);
        if (!bound) {
            return std::unexpected(database_error(database, "Could not bind a list entry"));
        }
        if (auto done = step_done(database, raw, "Could not store a list entry"); !done) {
            return done;
        }
    }
    return {};
}

// A list's entries in order; with `positions`, where each is stored too.
[[nodiscard]] core::Result<std::vector<EngineListItem>>
select_engine_list_items(sqlite3* database, const core::StableId& id,
                         std::vector<std::int64_t>* positions = nullptr) {
    std::vector<EngineListItem> items;
    const auto sql = std::string{"SELECT entry_id, raw_path, logical_reference, "
                                 "segment_start_sample, segment_end_sample, audio_stream_index, "
                                 "subsong_index, duration_ms, title, artist, album, album_artist, "
                                 "date, track_gain_db, track_peak, album_gain_db, album_peak, "
                                 "position FROM engine_list_items "
                                 "WHERE list_id=?1 ORDER BY position"};
    auto statement = prepare(database, sql.c_str());
    if (!statement || !bind_text(statement->get(), 1, id.to_string())) {
        return std::unexpected(database_error(database, "Could not read a list"));
    }
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement->get())) == SQLITE_ROW) {
        auto* row = statement->get();
        const auto entry = core::StableId::parse(column_text(row, 0));
        if (!entry) {
            return std::unexpected(database_error(database, "Invalid stored list entry"));
        }
        EngineListItem item{.entry_id = *entry,
                            .raw_path = column_blob(row, 1),
                            .logical_reference = std::nullopt,
                            .segment = std::nullopt,
                            .source_selection = std::nullopt,
                            .duration_ms = column_optional_int(row, 7),
                            .title = column_blob(row, 8),
                            .artist = column_blob(row, 9),
                            .album = column_blob(row, 10),
                            .album_artist = column_blob(row, 11),
                            .date = column_blob(row, 12),
                            .replay_gain = std::nullopt};
        const ListItemReplayGain gain{.track_gain_db = column_optional_real(row, 13),
                                      .track_peak = column_optional_real(row, 14),
                                      .album_gain_db = column_optional_real(row, 15),
                                      .album_peak = column_optional_real(row, 16)};
        if (gain != ListItemReplayGain{}) {
            item.replay_gain = gain;
        }
        if (positions != nullptr) {
            positions->push_back(sqlite3_column_int64(row, 17));
        }
        if (sqlite3_column_type(row, 2) != SQLITE_NULL) {
            item.logical_reference = column_blob(row, 2);
        }
        if (const auto start = column_optional_int(row, 3)) {
            item.segment =
                ListItemSegment{.start_sample = *start, .end_sample = column_optional_int(row, 4)};
        }
        const auto stream = column_optional_int(row, 5);
        const auto subsong = column_optional_int(row, 6);
        if (stream || subsong) {
            item.source_selection = ListItemSourceSelection{
                .audio_stream_index =
                    stream ? std::optional<int>{static_cast<int>(*stream)} : std::nullopt,
                .subsong_index =
                    subsong ? std::optional<int>{static_cast<int>(*subsong)} : std::nullopt};
        }
        items.push_back(std::move(item));
    }
    if (step != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not read a list"));
    }
    return items;
}

// Runs `work` inside BEGIN IMMEDIATE, committing when it succeeds.
template <typename Work>
[[nodiscard]] auto in_transaction(sqlite3* database, Work work) -> decltype(work()) {
    if (auto begun = execute(database, "BEGIN IMMEDIATE"); !begun) {
        return std::unexpected(std::move(begun.error()));
    }
    auto result = work();
    if (!result) {
        static_cast<void>(execute(database, "ROLLBACK"));
        return result;
    }
    if (auto committed = execute(database, "COMMIT"); !committed) {
        static_cast<void>(execute(database, "ROLLBACK"));
        return std::unexpected(std::move(committed.error()));
    }
    return result;
}

} // namespace

core::Result<std::vector<EngineListSummary>> ListRepository::load_engine_lists() const {
    return select_engine_list_summaries(implementation_->database, std::nullopt);
}

core::Result<std::optional<EngineList>>
ListRepository::load_engine_list(const core::StableId& id) const {
    auto* database = implementation_->database;
    auto summaries = select_engine_list_summaries(database, id);
    if (!summaries) {
        return std::unexpected(std::move(summaries.error()));
    }
    if (summaries->empty()) {
        return std::optional<EngineList>{};
    }
    EngineList list{.summary = std::move(summaries->front()), .items = {}};
    auto items = select_engine_list_items(database, id);
    if (!items) {
        return std::unexpected(std::move(items.error()));
    }
    list.items = std::move(*items);
    return std::optional{std::move(list)};
}

core::Result<std::vector<std::optional<std::pair<std::string, core::LocalSourceRevision>>>>
ListRepository::resolve_local_source_moves(
    const std::vector<std::pair<std::string, core::LocalSourceRevision>>& sources) const {
    auto resolver = local_source_relocation_resolver(implementation_->database);
    if (!resolver) {
        return std::unexpected(std::move(resolver.error()));
    }
    std::vector<std::optional<std::pair<std::string, core::LocalSourceRevision>>> moved;
    moved.reserve(sources.size());
    for (const auto& [path, revision] : sources) {
        auto now_at = path;
        std::optional<core::LocalSourceRevision> now_revision = revision;
        if (auto resolved = resolver->resolve(now_at, now_revision); !resolved) {
            return std::unexpected(std::move(resolved.error()));
        }
        if (now_at != path && now_revision) {
            moved.emplace_back(std::pair{std::move(now_at), *now_revision});
        } else {
            moved.emplace_back(std::nullopt);
        }
    }
    return moved;
}

core::Result<EngineListSummary> ListRepository::save_engine_list(
    const core::StableId& id, const std::string_view name, const EngineListKind kind,
    const std::vector<EngineListItem>& items, const std::optional<std::uint64_t> expected_revision,
    const std::int64_t now_ms) {
    if (auto valid = validate_engine_list(name, items); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    if (id.is_nil()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "A list needs an identity",
                                           .context = {}});
    }
    auto* database = implementation_->database;
    return in_transaction(database, [&]() -> core::Result<EngineListSummary> {
        auto stored = engine_list_revision(database, id);
        if (!stored) {
            return std::unexpected(std::move(stored.error()));
        }
        if (auto checked = check_revision(*stored, expected_revision); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        if (*stored) {
            auto update = prepare(database, "UPDATE engine_lists SET name=?2, kind=?4, "
                                            "revision=revision+1, modified_ms=?3 WHERE id=?1");
            if (!update || !bind_text(update->get(), 1, id.to_string()) ||
                !bind_blob(update->get(), 2, name) ||
                sqlite3_bind_int64(update->get(), 3, now_ms) != SQLITE_OK ||
                sqlite3_bind_int(update->get(), 4, static_cast<int>(kind)) != SQLITE_OK) {
                return std::unexpected(database_error(database, "Could not update a list"));
            }
            if (auto done = step_done(database, update->get(), "Could not update a list"); !done) {
                return std::unexpected(std::move(done.error()));
            }
            auto clear = prepare(database, "DELETE FROM engine_list_items WHERE list_id=?1");
            if (!clear || !bind_text(clear->get(), 1, id.to_string())) {
                return std::unexpected(database_error(database, "Could not update a list"));
            }
            if (auto done = step_done(database, clear->get(), "Could not update a list"); !done) {
                return std::unexpected(std::move(done.error()));
            }
        } else {
            auto existing = select_engine_list_summaries(database, std::nullopt);
            if (!existing) {
                return std::unexpected(std::move(existing.error()));
            }
            if (existing->size() >= maximum_engine_lists) {
                return std::unexpected(core::Error{.code = core::ErrorCode::limit_exceeded,
                                                   .message = "At most 4096 lists can be kept",
                                                   .context = {}});
            }
            auto insert = prepare(database, "INSERT INTO engine_lists(id, name, kind, revision, "
                                            "created_ms, modified_ms) VALUES(?1,?2,?4,1,?3,?3)");
            if (!insert || !bind_text(insert->get(), 1, id.to_string()) ||
                !bind_blob(insert->get(), 2, name) ||
                sqlite3_bind_int64(insert->get(), 3, now_ms) != SQLITE_OK ||
                sqlite3_bind_int(insert->get(), 4, static_cast<int>(kind)) != SQLITE_OK) {
                return std::unexpected(database_error(database, "Could not create a list"));
            }
            if (auto done = step_done(database, insert->get(), "Could not create a list"); !done) {
                return std::unexpected(std::move(done.error()));
            }
        }
        if (auto stored_items = insert_engine_list_items(database, id, items); !stored_items) {
            return std::unexpected(std::move(stored_items.error()));
        }
        auto summary = select_engine_list_summaries(database, id);
        if (!summary || summary->empty()) {
            return std::unexpected(database_error(database, "Could not read a saved list"));
        }
        return std::move(summary->front());
    });
}

core::Result<EngineList>
ListRepository::edit_engine_list(const core::StableId& id, const std::uint64_t expected_revision,
                                 const std::vector<ListEdit>& edits, const std::int64_t now_ms,
                                 const std::optional<std::string_view> name,
                                 const std::optional<EngineListKind> kind, const bool with_items) {
    auto* database = implementation_->database;
    return in_transaction(database, [&]() -> core::Result<EngineList> {
        auto stored = engine_list_revision(database, id);
        if (!stored) {
            return std::unexpected(std::move(stored.error()));
        }
        if (!*stored) {
            return std::unexpected(core::Error{.code = core::ErrorCode::not_found,
                                               .message = "There is no such list",
                                               .context = {}});
        }
        if (auto checked = check_revision(*stored, expected_revision); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        auto summaries = select_engine_list_summaries(database, id);
        if (!summaries || summaries->empty()) {
            return std::unexpected(database_error(database, "Could not read a list"));
        }
        const auto list = id.to_string();
        const auto list_name = name.value_or(summaries->front().name);
        // Appends -- inserts, the first after the last entry, each after the
        // one before ended -- are what making a long list in batches is, and
        // they cost what they add: nothing is read back or written again.
        auto last = prepare(database, "SELECT entry_id, position FROM engine_list_items "
                                      "WHERE list_id=?1 ORDER BY position DESC LIMIT 1");
        if (!last || !bind_text(last->get(), 1, list)) {
            return std::unexpected(database_error(database, "Could not read a list"));
        }
        std::optional<core::StableId> tail;
        std::int64_t tail_position = -1;
        if (sqlite3_step(last->get()) == SQLITE_ROW) {
            if (const auto parsed = core::StableId::parse(column_text(last->get(), 0))) {
                tail = *parsed;
            }
            tail_position = sqlite3_column_int64(last->get(), 1);
        }
        bool appends = !edits.empty();
        std::vector<EngineListItem> appended;
        auto anchor = tail;
        for (const auto& edit : edits) {
            if (edit.kind != ListEdit::Kind::insert || edit.after != anchor) {
                appends = false;
                break;
            }
            appended.insert(appended.end(), edit.items.begin(), edit.items.end());
            if (!edit.items.empty()) {
                anchor = edit.items.back().entry_id;
            }
        }
        std::vector<EngineListItem> items;
        if (appends) {
            if (auto valid = validate_engine_list(list_name, appended); !valid) {
                return std::unexpected(std::move(valid.error()));
            }
            if (summaries->front().tracks + appended.size() > maximum_items_per_document) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::invalid_argument,
                                .message = "A list holds at most 1000000 entries",
                                .context = {}});
            }
            auto held = prepare(database, "SELECT 1 FROM engine_list_items "
                                          "WHERE list_id=?1 AND entry_id=?2");
            if (!held) {
                return std::unexpected(std::move(held.error()));
            }
            for (const auto& item : appended) {
                if (!bind_text(held->get(), 1, list) ||
                    !bind_text(held->get(), 2, item.entry_id.to_string())) {
                    return std::unexpected(database_error(database, "Could not edit a list"));
                }
                const bool taken = sqlite3_step(held->get()) == SQLITE_ROW;
                sqlite3_reset(held->get());
                sqlite3_clear_bindings(held->get());
                if (taken) {
                    return std::unexpected(core::Error{
                        .code = core::ErrorCode::invalid_argument,
                        .message = "an inserted entry needs an identity the list does not hold",
                        .context = {}});
                }
            }
            if (auto stored_items =
                    insert_engine_list_items(database, id, appended, tail_position + 1);
                !stored_items) {
                return std::unexpected(std::move(stored_items.error()));
            }
            if (with_items) {
                auto read = select_engine_list_items(database, id);
                if (!read) {
                    return std::unexpected(std::move(read.error()));
                }
                items = std::move(*read);
            }
        } else {
            std::vector<std::int64_t> positions;
            auto read = select_engine_list_items(database, id, &positions);
            if (!read) {
                return std::unexpected(std::move(read.error()));
            }
            const auto before = *read;
            items = std::move(*read);
            if (auto applied = apply_list_edits(items, edits); !applied) {
                return std::unexpected(std::move(applied.error()));
            }
            if (auto valid = validate_engine_list(list_name, items); !valid) {
                return std::unexpected(std::move(valid.error()));
            }
            const bool only_removals = std::ranges::all_of(
                edits, [](const ListEdit& edit) { return edit.kind == ListEdit::Kind::remove; });
            if (only_removals) {
                // Order is by position, and a gap is no matter: what is left
                // stays where it is, so taking one entry from a long list --
                // the consume mode does it at every track -- writes one row.
                auto remove = prepare(
                    database, "DELETE FROM engine_list_items WHERE list_id=?1 AND entry_id=?2");
                if (!remove) {
                    return std::unexpected(std::move(remove.error()));
                }
                for (const auto& edit : edits) {
                    for (const auto& entry : edit.entries) {
                        if (!bind_text(remove->get(), 1, list) ||
                            !bind_text(remove->get(), 2, entry.to_string())) {
                            return std::unexpected(
                                database_error(database, "Could not edit a list"));
                        }
                        if (auto done = step_done(database, remove->get(), "Could not edit a list");
                            !done) {
                            return std::unexpected(std::move(done.error()));
                        }
                    }
                }
            } else {
                // What leads the list unchanged stays where it is stored;
                // from the first difference on, it is written again.
                std::size_t same = 0;
                while (same < before.size() && same < items.size() && before[same] == items[same]) {
                    ++same;
                }
                if (same < before.size()) {
                    auto clear = prepare(database, "DELETE FROM engine_list_items "
                                                   "WHERE list_id=?1 AND position>=?2");
                    if (!clear || !bind_text(clear->get(), 1, list) ||
                        sqlite3_bind_int64(clear->get(), 2, positions[same]) != SQLITE_OK) {
                        return std::unexpected(database_error(database, "Could not edit a list"));
                    }
                    if (auto done = step_done(database, clear->get(), "Could not edit a list");
                        !done) {
                        return std::unexpected(std::move(done.error()));
                    }
                }
                const auto from = same > 0 ? positions[same - 1] + 1 : 0;
                if (auto stored_items = insert_engine_list_items(
                        database, id, std::span{items}.subspan(same), from);
                    !stored_items) {
                    return std::unexpected(std::move(stored_items.error()));
                }
            }
            if (!with_items) {
                items.clear();
            }
        }
        auto bump = prepare(database, "UPDATE engine_lists SET revision=revision+1, "
                                      "modified_ms=?2, name=coalesce(?3, name), "
                                      "kind=coalesce(?4, kind) WHERE id=?1");
        if (!bump || !bind_text(bump->get(), 1, list) ||
            sqlite3_bind_int64(bump->get(), 2, now_ms) != SQLITE_OK ||
            !(name ? bind_blob(bump->get(), 3, *name)
                   : sqlite3_bind_null(bump->get(), 3) == SQLITE_OK) ||
            (kind ? sqlite3_bind_int(bump->get(), 4, static_cast<int>(*kind))
                  : sqlite3_bind_null(bump->get(), 4)) != SQLITE_OK) {
            return std::unexpected(database_error(database, "Could not edit a list"));
        }
        if (auto done = step_done(database, bump->get(), "Could not edit a list"); !done) {
            return std::unexpected(std::move(done.error()));
        }
        auto summary = select_engine_list_summaries(database, id);
        if (!summary || summary->empty()) {
            return std::unexpected(database_error(database, "Could not read an edited list"));
        }
        return EngineList{.summary = std::move(summary->front()), .items = std::move(items)};
    });
}

core::Result<EngineListSummary>
ListRepository::rename_engine_list(const core::StableId& id, const std::string_view name,
                                   const std::optional<std::uint64_t> expected_revision,
                                   const std::int64_t now_ms) {
    if (auto valid = validate_engine_list(name, {}); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    auto* database = implementation_->database;
    return in_transaction(database, [&]() -> core::Result<EngineListSummary> {
        auto stored = engine_list_revision(database, id);
        if (!stored) {
            return std::unexpected(std::move(stored.error()));
        }
        if (!*stored) {
            return std::unexpected(core::Error{.code = core::ErrorCode::not_found,
                                               .message = "There is no such list",
                                               .context = {}});
        }
        if (auto checked = check_revision(*stored, expected_revision); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        auto update = prepare(database, "UPDATE engine_lists SET name=?2, revision=revision+1, "
                                        "modified_ms=?3 WHERE id=?1");
        if (!update || !bind_text(update->get(), 1, id.to_string()) ||
            !bind_blob(update->get(), 2, name) ||
            sqlite3_bind_int64(update->get(), 3, now_ms) != SQLITE_OK) {
            return std::unexpected(database_error(database, "Could not rename a list"));
        }
        if (auto done = step_done(database, update->get(), "Could not rename a list"); !done) {
            return std::unexpected(std::move(done.error()));
        }
        auto summary = select_engine_list_summaries(database, id);
        if (!summary || summary->empty()) {
            return std::unexpected(database_error(database, "Could not read a renamed list"));
        }
        return std::move(summary->front());
    });
}

core::Result<bool>
ListRepository::delete_engine_list(const core::StableId& id,
                                   const std::optional<std::uint64_t> expected_revision) {
    auto* database = implementation_->database;
    return in_transaction(database, [&]() -> core::Result<bool> {
        auto stored = engine_list_revision(database, id);
        if (!stored) {
            return std::unexpected(std::move(stored.error()));
        }
        if (!*stored) {
            return false;
        }
        if (auto checked = check_revision(*stored, expected_revision); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        auto remove = prepare(database, "DELETE FROM engine_lists WHERE id=?1 OR draft_of=?1");
        if (!remove || !bind_text(remove->get(), 1, id.to_string())) {
            return std::unexpected(database_error(database, "Could not delete a list"));
        }
        if (auto done = step_done(database, remove->get(), "Could not delete a list"); !done) {
            return std::unexpected(std::move(done.error()));
        }
        return true;
    });
}

namespace {

// One statement binding the given texts to ?1, ?2..., run to its end.
[[nodiscard]] core::Result<void> run_with_texts(sqlite3* database, const char* sql,
                                                std::initializer_list<std::string> texts,
                                                const char* failure) {
    auto statement = prepare(database, sql);
    if (!statement) {
        return std::unexpected(database_error(database, failure));
    }
    // As many as the statement names: ?1 and ?2 are not always both used.
    const auto parameters = sqlite3_bind_parameter_count(statement->get());
    int index = 1;
    for (const auto& text : texts) {
        if (index > parameters) {
            break;
        }
        if (!bind_text(statement->get(), index++, text)) {
            return std::unexpected(database_error(database, failure));
        }
    }
    return step_done(database, statement->get(), failure);
}

[[nodiscard]] core::Error list_refused(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

} // namespace

core::Result<std::pair<std::uint64_t, std::vector<std::optional<std::string>>>>
ListRepository::engine_list_entry_paths(const core::StableId& id,
                                        const std::vector<core::StableId>& entries) const {
    auto* database = implementation_->database;
    auto revision = engine_list_revision(database, id);
    if (!revision) {
        return std::unexpected(std::move(revision.error()));
    }
    if (!*revision) {
        return std::unexpected(list_refused(core::ErrorCode::not_found, "There is no such list"));
    }
    auto statement = prepare(database, "SELECT raw_path FROM engine_list_items "
                                       "WHERE list_id=?1 AND entry_id=?2");
    if (!statement || !bind_text(statement->get(), 1, id.to_string())) {
        return std::unexpected(database_error(database, "Could not read a list"));
    }
    std::vector<std::optional<std::string>> paths;
    paths.reserve(entries.size());
    for (const auto& entry : entries) {
        sqlite3_reset(statement->get());
        if (!bind_text(statement->get(), 2, entry.to_string())) {
            return std::unexpected(database_error(database, "Could not read a list"));
        }
        const auto step = sqlite3_step(statement->get());
        if (step == SQLITE_ROW) {
            paths.emplace_back(column_blob(statement->get(), 0));
        } else if (step == SQLITE_DONE) {
            paths.emplace_back();
        } else {
            return std::unexpected(database_error(database, "Could not read a list"));
        }
    }
    return std::pair{**revision, std::move(paths)};
}

core::Result<EngineListSummary> ListRepository::draft_engine_list(const core::StableId& of,
                                                                  const core::StableId& id,
                                                                  const std::int64_t now_ms) {
    auto* database = implementation_->database;
    return in_transaction(database, [&]() -> core::Result<EngineListSummary> {
        auto all = select_engine_list_summaries(database, std::nullopt);
        if (!all) {
            return std::unexpected(std::move(all.error()));
        }
        const auto saved = std::ranges::find(*all, of, &EngineListSummary::id);
        if (saved == all->end()) {
            return std::unexpected(
                list_refused(core::ErrorCode::not_found, "There is no such list"));
        }
        if (saved->kind != EngineListKind::saved || saved->draft_of) {
            return std::unexpected(
                list_refused(core::ErrorCode::invalid_argument, "Only a saved list has a draft"));
        }
        if (const auto existing =
                std::ranges::find(*all, std::optional{of}, &EngineListSummary::draft_of);
            existing != all->end()) {
            return *existing;
        }
        if (all->size() >= maximum_engine_lists) {
            return std::unexpected(
                list_refused(core::ErrorCode::limit_exceeded, "At most 4096 lists can be kept"));
        }
        auto insert =
            prepare(database, "INSERT INTO engine_lists(id, name, kind, revision, created_ms, "
                              "modified_ms, draft_of, draft_base) "
                              "SELECT ?1, name, 0, 1, ?3, ?3, id, revision FROM engine_lists "
                              "WHERE id=?2");
        if (!insert || !bind_text(insert->get(), 1, id.to_string()) ||
            !bind_text(insert->get(), 2, of.to_string()) ||
            sqlite3_bind_int64(insert->get(), 3, now_ms) != SQLITE_OK) {
            return std::unexpected(database_error(database, "Could not begin a draft"));
        }
        if (auto done = step_done(database, insert->get(), "Could not begin a draft"); !done) {
            return std::unexpected(std::move(done.error()));
        }
        if (auto copied = run_with_texts(
                database,
                "INSERT INTO engine_list_items SELECT ?1, position, entry_id, raw_path, "
                "logical_reference, segment_start_sample, segment_end_sample, "
                "audio_stream_index, subsong_index, duration_ms, title, artist, album, "
                "album_artist, date, track_gain_db, track_peak, album_gain_db, album_peak "
                "FROM engine_list_items WHERE list_id=?2",
                {id.to_string(), of.to_string()}, "Could not begin a draft");
            !copied) {
            return std::unexpected(std::move(copied.error()));
        }
        auto summary = select_engine_list_summaries(database, id);
        if (!summary || summary->empty()) {
            return std::unexpected(database_error(database, "Could not read a draft"));
        }
        return std::move(summary->front());
    });
}

core::Result<EngineListSummary>
ListRepository::commit_engine_list_draft(const core::StableId& id, const bool force,
                                         const std::int64_t now_ms) {
    auto* database = implementation_->database;
    return in_transaction(database, [&]() -> core::Result<EngineListSummary> {
        auto drafts = select_engine_list_summaries(database, id);
        if (!drafts) {
            return std::unexpected(std::move(drafts.error()));
        }
        if (drafts->empty()) {
            return std::unexpected(
                list_refused(core::ErrorCode::not_found, "There is no such list"));
        }
        const auto draft = drafts->front();
        if (!draft.draft_of) {
            return std::unexpected(
                list_refused(core::ErrorCode::invalid_argument, "The list is no draft"));
        }
        const auto saved = draft.draft_of->to_string();
        auto stored = engine_list_revision(database, *draft.draft_of);
        if (!stored) {
            return std::unexpected(std::move(stored.error()));
        }
        if (!*stored) {
            return std::unexpected(
                list_refused(core::ErrorCode::not_found, "The list this drafts was deleted"));
        }
        if (!force && **stored != draft.draft_base) {
            return std::unexpected(list_refused(core::ErrorCode::conflict,
                                                "The list was changed since its draft began"));
        }
        const auto draft_id = id.to_string();
        for (const auto* sql :
             {"DELETE FROM engine_list_items WHERE list_id=?1",
              "INSERT INTO engine_list_items SELECT ?1, position, entry_id, raw_path, "
              "logical_reference, segment_start_sample, segment_end_sample, "
              "audio_stream_index, subsong_index, duration_ms, title, artist, album, "
              "album_artist, date, track_gain_db, track_peak, album_gain_db, album_peak "
              "FROM engine_list_items WHERE list_id=?2",
              "UPDATE engine_lists SET name=(SELECT name FROM engine_lists WHERE id=?2), "
              "revision=revision+1 WHERE id=?1",
              "DELETE FROM engine_lists WHERE id=?2"}) {
            if (auto done =
                    run_with_texts(database, sql, {saved, draft_id}, "Could not save a draft");
                !done) {
                return std::unexpected(std::move(done.error()));
            }
        }
        auto touch = prepare(database, "UPDATE engine_lists SET modified_ms=?2 WHERE id=?1");
        if (!touch || !bind_text(touch->get(), 1, saved) ||
            sqlite3_bind_int64(touch->get(), 2, now_ms) != SQLITE_OK) {
            return std::unexpected(database_error(database, "Could not save a draft"));
        }
        if (auto done = step_done(database, touch->get(), "Could not save a draft"); !done) {
            return std::unexpected(std::move(done.error()));
        }
        auto summary = select_engine_list_summaries(database, *draft.draft_of);
        if (!summary || summary->empty()) {
            return std::unexpected(database_error(database, "Could not read a saved list"));
        }
        return std::move(summary->front());
    });
}

core::Result<std::vector<EngineListSummary>> ListRepository::relocate_engine_list_paths(
    const std::vector<std::pair<std::string, std::string>>& moves, const std::int64_t now_ms) {
    if (moves.size() > maximum_source_relocations) {
        return std::unexpected(core::Error{.code = core::ErrorCode::limit_exceeded,
                                           .message = "Too many moves at once",
                                           .context = {}});
    }
    auto* database = implementation_->database;
    return in_transaction(database, [&]() -> core::Result<std::vector<EngineListSummary>> {
        auto touched = prepare(database, "SELECT DISTINCT list_id FROM engine_list_items "
                                         "WHERE raw_path=?1");
        auto update = prepare(database, "UPDATE engine_list_items SET raw_path=?2 "
                                        "WHERE raw_path=?1");
        if (!touched || !update) {
            return std::unexpected(database_error(database, "Could not relocate list entries"));
        }
        std::unordered_set<std::string> changed;
        for (const auto& [from, to] : moves) {
            if (from.empty() || to.empty() || from == to) {
                continue;
            }
            if (!bind_blob(touched->get(), 1, from)) {
                return std::unexpected(database_error(database, "Could not relocate list entries"));
            }
            int step = SQLITE_ROW;
            while ((step = sqlite3_step(touched->get())) == SQLITE_ROW) {
                changed.insert(column_text(touched->get(), 0));
            }
            sqlite3_reset(touched->get());
            sqlite3_clear_bindings(touched->get());
            if (step != SQLITE_DONE) {
                return std::unexpected(database_error(database, "Could not relocate list entries"));
            }
            if (!bind_blob(update->get(), 1, from) || !bind_blob(update->get(), 2, to)) {
                return std::unexpected(database_error(database, "Could not relocate list entries"));
            }
            if (auto done = step_done(database, update->get(), "Could not relocate list entries");
                !done) {
                return std::unexpected(std::move(done.error()));
            }
        }
        std::vector<EngineListSummary> summaries;
        auto bump = prepare(database, "UPDATE engine_lists SET revision=revision+1, "
                                      "modified_ms=?2 WHERE id=?1");
        if (!bump) {
            return std::unexpected(database_error(database, "Could not relocate list entries"));
        }
        for (const auto& id : changed) {
            if (!bind_text(bump->get(), 1, id) ||
                sqlite3_bind_int64(bump->get(), 2, now_ms) != SQLITE_OK) {
                return std::unexpected(database_error(database, "Could not relocate list entries"));
            }
            if (auto done = step_done(database, bump->get(), "Could not relocate list entries");
                !done) {
                return std::unexpected(std::move(done.error()));
            }
            auto parsed = core::StableId::parse(id);
            if (!parsed) {
                continue;
            }
            auto summary = select_engine_list_summaries(database, *parsed);
            if (!summary) {
                return std::unexpected(std::move(summary.error()));
            }
            for (auto& one : *summary) {
                summaries.push_back(std::move(one));
            }
        }
        return summaries;
    });
}

} // namespace trackknife::persistence
