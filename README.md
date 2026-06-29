# Future Mod — Geometry Dash Geode mod

Press a key to instantly skip a level's ending — the suck-into-the-wall
animation **and** the dead-air delay — and jump straight to the **Level
Complete** screen.

## Setting the key

The keybind is fully rebindable in-game using Geode's native keybind system
(no extra mods required on Geode 5.x):

**Geode → Future Mod → settings ⚙️ → Skip Level Ending → click the box → press your key.**

Default is **Space**.

## Why Space is safe to use

The skip only fires once a level is **actually finishing** (`m_hasCompletedLevel`).
During normal play the key behaves normally (so Space still jumps); the instant
the ending begins, the same key takes you out. If you'd rather it never share
with jump, just bind it to something else.

## How it works

- A `keybind`-type setting (`skip-end`, category `gameplay`) gives the in-game
  rebind UI.
- `listenForKeybindSettingPresses("skip-end", ...)` receives the press.
- On press during the ending, it stops the end animation on the layer and both
  players, `unscheduleAllSelectors()` to drop the pre-panel delay, then calls
  `PlayLayer::showCompleteText()` to show the completion screen immediately.

See `BUILD.md` for build instructions (cloud build via GitHub Actions).
