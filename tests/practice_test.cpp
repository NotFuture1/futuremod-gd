// Host tests for the click-practice engine (no GD needed):
//   clang++ -std=c++20 -I src tests/practice_test.cpp -o /tmp/pt && /tmp/pt
#include "practice_logic.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

using namespace fw::practice;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) { g_pass++; }                                                 \
        else { g_fail++; std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
               std::printf(__VA_ARGS__); std::printf("\n"); }                   \
    } while (0)

// A tiny stand-in for the game: which buttons each player holds, what the
// bot did, and what the player managed to do.
struct Sim {
    Engine e;
    std::vector<Input> in;
    std::map<std::pair<int, int>, bool> held; // (button, player 1/2) -> held
    std::vector<size_t> botApplied;          // every bot input applied (index)
    std::vector<double> botTimes;
    int releasesSynced = 0;

    bool holds(int button, int mask) {
        for (int p = 1; p <= 2; p++)
            if ((mask & p) && held[{ button, p }]) return true;
        return false;
    }
    void set(int button, int mask, bool down) {
        for (int p = 1; p <= 2; p++)
            if (mask & p) held[{ button, p }] = down;
    }
    // one physics step; `player` = real inputs the player makes this step
    void step(double now, std::vector<Input> const& player = {}) {
        auto out = e.step(now, [&](int b, int m) { return holds(b, m); });
        for (size_t i : out.apply) {
            botApplied.push_back(i);
            botTimes.push_back(now);
            set(e.input(i).button, e.input(i).players(), e.input(i).down);
        }
        for (auto const& r : out.release) { set(r.button, r.mask, false); releasesSynced++; }
        for (auto const& p : player)
            if (e.allowPlayer(now, p.button, p.player1)) set(p.button, p.players(), p.down);
    }
    void respawn() { held.clear(); e.reset(); } // GD releases everything on reset
    bool isHeld(int button, int player) { return held[{ button, player }]; }
    bool appliedIdx(size_t i) const {
        for (size_t a : botApplied) if (a == i) return true;
        return false;
    }
};

// press/release pairs: press at every `period` from `first`, held `hold` frames
static std::vector<Input> taps(double first, double last, double period, double hold, bool p1 = true, int mask = 0) {
    std::vector<Input> v;
    for (double t = first; t <= last; t += period) {
        v.push_back({ t, 1, true, p1, mask });
        v.push_back({ t + hold, 1, false, p1, mask });
    }
    return v;
}

static size_t idxAt(std::vector<Input> const& in, double t, bool down) {
    for (size_t i = 0; i < in.size(); i++) if (std::fabs(in[i].t - t) < 1e-6 && in[i].down == down) return i;
    std::printf("no input at %.2f\n", t);
    std::exit(2);
}

static void test_bot_replays_after_death() {
    std::puts("respawn: the bot plays the early clicks again (the reported 'inputs turned off' bug)");
    Sim s;
    s.in = taps(100, 1000, 40, 10);
    s.e.setInputs(s.in, 0);
    s.in = s.e.inputs();
    s.e.setSelection({ 500 }, 30);
    CHECK(s.e.active(), "practice should be active");
    for (int t = 1; t <= 700; t++) s.step(t); // attempt 1, dies at 700
    size_t firstAttempt = s.botApplied.size();
    CHECK(firstAttempt > 20, "bot applied %zu inputs in attempt 1", firstAttempt);
    s.respawn();
    s.botApplied.clear();
    for (int t = 1; t <= 300; t++) s.step(t); // attempt 2
    CHECK(s.appliedIdx(idxAt(s.in, 100, true)), "press at 100 replayed on attempt 2");
    CHECK(s.appliedIdx(idxAt(s.in, 270, false)), "release at 270 replayed on attempt 2");
    CHECK(s.botApplied.size() >= 10, "attempt 2 bot inputs: %zu", s.botApplied.size());
}

