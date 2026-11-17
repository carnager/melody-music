# ADR-0354: One schema, no migrations

## Status

Accepted, 2026-11-17.

## Context

- **The state database was built by 57 migration steps**, each run on every
  new database in turn: ~1,300 lines of SQL inline in
  `list_repository.cpp`, a second copy as 110 up/down files under
  `src/persistence/migrations` that only tests read, and C++ backfills for
  rows older steps left incomplete (rating identities, dates added).
- **Nothing was ever released.** The only databases are the owner's, and
  both were at schema 57 when this was decided; no one has one to bring
  forward.
- Every schema change meant another step, another down file, and tests that
  replay old versions -- most of the cost of touching persistence.

## Decision

1. **The schema is one script**, `src/persistence/src/current_schema.hpp`:
   what the 57 steps made of an empty database, written out. A new database
   is created from it.
2. **A database at another version is not opened.** Newer: refused as
   before. Older: refused, to be moved aside -- there is nothing between.
3. **No backfills.** The scan writes every column it reads; a row is never
   left for a later open to complete.
4. **A schema change from now on** edits the script and bumps the version.
   Until there is a release, existing databases are brought along by hand
   (or rebuilt); after one, migrations start again from that release's
   schema, not from zero.

## Consequences

- The migrations folder, the downgrade instructions in `docs/melody.md`
  and the tests that replayed old versions are gone.
- Verified when made: the script produces a database whose dump is
  identical to the old chain's, and whose tables, columns and foreign keys
  match the owner's real database.
- Tables the window no longer uses (`list_documents` and the rest of the
  client-era lists) are still in the script; dropping them is the next
  change, and the first one made this way.

## Amendment, 2026-11-17: schema 58

The first change made this way. Schema 58 drops what the window kept before
its tabs were its own (ADR-0259, ADR-0286) -- `list_documents`,
`list_items`, `list_item_fields`, `connection_profiles`,
`track_view_presets`, the `local_metadata_*` cache -- and the lists schema 56
retired. Nothing read them but the window's one-time import into its tab
store and `adopt_presets`, both gone with them.

The move journal, `local_source_relocations`, stays and loses its
list-occurrence columns. It recorded a move only when the moved file was in
one of those old lists, so a tab could not find a file moved while its
window was closed (`catalogue.resolve_moves`); every move is recorded now.
And a tab's row whose file cannot be read on opening keeps the revision it
was seen at, so the journal can be asked; once followed, it is read where
the file is.

The owner's databases (both engines' `lists.sqlite`, caprica's
`presets.sqlite`) are brought along with this script, run with
`sqlite3 -bail` while nothing has them open; it refuses a database not at
57. Checked on copies: integrity and foreign keys hold, and the result has
the same tables, columns, foreign keys and indexes as a database made fresh
at 58.

```sql
-- Melody state database 57 -> 58 (ADR-0354: by hand, no migration chain).
-- The client-era lists, profiles, view presets, metadata cache and the
-- lists retired by 56 go; the move journal keeps only the move.
PRAGMA foreign_keys = OFF;
BEGIN IMMEDIATE;
CREATE TEMP TABLE at_schema_57 (version INTEGER CHECK(version = 57));
INSERT INTO at_schema_57 SELECT version FROM schema_version;
DROP TABLE IF EXISTS list_item_fields;
DROP TABLE IF EXISTS list_items;
DROP TABLE IF EXISTS list_documents;
DROP TABLE IF EXISTS connection_profiles;
DROP TABLE IF EXISTS track_view_presets;
DROP TABLE IF EXISTS local_metadata_cache_fields;
DROP TABLE IF EXISTS local_metadata_cache;
DROP TABLE IF EXISTS local_metadata_refreshes;
DROP TABLE IF EXISTS retired_engine_list_items;
DROP TABLE IF EXISTS retired_engine_lists;
ALTER TABLE local_source_relocations DROP COLUMN affected_occurrences;
ALTER TABLE local_source_relocations DROP COLUMN cache_rekeyed;
ALTER TABLE local_source_relocations DROP COLUMN metadata_refreshed;
UPDATE schema_version SET version = 58;
COMMIT;
```
