# Field impact experiments

These experiments compare low-cost, streaming audio-impact policies against the
same 247-second, 12-swing development recording. The contract in
[`BENCHMARK.md`](BENCHMARK.md) freezes labels, readiness, first-terminal
semantics, lifecycle costs, and ranking. Results from this recording rank
prototypes; they are not production accuracy estimates because phone, camera
view, microphone placement, and room acoustics are confounded.

## Results

| Branch | Best defensible result | Decision |
| --- | --- | --- |
| Adaptive envelope | A conservative 120 Hz high-pass and robust 12x-20x background gate retains 12/12 local impacts on each phone and scores 11/12 first-terminal per view. | Keep as the next streaming detector prototype. Do not ship the in-sample 20x plus high-frequency threshold that happens to score 12/12. |
| Statistical classifier | Nested leave-one-swing-out temporal features score 23/24 first-terminal, but either adjacent threshold regresses to 22/24. | Do not ship fitted weights; collect a negative-rich holdout first. |
| Spectral/template classifier | Template variants score 20/24 or 21/24 and introduce misses. | Reject for the current prototype. |
| Two-phone consensus | ATL-leader immediate capture records 12/12 swings with one practice-swing attempt and 28.19% high-speed duty. Hard consensus does not improve it and adds network-dependent latency. | Keep ATL local audio authoritative; do not make capture depend on both microphones. |
| Shadow timestamp selection | The former latest-within-250-ms policy was within 100 ms for 11/12 shadow clips. A clock-mapped candidate ring with a 75 ms bound reaches 12/12 on this recording. | Implemented in production with measured clock offset; peer arrival remains an explicit fallback. |

The practice swing before S01 is the remaining conservative-envelope early
terminal. Peak amplitude cannot safely remove it: on DTL it is louder than the
quietest credited real impact.

In the complete ATL-pose lifecycle, the conservative envelope still captures
12/12 with one practice-swing attempt, but its 20-67 ms bounded decisions raise
high-speed duty from 28.19% to 28.35% and reduce the minimum pre-takeaway video
lead from 995 ms to 796 ms. It therefore provides no end-to-end reason to
replace the current ATL production detector yet. Changing which attempt
terminates first can alter later pose blindness and thermal duty, so these
complete-lifecycle metrics—not only isolated terminal-window precision—remain
required for future detector work.

Detailed reports are generated beneath the ignored field-recording artifact:

- `experiments/envelope/RESULT.md` and the two complete candidate JSON files;
- `experiments/spectral/REPORT.md` and `report.json`; and
- `experiments/consensus/report.json` for paired terminal, shadow timestamp,
  and complete camera-lifecycle evaluation.

## Production boundary

The offline envelope implementation uses prefix arrays to make parameter sweeps
simple. A phone implementation must instead use fixed-size streaming state: a
one-pole high-pass, short RMS accumulators, a robust background block ring, and
a bounded candidate cluster. Threshold constants selected on this recording
must remain experimental until a phone/view swap and a recording rich in quiet
impacts, practice swings, mat strikes, waggles, speech, footsteps, club drops,
and aborted addresses are held out from selection.
