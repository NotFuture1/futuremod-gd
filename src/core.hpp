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

// The macro system is doing nothing (no record / replay / analysis): the
// player is playing normally.
bool idle();

// The macro is replaying (K): the bot is playing the run.
bool replaying();

// This attempt's current step on the level clock (inside a physics step: that
// step's number, same convention as the windows' times).
double now();

// Every frame window this level has (all analyses incl. other start positions,
// plus imports), sorted by time.
std::vector<fw::Timing> const& levelTimings();

// Level-clock time of the click you last died on (NaN = none this level).
double lastDiedOn();

// The level's key without start position, for per-level saved data.
std::string baseKey();

} // namespace fw::core
