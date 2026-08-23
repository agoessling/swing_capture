# Combined trigger lifecycle replay

`//android/core/trigger:combined_trigger_lifecycle_test` is the deterministic,
Android-independent acceptance test for the leader-side automatic-trigger
lifecycle. It feeds compact synthetic event fixtures through the production pose
trigger controller, then models the interval where the leader's standby camera is
unavailable. Peer behavior is a scripted readiness outcome; this is not a second
pose controller, HTTP/network replay, or complete two-phone lifecycle simulation.

- high-speed video startup, the local pre-roll milestone, and a scripted peer
  readiness resolution;
- leader audio terminal candidates;
- active-evidence and absolute thermal deadlines;
- post-terminal capture completion and standby-camera restart; and
- remembered reset evidence, cooldown, and pose rearm.

The `field_twelve_with_practice.events` fixture is a synthetic analogue of the
first field session. It requires one separate practice-swing attempt followed by
12/12 captured targets named `S01` through `S12`. The off-nominal fixture covers
arm-without-impact recovery, a false audio terminal before a real impact,
remaining in frame between shots, rapid back-to-back capture, and a late terminal
peer failure. Focused boundary cases also cover local startup failure and a fatal
thermal hard cap after an `UNCONFIRMED` peer outcome.

Fixture rows are deliberately event-level rather than recorded media. A `pose`
row expands an inclusive constant-value 5 Hz range; `target` and `audio` rows
keep truth separate from detector output. A four-field `startup` row is shorthand
for video/local readiness with a simultaneous `READY` peer. The explicit six-field
form supplies video, local readiness, peer-resolution delay, and `ready`, `failed`,
`unconfirmed`, or `pending`; `never` represents a missing milestone.
This makes lifecycle regressions hermetic and fast. Recorded preview and audio
replays remain necessary to validate perception and detector quality.
