# ADR-0351: The queue knows which dynamic playlist it is

## Status

Accepted, 2026-11-11. Follows ADR-0349 (the engine plays a rule) and sits
beside ADR-0253 (the engine knows which list a queue was played from).
Asked for in GitHub issue #11.

## Context

Since ADR-0349 a client asks the engine to play a dynamic playlist, and the
engine picks the tracks and replaces its queue. Then it forgets which rule
they came from. A Home Assistant button for a room ("play Random album in
the kitchen") needs to know: pressed while paused, it should resume when the
queue is still that playlist's, and start the playlist again when another
client -- the window, melody-cli, the phone -- has put something else there
since. A helper in Home Assistant cannot keep track, because the other
clients do not tell it, and the track playing does not say where the queue
came from. The engine is the only one that knows, and playback is the
engine's (ADR-0220).

## Decision

- **The engine keeps the rule with the queue.** `dynamic.play` installs the
  rule's selection together with the rule's id, in one step under the
  player's lock. A play that fails -- the rule finds nothing, Last.fm cannot
  be reached, the job is cancelled -- leaves the queue and its rule as they
  were.
- **Playback keeps it.** Pause, resume, seek, next and previous, changing
  modes, moving to another output, Up Next and a file moved by the engine
  leave it.
- **Anything else forgets it.** The queue replaced by any client
  (`playback.replace_queue`, a list played, joining together), edited in
  place (`list.edit` on the queue: removed, moved, added tracks), or emptied
  by consume, is no rule's any more. The id says where the queue came from,
  not that every track still matches the rule, so the conservative choice is
  to forget on any edit.
- **The rule removed, forgotten too.** Renaming or editing a rule keeps it:
  the id is what a client matches, never the name.
- **Kept across a restart** with the queue it describes (`dynamic_rule` in
  the stored playback state); a rule removed meanwhile is not claimed.
- **Protocol** (level 31): `playback.state` and every `playback.changed`
  carry `dynamic_rule`, the rule's id or null; `playback.queue` answers it
  beside `list`. A client that connects learns it from the first state it
  reads.
- **Home Assistant** shows it as the player's `dynamic_playlist_id`
  attribute, with `dynamic_playlist` the rule's current name for display.

## Consequences

- A room button can be a short script: playing -> pause; paused and
  `dynamic_playlist_id` is the button's -> `media_play`; otherwise ->
  `play_media` the playlist. Which output plays stays the script's own
  condition.
- Only `dynamic.play` sets it. A tab of the window's dynamic results played
  as a list is that list's (ADR-0253), not the rule's.
- It is not per room: there is still one queue (ADR-0220).

## Verification

- `dynamic-rules`: set by `dynamic.play` (a tkq-1 rule and a Last.fm job),
  in `playback.state` and `playback.queue`; kept through pause, resume,
  seek, next and a rename; unchanged by a play that fails; forgotten by an
  edit, a replacement and removing the rule; restored after a restart, and
  not when the rule went meanwhile.
- `engine-player`: consume emptying the queue forgets it.
- Home Assistant: the attributes follow play, pause, a rename and another
  client's queue; against a real melodyd, null for an empty queue.
