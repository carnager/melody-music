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

## Filter extensions in find/search

`find`, `search`, `findadd`, and `searchadd` accept MPD 0.21+ filter
expressions. Beyond the standard tags, Melody accepts:

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
