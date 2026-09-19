# Protocol roadmap: parity with Trackbench's local mode

Goal: a client connected to Melody should be able to offer the same browsing
and search experience Trackbench offers over its local library — album-shaped
results, technical and rating predicates, saved searches — while every stock
MPD client keeps working untouched. The ground rules from
[protocol.md](protocol.md) apply to everything here: one advertised command
per feature, `X-` lines for extension data in standard responses, stock
semantics never change.

The phases are ordered by user-visible payoff. Each phase stands alone.

## Phase 1 — album-shaped search: `searchalbums` (done)

Implemented and specified in [protocol.md](protocol.md); the notes below are
the original design draft.

The MPD protocol can only answer searches with song lists, so
"albums rated ≥ 4 stars" currently returns a pile of member tracks that the
client must re-group. Add a native album search:

```text
searchalbums {FILTER} [sort {[-]TAG}] [window {START:END}]
```

- Accepts the same filter expressions as `search`, evaluated against albums:
  album-level terms (`albumrating`, `albumartist`, `album`, `date`,
  `added-since`) apply directly; track-level terms match albums containing
  at least one matching track.
- Response is one record per album:

  ```text
  AlbumArtist: ...
  Album: ...
  Date: ...
  X-AlbumId: ...
  X-TrackCount: ...
  X-Duration: {seconds}
  X-Rating: {1-10}          (stored album rating, omitted when unrated)
  X-ComputedRating: {float} (mean of track ratings at the 70% threshold)
  X-ArtworkUri: {path of a representative track, for albumart/readpicture}
  ```

- Sortable by `albumartist`, `album`, `date`, `added`, and `rating`.
- Implementation notes: `albumIDsByRatingOp`, `allAlbums`, and the grouped
  `list Album` plumbing already cover most of the query surface; this is
  largely response shaping.

## Phase 2 — technical filter conditions (done)

Implemented and specified in [protocol.md](protocol.md); the notes below are
the original design draft. The scanner half landed as Go-side header parsing
with a lazy backfill keyed on missing technicals.

Trackbench's local queries filter on probe facts (`samplerate`,
`bitspersample`, `channels`, `codec`, `length`). Melody's scanner currently
stores none of these (`tracks` has only duration), so this phase has two
halves:

1. **Scanner:** capture codec name, sample rate, bits per sample, and
   channel count per track (ffprobe or Go-side header parsing, whichever the
   scanner can afford), with a lazy backfill for already-indexed rows so a
   full rescan is not required.
2. **Filters:** accept the conditions with the numeric operators, aligned
   with Trackbench's tkq pseudo-field vocabulary so a client can translate
   queries mechanically:

   ```text
   (samplerate >= 96000)  (bitspersample == 24)  (channels == 2)
   (codec == "flac")      (length >= 600)
   ```

   Emit the values as `X-` lines in song listings (`X-Codec`,
   `X-SampleRate`, `X-BitsPerSample`, `X-Channels`) so clients can show them
   without probing files.

The vocabulary contract matters more than the exact storage: once the
condition names and semantics match tkq's pseudo-fields, Trackknife can
offer its structured Search dialog (including saved searches) against a
"Server library" scope by translating each pushable predicate and rejecting
untranslatable queries with a clear error, instead of Melody ever learning
tkq or tkfmt itself.

## Phase 2b — full filter grammar (done)

Implemented and specified in [protocol.md](protocol.md). The flat AND-only
filter parser was replaced with a recursive one covering stock MPD 0.21+
in full — nesting, `!` negation, `!=`, and the empty-value
present/missing forms — plus `OR` disjunction as a Melody extension and
numeric leading-integer comparisons on ordinary tags. The `filtergrammar`
command (`grammar: 2`) is the capability gate, so Trackknife's tkq
translator can emit `OR`/`NOT`/`PRESENT`/`MISSING` and numeric tag
comparisons against new servers while degrading to the flat subset (with
a clear "not supported" error for the rest) against old ones. Flat
conjunctions keep the indexed fast paths; structured trees evaluate per
track and feed the shared sort/window pipeline. Malformed expressions are
protocol errors, never silently empty results.

## Phase 3 — conformance and hygiene

Small items that make the extension surface trustworthy:

- Regression-test `window` and `sort` on `find`/`search` against real MPD
  0.24 behavior (windowing after sorting, `ACK` on unsupported sort tags),
  since paginated clients depend on them once rating/album queries can
  return large result sets.
- Fix the add-to-empty-queue playback transient (`todo.md`) so
  `melody_version`'s advertised meaning is fully true and Trackknife can
  drop its compatibility workaround.
- Decide whether `stickernames`/`stickertypes` style discovery is worth
  mirroring for ratings, or whether `getrating`'s presence stays the single
  gate (current position: single gate, keep it).
- Document every future condition tag and `X-` line in
  [protocol.md](protocol.md) in the same change that implements it.

## Phase 4 — listening statistics

The natural follow-on once ratings have proven the identity-hash model:
per-track play counts, last-played timestamps, and skip counts, stored
server-side under the same track identity hashes so they survive rescans and
path changes.

- Writes happen server-side on playback progression (define the play/skip
  thresholds once, in the server, so every client agrees).
- Expose per song as `X-PlayCount` and `X-LastPlayed` listing lines.
- Filter conditions `(playcount OP N)` and `(lastplayed-since TIMESTAMP)`,
  plus `sort playcount` / `sort lastplayed` for "most played" and "recently
  played" views.
- Idle notification via the existing `rating`-style pattern (a dedicated
  `stats` subsystem name, same caveats).
- Trackbench plans the same statistics for its local store with the same
  identity keying, which keeps a future local↔server statistics sync — like
  ratings today — a pure key join.

## Phase 5 — ordered "Up next" sub-queue (future)

A client-visible ordered sub-queue layered on the main queue: "play these
next, in this order, then resume where I was" as one server-side concept
instead of every client re-deriving it from priorities and insert
positions. Stock `prio`/`prioid` clients must keep working unchanged.

Half the substrate exists: per-position priorities (`queuePriority`,
persisted with the queue), the scheduler override in `nextQueuePos`, the
`prioReturnPos` resume cursor, and the `prioPlayedIDs` once-played
suppression. The gaps a real sub-queue must close:

- No ordering among equal priorities beyond queue index — an "Up next"
  list needs insertion order, so either monotonically decreasing
  priorities per add or a separate order vector.
- `prioPlayedIDs` keys on MPD songids, which are not stable across daemon
  restarts, so sub-queue progress does not survive a restart today.
- Batch adds apply one uniform priority to the whole batch, losing
  intra-batch order.

Design sketch, per the compatibility ground rules (one advertised command,
stock semantics untouched): either a `melody_upnext` command family with an
`X-`-line projection over `playlistinfo`, or pure priority encoding with
documented client-side reconstruction. Not scheduled; recorded so the
priority machinery is not evolved in a direction that forecloses it.

## Explicitly out of scope

- Running tkq or tkfmt server-side: two implementations of a versioned
  language drift apart; the client translates instead (Phase 2).
- Changing standard response shapes or `tagtypes`: extension data stays in
  `X-` lines and dedicated commands.
- Any feature that requires clients to special-case Melody without a
  `commands`-advertised gate.
