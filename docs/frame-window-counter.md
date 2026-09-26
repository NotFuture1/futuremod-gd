# Automated Frame Window Counter — proposal

> **Status:** v1 implemented (v1.2.0, 2026-09-26), awaiting on-hardware
> validation. See "What v1.2.0 ships" at the end. Builds on the analyzer in
> `src/macro.cpp` and the research in `frame-perfect-analyzer.md`.

## 0. The gap

The community's frame-window pipeline is split in half:

| Half | Who does it today | How |
| --- | --- | --- |
| **Measure** each input's window | NaN GD, by hand | frame-step / re-attempt each timing, write the number down |
| **Present + score** the windows | `frame-window-counter` (Geode) + NaNDL site | typed-in per-action windows → HUD, markers, `L*` precision |

The measuring half is the expensive one — hours per level, one person, not
reproducible. We already own the piece nobody else has automated: a
deterministic re-simulator that perturbs a recorded input by ±k ticks and
observes survival. **The proposal is to finish the analyzer into a true window
counter and emit the community's data format, so the manual half disappears.**

## 1. What changes vs. today's analyzer

Today: per input, expand offsets −1,+1,−2,+2… until both sides die, **stop as
soon as the width exceeds 4 ticks**, and bucket the result into three counters
(FP@240 / @120 / @60).

That bucketing is exactly the metric NaN argues against: a 60 Hz "frame
perfect" is anywhere from a 1- to a 7-tick window at 240. We already compute the
tick-exact number and then throw it away.

1. **Measure the true window `N_i` out to 10 ticks** (`kMaxK` 8 → 10, early-exit
   threshold 4 → 10). NaN's list tabulates windows 0–10 per level; anything
   wider is "not a timing" and resolves in 2 probes.
2. **Both edges, always.** Press *and* release get their own `N_i`
   (`analyze-releases` on by default). Wave/ship straight-fly is release timing;
   omitting releases undercounts those levels badly.
3. **Window = gap-clamped.** If shifting the edge by `d` would move it past the
   adjacent edge of the same button, the window is genuinely bounded by that
   gap: clamp, and flag the input `capped` instead of probing through it.
4. **Per-input confidence.** Every window carries a status: `exact`,
   `capped`, `anchor-fallback`, `horizon-truncated`, `timeout`. A number we
   aren't sure of must never silently enter a total — the whole value of an
   automated counter is that its output is auditable.

## 2. Output: speak NaNDL

Per analyzed run, write a table of `{ i, t_i (s), N_i, player, I/F, status }`
and export it two ways:

- **NaNDL JSON** — the format the site's calculator imports (`Game FPS`,
  `Window FPS` = 240, `respawn time`, then rows of input number / time / frame
  window). Drops straight into <https://nandl.pages.dev> and into
  `frame-window-counter`'s importer.
- **`.gdr`** for the raw macro, so the run itself is replayable elsewhere.

Then compute `L*` **in-mod**, using NaNDL's published model verbatim:

```
w_i = N_i / f                      f = 240
s_i = ½ · w_i · L                  (× nerve e^(−k_t·t_i), fatigue e^(−k_u·i))
p_i = erf(s_i / (σ√2))             two-sided normal
P(C) = Π p_i
E[T_A] = t_n·P(C) + Σ t_i·r_i·q_i     r_i = Π_{j<i} p_j
E[T_C] = E[T_A] / P(C)
L*  = the L where E[T_C] = 24 h     (monotone in L → bisection)
```

Constants from the site: `k_t = 0.0016520833717346`,
`k_u = 0.0002727763242154`. A `σ/s` readout in the corner during replay is the
same number NaN puts in his videos — directly comparable, which is the point.

## 3. Making it fast enough to be usable

Cost is `Σ (2·N_i + 2)` probes, ~22 worst case per input. On a 300-input extreme
that's ~3–6k probes; at full-level replay each (`O(L)` per probe) it is hours.
Three levers, in order of payoff:

1. **Save-state anchors as the default path** (`analyze-fast`, already written
   and verified-per-anchor against the baseline track): a probe costs
   `kMaxK + horizon` ≈ 100 ticks instead of the whole level. This is the
   difference between `O(n·L)` and `O(n·k)` — everything else is a rounding
   error next to it. Promote it to default once it's confirmed on hardware,
   keeping the automatic per-anchor fallback.
2. **Cheap prefilter.** Before probing, look at the baseline trajectory around
   the input: if the player clears every hazard hitbox by a wide margin across
   the whole ±10 sweep envelope, the window is "wide" — report `>10` from the
   geometry and spend zero probes. Most inputs in most levels are not timings.
3. **Gallop + verify per side** instead of stepping 1,2,3…: probe
   `d = 1, 2, 4, 8`, bisect between the last survivor and the first death, then
   **verify contiguity** by re-probing the claimed boundary−1. Non-contiguous
   survival (a shifted press catching a *different* orb) is real, so the
   verification pass is mandatory, not optional — without it galloping
   over-reports windows.

