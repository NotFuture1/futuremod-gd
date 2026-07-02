# Future Mod — Geometry Dash Geode mod

Two features:

1. **Skip level endings** — press a key while a level is finishing to
   register the completion and instantly exit the level, skipping the
   suck-into-the-wall animation, the dead-air delay, and the completion panel.
2. **Macro + frame-perfect analyzer** — record a run as a physics-step macro
   (J), replay it (K), and analyze it (N): every input is shifted ±ticks and
   re-simulated to measure its timing window, yielding frame-perfect counts
   at 240/120/60. See `docs/frame-perfect-analyzer.md` for the design.

## Setting the key

The keybind is fully rebindable in-game using Geode's native keybind system
(no extra mods required on Geode 5.x):

**Geode → Future Mod → settings ⚙️ → Skip Level Ending → click the box → press your key.**

Default is **Space**.

## Why Space is safe to use

The skip only fires once a level is **actually finishing** (the end animation
has started).
During normal play the key behaves normally (so Space still jumps); the instant
the ending begins, the same key takes you out. If you'd rather it never share
with jump, just bind it to something else.

## How it works

- A `keybind`-type setting (`skip-end`, category `gameplay`) gives the in-game
  rebind UI; `listenForKeybindSettingPresses("skip-end", ...)` receives the press.
- `playEndAnimationToPos` / `playPlatformerEndAnimationToPos` mark the "ending
  is active" window, so the key is inert during normal play.
- On press during the ending: if the completion isn't registered yet, call
  `levelComplete()` (saves best %, stars, orbs) with the completion panel
  suppressed, then `onQuit()` straight back to where you came from.

The macro recorder / frame-perfect analyzer lives in `src/macro.cpp`; the
approach (deterministic step-indexed replay + per-input ±tick perturbation)
is documented in `docs/frame-perfect-analyzer.md`.

See `BUILD.md` for build instructions (cloud build via GitHub Actions).