static void test_window_clamped_by_neighbours() {
    std::puts("window never swallows a neighbouring input of the same button");
    Sim s;
    s.in = { { 400, 1, true }, { 410, 1, false }, { 420, 1, true }, { 430, 1, false }, { 440, 1, true }, { 450, 1, false } };
    s.e.setInputs(s.in, 0);
    s.in = s.e.inputs();
    s.e.setSelection({ 420 }, 30);
    CHECK(s.e.matched() == 1, "matched %zu", s.e.matched());
    auto const& w = s.e.windows()[0];
    CHECK(std::fabs(w.from - 410.5) < 1e-9, "from %.2f", w.from);
    CHECK(std::fabs(w.to - 439.5) < 1e-9, "to %.2f", w.to);
    for (int t = 1; t <= 460; t++) s.step(t);
    CHECK(s.appliedIdx(1), "bot played the release at 410 (just before the window)");
    CHECK(s.appliedIdx(4), "bot played the press at 440 (just after the window)");
    CHECK(!s.appliedIdx(2) && !s.appliedIdx(3), "bot never plays the picked press/release");
    CHECK(!s.e.allowPlayer(405, 1, true), "405: before the window -> blocked");
    CHECK(s.e.allowPlayer(415, 1, true), "415: inside -> allowed");
    CHECK(!s.e.allowPlayer(445, 1, true), "445: after -> blocked");
}

static void test_outside_macro_coverage() {
    std::puts("before the macro's start and after its end the player plays freely");
    Engine e;
    e.setInputs(taps(600, 1000, 40, 10), 500); // macro recorded from a start pos at 500
    e.setSelection({ 800 }, 30);
    CHECK(e.allowPlayer(100, 1, true), "100: before the macro starts");
    CHECK(!e.allowPlayer(700, 1, true), "700: bot's stretch");
    CHECK(e.allowPlayer(1200, 1, true), "1200: after the macro ends");
}

static void test_unmatched_selection_is_inert() {
    std::puts("picks that don't match the macro: practice stays off, nothing is blocked");
    Sim s;
    s.in = taps(100, 1000, 40, 10);
    s.e.setInputs(s.in, 0);
    s.in = s.e.inputs();
    s.e.setSelection({ 333, 777 }, 30);
    CHECK(s.e.matched() == 0, "matched %zu", s.e.matched());
    CHECK(!s.e.active(), "inactive");
    CHECK(s.e.allowPlayer(500, 1, true), "player allowed");
    for (int t = 1; t <= 300; t++) s.step(t);
    CHECK(s.botApplied.empty(), "bot must not play when inactive (%zu)", s.botApplied.size());
}

static void test_seek_restores_hold() {
    std::puts("respawning mid-hold (checkpoint/start pos): the bot re-presses the held button");
    Sim s;
    s.in = { { 100, 1, true }, { 600, 1, false }, { 700, 1, true }, { 710, 1, false } };
    s.e.setInputs(s.in, 0);
    s.in = s.e.inputs();
    s.e.setSelection({ 700 }, 30);
    s.respawn();
    s.step(300); // respawn lands at 300, in the middle of the 100..600 hold
    CHECK(s.appliedIdx(0), "press at 100 re-applied on seek");
    CHECK(s.isHeld(1, 1), "button held after seek");
    for (int t = 301; t <= 620; t++) s.step(t);
    CHECK(!s.isHeld(1, 1), "released at 600 by the bot");
}

static void test_seek_inside_window_does_not_press_for_you() {
    std::puts("respawning inside a picked click's window: the bot doesn't press it for you");
    Sim s;
    s.in = { { 100, 1, true }, { 110, 1, false }, { 500, 1, true }, { 560, 1, false } };
    s.e.setInputs(s.in, 0);
    s.in = s.e.inputs();
    s.e.setSelection({ 500 }, 30);
    s.respawn();
    s.step(520); // the picked press (500) is in the past, its release (560) isn't
    CHECK(!s.appliedIdx(2), "picked press not applied by seek");
    CHECK(!s.isHeld(1, 1), "not held");
}

static void test_release_sync_each_attempt() {
    std::puts("holding past your window: let go once per attempt, every attempt");
    Sim s;
    s.in = { { 100, 1, true }, { 110, 1, false }, { 300, 1, true }, { 310, 1, false }, { 500, 1, true }, { 510, 1, false } };
    s.e.setInputs(s.in, 0);
    s.in = s.e.inputs();
    s.e.setSelection({ 300 }, 30);
    for (int attempt = 0; attempt < 3; attempt++) {
        s.respawn();
        int before = s.releasesSynced;
        for (int t = 1; t <= 400; t++) {
            std::vector<Input> p;
            if (t == 300) p.push_back({ 300, 1, true }); // press on time, never release
            s.step(t, p);
        }
        CHECK(s.releasesSynced == before + 1, "attempt %d: synced %d", attempt, s.releasesSynced - before);
        CHECK(!s.isHeld(1, 1), "attempt %d: released", attempt);
    }
}

