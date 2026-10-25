# Slopes converter: plan

Turn a level's spike-based wave parts into **solid black slope corridors**
that follow the gap between the spikes, like the community's "… but
slopes" levels. Status: **plan only** (decisions in section 9), nothing
built yet.

---

## 1. What the existing levels actually do

I downloaded four levels from GD's server and took them apart object by
object: the original Sonic Wave (26681070) and three slope-style levels.
They use three different techniques.

| Level | How it's built |
|---|---|
| **SONIC WAVE SLOPES** (104911684, 228 objects) | **Slope tunnel.** The wave part rebuilt from nothing but slope blocks (ID 1743). They sit on the 30-unit grid in diagonal staircases (+30 x, ±30 y per piece): a floor slope (rot 0) and a ceiling slope (rot −90) run in parallel, usually about 2 blocks apart. That makes a 45° tunnel exactly along the wave's path. Where the wave turns, four slopes (rot 0/90/180/270, scale 1.25) form a **diamond**, standing in for the spike tips. |
| **Sonic Wave Slopes** (129230861, a full copy of the original plus 2,016 objects) | **Traced path + blackout.** The original level kept, and one section (x 12,000–15,000) changed: about 600 tiny deco objects (ID 917) placed 7.5 units apart **trace the wave's exact route as a dotted line**, and 1,020 black squares (ID 211) black out everything else. Only 162 original objects were removed. |
| **Silent Slopes** (113200656) | **Filled corridor.** Walls built from solid blocks (ID 5), with 2:1 slopes (ID 1744) only on the diagonal edges. |

All three are made by hand, with the creator eyeballing the path.

**The look we're going for** (your answer): like Sonic Wave, but each wave
part's decoration and spikes are replaced by **black slopes**. That combines
the slope tunnel (row 1) with the blackout (row 2).

## 2. What we have that they didn't

The mod already records **the exact path**, the player's position every
1/240 s, and can **replay it** in a level. So the converter doesn't have to
guess the route: it builds the corridor around the real one. It can also
**check its own output**: replay your macro through the generated level, and
if it gets through untouched, the walls are in the right place.

It can also read and write level data (the LDM swap already does), and find
out at runtime which objects are hazards, without a hand-kept ID list.

## 3. What you get (decided)

Pause menu > **Slopes**, on a level you've recorded:

- **Wave parts only, always.** Every other part of the level is left exactly
  as it is.
- In each wave part, **all decoration and spikes are gone**. The corridor is
  rebuilt as **solid black slopes** that follow **the real gap between the
  spikes**, with black filling everything outside it. Like Sonic Wave, but
  the wave parts are black slope silhouettes on the level's own background.
- **Solid:** the wave slides along the walls, as in the "but slopes"
  levels. Nothing see-through.
- It plays straight away (an in-memory copy, like the LDM swap) and checks
  itself (section 6).

## 4. The conversion, step by step

The walls come from **the level's spikes**, not from a fixed distance
around your path. Your recorded path is only used for three things: where
the wave parts are, which side of each spike row is the inside of the
corridor, and the final check.

1. **Find the wave parts.** Record the game mode every step (wave, mini,
   gravity, dual) and take the stretches where player 1 is a wave. Today's
   recording stores only x/y, so this is a small addition. The converter
   then works only on what lies within those x ranges.
2. **Collect what's in the way:** every hazard and solid block in those
   stretches, as its **real hitbox**. GD gives each object's box at runtime,
   rotated ones included, so spike sizes and rotations don't need a
   hand-made table. Hitboxes, not the drawn spike, because your path may
   graze a spike's drawn tip without dying. Walls on the drawn tips could
   block it.
3. **Measure the gap.** At every point of your path, look straight down to
   the nearest hitbox (the floor) and straight up (the ceiling). That gives
   the gap the level actually leaves, along the whole stretch.
4. **Turn each edge into a slope line.**
   - A spike row's edge is jagged (a narrow hitbox per spike), so the floor
     edge becomes the line **connecting the spike tips facing the
     corridor**. Same for the ceiling.
   - That line is snapped to the angles GD slopes come in: 45° (1×1), the
     2:1 slope lying flat or standing up (about 27° or 63°, the 63° being
     mini wave's angle), and flat.
   - Two rules for every point of your path: the wall **never crosses into
     the gap**, and it keeps at least the wave's own half-size of clearance
     from your path. The result is the spike corridor's own shape, in
     slopes.
5. **Build:**
   - The slopes are placed at exact positions (not grid-snapped) and scaled
     to fit each run, with diamond pieces where two runs meet at a corner.
   - Everything outside the corridor gets black fill: one stretched block
     per column, to keep the object count low.
   - All of it uses GD's built-in **black** colour (special colour 1010),
     so colour triggers can't tint it.
6. **Clear out the wave parts:** remove spikes, saws, solid blocks and
   decoration there. GD is asked what each object is (no ID list).
   - **Kept:** portals, speed changes, orbs, pads and every trigger.
   - **Skipped and reported:** anything moved by a trigger (moving spikes,
     platforms). Converting those would break the level.
7. **Assemble the level:** the original data with those objects removed and
   the new ones appended, built like the LDM swap's copy.

The geometry is the core: path samples plus hitboxes go in, the list of
slopes and fill comes out. It gets written GD-free, like the window
extender, with tests that also **draw each corridor to an image**: the
original hitboxes, your path, and the new slopes on top of each other.

## 5. Copy protection

Building and playing the slopes version in memory is fine, the same as the
LDM swap. **Saving it as an editable level** is only offered when the
original is free to copy, or is your own level. Otherwise the converter
would be a way around the creator's copy lock.

## 6. Checking it worked

- **Self-check after every build:** replay the macro through the generated
  level. It should match your original run exactly and never touch a wall.
  If it does, the corridor is too tight there, and it says where.
- **Images of the generated corridors** (host tests) for a few recorded
  paths: Sonic Wave, plus a mini-wave part and a level with speed changes.
- **Compare with 104911684:** run our converter on Sonic Wave and put the
  result next to that hand-built tunnel.

## 7. Phases (wave only, always)

1. **v1:**
   - normal and mini wave, gravity flips, speed changes;
   - recording the game mode with the path;
   - the gap measurement, slope fitting and black fill;
   - the replay self-check, and in-memory play.
2. **v2:**
   - dual wave (two corridors, merged where they overlap);
   - wave parts with teleport portals;
   - saving as an editable level where allowed (free copy or your own).
3. **v3:** wave parts that include moving spikes or platforms: convert
   them where the movement can be followed, or leave just those objects in
   place.

## 8. Risks / unknowns

- **How the wave behaves on slope corners** in 2.2: whether hitting the
  inside of a peak slides or kills. To be measured in-game in v1 with a
  small test level before tuning the corner pieces.
- **Collision on scaled slopes:** I believe it follows the scale, since the
  hand-built tunnel uses scale 1.25. Also to be confirmed in v1.
- **Wave parts built from moving objects or teleports:** removing hazards
  there could break the level. v1 skips those objects and says so.
- **Spikes that slant off-grid:** the tip-line approach assumes that each
  spike's hitbox corner facing the corridor marks the edge. Rotated spikes
  give a box at an angle, and the corner nearest the path is used. Checked
  on the images in the tests.

## 9. Decided (2026-10-24)

1. **Look:** like Sonic Wave, but each wave part's decoration and spikes
   become **black slopes**.
2. **Solid** slopes. Nothing see-through.
3. **The corridor is the real gap between the spikes**, not a chosen
   height.
4. **Wave parts only, always.**
