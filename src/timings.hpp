#pragma once

#include <Geode/Geode.hpp>
#include <filesystem>
#include <string>
#include <vector>

// ===========================================================================
// Per-click frame windows as plain data, independent of where they came from
// (our own analysis, or a file someone else made), plus the file formats the
// community uses for them:
//   - NaNDL calculator JSON (NaNDL site, our .nandl.json / .fwc.json exports)
//   - frame-window-counter's binary .fwc ("FWC2", legacy "FWCB")
// Times are always 240 Hz frames from the level start.
// ===========================================================================

namespace fw {

struct Timing {
    double t = 0.0;  // when the click happens, 240 Hz frames from the level start
    double w = 0.0;  // its window, frames
    bool p2 = false; // player 2's click
    int input = 0;   // 1-based input number (for display)
};

namespace timings {

// Reads a .json (NaNDL calculator) or .fwc file. Non-timings ("-", zero or
// negative windows) are skipped; the result is sorted by time.
geode::Result<std::vector<Timing>> readFile(std::filesystem::path const& path);

// NaNDL calculator JSON for `list` (what readFile reads back).
std::string toNandlJson(std::vector<Timing> const& list);

// frame-window-counter's binary .fwc (FWC2, 240 fps).
std::vector<uint8_t> toFwc(std::vector<Timing> const& list);

} // namespace timings
} // namespace fw
