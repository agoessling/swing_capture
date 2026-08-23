# Field audio experiment benchmark

## Frozen evidence

The development recording is
`artifacts/field_recording_95482d93-f024-400e-9532-5ba7082de05e`. Experiments
must not modify these inputs:

- `pose_replay/target_swing_index.json` defines S01 through S12 on both WAV and
  video time axes;
- `pose_replay/full_frame_roi/face_on/observations.csv` is the initial
  Pixel 6/ATL leader pose stream;
- `pose_replay/full_frame_roi/face_on/session_replay_roi_free.json` defines the
  baseline full-session pose attempts;
- `production_impact_candidates.json` contains complete production-policy local
  candidates and the measured 76.917 ms WAV-axis alignment; and
- `*_production_audio_windows.json` preserves the current per-attempt detector
  behavior.

This one recording confounds device, view, mounting location, and acoustics. It
is a development set for ranking prototypes, not a production accuracy claim.

## Evaluation layers

Every experiment reports all three layers when applicable.

### Candidate generation

- A target is recalled when at least one candidate is within 100 ms of its
  curated WAV peak.
- Report recall separately for DTL, ATL, and any paired candidate stream.
- Report all non-credited candidates and false candidates per minute.
- Do not discard candidates merely because they are inconvenient for terminal
  scoring.

### Armed terminal behavior

- Start from the same pose arm times and WAV/video offset.
- A candidate before full trigger readiness is diagnostic evidence, not a
  terminal trigger.
- The first accepted candidate after readiness is terminal. Later knowledge
  cannot replace it unless the proposed online algorithm explicitly waits and
  has a bounded decision deadline.
- Report target-first, false-early, late, and no-candidate outcomes for every
  window, plus the terminal timestamp error.
- Any look-ahead, cluster, or peer wait must be included in the reported
  decision latency.

### Complete capture lifecycle

- Begin with the ATL pose stream as leader authority.
- Model pose blindness while 240 fps capture owns Camera2.
- Model false terminal events, one-second completion, 800 ms camera restart,
  post-capture reset/cooldown, no-impact timeout, and the 30-second thermal cap.
- Report real swings captured, false attempts, attempts total, high-speed
  duration/duty cycle, minimum video lead before takeaway, and minimum retained
  history at impact.

## Ranking rule

Compare configurations lexicographically:

1. maximize complete-lifecycle real swings captured;
2. require 12/12 local impact evidence before using precision as a tiebreaker;
3. minimize early terminal events that prevent a real capture;
4. minimize false attempts and high-speed duty cycle;
5. minimize terminal timestamp error and added decision latency; and
6. prefer lower on-phone CPU, memory, and implementation complexity when the
   preceding results are equivalent.

Threshold sweeps must report a stable neighborhood, not only the single best
configuration. Learned/template methods must exclude the evaluated swing from
their training/reference set. A result selected on this recording remains a
prototype until it passes a phone-swap and negative-rich holdout recording.
