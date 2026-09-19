# Melody protocol extensions

Melody speaks the MPD protocol (greeting `OK MPD 0.24.0`) and stays usable
from any stock MPD client. On top of that it provides its own extensions.
This document is the normative reference for them, written for client
authors; the Go sources in `melodyd/` are the implementation, not the spec.

## Compatibility rules

These rules keep every extension invisible to clients that do not ask for it:

1. **One advertised command per feature.** Every extension is a distinct
   command listed by `commands`. Clients gate features on the advertised
   name, never on version numbers or heuristics. Unknown commands return an
   ordinary `ACK`.
2. **Stock commands keep stock semantics.** Extensions never change what an
   existing MPD command means. Where Melody adds data to a standard
   response, it uses `X-`-prefixed lines that spec-abiding clients ignore.
3. **Filter extensions degrade cleanly.** Extra filter condition tags are
   only meaningful to Melody; clients should only emit them after seeing the
   corresponding command advertised (for ratings: `getrating`).

## Discovery

- `commands` enumerates every supported command, including all extensions.
  It is the capability probe.
- `melody_version` → `version: <semver>` identifies the server. Its presence
  in `commands` also tells clients that `add`/`addid`/`findadd`/`searchadd`
  preserve playback state (releases before it auto-played when populating an
  empty queue; see `todo.md` for the remaining fix in that area).
- `tagtypes` lists the standard tags. The `X-` listing lines below are
  deliberately not tag types.

## Extension lines in song listings

Every song listing (`playlistinfo`, `currentsong`, `find`, `search`,
`lsinfo`, playlist contents) may carry these lines per song:

| Line | Meaning |
| --- | --- |
| `X-SongId: <id>` | Melody's stable database track id. This — not the queue `Id` — is the argument to `rate`. |
| `X-AlbumId: <id>` | Melody's database album id. Also emitted per album by grouped `list Album` responses. |
| `X-Rating: <1-10>` | The song's stored track rating. Omitted when unrated. |

Clients that project unknown pairs into generic metadata will see these as
odd tag names; per the MPD spec they must simply be ignored when
unrecognized.

## Ratings

Ratings are integers 0–10 (five stars in half-star steps); 0 means unrated
and deletes the stored value. Advertised commands: `rate`, `albumrate`,
`getrating`, `getalbumrating`.

```text
rate {songid} {0-10}                          -> OK
getrating {songid}                            -> rating: {0-10}
albumrate {albumartist} {album} {date} {0-10} -> OK
getalbumrating {albumartist} {album} {date}   -> rating: {0-10}
                                                 computed: {float}
```

- `{songid}` is the `X-SongId` value, never the queue id.
- Album identity is the literal AlbumArtist/Album/Date strings from the
  listing; an empty date is part of the identity and must be sent verbatim.
- `getalbumrating`'s `computed` is the mean of the album's track ratings,
  reported (`%.1f`) only once at least 70% of its tracks are rated, else
  `0.0`. It is derived at read time and never stored.
- Track and album ratings are independent stores: rating an album does not
  touch its tracks, and vice versa.
- Successful `rate`/`albumrate` notify the idle subsystem `rating`. This is
  a non-standard subsystem name: a bare `idle` receives it, but clients that
  enumerate subsystems must list `rating` explicitly, and libmpdclient's
  typed idle API cannot represent it at all — such clients should refresh
  ratings when listings reload instead.

### Storage identity (interoperability contract)

Ratings survive path changes and database rebuilds because they are keyed by
content-identity hashes, stored in the `ratings` table as lowercase hex
sha256 with a `type` of `track` or `album`:

```text
track: sha256(albumartist 0x00 album 0x00 title 0x00 tracknumber)
album: sha256(albumartist 0x00 album 0x00 date)
```

Inputs use the scanner's normalization: albumartist falls back to artist and
then `Unknown Artist`; album falls back to the parent directory name; title
falls back to the file stem; tracknumber is the bare integer (0 when
missing); date is the four-digit year or `0000`. These formulas are a
compatibility contract — Trackbench stores its local ratings under the same
hashes so the two stores can be synchronized by key.

## Filter grammar

`find`, `search`, `findadd`, `searchadd`, and `searchalbums` accept MPD
0.21+ filter expressions in full, plus one Melody extension. The grammar,
advertised by the `filtergrammar` command (`filtergrammar` →
`grammar: 2`):

