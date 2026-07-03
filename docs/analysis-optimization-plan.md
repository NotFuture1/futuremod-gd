# Analyzer speed — why it's slow, and the plan to fix it

> **Status:** P1 (anchoring) implemented behind the **"Fast analysis
> (EXPERIMENTAL)"** setting (`analyze-fast`, default off). Falls back to the
> normal full-replay path for any input it can't snapshot, and guards against
> freed/removed checkpoints so it never crashes — worst case it's just slow.
> P0 (horizon tightening) and P2 (prefilter) are still future work.
> Companion to `frame-perfect-analyzer.md` §4 (save-states) and §7.13.

## 1. Why it gets exponentially slower on long levels

Every probe restarts from the **level start**. Look at `beginTest()`:
it calls `pl->resetLevel()` (back to step 0) and then `processQueuedButtons`
replays the whole macro forward until the player reaches
`marginEnd = target.step + kMargin` or dies.

So to test one shift on an input at step **T**, we re-simulate **steps 0…T**
every single time — even though that prefix is identical on every probe (only
the shifted input and what comes after it can differ).

Cost ≈ `Σ over targets ( probes_per_target × target.step )`. Targets are spread
through the level and there are *more* of them the longer the level is, and each
sits at a *larger* step — so the total scales like **O(level_length²)**. Double
the level, ~4× the analysis time. That's the blow-up you're feeling.

The scheduler speed-up (`setAnalyzeSpeed`) only reduces *render* overhead — it
still simulates every physics step of the prefix. The only way to win is to
**stop re-simulating the prefix**.

## 2. The fix: anchor state before each input, restore instead of replay

Take **one** baseline pass (we already do a baseline for the determinism gate).
As it runs, drop a **state anchor** just before each target — at
`anchorStep = target.step - kMaxK - guard` (far enough back that even a `-kMaxK`
shift still lands *after* the anchor). Then each probe:

1. **restore** that target's anchor (mid-level), instead of `resetLevel()`,
2. replay only from `anchorStep`, applying the shifted input,
3. simulate to `target.step + kMargin` and check survival.

That's ~`kMaxK + kMargin` (~56) steps per probe **regardless of where the input
is in the level**. Total drops from O(L²) to ≈ **O(L)** (one baseline pass) **+
O(targets × probes × 56)**. On a long demon this is the difference between
minutes and seconds.

### Anchor = native checkpoint + the state GD's checkpoint misses
Use `PlayLayer::markCheckpoint()` at `anchorStep` during the baseline pass (store
the `CheckpointObject*` per target, same pattern as the existing `cpStep` map).
On restore, `loadFromCheckpoint(cp)` **plus** re-apply what a checkpoint doesn't
cover — the same things `resetLevel()` already restores for playback:
`m_randomSeed`, `m_replayRandSeed`, and our `m.step / m.gameTime / playIndex`
(seek `playIndex` to the first input at/after `anchorStep`).

## 3. Why the last save-state attempt insta-killed — and how to avoid it

The analyzer's **death detector is position-based and deliberately stateful**.
In `resetLevel()`'s Analyzing branch there's even a comment: *don't reset `maxX`,
so a respawn (x→0) reads as a death.* And in `processCommands`:
`respawned = cx < maxX - kBack` → any backward jump in x = "dead".

A mid-level checkpoint restore sets the player's x to the **anchor's mid-level
x**. If `maxX` is still carrying a higher value from a previous test, the restore
looks like a huge backward jump → the detector fires **instant death**. That is
almost certainly why restores "insta-killed the player."

**Fix — decouple restore from the respawn heuristic.** On every anchor restore,
before stepping:
- set `maxX = restoredPlayerX`, `lastProgressStep = anchorStep`, `deathStep = -1`,
  `testFrames = 0` (a fresh progress baseline at the restore point), and
- ignore the backward-death check on the very first frame after a restore.

With the progress baseline re-seeded to the restore point, forward motion reads
as alive and a real death still reads as death — exactly as it does today, just
anchored mid-level instead of at step 0.

## 4. Correctness guard (never trust a bad anchor)

Native checkpoints can be slightly coarse; a missed field = silent desync = wrong
frame-perfect counts. So **validate every anchor with a determinism oracle**:

- The baseline pass already visits every step; record the player `x` (a small
  per-target `float baselineX[anchorStep .. target.step]`, or reuse the existing
  `track`).
- Right after creating an anchor, restore it and re-sim a few steps **unshifted**;
  compare x against the baseline. If drift > ε, mark that target
  **`noAnchor`** and fall back to the current full-replay path for it.

Fast path where it's provably safe, correct slow path where it isn't. Most of a
level restores cleanly, so most targets get the fast path.

## 5. Complementary win — prefilter to skip inputs that can't be tight

Most inputs in a run are *not* frame-perfect, yet we probe all of them. Add a
cheap **candidate filter** (analyzer doc §7.13a): during the baseline pass, flag
an input as a candidate only if the player passed **within ε of a hazard hitbox**
near its step. Non-candidates get `window = wide (>4)` with **zero probes**.

This cuts the number of *probed* targets to the tight subset — a big constant
factor on top of anchoring, and it's low-risk (no state restore). Rough proxy if
full hazard geometry is a pain: one max-shift probe (`±kMaxK`) — if the input
survives the biggest shift it isn't tight — but that only pays off *after*
anchoring makes a probe cheap, so ship it in that order.

## 6. Rollout (each step independently shippable + testable)

- **P0 — cheap, no restore.** Tighten the horizon: `marginEnd` = clear-the-hazard
  / next target step instead of a flat `+48` where possible; cache the baseline
  `x` track for the oracle. Small, safe speedups; validates the harness.
- **P1 — anchoring (the O(L²)→O(L) win).** Checkpoint anchor per target in the
  baseline pass; restore-instead-of-replay in `beginTest`; the §3 death-detector
  re-seed; the §4 oracle + `noAnchor` fallback. Gate it behind a setting
  (`analyze-fast`) defaulting **on**, with the current full-replay as the proven
  fallback so results can be A/B'd.
- **P2 — prefilter.** Candidate detection to drop non-tight inputs entirely.

## 7. Touch points (in `src/macro.cpp`)

- `beginTest()` — branch: anchored targets restore + seek; else current reset.
- baseline pass in `processCommands` (Analyzing) — create + validate anchors; the
  death-detector re-seed on restore.
- `resetLevel()` Analyzing branch — leave for the fallback path; anchored restores
  bypass it.
- `advanceAnalysis()` / `startProbing()` — honor `noAnchor` + the candidate set.
- `Macro` struct — add `std::unordered_map<size_t, CheckpointObject*> anchors;`,
  `std::vector<char> isCandidate;`, per-target `noAnchor`.

## 8. Expected result

Long-level analysis goes from **quadratic** to **~linear** in level length
(anchoring) with a further constant-factor cut (prefilter). Estimated
order-of-magnitude faster on full-length demons, with correctness protected by
the per-anchor determinism oracle + automatic fallback.