static void test_player_input_in_window_counts() {
    std::puts("inside the window your press/release go through; outside they don't");
    Sim s;
    s.in = { { 100, 1, true }, { 110, 1, false }, { 300, 1, true }, { 310, 1, false }, { 500, 1, true }, { 510, 1, false } };
    s.e.setInputs(s.in, 0);
    s.in = s.e.inputs();
    s.e.setSelection({ 300 }, 30);
    s.respawn();
    for (int t = 1; t <= 520; t++) {
        std::vector<Input> p;
        if (t == 200) p.push_back({ 200, 1, true });   // random click on the bot's stretch
        if (t == 201) p.push_back({ 201, 1, false });
        if (t == 302) p.push_back({ 302, 1, true });   // the picked click, 2 frames late
        if (t == 315) p.push_back({ 315, 1, false });
        s.step(t, p);
        if (t == 200) CHECK(!s.isHeld(1, 1), "click at 200 ignored");
        if (t == 302) CHECK(s.isHeld(1, 1), "picked click at 302 went through");
        if (t == 315) CHECK(!s.isHeld(1, 1), "release at 315 went through");
    }
    CHECK(s.appliedIdx(4), "bot still played 500");
}

static void test_two_player() {
    std::puts("2-player: picking a P1 click doesn't give you P2 there");
    Sim s;
    s.in = { { 300, 1, true, true }, { 310, 1, false, true }, { 305, 1, true, false }, { 315, 1, false, false } };
    s.e.setInputs(s.in, 0);
    s.in = s.e.inputs();
    s.e.setSelection({ 300 }, 30);
    CHECK(s.e.allowPlayer(300, 1, true), "P1 allowed in its window");
    CHECK(!s.e.allowPlayer(305, 1, false), "P2 still driven by the bot");
    for (int t = 1; t <= 320; t++) s.step(t);
    CHECK(s.appliedIdx(idxAt(s.in, 305, true)), "bot played P2's press");
}

static void test_dual_mask() {
    std::puts("dual (one button moves both icons): releasing syncs both");
    Sim s;
    s.in = { { 100, 1, true, true, 3 }, { 110, 1, false, true, 3 }, { 300, 1, true, true, 3 }, { 310, 1, false, true, 3 } };
    s.e.setInputs(s.in, 0);
    s.in = s.e.inputs();
    s.e.setSelection({ 300 }, 30);
    s.respawn();
    for (int t = 1; t <= 360; t++) {
        std::vector<Input> p;
        if (t == 300) p.push_back({ 300, 1, true, true, 3 });
        s.step(t, p);
    }
    CHECK(!s.isHeld(1, 1) && !s.isHeld(1, 2), "both icons released after the window");
}

static void test_many_attempts_random() {
    std::puts("fuzz: 300 random attempts never leave the bot silent on its own stretch");
    Sim s;
    s.in = taps(50, 3000, 23, 7);
    s.e.setInputs(s.in, 0);
    s.in = s.e.inputs();
    s.e.setSelection({ 50 + 23 * 10, 50 + 23 * 40, 50 + 23 * 90 }, 20);
    std::srand(7);
    for (int a = 0; a < 300; a++) {
        int from = (std::rand() % 4 == 0) ? std::rand() % 2500 : 1; // sometimes a mid-level respawn
        int to = from + 50 + std::rand() % 500;
        s.respawn();
        s.botApplied.clear();
        for (int t = from; t <= to; t++) s.step(t);
        // every bot-owned input inside [from+1, to] must have been applied
        for (size_t i = 0; i < s.in.size(); i++) {
            double t = s.in[i].t;
            if (t < from + 1 || t > to) continue;
            bool owned = false;
            for (auto const& w : s.e.windows())
                if (i == w.press || (i == w.press + 1)) owned = true;
            if (owned) continue;
            if (!s.appliedIdx(i)) {
                CHECK(false, "attempt %d [%d..%d]: input %zu at %.0f not played", a, from, to, i, t);
                return;
            }
        }
    }
    CHECK(true, "fuzz ok");
}

int main() {
    test_bot_replays_after_death();
    test_window_clamped_by_neighbours();
    test_outside_macro_coverage();
    test_unmatched_selection_is_inert();
    test_seek_restores_hold();
    test_seek_inside_window_does_not_press_for_you();
    test_release_sync_each_attempt();
    test_player_input_in_window_counts();
    test_two_player();
    test_dual_mask();
    test_many_attempts_random();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
