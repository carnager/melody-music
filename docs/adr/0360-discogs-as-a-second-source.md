# ADR-0360: Discogs as a second source for Identify

## Status

Accepted, 2026-11-20. Extends ADR-0086 (proposal boundary), ADR-0090
(Identify), ADR-0237 (the engine makes the lookups), ADR-0310 (a release
named by its link) and ADR-0316/0326 (covers).

## Context

Many releases -- new, small, regional, vinyl-only -- are on Discogs long
before anyone enters them in MusicBrainz, and some never are. Identify
found nothing for them, and the only way to tag such an album was by hand.

Discogs has a real interface: a database search and release records with
their tracklists, artists, labels, catalogue numbers, barcodes, formats
and images. Its release data is published under CC0. Search needs a
personal token, which anyone with an account can make in a minute; with
one, Discogs allows sixty requests a minute.

ADR-0326 reached Bandcamp only through MusicBrainz's links, never by
searching it, because Bandcamp has no interface and a search there would
be a guess. That is about Bandcamp, not a rule that only MusicBrainz may
be searched: a Discogs search is a search of structured records, and what
it finds is shown, chosen and previewed like MusicBrainz's.

## Decision

- **A second source, not a mode.** Identify stays one dialog. With a
  Discogs token set, a text search asks MusicBrainz and Discogs, and
  Discogs' releases are listed after MusicBrainz's -- at most ten, each
  row saying it is from Discogs. Without a token nothing changes.
- **CDs and digital releases only.** Files are tagged from those, and
  Discogs lists every pressing of a record and tape besides: a search and
  a master's versions list only releases on CD (CDr, SACD) or as files --
  a page of fifty asked for, the first ten of those kept. A release of no
  known format is kept; one named by its link or tagged is looked up
  whatever it is on.
  MusicBrainz stays first: its releases carry recording, work and artist
  ids, it can be reached by fingerprint, and Melody's other features
  (Listening together, the About panel) speak its ids.
- **A release named by its Discogs link or id** -- `discogs.com/release/N`,
  any language's page of it, or `[rN]` -- is looked up, as ADR-0310 does
  for MusicBrainz's; a master's link (`discogs.com/master/N`) lists its
  versions. Files tagged with `DISCOGS_RELEASE_ID` and no MusicBrainz
  release are looked up on Discogs as Identify opens, the release's
  master's other versions beneath it.
- **One release model.** A Discogs release becomes the same
  `musicbrainz::Release` the matcher, the pairing and the proposals
  already use, marked by its source. The matcher and the pairing view do
  not know where a release came from; only the parsing and the
  identifiers proposed do.
- **What is read from Discogs:**
  - Track positions are free text. `N-M`, `N.M` and `CDN-M` name disc N,
    track M. Lettered sides (`A1`, `B`, `C2`) are one disc for a single
    record, and a side pair (A/B, C/D, …) per disc where the release has
    as many records; the position is kept as the track's number, as
    MusicBrainz keeps vinyl's. Anything else is disc 1, in order.
    Headings are not tracks; an index track is one track.
  - Artist names lose Discogs' ` (N)` disambiguation suffix; a name as
    credited (`anv`) is used where given; `Various` is `Various Artists`.
  - A date of `YYYY-00-00` is `YYYY`; catalogue number `none` is none.
  - The format's descriptions give the release type where they say it:
    Album, EP, Single, Compilation.
- **What is proposed:** what MusicBrainz's releases propose where Discogs
  has it -- titles, artists, album, album artist, date, numbering and
  totals, label, catalogue number, barcode, country, media, release type
  -- and `DISCOGS_RELEASE_ID` and, where the release has one,
  `DISCOGS_MASTER_ID`, never a `MUSICBRAINZ_*` id. No genres or styles:
  Identify proposes none for either source; whether it should is its own
  decision.
- **Covers.** Files tagged with a Discogs release and no MusicBrainz one
  are offered that release's images, its primary image as the front --
  by Fetch cover… and by Identify's Fetch covers alike.
- **The engine keeps the token**, as it keeps the AcoustID key: set in
  Settings → Metadata services, handed to every engine that does file
  work, forgotten when emptied, readable by the engine's owner only. The
  engine adds it to the requests it makes to `api.discogs.com` and
  fetches nothing else of Discogs but its images (`i.discogs.com`), over
  HTTPS, paced at one request every 1.1 s apart from MusicBrainz's, from
  the same cache, asked again when told to slow down.

## Consequences

- One request more per search where a token is set, and one per release
  previewed; Discogs' answers are cached as MusicBrainz's are.
- Discogs' search tells no track counts: its rows say none until a
  release is previewed, and rank only by Discogs' own order.
- A track's length is often missing on Discogs; pairing then leans on
  numbers, order and titles, and says more pairs are worth a look.
- Files identified on Discogs carry no MusicBrainz ids: Listening
  together finds them by title, artist and album, as it does any file
  without one.
- An album already tagged with another release's MusicBrainz ids keeps
  them when identified on Discogs; Identify replaces values, it does not
  clear fields it has nothing for.

## Verification

- `musicbrainz-web-service`: Discogs' URLs and links told from lookalikes;
  a search and a master's versions parsed; a release's positions --
  numbered, discs named, lettered sides over one and two records,
  headings, index tracks, positions saying nothing -- its artists, date,
  label, barcode and type; its images as a cover listing.
- `musicbrainz-matching`: a Discogs release proposes its own ids and no
  MusicBrainz ones.
- `engine-file-work`: the engine fetches Discogs' interface and images and
  refuses Discogs' pages and lookalike hosts; without a token it says one
  is needed; the token is kept apart from the AcoustID key, its owner's
  only, forgotten when emptied, and handed over the protocol.
- `window-tagger`: the token is kept in Settings, masked.
- `window-musicbrainz`: Identify asks Discogs after MusicBrainz and lists
  its release after MusicBrainz's; a Discogs link is looked up there; the
  release applied stages its Discogs ids and no MusicBrainz ones.
