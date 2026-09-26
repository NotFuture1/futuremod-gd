# Future Mod

Two tools in one mod.

## Skip the level ending

Tired of the suck-into-the-wall animation, the dead air, and the **Level
Complete** panel? Press your **Exit Level Ending** key (default: **Space**)
the moment a level starts finishing and you're instantly back where you came
from. The completion still counts — best %, stars and orbs are saved.

The key only acts while a level is actually finishing, so it's safe to leave
on your jump key.

## Macro + frame window counter

- **Record** (default **J**): records your inputs as a physics-step macro.
  Works with **Click Between Frames**: each click's exact position inside the
  physics step is captured. Practice-mode aware: checkpoints stitch the macro
  together from cleared segments.
- **Play** (default **K**): replays the macro from the start of the attempt,
  reproducing CBF's mid-step clicks exactly.
- **Analyze** (default **N**): shifts every press and release earlier and
  later, re-runs, and measures its real **frame window** in 240 Hz frames
  (0 to 10, and sub-frame for CBF clicks). The results go to the HUD,
  `geode.log`, and export files in the macros folder (**pause menu > Files**):
  a CSV plus NaNDL-calculator JSON that also imports into the
  frame-window-counter mod.

A HUD in the top-right shows the tally, and later playbacks ding as you pass
each frame-perfect input. Macros save per level automatically. Reload them
from the **Macros** button on the pause menu.

Turn off speedhack, noclip and CBF's *Physics Bypass* while recording and
analyzing, and don't touch the controls while an analysis runs.
