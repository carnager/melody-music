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
