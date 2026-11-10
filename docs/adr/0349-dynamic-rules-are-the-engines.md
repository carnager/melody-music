# ADR-0349: Dynamic playlist rules are the engine's

## Status

Accepted, 2026-11-10. Amends ADR-0195 (where definitions are kept),
ADR-0253 (the engine kept a copy of a continued list's rule) and ADR-0258
(the shipped rules came from the window).

## Context

Home Assistant controls a Melody engine well, but cannot start a dynamic
playlist: the rules -- "Random album", "Rated 8 and higher", the user's own
-- were kept in the window's settings on the desktop. Only the window could
list them, and the engine knew a rule only while a list continued with it,
as a copy the window kept in step.

ADR-0339 keeps the user's presets in Melody and out of engines. A naming
layout is a tool for file work, which Melody does. A dynamic rule is not:
it is a list described by a rule instead of by its tracks. It is run against
an engine's library and played by that engine. Lists are the engine's
(ADR-0233, ADR-0259), and so is playback (ADR-0220).

## Decision

- **Each engine keeps its dynamic playlist rules**, in its key/value store
  (`dynamic-rules.v1`), as it keeps its lists. Every client reads and edits
  them through the protocol; the window keeps none of its own.
- **The shipped rules are the engine's**, under the same fixed identities
  (`shipped:…`). They are listed first, marked shipped, and cannot be
  changed or removed; saving one is saving a copy, as before.
- **Protocol** (level 29):
  - `dynamic.rules` → `{rules: [rule…]}`, the shipped ones first.
  - `dynamic.save {rule}` → `{rule}`: a rule without an id is new and the
    engine names it; with one, it replaces the rule of that id.
  - `dynamic.delete {id}`.
  - `dynamic.adopt {rules: [rule…]}` → `{adopted}`: each rule whose id the
    engine does not know yet is kept; one it knows is left as it is.
  - `dynamic.play {id}` → the queue replaced with the rule's selection, from
    this engine's library, playing from its first track. A Last.fm rule is
    refused: the engine does not fetch from Last.fm (yet).
  - The `dynamic.rules` event, with every rule, whenever one changes.
  - A rule is `{id, name, shipped, source, query, artist, track, user, tag,
    limit, shuffle, group_by, groups, per_group}`; `source` is `rules` or
    one of the Last.fm sources (`similar`, `loved`, `top`, `tag`).
- **A list continues with a rule by its id.** `list.continuation.set
  {list, rule: id}`. The engine looks the rule up as the list ends, so an
  edit applies at once and removing a rule ends every continuation with it.
  `list.continuations` still names each rule's name and query, for a tab's
  tooltip. The copies kept before (`list-continuation.v1`) are taken over
  once: a copied rule the engine does not know becomes one of its rules.
- **The window's rules move to its engines once.** As the window first
  reaches each engine, it hands over the rules from its settings with
  `dynamic.adopt`, so every engine the user has starts with the same set.
  The settings are left as they are, for an engine reached later.

## Consequences

- Home Assistant, the phone and melody-cli can list and play the same rules
  the window shows.
- Rules belong to one engine, as lists do: one made on the home server is
  not on the laptop's engine. Both start from the same set; copying a rule
  between engines is a later step.
- The window no longer keeps the engine's copies in step, and an engine
  that was away while a rule changed has nothing stale.
- A rule's Last.fm "previous picks" stay in the window, which still fetches
  Last.fm sources.

## Verification

- `dynamic-rules` (engine): shipped rules listed, unchangeable; save, edit,
  delete, adopt, kept across a restart; invalid rules refused; `dynamic.play`
  fills and plays the queue and refuses Last.fm rules.
- `list-continuation`: continuing by id, following an edit, ending when the
  rule goes; the v1 copies taken over.
- `engine-playback`: the tab menu offers the engine's rules and sends ids;
  removing the rule in the editor ends the continuation.
- `bench-main-window`: the editor lists, saves and removes the engine's
  rules; the window's old rules reach the engine once.
- Home Assistant: the rules browsed as playlists and played.