## 4. Presentation (the part that makes it a "counter")

- Markers spawned at the player's position for each input, coloured by `N_i`
  (1 = red … 10 = green), with world→screen transforms so they survive camera
  rotation/zoom/mirror — the visual language NaN's videos already established.
- HUD: a live tally per window bucket (configurable ranges), plus running `σ/s`.
- Ding pitched by window width (already there for 3 tiers — generalise to 10).

## 5. Honest limits to state up front

- **Sub-tick (CBF) windows.** Click Between Frames makes real windows
  fractional; the community tool already parses `1/2`. We measure at 240 Hz
  integer ticks, so we'd report `1` where the truth is `1/2`. Refinement via a
  higher internal tick rate changes physics and is not obviously sound — leave
  it out of v1 and say so rather than emitting fake precision.
- **One run is one path.** The windows measured are for *that* route; an input
  the player could have done differently isn't represented.
- **Independence assumption.** NaNDL's model assumes inputs are independent;
  measured windows are per-input-in-isolation, which matches the model but not
  a human's experience of a tight section.
- **`L*` is not difficulty.** NaN says this himself. Report it as precision,
  never as a placement.

## 6. Phasing

- **v1** — windows 0–10, both edges, gap-clamping, confidence flags, NaNDL JSON
  export, anchors on by default. No `L*` yet.
- **v2** — in-mod `L*` solver + live `σ/s`, window-coloured markers, bucket HUD.
- **v3** — prefilter + gallop/verify probing, `.gdr` import so any existing
  macro (zBot, Mega Hack, Eclipse) can be analyzed without re-recording.
- **v4** — calibration harness: run levels NaN has published counts for and diff
  per-input, since a counter nobody can check is worth nothing.

## 7. What v1.2.0 ships (and how it differs from the plan above)

**Measured in the real engine, not a ghost.** Sub-frame measurement was
planned on the ghost simulation (Phase 1). It isn't needed: Click Between
Frames' step split is small, public (theyareonit/Click-Between-Frames,
MIT) and deterministic once its wall-clock input is taken out. So the replay
reproduces the split itself, and every probe runs in the real engine. The
ghost probe stays as an optional diagnostic.

- **Recording.** Each input stores `(step, frac, mid, mask)`. `mid` = GD's
  `handleButton` fired inside player 1's `PlayerObject::update` (CBF calls
  it between substeps); `frac` = the player-update time already simulated
  that step divided by the step's total, measured by an innermost
  (`VeryLate`) update hook and finalized at the next `processCommands`.
  `mask` = which players GD actually pushed (hooked `pushButton` /
  `releaseButton`), so dual-mode replays push both icons.
- **Replay.** Boundary inputs are still applied in `processQueuedButtons`,
  as before. Mid-step inputs are applied by a `Late`-priority player-update
  hook that mirrors CBF v1.5.0 line for line: substep factors, not-buffering
  rule, on-ground fix, slope/dart collision delta, leftover rotation plus
  `m_lastPosition`, P2 folded into P1's update, and the Windows ship-rotation
  `Slerp2D` workaround (same address). While the macro drives,
  `m_queuedButtons` is cleared each frame so CBF never splits a replay step
  for a stray real click.
- **Windows.** Integer sweep −1,+1,−2,+2… up to `fw-max-window` (default
  10), then for `mid` inputs a bisection of each dead edge (`fw-subframe`,
  default 3 = 1/8 frame). Window = hi − lo, where whole-frame edges sit ±0.5
  outside the outermost survivor (N surviving frames = window N).
- **Taps move as one gesture.** A press whose release follows within
  `maxWindow + 2` frames is shifted together with that release. The plan's
  "clamp at the adjacent edge" would have capped every short tap's window
  at its hold length, which inflates the frame-perfect count. Remaining
  clamps (the neighbouring gesture) are flagged `capped`.
- **Statuses:** `exact`, `wide` (> max), `capped`, `timeout`. The planned
  `anchor-fallback` is only logged: a failed anchor falls back to a full
  replay, and the window stays exact.
- **Exports** (macros folder): `<level>.windows.csv` (everything),
  `<level>.nandl.json` (every input; non-timings as NaNDL's `"-"`),
  `<level>.fwc.json` (timings only, because the frame-window-counter mod
  reads `"-"` as 1).
- **Diagnostics:** `[fw] BASELINE ... driftVsRecording=... VERDICT=` compares
  the analyzer's replay against the recorded run (the recorded track is now
  saved as `<level>.track`). `[fw] PLAYBACK ... VERDICT=` does the same for K.
- **Refused:** CBF Physics Bypass (wall-clock step count, so runs aren't
  reproducible). Warned: GD's own "click between steps" without CBF.

Not done yet: in-mod `L*`, window-coloured markers, gallop/prefilter probing,
`.gdr` import (v2/v3 above).
