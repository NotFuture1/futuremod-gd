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
#include <utility>
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

// Is this point an outlier? Far (more than `z` spreads) from the middle of
// the runs there, where at least `minRuns` runs pass so the middle means
// something.
inline bool isOutlierPoint(Stats const& s, Pt p, double z = 3.0, double minSpread = 4.0, int minRuns = 3) {
    int b = s.bin(p.x);
    if (!s.has(b) || s.count[b] < minRuns) return false;
    double scale = std::max(static_cast<double>(s.spread[b]), minSpread);
    return std::fabs(p.y - s.centre[b]) > z * scale;
}

// Drop whole runs that are outliers for more than `maxFrac` of the stretch
// they share with the others (a run that took a different route, or played
// that part completely differently). One brief wobble doesn't drop a run;
// isOutlierPoint() hides those moments when drawing. Two passes, because
// removing a far-off run tightens the middle the others are judged by.
// Never drops below `keepAtLeast` runs.
inline std::vector<Run> removeOutlierRuns(std::vector<Run> runs, double binW = 3.0, double maxFrac = 0.3,
                                          double z = 3.0, size_t keepAtLeast = 2) {
    for (int pass = 0; pass < 2 && runs.size() > keepAtLeast; pass++) {
        Stats s = aggregate(runs, binW);
        std::vector<std::pair<double, size_t>> bad; // (outlier fraction, run index)
        for (size_t r = 0; r < runs.size(); r++) {
            int judged = 0, off = 0;
            for (auto const& p : runs[r]) {
                int b = s.bin(p.x);
                if (!s.has(b) || s.count[b] < 3) continue;
                judged++;
                if (isOutlierPoint(s, p, z)) off++;
            }
            double frac = judged > 0 ? static_cast<double>(off) / judged : 0.0;
            if (judged >= 20 && frac > maxFrac) bad.push_back({ frac, r });
        }
        if (bad.empty()) break;
        // worst first, but keep at least `keepAtLeast`
        std::sort(bad.begin(), bad.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        std::vector<char> drop(runs.size(), 0);
        size_t remaining = runs.size();
        for (auto const& b : bad) {
            if (remaining <= keepAtLeast) break;
            drop[b.second] = 1;
            remaining--;
        }
        std::vector<Run> kept;
        for (size_t r = 0; r < runs.size(); r++) if (!drop[r]) kept.push_back(std::move(runs[r]));
        runs = std::move(kept);
    }
    return runs;
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
        bool farEnough = dx * dx + dy * dy >= step * step; // (not "far": a macro on Windows)
        // turning point: the vertical direction changes here
        double d1 = r[i].y - r[i - 1].y, d2 = r[i + 1].y - r[i].y;
        bool turn = (d1 > 1e-4 && d2 < -1e-4) || (d1 < -1e-4 && d2 > 1e-4);
        if (farEnough || turn) out.push_back(r[i]);
    }
    out.push_back(r.back());
    return out;
}

} // namespace fw::paths
