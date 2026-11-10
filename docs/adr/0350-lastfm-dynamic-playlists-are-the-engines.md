# ADR-0350: Last.fm dynamic playlists are the engine's

## Status

Accepted, 2026-11-10. Completes ADR-0349 (whose Last.fm rules the engine
kept but could not play) and amends ADR-0145 (the window fetched Last.fm).

## Context

ADR-0349 moved the dynamic playlist rules into the engine, but a Last.fm
rule -- similar tracks, a user's loved or top tracks, a tag's top tracks --
was still run by the window: it asked Last.fm with an API key from its own
settings, looked each suggestion up in the engine's library one request at
a time, and remembered what it picked last in its settings. So Home
Assistant could not play one, and the window's editor was the only place
one worked.

The engine already talks to Last.fm: it scrobbles and loves with the
account a client hands it (`lastfm.set_session`), whose API key is all a
Last.fm read needs.

## Decision

- **The engine picks for every rule.** For a Last.fm rule it asks Last.fm
  with the API key of the account it was given, at most 500 suggestions,
  and matches them in its library -- a batch at a time, by artist and
  title as tkq-1's `IS` compares them, the first file by path where the
  library has several of one song. Without an account it says to sign in.
- **What was picked is the engine's to remember.** Those not picked the
  last time come first, as the window did it; the last pick of each
  Last.fm source (the source and its artist, track, user or tag) is kept in
  the engine's store (`dynamic-picks.v1`, the newest 64 sources).
- **Protocol** (level 30):
  - The job `dynamic.pick {rule}`: what a rule -- saved or not -- picks, as
    `{paths, matched, unmatched}`, reporting `{done, of}` while matching.
    A job because Last.fm can take seconds; it holds up nothing else.
  - `dynamic.play {id}` plays a Last.fm rule too. Its answer is a job's,
    `{job_id}`, whose outcome is `{playing, tracks}` or `{error}`; a tkq-1
    rule is answered at once, as before.
- **The window asks.** Its editor asks the engine's `dynamic.pick` for
  every rule and shows what comes back, named from that engine's library.
  It no longer fetches Last.fm, matches suggestions or keeps picks, and its
  separate "Last.fm API key" setting for dynamic playlists is gone: signing
  in to Last.fm and handing the account to an engine is what enables them.

## Consequences

- Home Assistant and any other client can play a Last.fm rule.
- Matching runs where the library is: a batch of suggestions per query,
  not a network round trip per suggestion.
- An engine without a Last.fm account cannot run Last.fm rules, where the
  window could with only an API key. The account is the one an engine
  scrobbles with, and signing in once covers every engine.
- A list still cannot continue with a Last.fm rule (ADR-0253): continuing
  happens as a track starts, and cannot wait on Last.fm. A later step.
- The window's last Last.fm picks (`dynamic-playlists/recent-v1`) are not
  carried over; the engine starts remembering from its first pick.

## Verification

- `engine-lastfm`: suggestions read with the account's key and nothing
  secret, in Last.fm's order, at most as many as asked; one track answered
  alone; Last.fm's refusal said; no account, no request.
- `dynamic-rules`: suggestions matched in the library whatever their case,
  one file per song; those not picked the last time first, across a
  restart; `dynamic.pick` for a rule being written, with progress and
  cancelling; `dynamic.play` of a Last.fm rule as a job.
- `bench-main-window`: the editor asks the engine for every rule, shows its
  progress and what it picked, and ignores an answer it no longer waits
  for; refreshed against a real engine without an account, it says to sign
  in.
- Home Assistant: Last.fm rules browsed and played.
