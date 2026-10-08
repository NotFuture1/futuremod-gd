#pragma once

#include <array>
#include <string>
#include <vector>

// ===========================================================================
// NaNDL-compatible counting + comparison against NaN's published data.
//
// NaNDL's "Frame Windows" table counts, per level, how many inputs have 0..10
// whole 240 Hz frames available. A window of continuous width w (CBF) covers
// floor(w) or floor(w)+1 frame instants depending on alignment, with
// probability frac(w) of the larger -- so each input adds (1 - frac) to bin
// floor(w) and frac to the next bin. That's why NaNDL's counts are fractional.
// ===========================================================================

namespace fw::nandl {

using Hist = std::array<double, 11>; // bins 0..10

// measured (non-wide) windows in frames -> NaNDL histogram
Hist histogram(std::vector<double> const& windows);

// the single bin an input mostly falls in (for per-input displays)
int nearestBin(double w);

std::string fmtHist(Hist const& h);

// Fetch NaNDL's published histogram for `levelName` and show it next to ours
// (popup + [fw] NANDL log line). `quiet`: say nothing if the level isn't listed.
void compare(std::string levelName, Hist ours, bool quiet);

} // namespace fw::nandl
