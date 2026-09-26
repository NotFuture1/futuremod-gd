# Testing the frame window counter (v1.2.0)

Do the tests in order. Each one tells you what to look for, what counts as a
pass, and what to send back. Everything the mod reports goes to `geode.log`
with a `[fw]` prefix, so after each test you can search the log for the lines
named in that test.

**Where things are**
- `geode.log`: in your GD folder under `geode/logs/`. Take the newest file.
- Macros and exports: in GD, open the pause menu and press **Files** (top
  left, next to **Macros**). That opens `…\GeometryDash\geode\mods\future.futuremod\macros\`.
  Files are named by level ID: `<id>.txt` (macro), `<id>.track` (your recorded
  path), `<id>.windows.csv`, `<id>.nandl.json`, `<id>.fwc.json`.

---

## Step 0: Install and set up (once)

1. Download `future.futuremod.geode` from the **v1.2.0** GitHub release (or
   from the latest Actions run's `futuremod-geode` artifact).
2. Put it in GD's `geode/mods` folder, replacing the old one. Start GD.
3. **Geode → Future Mod** should say **v1.2.0**. Open its settings and check:
   - Enable Macro Features: **on**
   - Analyze releases too: **on** (the new default)
   - Max window: **10**, Sub-frame precision: **3**
   - Analyzer speed-up: **4**, Fast analysis: **off** (you'll turn it on in Step 6)
   - Leave the two diagnostics (Ghost fidelity, Timestamp probe) **off**. They
     aren't needed any more.
4. **Click Between Frames** settings:
   - Disable CBF: **off** (so CBF is active)
   - Click on Steps: **off**
   - Enable Physics Bypass: **off** ← required. The analyzer refuses to run
     with this on.
5. **Mega Hack**: turn noclip, speedhack and any other physics cheats off.

---

## Step 1: Record a short CBF run

A short run keeps the first analysis quick. You don't need to finish the level.

1. Open **Stereo Madness** in normal mode (not practice).
2. Press **J**. You should see **"Macro: recording (CBF sub-frame timing on)"**.
   If it just says "Macro: recording", the mod doesn't see CBF as active; go
   back to Step 0.4.
3. Play about **15 seconds**, through at least one ship part if you can. If you
   die, recording restarts by itself, so just keep playing.
4. Press **J** again. You should see **"Macro: recorded + saved N inputs"**.
   (If you complete a level while recording, it saves automatically.)

**Check the log** for:
```
[fw] ENV record cbf=1 active=1 physicsBypass=0 ...
[fw] RECORDED inputs=N midStep=M lastStep=...
```
- ✅ **Pass:** `midStep` is close to `inputs` (nearly every click was captured
  mid-frame).
- ❌ **Fail:** `midStep=0` with CBF on. Send the log.

---

## Step 2: Replay it (does the replay match your run exactly?)

1. Still in the level, press **K**. The macro plays from the start.
2. Let it run past where you pressed J, then press **K** again to stop it (or
   let it die after the recorded part ends).

**Check the log** for:
```
[fw] PLAYBACK end=... step=... maxDrift=...u @step ... VERDICT=...
```
- ✅ **Pass:** `VERDICT=EXACT` (drift under 0.01 units). `CLOSE` (under 1 unit)
  is usable but tell me.
- ❌ **Fail:** `VERDICT=DRIFT`, or the replay dies inside the part you recorded.
  Send the log and tell me what gamemode you were in around the `@step` value
  (240 steps = 1 second).

**Bonus check:** press K again and click your mouse a few times while it
replays. Your clicks must be ignored: same verdict, same run.

---

## Step 3: Analyze it (the actual counter)

1. Press **N**. Then **hands off** the keyboard and mouse until it's done.
2. The HUD (top right) shows `FW x/y`, the FP@240 count, and `min:` (the
   tightest window so far). You'll see the level restart over and over. That's
   normal: each restart is one test.
3. Wait for **"Analysis done: … timings …"**. A 15-second run takes roughly
   5–20 minutes.

**Check the log** for:
```
[fw] ANALYZE start inputs=... targets=... midStep=... maxWindow=10 subframeSteps=3 ...
[fw] BASELINE OK reached step ... driftVsRecording=...u @step ... VERDICT=EXACT
[fw] WIN #12 press p1 step=... frac=0.412 window=2.375 lo=-1.125 hi=+1.250 status=exact tap=1 sub=1
...
[fw] SUMMARY inputs=... timings=... wide=... capped=... timeout=... subframe=... probes=... | <=1:.. <=2:.. ...
[fw] SUMMARY FP@240=... FP@120=... FP@60=... tightest=...
[fw] EXPORT dir='...' key=... csvRows=... nandlRows=... fwcRows=...
```
- ✅ **Pass:**
  - `BASELINE OK` with `VERDICT=EXACT`
  - `WIN` lines with fractional windows (`sub=1`) for tight inputs
  - `timeout=0`
  - the export files exist (pause menu → **Files**)
- ❌ **Fail:**
  - **"Can't analyze: …"** → send the log. The line `BASELINE FAIL` says
    where it broke.
  - `BASELINE OK … VERDICT=DRIFT` → the analyzer measured a slightly different
    run from the one you played. Send the log.
  - Any `timeout` > 0 → send the log.

**Sanity check the numbers:** open `<id>.windows.csv`. Most inputs should be
`wide` (not timings). The inputs you *know* were tight (a quick ship
correction, an orb click right before a spike) should have the smallest
windows. If an obviously easy click shows up as frame-perfect, note its
`input` number and time (`time_s`) and send it to me.

---

## Step 4: Check the exports import

1. Go to <https://nandl.pages.dev> → calculator → **Import JSON** → pick
   `<id>.nandl.json`. It should load with no error. Game FPS 240, frame
   numbers as the time unit, and wide inputs shown as `-`.
2. (Only if you use it) In the frame-window-counter mod, import `<id>.fwc.json`.
   It should show only the timings.

✅ Pass = both import. ❌ Tell me the error message.

Pressing **Files** in the pause menu rebuilds that level's export files from
its saved analysis before opening the folder. So if an export looks wrong
after a mod update, press **Files**. You don't have to analyze again.

---

## Step 5: Run it twice (determinism)

Press **N** again on the same macro and let it finish. Compare the two
`[fw] SUMMARY` lines in the log.

- ✅ **Pass:** identical.
- ❌ **Fail:** anything different. Send the log with both runs in it.

---

## Step 6: Fast analysis (the speed-up you'll want for long levels)

1. Settings → **Fast analysis: on**.
2. Press **N** on the same macro and let it finish.
3. Compare its `SUMMARY` with the one from Step 3/5. Also look for:
   ```
   [fw] fast mode: X probes from a save-state, Y full replays
   ```
- ✅ **Pass:** same `SUMMARY` as before, it finished much faster, and X is
  much bigger than Y.
- ❌ **Fail:** a different SUMMARY means fast mode isn't safe yet. Keep it off
  and send the log. Lots of `anchor ... failed verification` lines are fine:
  they just mean it fell back to the slow, safe path.

---

## Step 7: Without CBF (make sure the normal path still works)

1. CBF settings → **Disable CBF: on**.
2. On the same level: **J** → play ~15 s → **J**, then **K**, then **N**.
- ✅ **Pass:**
  - `RECORDED … midStep=0`
  - `PLAYBACK … VERDICT=EXACT`
  - `BASELINE OK`
  - every `WIN` line has `sub=0` with whole-number windows (`1.000`, `2.000`, …)
- Turn CBF back on afterwards (Disable CBF: off).

---

## Step 8: Harder gamemodes (with CBF on)

Repeat **Steps 1 → 2** (record + replay only) on each of these. They exercise
the parts of the CBF replay that Stereo Madness doesn't:

| Level | What it tests |
| --- | --- |
| **Hexagon Force** (the dual part) | both icons from one click (dual mode) |
| **Blast Processing** | wave (mostly release timing) |
| a level with **orbs + pads** (e.g. Polargeist) | orb buffering |
| any level with **slopes** | the slope collision path |

For each, report the `PLAYBACK … VERDICT` line. Once those replay EXACT, run
**N** on one of them too.

---

## What to send me

For every test that didn't pass, send:
1. the `geode.log` (the whole file is fine), and
2. which step, which level, and roughly where in the level.

If everything passes, the easiest thing to send is just the lines starting
with `[fw]` from Steps 3, 6 and 8.

---

## Quick reference: keys and settings

| Key | Does |
| --- | --- |
| **J** | start / stop recording (auto-saves on level complete or quit) |
| **K** | start / stop playback |
| **N** | start / cancel analysis |

| Setting | Default | Notes |
| --- | --- | --- |
| Max window (frames) | 10 | Windows wider than this are "wide" (not a timing). Lower = faster. |
| Sub-frame precision (CBF) | 3 | 3 = 1/8 frame. 0 = whole frames only. No effect on non-CBF runs. |
| Analyze releases too | on | Turn off for cube-only levels to halve the time. |
| Analyzer speed-up | 4 | Auto-drops to a slower speed if the sped-up replay isn't identical. |
| Fast analysis | off | Turn on once Step 6 passes. |