```text
EXPR  := '(' INNER ')'
INNER := '!' EXPR                       (negation, stock MPD)
       | EXPR ' AND ' EXPR [...]        (conjunction, stock MPD)
       | EXPR ' OR ' EXPR [...]         (disjunction, MELODY EXTENSION)
       | TAG OP VALUE
       | 'base' VALUE | 'added-since' VALUE | 'modified-since' VALUE
OP    := '==' | '!=' | 'contains' | '>' | '>=' | '<' | '<='
```

- Expressions nest arbitrarily. Mixing `AND` and `OR` at one nesting
  level without parentheses is an error — `((a) AND (b) OR (c))` is
  rejected, `(((a) AND (b)) OR (c))` is fine.
- `(TAG == '')` matches songs where the tag is absent and `(TAG != '')`
  matches songs where it is present, exactly like stock MPD. With a
  non-empty value, `!=` means "the tag is present and no value equals it".
- Numeric comparisons (`>` `>=` `<` `<=`) work on any tag: the leading
  integer of a value is compared, so `(date > 1995)` matches by year.
  Values without a leading integer never match.
- Values quote with single or double quotes; backslash escapes the quote
  character. Multiple expression arguments AND together, like stock MPD.
- Malformed expressions are an `ACK`, never a silently empty result.
- Stock MPD has no `OR`: a client emitting it must gate on the advertised
  `filtergrammar` command if it also talks to real MPD servers. Servers
  without `filtergrammar` are older Melody releases that accept flat
  conjunctions of positive conditions only (no `OR`, `!`, `!=`,
  empty-value forms, nesting, or numeric comparisons on ordinary tags).

Beyond the standard tags, Melody accepts:

| Condition | Operators | Meaning |
| --- | --- | --- |
| `(rating OP N)` | `==` `>` `>=` `<` `<=` | Track rating; `x-rating` is an alias. |
| `(albumrating OP N)` | `==` `>` `>=` `<` `<=` | Matches every member track of albums whose stored album rating satisfies the comparison. |

A missing operator defaults to `==`. Generic tag conditions match any tag
name stored in the library (`==` exact, `contains` substring), and the
standard `base`, `added-since`, and `modified-since` terms are supported.
Because the protocol's search responses are song lists, album-scoped queries
return the member tracks; clients group them client-side (see the roadmap
for a native album-shaped search).

`window START:END` bounds results and `sort [-]TAG` orders them. Sortable
tags: `added`, `last-modified`, `artist`, `albumartist`, `album`, `title`,
`track`, `disc`, `date`; an unsupported sort tag is an `ACK`, not a silent
fallback.

For `albumrate` and `getalbumrating`, an empty date argument is normalized
to the scanner's `0000` placeholder, because standard listings omit
`Date: 0000` and clients therefore address undated albums with an empty
string. Both spellings name the same album identity.

## Stream technicals

The scanner stores each track's stream properties — codec name, sample
rate, bits per sample (lossless only), and channel count — read from the
same container headers the duration readers parse (FLAC STREAMINFO, Ogg
Vorbis/Opus identification headers, MP3 frame headers, MP4 sample
entries). Rows scanned by older releases backfill automatically on the
next ordinary scan, unchanged files included.

Song listings report them as the standard `Format: rate:bits:channels`
line (`f` for codecs without a stored bit depth) plus `X-Codec: {name}`,
which `Format` cannot carry.

`find`/`search`/`findadd`/`searchadd` accept the matching filter
conditions, named for tkq's pseudo-fields so clients can translate
structured queries mechanically:

| Condition | Operators | Meaning |
| --- | --- | --- |
| `(samplerate OP N)` | `==` `>` `>=` `<` `<=` | Sample rate in Hz. |
| `(bitspersample OP N)` | numeric | Stored bit depth; lossy codecs never match. |
| `(channels OP N)` | numeric | Channel count. |
| `(length OP N)` | numeric | Duration in whole seconds. |
| `(codec == "flac")` | `==`, `contains` | Codec name, case-insensitive. |

ReplayGain values live in the same dedicated columns and answer the same
way, so filters can find scanned and unscanned files:

| Condition | Operators | Meaning |
| --- | --- | --- |
| `(replaygain_track_gain OP N)` | `==` `!=` `>` `>=` `<` `<=` | Track gain in dB (decimal). |
| `(replaygain_album_gain OP N)` | decimal | Album gain in dB. |
| `(replaygain_track_peak OP N)` | decimal | Track peak as a linear amplitude. |
| `(replaygain_album_peak OP N)` | decimal | Album peak. |

