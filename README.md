# Future Mod — Geometry Dash Geode mod

Press a key to instantly skip the level-ending sequence (the suck-into-the-wall
animation + the ~3s pause) and jump straight to the **Level Complete** screen.

## How it works

- Hooks `PlayLayer::levelComplete()` to know when a level is finishing.
- On the configured keypress, cancels the scheduled delay and end animation
  (`unscheduleAllSelectors()` + `stopAllActions()`) and calls
  `PlayLayer::showCompleteText()` to show the completion screen immediately.
- The key is read from the mod setting `skip-key` (a cocos2d key code).

## Changing the skip key

Edit it in-game (Geode → Future Mod → settings ⚙️) or in `mod.json`.
Common cocos2d key codes:

| Key   | Code |
|-------|------|
| Enter | 13   |
| Space | 32   |
| A     | 65   |
| Z     | 90   |
| F     | 70   |

Default is **13 (Enter)**.

## Before you build

- Change `id` / `developer` in `mod.json` from `swift` to your own name.
- Add a `logo.png` (an icon, ideally 336×336) to the project root — Geode
  requires one to package the mod.

See `BUILD.md` for full build instructions.
