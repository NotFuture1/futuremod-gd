// Host tests for noclip-path averaging (no GD needed):
//   clang++ -std=c++20 -I src tests/paths_test.cpp -o /tmp/pt && /tmp/pt
#include "paths_logic.hpp"

#include <cstdio>
#include <cstdlib>

using namespace fw::paths;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) { g_pass++; }                                                 \
        else { g_fail++; std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
               std::printf(__VA_ARGS__); std::printf("\n"); }                   \
    } while (0)

// a run going right at 1.3 units/step with y = f(x)
template <class F>
static Run line(double x0, double x1, F f) {
    Run r;
    for (double x = x0; x <= x1; x += 1.3) r.push_back({ static_cast<float>(x), static_cast<float>(f(x)) });
    return r;
}

static void test_consistent_runs_bright_outlier_faint() {
    std::puts("5 runs together + 1 far off: the five are bright, the odd one fades");
    std::vector<Run> runs;
    for (int i = 0; i < 5; i++) runs.push_back(line(0, 600, [i](double) { return 100.0 + i * 0.8; }));
    runs.push_back(line(0, 600, [](double) { return 160.0; }));
    auto s = aggregate(runs);
    CHECK(std::fabs(s.centre[s.bin(300)] - 101.6f) < 1.0f, "centre %.2f", s.centre[s.bin(300)]);
    CHECK(weight(s, { 300, 101.6f }) > 0.95f, "middle %.2f", weight(s, { 300, 101.6f }));
    CHECK(weight(s, { 300, 100.0f }) > 0.8f, "a near run %.2f", weight(s, { 300, 100.0f }));
    CHECK(weight(s, { 300, 160.0f }) < 0.01f, "outlier %.4f", weight(s, { 300, 160.0f }));
}

static void test_spread_widens_the_band() {
    std::puts("where runs disagree, the bright band is wider than where they agree");
    std::vector<Run> runs;
    // agree before x=300, spread out by +-12 after
    for (int i = 0; i < 7; i++)
        runs.push_back(line(0, 600, [i](double x) { return x < 300 ? 100.0 : 100.0 + (i - 3) * 4.0; }));
    auto s = aggregate(runs);
    float tight = weight(s, { 150, 108.f });   // 8 off where everyone agrees
    float loose = weight(s, { 450, 108.f });   // 8 off where runs spread +-12
    CHECK(tight < 0.2f, "8 off in the consistent part: %.3f", tight);
    CHECK(loose > 0.6f, "8 off in the spread part: %.3f", loose);
}

static void test_one_wild_run_doesnt_move_the_middle() {
    std::puts("median: one wild run can't drag the average");
    std::vector<Run> runs;
    for (int i = 0; i < 4; i++) runs.push_back(line(0, 300, [](double) { return 200.0; }));
    runs.push_back(line(0, 300, [](double) { return 900.0; }));
    auto s = aggregate(runs);
    CHECK(std::fabs(s.centre[s.bin(150)] - 200.f) < 1e-3f, "centre %.2f", s.centre[s.bin(150)]);
}

static void test_partial_runs_from_practice() {
    std::puts("practice runs covering different parts still line up by x");
    std::vector<Run> runs;
    runs.push_back(line(0, 400, [](double x) { return 100.0 + x * 0.1; }));
    runs.push_back(line(200, 800, [](double x) { return 100.0 + x * 0.1; }));
    runs.push_back(line(500, 900, [](double x) { return 100.0 + x * 0.1; }));
    auto s = aggregate(runs);
    CHECK(s.count[s.bin(300)] == 2, "two runs at 300 (%d)", s.count[s.bin(300)]);
    CHECK(s.count[s.bin(100)] == 1, "one run at 100 (%d)", s.count[s.bin(100)]);
    CHECK(std::fabs(weight(s, { 100, 110.f }) - 0.6f) < 1e-6f, "alone -> neutral %.2f", weight(s, { 100, 110.f }));
    CHECK(weight(s, { 300, 130.f }) > 0.95f, "shared part, on the line %.2f", weight(s, { 300, 130.f }));
}

static void test_run_counts_once_per_slice() {
    std::puts("a run hovering in one place counts once there");
    std::vector<Run> runs;
    Run hover;
    for (int i = 0; i < 200; i++) hover.push_back({ 50.f, 300.f }); // stuck at x=50
    runs.push_back(hover);
    runs.push_back(line(0, 100, [](double) { return 100.0; }));
    runs.push_back(line(0, 100, [](double) { return 102.0; }));
    auto s = aggregate(runs);
    CHECK(s.count[s.bin(50)] == 3, "3 runs, not 202 (%d)", s.count[s.bin(50)]);
    CHECK(std::fabs(s.centre[s.bin(50)] - 102.f) < 1e-3f, "median of 100/102/300 = 102 (%.2f)", s.centre[s.bin(50)]);
}

static void test_thin_keeps_shape() {
    std::puts("thinning keeps both ends and the turns of a wave zigzag");
    Run r;
    for (int i = 0; i <= 400; i++) {
        double x = i * 1.3;
        double y = 100 + 30 * (1 - std::fabs(std::fmod(x, 60.0) / 30.0 - 1)); // zigzag, period 60
        r.push_back({ static_cast<float>(x), static_cast<float>(y) });
    }
    auto t = thin(r, 4.0);
    CHECK(t.size() < r.size() / 2, "thinned %zu -> %zu", r.size(), t.size());
    CHECK(t.front().x == r.front().x && t.back().x == r.back().x, "ends kept");
    int peaks = 0;
    for (size_t i = 1; i + 1 < t.size(); i++)
        if (t[i].y > t[i - 1].y && t[i].y > t[i + 1].y) peaks++;
    CHECK(peaks >= 7, "zigzag peaks kept: %d", peaks);
}

static void test_empty() {
    std::puts("no runs: nothing breaks");
    auto s = aggregate({});
    CHECK(s.count.empty(), "empty");
    CHECK(std::fabs(weight(s, { 10, 10 }) - 0.6f) < 1e-6f, "neutral");
}

int main() {
    test_consistent_runs_bright_outlier_faint();
    test_spread_widens_the_band();
    test_one_wild_run_doesnt_move_the_middle();
    test_partial_runs_from_practice();
    test_run_counts_once_per_slice();
    test_thin_keeps_shape();
    test_empty();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