The separator-free spellings (`replaygainalbumgain`) address the same
values, and the empty-value forms carry their usual meaning:
`(replaygain_album_gain == '')` selects files the scanner found no album
gain in, `!= ''` selects the ones that have it.

Unprobed tracks (not yet rescanned after the upgrade) never match a
technical condition. All five also work as track-level terms in
`searchalbums`.

## Playback contexts

```text
melody_context                     -> context: {NAME}   ("" = the queue)
melody_context play {NAME} [POS]   -> OK
melody_context queue [POS]         -> OK
melody_context queueinfo           -> song list (listplaylistinfo shape)
```

A *context* is a stored playlist materialized into the one MPD queue.
`play` replaces the queue with the playlist's tracks and starts it: with
`POS` at that row from the beginning, without one at the row and offset
where that playlist was last left (0/0 the first time). Switching away
from the live queue stashes it — songs, priorities, position, and
elapsed — so `melody_context queue` restores it exactly, preserving the
current pause state because a switch back is not a play command;
`melody_context queue POS` instead starts the restored queue at that row.
`queueinfo` lists the stashed queue (or the live one when nothing is
stashed) so clients can show the queue while a playlist plays.

Stock clients are unaffected: every switch is an ordinary whole-queue
replacement with a version bump, `status`/`currentsong`/`playlistinfo`
stay consistent, and no standard response gains fields. Switches notify
`playlist` and `player` (which stock clients already observe) plus the
Melody-only `context` subsystem.

Rules worth knowing:

- Queue edits while a playlist context is active apply to the
  materialization only. They are never written back to the playlist and
  are lost on the next switch — the same contract as MPD's `load`.
- Editing the **active** playlist with `playlistadd`, `playlistdelete`,
  or `playlistmove` re-materializes it immediately, keeping the playing
  track playing where it moved to, so the queue mirrors the list you are
  editing. Other playlists never touch the queue.
- `rename` carries the active context (and its resume point) to the new
  name. `rm` and `playlistclear` release the name — the materialized
  content keeps playing and the stashed queue stays restorable.
- Resume positions are clamped when a playlist shrank. The elapsed seek
  is best effort, the same accuracy class as the periodic play-state
  snapshot.
- Context state (active name, stash, per-playlist positions) persists
  with the queue and survives daemon restarts. Files written before this
  extension load as "no contexts".

## Album-shaped search: `searchalbums`

```text
searchalbums {FILTER} [sort {[-]TAG}] [window {START:END}]
```

Because the protocol's song commands can only answer with track lists,
album-scoped queries would otherwise return member tracks for the client to
re-group. `searchalbums` evaluates the same filter expressions against
albums and answers one record per album:

```text
AlbumArtist: ...
Album: ...
Date: ...                  (always present, including 0000)
X-AlbumId: ...
X-TrackCount: ...
X-Duration: {whole seconds}
X-Rating: {1-10}           (stored album rating; omitted when unrated)
X-ComputedRating: {float}  (track-rating mean; omitted below the 70% threshold)
X-ArtworkUri: {relative track path usable with albumart/readpicture}
```

Album-level terms — `albumartist`, `album`, `date` (`==` exact,
`contains` substring, both case-insensitive), `albumrating` with the rating
operators, and `added-since` — apply to the album record directly.
Track-level terms — `rating`, `artist`, `title`, `any`, and every generic
tag — match albums containing at least one matching track. Any other term
is an `ACK` rather than a silent broadening.

`sort [-]TAG` accepts `albumartist`, `album`, `date`, `added`, and
`rating` (stored album rating; unrated sorts as 0); the default order is
the library's album order (album artist, date, title). `window` applies
after sorting. Advertised as the `searchalbums` command.

## Launcher list commands

`melody_albums`, `melody_albums_latest`, and `melody_tracks` return
pre-built line-per-entry lists for launcher clients (rofi/dmenu). They are
presentation conveniences, not part of the data model; other clients should
prefer the standard database commands.

## Playback agents

`agent_register {name} v2 instance={id}` hands the connection over to the
agent protocol that lets a client machine act as a Melody output (streamed
or mapped-file playback, gapless preload, clock reporting). It is documented
in [Client configuration](clients.md); protocol level `v2` is the current
contract.

## Known limits

- `X-Rating` is emitted in listings but intentionally absent from
  `tagtypes`; `rating`/`albumrating` work only as filter conditions, not as
  `list` tags.
- The `rating` idle subsystem is invisible to libmpdclient-based clients
  (see above).
- The legacy `tracks.rating` text column is unused; the `ratings` table is
  authoritative.
