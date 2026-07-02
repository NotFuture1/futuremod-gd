# Future Mod

Two tools in one mod.

## Skip the level ending

Tired of the suck-into-the-wall animation, the dead air, and the **Level
Complete** panel? Press your **Exit Level Ending** key (default: **Space**)
the moment a level starts finishing and you're instantly back where you came
from. The completion still counts — best %, stars and orbs are saved.

The key only acts while a level is actually finishing, so it's safe to leave
on your jump key.

## Macro + frame-perfect analyzer

- **Record** (default **J**): records your inputs as a physics-step macro.
  Practice-mode aware — checkpoints stitch the macro from cleared segments.
- **Play** (default **K**): replays the macro from the start of the attempt.
- **Analyze** (default **N**): re-runs your macro shifting every input a few
  ticks earlier/later to measure its real timing window, then counts the
  frame perfects at **240 / 120 / 60** (choose which rates to show in
  settings). Runs sped-up when it can prove the replay stays deterministic,
  otherwise in real time.

A HUD in the top-right shows the tally, and later playbacks ding as you pass
each frame-perfect input. Macros save per level automatically — reload them
from the **Macros** button on the pause menu.

Turn off speedhack and noclip while analyzing.
