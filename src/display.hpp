#pragma once

#include <Geode/Geode.hpp>
#include <vector>

using namespace geode::prelude;

// ===========================================================================
// Frame window display (macro playback): the same visual language as NaN GD's
// videos and the frame-window-counter mod. Every analyzed click/release gets a
// ring at the player the instant it fires, labelled with its window and
// coloured by it, plus a pitched sound and a per-window counter top-left.
// ===========================================================================

namespace fw::display {

// Playback started. `windows` = every measured (non-wide) window in the macro,
// so the counter can show "hit / total" per window.
void start(PlayLayer* pl, std::vector<double> const& windows);
// The level restarted during playback: clear rings, zero the counter.
void restart(PlayLayer* pl);
// Playback stopped: remove rings and the counter.
void stop(PlayLayer* pl);
// A measured input just fired on `player` (mid-step for CBF inputs, so the
// player is exactly where the click happened).
void onInput(PlayLayer* pl, PlayerObject* player, double window);
// Once per frame after the camera has moved: keep the rings pinned to the level.
void tick(PlayLayer* pl);

} // namespace fw::display
