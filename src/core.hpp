#pragma once

#include "timings.hpp"

#include <Geode/Geode.hpp>
#include <string>
#include <vector>

// ===========================================================================
// What the macro/analysis core (macro.cpp) exposes to the modules
// (modules.cpp). Everything is on the LEVEL clock: 240 Hz frames from the
// level's real start, whatever start position or checkpoint you're on.
// ===========================================================================

namespace fw::core {

// one recorded input, on the level clock
struct BotInput {
    double t = 0.0;
    int button = 1;
    bool down = true;
    bool player1 = true;
    int mask = 0; // players it moved (bit0 = P1, bit1 = P2); 0 = unknown
};

// The macro system is doing nothing (no record / replay / analysis): the
// player is playing normally.
bool idle();

// This attempt's current step on the level clock (inside a physics step: that
// step's number, same convention as the windows' times).
double now();

// Every frame window this level has (all analyses incl. other start positions,
// plus imports), sorted by time.
std::vector<fw::Timing> const& levelTimings();

// The level's recorded inputs (this start position's macro, else the level's
// full macro), sorted by time. Empty = nothing recorded.
std::vector<BotInput> const& botInputs();

// Press / release exactly as replay does.
void applyBotInput(GJBaseGameLayer* layer, BotInput const& in);

// Level-clock time of the click you last died on (NaN = none this level).
double lastDiedOn();

// The level's key without start position, for per-level saved data.
std::string baseKey();

} // namespace fw::core
