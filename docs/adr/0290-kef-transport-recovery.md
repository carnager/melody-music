# ADR-0290: KEF transport compatibility trial

Status: Accepted for a trial, 2026-10-17.

## Evidence

An LS50 Wireless II on firmware 4.1.192 repeatedly remained in
`TRANSITIONING`. Its stream was readable from the engine and another LAN
client. `Play` returned 701 (Transition not available); `Stop` acknowledged
success without changing the state. Power cycling restored playback. Other
albums failed while wedged; the same user's Sonos outputs did not exhibit it.

**Unknown:** what first puts the KEF in that state. Next-track preloading
and tightly spaced transport commands are hypotheses, not established causes.

## Trial decision

Only renderers identifying themselves as manufacturer `KEF`, model
`LS50 Wireless II`, use this compatibility path. Other renderers keep their
existing behavior, including Sonos gapless playback and sample-rate limits.

- Do not send `SetNextAVTransportURI`, even to clear a continuation. Decline
  gapless preparation so the engine's existing gapped progression advances
  the queue. There is no second queue or playback implementation.
- Stop, confirm STOPPED/NO_MEDIA_PRESENT on a poll, set the URI, and confirm
  readiness on another poll before Play. Network work stays on the output
  worker. A newer track, pause or stop supersedes pending recovery.
- Allow 20 seconds without startup progress, or in TRANSITIONING after
  playback. Attempt one Stop, confirm readiness, clear the current URI,
  confirm readiness, load a newly ticketed URI and Play. Restore the last
  known position only after playback is seekable. Do not change occurrence
  identity, manufacture an end-of-track, or seize another controller's URI.
- Allow one further 20-second window. If still stuck, pause with a persistent
  error. Do not repeatedly hammer a wedged speaker. Explicit user Play or a
  new track starts a new attempt. The SDK's own network-call timeouts can
  extend the wall-clock time; the player/UI never waits on them.

**Melody decision:** small inter-track gaps are acceptable for this trial.
This is an automatic compatibility policy for the identified model, not a
promise about all KEF models or a proven firmware repair. There is no firmware
update, speaker reboot, new dependency or database migration in this change.

## Validation

The injected clock and simulated renderer cover delayed URI readiness,
fresh-ticket recovery and deferred seek, no next-URI calls, normal completion,
permanent TRANSITIONING, bounded failure, pause/new-track cancellation and
another controller taking over. Existing Sonos/general-renderer tests remain.

On 2026-10-17, the physical LS50 Wireless II on firmware 4.1.192 passed
startup, seeking, pause/resume, three Stop/Play cycles, and six track
transitions through the engine's gapped progression. The listener confirmed
audible playback. The UPnP, engine-player and output-agent suites passed on
both the Studio and server native builds. These healthy-device checks do not
prove recovery from a physical firmware hang; that path was tested with the
simulated renderer.

The deployed native server engine also passed KEF startup with advancing
playback and a seek. Its original queue, modes, volume and paused position
were restored afterwards. A separate delay opening NAS files preceded this
check; macOS logged network-volume authorization as allowed before the
successful retry. That delay is not evidence of a KEF transport failure.

**Unknown:** whether this reduces long-running failures on the physical KEF.
An extended listening trial is required before claiming that the recurring
problem is fixed.

## Amendment, 2026-11-08: resume in place

**Evidence.** On the same LS50 Wireless II (model SP4025), each resume after a
pause reloaded the track: Stop, set the URI, Play, then Seek once the KEF
reported PLAYING. The KEF's own GetPositionInfo, polled every 250 ms, showed
STOPPED at 0:00 for 1.6 s, then PLAYING from 0:00 to 0:01 for about 1.4 s, and
only then the paused position. The listener heard the song's first seconds
before every resume.

**Decision.** A KEF that reports PAUSED_PLAYBACK on the current track's URI,
with a ticket that is not about to expire, is resumed with `Play`. The
watchdog above watches it with a five-second window: a resume that has not
advanced by then gets the one stop/clear/reload recovery, from the paused
position. A `Play` refused mid-transition (701) reloads at once. A KEF that
is not paused on the track, a deferred (restored) track and an expiring
ticket are reloaded as before.

**Validation.** The simulated renderer covers resume in place, a KEF no
longer paused on the track, a resume that does not take within five seconds,
and a 701 on resume; the reload cases check that the position is sought.
