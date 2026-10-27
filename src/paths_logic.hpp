#pragma once

// ===========================================================================
// Noclip paths: averaging several runs' paths and how "average" each point of
// each run is. No GD/Geode dependency, tested on its own
// (tests/paths_test.cpp).
//
// Runs are lined up by level x (where you are in the level, not when): for
// every slice of x, the middle of all runs that pass through it (median, so
// one wild run can't drag it) and how spread out they are there (MAD, scaled
// to match a standard deviation). A point's weight is 1 on the middle and
// fades with its distance in units of that spread: where you're consistent,
// small deviations already fade; where you're all over the place, the band
// of bright paths is wider.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <vector>

namespace fw::paths {

struct Pt {
    float x = 0.f, y = 0.f;
};
using Run = std::vector<Pt>;

struct Stats {
    double x0 = 0.0;            // left edge of slice 0
    double binW = 3.0;          // slice width (level units)
    std::vector<float> centre;  // median y of the runs in each slice
    std::vector<float> spread;  // robust spread (MAD * 1.4826) per slice
    std::vector<int> count;     // runs that pass through each slice

    int bin(double x) const { return static_cast<int>(std::floor((x - x0) / binW)); }
    bool has(int b) const { return b >= 0 && b < static_cast<int>(count.size()) && count[b] > 0; }
};

inline float medianOf(std::vector<float>& v) {
    if (v.empty()) return 0.f;
    size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + m, v.end());
    float hi = v[m];
    if (v.size() % 2) return hi;
    float lo = *std::max_element(v.begin(), v.begin() + m);
    return 0.5f * (lo + hi);
}

// Each run counts once per slice (its first point in it), so a run that
// lingers or goes back and forth can't outvote the others.
inline Stats aggregate(std::vector<Run> const& runs, double binW = 3.0) {
    Stats s;
    s.binW = binW;
    double lo = 1e300, hi = -1e300;
    for (auto const& r : runs)
        for (auto const& p : r) { lo = std::min(lo, static_cast<double>(p.x)); hi = std::max(hi, static_cast<double>(p.x)); }
    if (lo > hi) return s;
    s.x0 = lo;
    size_t nb = static_cast<size_t>(std::floor((hi - lo) / binW)) + 1;
    std::vector<std::vector<float>> ys(nb);
    for (auto const& r : runs) {
        std::vector<char> visited(nb, 0);
        for (auto const& p : r) {
            int b = s.bin(p.x);
            if (b < 0 || b >= static_cast<int>(nb) || visited[b]) continue;
            visited[b] = 1;
            ys[b].push_back(p.y);
        }
    }
    s.centre.assign(nb, 0.f);
    s.spread.assign(nb, 0.f);
    s.count.assign(nb, 0);
    for (size_t b = 0; b < nb; b++) {
        auto& v = ys[b];
        s.count[b] = static_cast<int>(v.size());
        if (v.empty()) continue;
        std::vector<float> c = v;
        float med = medianOf(c);
        std::vector<float> dev;
        dev.reserve(v.size());
        for (float y : v) dev.push_back(std::fabs(y - med));
        s.centre[b] = med;
        s.spread[b] = 1.4826f * medianOf(dev);
    }
    return s;
}

// How "average" a point is: 1 on the middle of the runs there, fading with
// distance in units of their spread (never narrower than `minSpread`, so
// identical runs don't make a 1-unit wobble disappear). A slice only one run
// passes through has nothing to compare with: neutral `alone`.
inline float weight(Stats const& s, Pt p, double minSpread = 4.0, float alone = 0.6f) {
    int b = s.bin(p.x);
    if (!s.has(b)) return alone;
    if (s.count[b] < 2) return alone;
    double scale = std::max(static_cast<double>(s.spread[b]), minSpread);
    double d = (p.y - s.centre[b]) / scale;
    return static_cast<float>(std::exp(-0.5 * d * d));
}

// Fewer points to draw: keep a point when it's at least `step` units from the
// last kept one (or where the path turns), always keep both ends.
inline Run thin(Run const& r, double step = 4.0) {
    if (r.size() <= 2) return r;
    Run out;
    out.push_back(r.front());
    for (size_t i = 1; i + 1 < r.size(); i++) {
        auto const& a = out.back();
        double dx = r[i].x - a.x, dy = r[i].y - a.y;
        bool far = dx * dx + dy * dy >= step * step;
        // turning point: the vertical direction changes here
        double d1 = r[i].y - r[i - 1].y, d2 = r[i + 1].y - r[i].y;
        bool turn = (d1 > 1e-4 && d2 < -1e-4) || (d1 < -1e-4 && d2 > 1e-4);
        if (far || turn) out.push_back(r[i]);
    }
    out.push_back(r.back());
    return out;
}

} // namespace fw::paths
