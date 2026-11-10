# Dynamic playlists

Choose **File → Dynamic playlists**. It works on the library engine's
rules: the engine keeps them, so every client sees the same ones -- the
phone, melody-cli and Home Assistant included -- and what a rule finds
plays on that engine. Rules belong to one engine, as lists do: a rule made
on the home server is not on the laptop's own engine. The first time Melody
reaches an engine it hands over the rules it kept itself before, so each
engine starts with the same set (ADR-0349).

Choose **Library rules**, enter a name and a `tkq-1` rule, then **Refresh**:

- `genre HAS rock AND rating GREATER 6` — rock rated above three stars.
- `rating MISSING` — unrated tracks.
- `genre HAS jazz SORT BY %album%` — jazz ordered by album.

Ratings use 0–10, so 8 means four stars. Rules use the complete
[query language](query-language.md) on either library, because the engine
evaluates them the same way.

Set a maximum of 1–500 tracks and optionally shuffle. With shuffle, the
tracks are a random sample of every match, however many there are, so `ALL`
gives random tracks from the whole library. Without it, the first ones in
library order (artist, year, album, disc, track) or the rule's `SORT`.

To pick by album or artist rather than by track, enter **Pick groups by** — a
[formatting expression](title-formatting.md); tracks giving the same text form
a group — and how many **Groups** to pick. Every group is as likely as any
other, whatever its size, so an artist with 400 tracks comes up no more often
than one with 10. **Tracks per group** takes that many at random from each,
kept in album order, or all of them. For example, `%albumartist%`, 10 groups
and 3 tracks per group gives three songs each by ten random artists.

The engine selects: only the chosen tracks reach the window.

### Rules that ship

These are always in the list and can be continued with like any other:

| Rule | Picks |
| --- | --- |
| Random tracks | 50 tracks from the whole library, shuffled |
| Random album | One album, whole, in order |
| Random artists | Ten artists, three tracks each, shuffled |
| Rated 8 and higher | 100 tracks rated four stars or more, shuffled |
| Unrated | 100 unrated tracks, shuffled |
| Recently added | Up to 100 tracks added in the last 30 days, newest first |
| Not heard in a year | 100 tracks not played for a year or never, shuffled |

They cannot be changed or removed. Edit one and choose **Save as copy** to
keep your own version. Definitions saved by Melody's earlier MPD backend
are moved into the one list on start.

**Save definition** keeps the rule/source. Saved rules refresh when selected and remain current while the
window is open. Local rules update after library scans, rating edits, and
committed tag/path changes. Changes arriving during a query trigger a fresh
evaluation. **Stop** cancels the current work and automatic refresh.
**Open snapshot in new tab** creates an ordinary playlist you can play or edit;
that snapshot stays stable as the dynamic definition changes. Results and
snapshot tabs show one line per track, without album grouping. Refresh never
replaces your playing queue.

Results support row/multi-selection, keyboard navigation, Enter/double-click
playback, ratings, Go to artist/album, local or explicitly mapped file tools,
Last.fm, Up Next, and copying/dragging into ordinary lists. Up Next shortcuts
act on the result selection, not the tab behind the editor. Outward drag is
copy-only; dropping into or manually reordering/removing dynamic results is
not a definition edit. Choose **Open editable snapshot…** for those operations.

Play captures the results as a playback list on the engine the library belongs
to. Later refreshes never rewrite it. Selection, current row, and scroll anchors survive refresh by raw source
identity and duplicate ordinal. Playing markers follow the source, including
duplicate occurrences within the captured playback context. Editing metadata
captures the selected inputs so a result refresh cannot retarget the operation.
Closed dynamic editors do not keep refreshing; persistent live autoplaylist tabs
remain future work.

## Continuing a list

Any list can carry on past its end with a saved library rule. Right-click its
tab and choose **Continue with**, then a rule. The tab shows ∞, and its
tooltip names the rule. When the last track of the list starts and nothing
would follow it, the engine adds 10 tracks the rule finds — or, for a rule
that picks groups, its whole selection: the next album, or the next ten
artists. It skips tracks
already in the list and tracks played in the last 7 days. If the rule finds
only those, it uses them anyway, still skipping what is in the list.

This happens in the engine, so it works with the window closed. Repeat never
ends a list, so a repeating list is not extended, and neither is one playing
in single mode. Added tracks are ordinary entries: remove, move or rate them
as usual. Choose **Nothing — the list ends** to stop. The tracks already added
stay.

Editing a rule updates every list that continues with it at once, and
removing the rule ends their continuation -- the engine looks the rule up
by its id as the list ends. Last.fm sources are not offered: a list
continues as a track starts, and cannot wait for Last.fm to answer. See
ADR-0253 and ADR-0258.

History rules may also order results, for example
`ALL SORT DESCENDING HISTORY(playcount)`. Turn off **Shuffle results on refresh**
to retain query ordering. See [history ordering](query-language.md#history-ordering-adr-0216).

Last.fm rules are run by the engine too (ADR-0350). It asks Last.fm with the
account it was given: sign in under **Settings → Last.fm** and use the account
on the engine (see [Last.fm accounts](lastfm.md)) -- the same step that makes
it scrobble. Choose similar tracks, a user's loved/top tracks, or a tag.
Enter the seed artist/title, username, or tag, then Refresh. The engine sends
these details to Last.fm when a rule is refreshed or played.

The response is matched to music already in that engine's library. Missing
tracks are counted, duplicate matches are removed, and a song the library has
several files of uses the first by path. Matching requires the same artist and
title apart from simple Unicode case, as tkq-1's `IS` compares them; it does
not guess alternate credits or editions. Each refresh reads up to 500
suggestions, independently of your playlist length, and randomly selects from
the library matches. Tracks outside the previous result are preferred;
previous tracks fill any remaining slots. The engine remembers the previous
selection across restarts, for each source -- the same seed, user or tag
shares it, whichever rule asks.

**Shuffle selected tracks** also randomizes the order. With it unchecked, the
fresh selection keeps Last.fm’s ranking order. If your library has too few
matches to vary the selection, the result reports that all available matches
are included. Missing matches can leave fewer tracks than requested.

## Playing a rule from elsewhere

The engine plays a rule itself: `dynamic.play` replaces its queue with what
the rule picks and starts it. From a terminal, `melody-cli dynamic` names the
rules and `melody-cli play dynamic random album` plays one. Home Assistant shows the rules under **Browse
media** as *Dynamic playlists*, and an automation can play one by name:

```yaml
action: media_player.play_media
target:
  entity_id: media_player.gemenon
data:
  media_content_type: playlist
  media_content_id: Random album
```

Last.fm rules are offered too; the engine plays one once Last.fm has
answered, which can take a few seconds.

Results come entirely from the chosen engine's library. Use Library Refresh
first if new files are not indexed. Closing the dynamic editor leaves saved
definitions and opened snapshots intact.
