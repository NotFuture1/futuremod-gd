# Frame-Perfect Analyzer — Design & Research

> **Status (2026-07, `src/macro.cpp`):** implemented — step-indexed
> record/replay with RNG-seed lock, per-level save files, baseline
> determinism gate, ±k perturbation probing with early exit (window > 4
> ticks stops immediately), optional release analysis, and a scheduler
> time-scale speed-up that is verified by a second sped-up baseline and
> auto-falls back to 1x. Death detection is progress-based (`maxX` +
> stall), NOT `destroyPlayer` (it can fire without killing). Not yet done:
> `.gdr` import/export (§3), buffer classification (§6), save-state
> acceleration (§4 — tried once, reverted; restores insta-killed the player).

Goal: after a single piece of human input (a completed run / macro of a level),
**automatically** determine how many "frame perfects" the run contains, and
where they are — far faster than manually frame-stepping each input.

This document is the plan, grounded in research of GD 2.2 internals, the
community definition of "frame perfect", and how existing open-source bots work.

---

## 0. TL;DR — the recommended approach

1. **Capture** a successful run as a **tick-indexed input macro** (reuse the
   `.gdr` / GDReplayFormat standard), recorded at a **fixed 240 TPS, speed 1.0**,
   with the level's **RNG seed** stored.
2. **Re-simulate deterministically**: drive GD's own physics engine (no
   reimplementation) forward, injecting the recorded inputs at their exact
   physics steps.
3. **Perturb each input by ±1, ±2, … ticks** and re-run from a snapshot taken
   just before it. Count how many consecutive offsets survive → that input's
   **frame window** (in 240 TPS frames). **Window == 1 ⇒ frame perfect.**
4. **Classify** each input as a *true timing input* vs a *bufferable*
   orb/pad activation (which only looks frame-perfect), so we don't overcount.
5. **Report**: total count, per-input window, location (x / % / time), overlay.

The whole thing is the **±frame perturbation + re-simulation** method. The
community's modern "Frame Window" metric (count valid frames at 240 TPS) is
literally the human-facing output of this exact analysis — so we're building the
automated form of what TASers/creators do by hand.

---

## 1. What "frame perfect" actually means (research-backed)

- **Definition:** an input (press *or* release) that succeeds on **exactly one
  available frame** — one frame earlier or later = death/miss. (Universal
  community definition.)
- **Which rate? This is the crux.** GD 2.2 physics is a **fixed-timestep loop at
  240 TPS** (ticks/sec), decoupled from render FPS. Each rendered frame is split
  into `stepCount = round(dt * 240)` physics substeps:
  - 60 FPS → 4 substeps/frame, 240 FPS → 1 substep/frame.
  - The engine choke points (verified in 2.2081 bindings):
    `GJBaseGameLayer::getModifiedDelta(float)` then a loop calling
    `processCommands(float dt, bool isHalfTick, bool isLastTick)` per substep.
- **Legacy vs modern metric:**
  - *Legacy*: "frame perfect" = 1 frame at **60 FPS** (~16.7 ms window).
  - *Modern* ("Frame Window", popularized by NaN GD): count valid frames at
    **240 TPS** (~4.2 ms each). A single 60-fps frame perfect can be anywhere
    from ~1 to ~7 frames at 240 depending on alignment.
