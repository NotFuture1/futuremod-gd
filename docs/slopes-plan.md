# Slopes converter: plan

Turn a level's spike-based wave parts into **slope corridors** that trace
the path, like the community's "… but slopes" levels. Status: **plan only**,
nothing built yet.

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

**"Shadow" levels:** I couldn't confirm what you mean. On GD's servers,
"shadow wave" levels (e.g. 73968038) are just nerfed Sonic Wave copies. My
guess is the blackout style in the second row. **Send me the ID of one you
mean** and I'll take it apart the same way.

## 2. What we have that they didn't

The mod already records **the exact path**, the player's position every
1/240 s, and can **replay it** in a level. So the converter doesn't have to
guess the route: it builds the corridor around the real one. It can also
**check its own output**: replay your macro through the generated level, and
if it gets through untouched, the walls are in the right place.

It can also read and write level data (the LDM swap already does), and find
out at runtime which objects are hazards, without a hand-kept ID list.

## 3. What you get

Pause menu > **Slopes**, on a level you've recorded (the macro supplies the
path):

1. Pick a style:
   - **Slope tunnel:** the wave parts' spikes are replaced with a slope
     corridor along the path. Playable: the wave slides along the walls,
     so it's the forgiving "but slopes" version.
   - **Guide overlay:** the level stays as it is; passable slopes and a
     path line show the route on top of the spikes.
   - **Shadow** (once we've pinned down what it means): decoration blacked
     out, with only the corridor/path visible.
2. Corridor height (tight / normal / wide), and wave parts only vs. every
   game mode (later phase).
3. **Build:** it plays immediately, as an in-memory copy like the LDM swap,
   and checks itself by replaying your macro through it (section 6).

## 4. The conversion, step by step

1. **Find the wave parts.** Record the game mode with the path every step
   (wave, mini, gravity, dual) and take the stretches where player 1 is a
   wave. Today's recording stores only x/y, so this is a small addition.
2. **Simplify the path** into straight pieces. A wave only moves three
   ways: up, down, or sliding flat along a floor or ceiling. That gives a
   list of corner points.
   - Normal wave: **45°**. Mini wave: about **63°** (2 up per 1 across).
   - I believe the angle doesn't change with speed portals. To be confirmed
     by measuring a recorded path.
3. **Build the walls.**
   - Offset the path up and down by half the corridor height.
   - Tile each wall line with slopes: 1×1 at 45°, 2:1 turned upright at
     63°, and flat blocks for sliding stretches. Pieces go at exact
     positions, not snapped to the grid, with the last piece of a run
     scaled to fit.
   - Corners get the corner piece the community uses: the four-slope
     diamond.
4. **Fill behind the walls** so the corridor reads clearly (Silent Slopes
   style): one stretched block per column, using 2.2's separate X/Y scale,
   to keep the object count low.
5. **Remove the old hazards** in those stretches: spikes, saws, hazard
   hitboxes. Identified by asking GD for each object's type, not by ID.
   **Kept:** portals, speed changes, orbs, pads and **every trigger**.
   Moving spikes (move triggers) get flagged rather than guessed at.
6. **Assemble the new level:** the original data with those objects removed
   and the new ones appended, built the same way as the LDM swap's copy.

The geometry is the core: it takes a path and settings and returns a list of
objects to place. It gets written GD-free, like the window extender, with a
test suite that also **draws each result to an image**, so the corridors
can be checked by eye before anything runs in GD.

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

## 7. Phases

1. **v1, normal and mini wave.** Slope tunnel and guide overlay, the
   self-check, in-memory play.
2. **v2, the awkward cases.** Dual (a second corridor, merged where they
   overlap), gravity and size portals mid-section, teleports, reverse
   gameplay. Plus saving as an editable level where allowed.
3. **v3, other game modes.** Ship, UFO and swing paths are curves, so
   approximate them with the three slope angles plus flat pieces. That
   gives "but slopes" versions of non-wave parts too.
4. **v4, corridors from the frame windows.** Instead of a fixed height,
   shape each corridor from how far each click could actually be off (our
   analysis). The walls then show the real room you have: tight where the
   timing is tight.

## 8. Risks / unknowns

- **How the wave behaves on slope corners** in 2.2: whether hitting the
  inside of a peak slides or kills. To be measured in-game in v1 with a
  small test level before tuning the corner pieces.
- **Collision on scaled slopes:** I believe it follows the scale, since the
  hand-built tunnel uses scale 1.25. Also to be confirmed in v1.
- **Wave parts built from moving objects or teleports:** removing hazards
  there could break the level. v1 skips those sections and says so.

## 9. Questions for you

1. **"Shadow":** a level ID of one you mean.
2. **Tunnel:** should it be solid (the wave slides on it, so it's easier),
   or only a visual guide over the original spikes? Or both as options, as
   planned above.
3. **Default corridor:** tight, about the wave's own size, or the
   community's roughly 2-block tunnel?
4. **Scope for v1:** wave only, or should ship be in v1 as well?
