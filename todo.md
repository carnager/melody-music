# TODO

## Preserve playback state when adding to an empty queue

Melody currently starts playback when MPD `add`/`addid` populates an empty queue. These commands
must only mutate the queue; they must not act like `play`.

The behavior originates in `addSongsWithPriority()` in `melodyd/main.go`. In the default `"add"`
branch, `wasEmpty` sets `curQueuePos` to zero and calls `execSyncPlan()`. Loading that first item
into an enabled output starts it.

Fix the server so that:

- `add`, `addid`, `findadd`, and `searchadd` preserve stopped or paused playback state.
- Adding to either an empty or non-empty queue never produces an audible playback transient.
- Explicit replace-and-play and `play` commands continue to start playback intentionally.
- The behavior is consistent for agent targets and other output implementations.

Add MPD protocol regression tests covering at least:

1. Start stopped with an empty queue, issue `addid`, and verify the queue changes while playback
   remains stopped and no output begins playing.
2. Start paused, issue an add operation, and verify playback remains paused.
3. Issue an explicit play operation after adding and verify playback starts normally.

Trackknife currently has a Melody-specific compatibility workaround that restores the previous
transport state after adding to an empty queue. Remove or version-gate that workaround once fixed
Melody releases can be identified reliably.
