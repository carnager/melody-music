# ADR-0120: MPD search in the library panel

- Status: retired by ADR-0224 (Melody no longer speaks MPD); was accepted; result presentation and keyboard behavior superseded by ADR-0122
- Date: 2026-09-06
- Owners: Trackknife project

## Decision

The user prefers local library search as part of the library and requests the
same placement for MPD. The MPD search field now lives below the Sources
heading, above the library content. A nonempty query replaces the browse tree
with album/track results inside that panel. Clearing the field or pressing
Escape restores the existing tree, including its loaded branches and expansion
state. Go to Artist/Album also clears search before revealing the target.

This supersedes the MPD search overlay and focus-loss dismissal specified in
ADR-0013 and the unified-workspace addendum in ADR-0058. Moving focus to the
queue or another application leaves results visible. Switching to local
context hides MPD search and preserves its query/results for the return to MPD.
Ctrl+L focuses the library search. Searching never replaces the active queue
or list tab, and the Track Lists tab strip uses its full width again.

**Trackknife decision:** retain the existing asynchronous MPD search service,
two-character minimum, 180 ms debounce, pagination, complete-release album
actions, and bounded serial artwork loading. A compact model presentation
combines artist and result title into one elided sidebar column beside the
three existing Append / Next / Replace actions. Covers, keyboard action
navigation, Enter, Ctrl+Enter, and query editing from results remain available;
full context and duration are available in the result tooltip. The underlying
wide result model remains available to its other consumers.

Query edits immediately clear old actionable results. Responses for a different
or cleared query are ignored. Results persist through ordinary focus changes;
there are no floating-window positioning or tab-strip overlay resize hooks.
Both authorities retain their own data sources and commands. No library scan,
new dependency, or persistence migration is introduced.

## Verification

Offscreen workspace tests cover field/result containment within Sources,
full-width tab strips, compact no-horizontal-scroll results, artwork,
keyboard actions, persistent results after queue focus and authority switches,
Escape/clear returning to browsing, immediate stale-result removal, and late
response rejection. Existing Go to Artist/Album and model tests remain
regression coverage.