- **Decision for this tool:** measure natively in **240 TPS frames** (the
  engine's true resolution), and *also* emit a legacy **"60 fps frame-perfect:
  yes/no"** flag for familiarity. Reporting the 240-window count is strictly
  more informative and is where the community is heading.
- **Edges matter independently:** the **down-edge (press)** and **up-edge
  (release)** of each input each have their own window. Wave/ship straight-fly is
  full of *release* frame perfects; spam is many near-frame-perfect cycles.

---

## 2. Core method — deterministic re-simulation + perturbation

GD physics is **deterministic** when four things are held fixed: **TPS (240),
speed (1.0), level state, and RNG seed**. Bots rely on this; that's why a macro
only needs to store *input tick indices*, not positions.

**Algorithm (per input edge `e` at tick `t`):**
```
window = {0}                       // offsets that survive, including 0 (the real run)
for d in [-1,-2,-3,...] and [+1,+2,+3,...]:
    restore snapshot taken just before tick t   // see §4
    replay the run but move edge e to tick t+d  // all other inputs unchanged
    simulate forward to the "verification horizon" (§5)
    if player survived the horizon: window.add(d)
    else: stop expanding in that direction
windowSize = count of the maximal run of consecutive offsets around 0
e.isFramePerfect(240) = (windowSize == 1)
```
The maximal *consecutive* run around 0 is the timing window. We expand outward
until we hit death on each side, so a single re-sim per offset is enough.

This uses the **real engine** for physics/collision, so every mechanic
(cube/ship/ball/wave/ufo/robot/spider/swing, mini, mirror, dual, gravity
portals, slopes, dash orbs, moving hazards, triggers) is handled correctly for
free — we never reimplement physics.

---

## 3. Architecture / pipeline

```
[Capture]        record successful run  → tick-indexed macro (.gdr) + seed
   │
[Harness]        deterministic replay + fast-forward physics (render off)
   │
[Snapshots]      save/restore mid-level state before each tested input
   │
[Perturb]        per input edge: shift ±k ticks, re-sim, find window
   │
[Classify]       buffer-activation vs true timing; required vs free input
   │
[Report/UI]      counter + per-input windows + overlay markers + export
```

**Key hook points (all verified present in 2.2081 bindings):**
- Determinism / stepping: `GJBaseGameLayer::getModifiedDelta`,
  `GJBaseGameLayer::update`, `processCommands(dt, isHalfTick, isLastTick)`.
- Input inject/record: `GJBaseGameLayer::handleButton(bool,int,bool)`,
  `queueButton(int,bool,bool,double)`, `processQueuedButtons(dt,clear)`,
  `m_queuedButtons`. Record by wrapping `handleButton` with a "bot input" guard
  so injected inputs aren't re-recorded (the xdBot/GDH pattern).
- Player physics: `PlayerObject::update/updateJump/pushButton/releaseButton/
  collidedWithObject`, fields `m_yVelocity`, `m_vehicleSize`, etc.
- Snapshots: `PlayLayer::markCheckpoint()` → `CheckpointObject*`,
  `loadFromCheckpoint(CheckpointObject*)`, `removeCheckpoint`,
  `PlayerObject::loadFromCheckpoint(PlayerCheckpoint*)`.
- RNG/step: `m_randomSeed (uint64)`, `m_replayRandSeed (uint64)`,
  `m_currentStep (int)`, `m_timestamp (double)`, `m_gameState (GJGameState)`,
  `m_effectManager (GJEffectManager*)`.

**Macro format:** adopt **GDReplayFormat (`.gdr`, gdr2 branch)** — the de-facto
standard used by xdBot and Eclipse. Per input: `{ uint64 frame; int button;
bool player2; bool down; }`. Header carries `framerate` (240), `seed`, level
info, and `deaths`. This buys compatibility with runs users already have.

---

## 4. The hardest subproblem — deterministic save-states

To avoid re-simulating from the level start for every perturbation (O(N²)), we
snapshot state **just before** the input under test and restore it per offset.

Two strategies, in order of preference:

**(A) Native checkpoint + seed lock (recommended first).** GD already implements
a robust mid-level snapshot for Practice mode: `markCheckpoint()` /
`loadFromCheckpoint()` capture player kinematics, mode, moving-object states,
trigger state, camera, etc. We layer on top:
- also save/restore `m_randomSeed`, `m_replayRandSeed`, `m_currentStep`,
  `m_timestamp` (the checkpoint may not cover all of these).
- **Validate** by comparing the re-sim against the macro's optional **physics
  track** (the `.gdr` PhysicsInput extension: per-frame `x,y,rotation,
  xVel,yVel`). If they diverge, we have a desync → snapshot is incomplete.

**(B) Full manual snapshot (fallback / for true random-access).** Deep-copy the
enumerated state. Non-exhaustive field list gathered from the bindings + xdBot's
`PlayerData`:
- **RNG/step:** `m_randomSeed`, `m_replayRandSeed`, `m_currentStep`, `m_timestamp`.
- **`m_gameState` (GJGameState):** level/total time, progress, camera
  (pos/zoom/angle/shake/offset), dual/gravity/flip flags, timewarp, and the
  active-effect instance vectors (move/rotate/scale/fade/tint/follow, dynamic
  move/rotate, tween actions, `m_gameObjectPhysics`, activated-object IDs).
- **`m_effectManager` (GJEffectManager):** pulse/opacity/color action maps,
  spawn-trigger actions, item/count/timer maps — or color/pulse/count triggers
  desync.
- **Per `PlayerObject` (p1+p2):** position, `m_yVelocity`, fall/slope velocity,
  rotation+rotation speed, gravity/upside-down/sideways, mode booleans
  (ship/bird/ball/dart/robot/spider/swing), `m_vehicleSize` (mini), speed,
  ground/slope/dash/slide/ice flags, hold/lock/buffer flags, touched
  ring/pad/portal, last jump/flip times, dead flag.
- **`PlayLayer` progress:** checkpoints array, time/attempt, jumps,
  dynamic/active "saved object state" refs (toggled/moved objects), collected
  items. (Note: many counters like `m_jumps`/`m_attempts`/`m_normalPercent` are
  wrapped in GD's `SeedValueRSV` anti-cheat structs — read/write via the wrapper.)

**Reality check from research:** *no* open-source bot hand-rolls a full
snapshot — they all lean on the native checkpoint and/or replay-from-start with
locked seeds, and add **periodic state anchors** (e.g. ToastyReplay snapshots
both players + seed every 240 ticks) to **detect and correct drift**. We adopt
the same posture: native checkpoint + seed lock + anchor-based desync detection,
and only escalate to manual snapshotting for fields proven to desync.

---

## 5. Defining "survived the horizon"

Shifting an input changes downstream alignment, so we must bound how far we
check and hold everything else fixed:
- **Horizon = from the tested input until the player clears the associated
  hazard / reaches the next recorded input (or N ticks, whichever first).**
- All *other* inputs stay at their original ticks. This yields "the tolerance of
  *this* input given the rest of the run is played as recorded" — exactly the
  community/TAS definition.
- Success = the player does not die and reaches the same downstream reference
  state (next checkpoint / next input's x-position) the real run did.

---

## 6. Classification — avoid overcounting

A naive press-window of 1 is **not** always a frame perfect:

- **Bufferable activations (orbs, dash orbs, pads):** GD checks a *held* input
  against collision **every tick**, so pre-holding triggers the object on the
  first contact frame. Effective window is wide even if the *exact press tick* is
  unique. → Detect the object type activated on that tick; if it's a
  ring/pad and the input is held into contact, classify as **buffered (not a
  true frame perfect)**. Test this by also shifting the press *earlier with hold*
  and seeing if it still works.
- **True timing inputs:** gaps/spikes that require a *fresh* press that can't be
  pre-held, and **all releases** (straight-fly), are genuine frame-perfect
  candidates.
- **Required vs free input:** an input is "required" if removing it (or shifting
  it far) changes survival. Free inputs (extra clicks that don't matter) get
  window = ∞ and are excluded from the count.

Output buckets: `truePress`, `trueRelease`, `bufferedActivation`, `free`.
The headline "frame perfect count" = true press/release inputs with window 1.

---

## 7. Issues & failure modes (the hard part — considered exhaustively)

**Determinism**
1. **Physics bypass / non-240 TPS / FPS coupling** → outcomes change. *Mitigate:*
   force 240 TPS + speed 1.0 + click-on-steps during analysis; detect & warn if
   the source run used physics bypass.
2. **Click Between Frames (sub-tick inputs).** If the human played with CBF,
   inputs land *between* ticks (CBF splits a tick into substeps, applying input
   at a fractional `deltaFactor`). Our tick-quantized analysis must **canonicalize
   to 240** (quantize the press to its tick) — and optionally report sub-tick
   timing separately. Disable CBF/force click-on-steps during re-sim.
3. **RNG seeds.** Random triggers diverge unless `m_randomSeed`/`m_replayRandSeed`
   are restored. *Mitigate:* snapshot/restore seeds; this is mandatory.
4. **Residual drift.** Even bots see replays "randomly die" from drift (RobTop
   uses a time-derived value somewhere). *Mitigate:* anchor comparison against
   the `.gdr` physics track; re-snapshot on detected drift.

**Snapshot completeness**
5. Missing one field → silent desync. *Mitigate:* the physics-track diff is our
   oracle; any mismatch flags an incomplete snapshot before we trust results.
6. Native checkpoints may be slightly coarse (Practice-mode granularity). *Validate*
   the same way; escalate to manual fields only where needed.

**Definition / measurement**
7. **Rate ambiguity (60 vs 240).** *Mitigate:* report both; primary = 240 window.
8. **Buffering** (see §6) — biggest overcount risk.
9. **Horizon choice** — too short → false "survived"; too long → another input's
   change dominates. *Mitigate:* horizon = clear-the-hazard / next input.
10. **Multi-input chains** (e.g. tight sequence where each depends on the prior).
    We measure each given others fixed; note that joint windows can be tighter
    than any single one (report sequences flagged as "all ≤2 frames").

**Mechanics coverage**
11. Dual, mirror, gravity/teleport portals, robot/spider/swing/ufo, slopes, ice,
    dash orbs — all handled because we use the real engine. (This is the whole
    reason not to reimplement physics.)
12. **Releases** must be analyzed as first-class edges (wave). Easy to forget.

**Practicality**
13. **Performance.** Long level × many inputs × perturbations × horizon re-sims.
    *Mitigate:* (a) **prefilter** to candidate inputs using a cheap near-miss
    heuristic (player passed within ε of a hazard hitbox this frame) — only those
    can be tight; (b) snapshot reuse; (c) fast-forward with rendering suppressed
    (`dt = k/240` looping + skip draw); (d) bound `k` (e.g. test ±6 ticks). Most
    inputs resolve in a handful of short sims. Offline, so seconds–minutes is fine.
14. **Run must complete.** A macro that dies can only be analyzed up to death.
15. **Measures the recorded route**, not the theoretically easiest route — which
    is exactly what "how many frame perfects did *this* run need" should mean.
16. **TAS-adjacent tech.** Same machinery as macro bots; purely an analyzer, but
    worth a disclaimer in the UI.

---

## 8. Alternatives considered (and why the main method wins)

- **Hitbox/near-miss proximity only** (flag when player skims a hazard): cheap,
  but measures *spatial* closeness, not *timing* window — a wide-window jump can
  still pass visually close, and a frame-perfect can have spatial margin. **Verdict:
  use only as a prefilter** (§7.13) to find candidates, then confirm by perturbation.
- **Live real-time counter** (no macro; analyze each click as you play): needs a
  forward-sim + snapshot at every click in real time — expensive and racy.
  **Verdict:** the offline macro approach is cleaner and matches the ask
  ("automatic after human input"); a live mode can come later atop the same core.
- **Static level-data analysis** (parse objects, compute gaps analytically):
  requires fully reimplementing GD physics to be correct. **Verdict: not worth
  it** vs driving the real engine.

---

## 9. Incremental roadmap (each step independently testable)

- **Phase 0 — Deterministic replay.** Record/replay at 240 TPS via `handleButton`
  with a bot-input guard; reuse `.gdr`. Success = a recorded clear replays to the
  end identically (validate against physics track). *Foundation for everything.*
- **Phase 1 — Snapshot + seed lock + desync oracle.** `markCheckpoint`/
  `loadFromCheckpoint` + RNG/step restore; physics-track diff to prove
  determinism. Success = restore mid-level, replay, zero drift.
- **Phase 2 — Perturbation engine.** For a prefiltered candidate set, shift ±k,
  compute 240-frame windows. Success = correct window on hand-checked jumps.
- **Phase 3 — Classification + count.** Buffer vs true, required vs free; headline
  counter + per-input list.
- **Phase 4 — UI/UX.** In-level overlay markers at frame-perfect positions, a
  results panel (count, %/time locations), CSV/JSON export, optional legacy-60
  flag. (Keybind to run analysis fits naturally next to the existing skip-ending
  keybind.)

---

## 10. Locked decisions

1. **Input source: BOTH** — record runs natively in the mod *and* import existing
   `.gdr` macros (xdBot/Eclipse). Phase 0 builds native recording first; `.gdr`
   import slots in behind the same internal macro representation.
2. **Scope: offline run analysis first** — analyze a completed run after the
   fact; live in-play counter is a later phase on the same core.
3. **Metric: 240-window (primary) + legacy-60 flag** — per input, report the
   number of valid frames at 240 TPS, plus a "frame perfect at 60fps: yes/no".
4. **Buffered orbs: exclude from the headline count, list separately** — bucket
   activations as `truePress / trueRelease / bufferedActivation / free`; headline
   count = true press/release with 240-window == 1.

## 10b. Analyzer display requirement (locked)

The frame-perfect readout must have a **configurable rate display**: the user
multi-selects any of **60 / 120 / 240** and the analyzer shows the frame-perfect
count/window at **each selected rate**. Internally everything is measured at the
240 TPS tick; the 120 and 60 figures are derived by grouping ticks (a window of
N ticks at 240 = N/2 at 120 = N/4 at 60; "frame perfect" at rate R = window ≤
one R-frame). So one analysis pass yields all three; the setting just controls
which columns are shown.

## 11. Phase 0 — concrete first build

1. Internal `Macro` representation: `vector<InputEdge { uint32 step; uint8
   button; bool player2; bool down; }>` + `seed`, `tps=240`. (`.gdr` maps onto
   this 1:1 for later import.)
2. Force deterministic conditions while recording/analyzing: 240 TPS, speed 1.0,
   click-on-steps (CBF off).
3. Record by wrapping `GJBaseGameLayer::handleButton` with a `s_botInput` guard;
   store `m_currentStep` as the tick index.
4. Replay by injecting `handleButton` at matching `m_currentStep`.
5. **Acceptance test:** record a clear, replay it, and confirm it still completes
   — and (if physics track enabled) zero drift vs the recorded `x,y,rot,vel`.
   This proves determinism, the foundation for all later phases.
