// Host tests for the window extender engine (no GD needed):
//   clang++ -std=c++20 -I src tests/assist_test.cpp -o /tmp/at && /tmp/at
#include "assist_logic.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

using namespace fw::assist;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) { g_pass++; }                                                 \
        else { g_fail++; std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
               std::printf(__VA_ARGS__); std::printf("\n"); }                   \
    } while (0)

// The player: "I click (or let go) when the game shows step g".
struct Click {
    double g;
    bool down = true;
    bool p2 = false;
};

// A stand-in for GD's frame loop: `k` steps per rendered frame, real inputs
// queue up between frames and are applied at the first step of the next
// frame (no CBF), steps the engine doesn't run are carried (or dropped while
// it waits). The player's click times follow what the game shows, so time
// the game spent waiting earlier pushes later clicks back too.
struct Sim {
    Engine e;
    int k = 1;
    long long game = 0;            // last step run
    double real = 0;               // real time, in steps
    double carry = 0;
    double dropped = 0;            // real time the game spent waiting
    std::vector<double> droppedAt; // dropped so far when each step ran
    std::vector<Click> clicks;
    size_t nextClick = 0;
    std::vector<Queued> queue;
    struct Applied { long long step; bool down; bool p2; };
    std::vector<Applied> applied;

    explicit Sim(int stepsPerFrame) : k(stepsPerFrame) { droppedAt.push_back(0); }

    double clickReal(Click const& c) const {
        long long seen = std::min<long long>(static_cast<long long>(std::ceil(c.g)), game);
        if (seen < 0) seen = 0;
        return c.g + droppedAt[static_cast<size_t>(std::min<long long>(seen, static_cast<long long>(droppedAt.size()) - 1))];
    }

    void frame() {
        real += k;
        while (nextClick < clicks.size() && clickReal(clicks[nextClick]) <= real + 1e-9) {
            queue.push_back({ clicks[nextClick].down, clicks[nextClick].p2 });
            nextClick++;
        }
        int n = static_cast<int>(std::lround(k + carry));
        carry = k + carry - n;
        auto pl = e.plan(game, n, k, queue);
        if (pl.carry) carry += n - pl.steps;
        else dropped += n - pl.steps;
        for (int i = 0; i < pl.steps; i++) {
            game++;
            droppedAt.push_back(dropped);
            for (auto const& inj : e.due(game)) applied.push_back({ game, inj.down, inj.p2 });
            if (i == 0) {
                for (auto const& q : queue)
                    if (e.onInput(game, q.down, q.p2)) applied.push_back({ game, q.down, q.p2 });
                queue.clear();
            }
        }
    }

    void run(long long untilGame) {
        int guard = 0;
        while (game < untilGame && guard++ < 100000) frame();
    }

    // step the n-th applied input of this kind happened at (-1 = never)
    long long stepOf(bool down, bool p2 = false, int nth = 0) const {
        for (auto const& a : applied)
            if (a.down == down && a.p2 == p2 && nth-- == 0) return a.step;
        return -1;
    }
};

static Timing win(double t, double lo, double hi, bool down = true, bool p2 = false) {
    return { t, lo, hi, down, p2 };
}

// one press timing at 100 accepting steps 99..101; the player presses at
// 100 + offset and lets go 5 steps later. Did the press land in 99..101?
static bool survives(int fps, double X, double offset, bool assisted = true) {
    Sim s(fps);
    std::vector<Timing> list;
    if (assisted) list.push_back(win(100, -1.5, 1.5));
    s.e.setTimings(list, X);
    s.clicks = { { 100 + offset, true }, { 105 + offset, false } };
    s.run(200);
    long long st = s.stepOf(true);
    return st >= 99 && st <= 101;
}

static void test_offsets_240fps() {
    std::puts("240 fps (1 step per frame), X = 2: early/late within 2 survive, beyond dies");
    for (int o = -1; o <= 1; o++) CHECK(survives(1, 2, o), "on time %+d", o);
    CHECK(survives(1, 2, -2), "early 1 past the window");
    CHECK(survives(1, 2, -3), "early 2 past the window");
    CHECK(!survives(1, 2, -4), "early 3 past -> dies");
    CHECK(survives(1, 2, 2), "late 1 past");
    CHECK(survives(1, 2, 3), "late 2 past");
    CHECK(!survives(1, 2, 5), "late 4 past -> dies");
    CHECK(!survives(1, 2, 8), "late 7 past -> dies");
}

static void test_offsets_60fps() {
    std::puts("60 fps (4 steps per frame), X = 2: same rules, + one frame of polling slack late");
    for (double o : { -1.0, 0.0 }) CHECK(survives(4, 2, o), "on time %+.0f", o);
    CHECK(survives(4, 2, -3), "early 2 past");
    CHECK(survives(4, 2, 3), "late 2 past");
    CHECK(!survives(4, 2, 12), "late 11 past -> dies");
}

static void test_picked_gets_no_help() {
    std::puts("picked clicks (not in the list) get no help");
    CHECK(survives(1, 2, 0, false), "on time still fine");
    CHECK(!survives(1, 2, 2, false), "late 1 past -> dies");
    CHECK(!survives(1, 2, -2, false), "early 1 past -> dies");
}

static void test_no_time_lost_when_on_time() {
    std::puts("on time / early: the game never waits");
    Sim s(1);
    s.e.setTimings({ win(100, -1.5, 1.5), win(200, -2.5, 2.5), win(300, -0.5, 0.5) }, 2);
    s.clicks = { { 100, true }, { 104, false }, { 197, true }, { 201, false }, { 300, true }, { 302, false } };
    s.run(400);
    CHECK(s.dropped == 0, "dropped %.1f", s.dropped);
    CHECK(s.stepOf(true, false, 0) == 100, "1st at %lld", s.stepOf(true, false, 0));
    CHECK(s.stepOf(true, false, 1) == 198, "2nd (early by 1, X=2) at %lld", s.stepOf(true, false, 1));
    CHECK(s.stepOf(true, false, 2) == 300, "3rd at %lld", s.stepOf(true, false, 2));
}

static void test_wait_is_bounded() {
    std::puts("late and never clicking: the game waits at most ~X, then carries on");
    for (int fps : { 1, 4 }) {
        Sim s(fps);
        s.e.setTimings({ win(100, -1.5, 1.5) }, 3);
        s.run(150);
        CHECK(s.dropped <= 3 + 2 * fps, "fps %d waited %.0f", fps, s.dropped);
        CHECK(s.game >= 150, "game went on (%lld)", s.game);
    }
}

static void test_delay_keeps_hold_length() {
    std::puts("an early press held back also holds back its release (short tap stays a tap)");
    Sim s(1);
    s.e.setTimings({ win(100, -1.5, 1.5) }, 2);
    s.clicks = { { 97, true }, { 98, false } };
    s.run(150);
    CHECK(s.stepOf(true) == 99, "press at %lld", s.stepOf(true));
    CHECK(s.stepOf(false) == 100, "release at %lld (1 step after the press)", s.stepOf(false));
}

static void test_release_timings() {
    std::puts("release timings get the same help");
    auto rel = [](double off) {
        Sim s(1);
        s.e.setTimings({ win(100, -1.5, 1.5, false) }, 2);
        s.clicks = { { 50, true }, { 100 + off, false } };
        s.run(200);
        long long st = s.stepOf(false);
        return st >= 99 && st <= 101;
    };
    CHECK(rel(0), "on time");
    CHECK(rel(3), "late 2 past");
    CHECK(rel(-3), "early 2 past");
    CHECK(!rel(-4), "early 3 past -> dies");
    CHECK(!rel(6), "late 5 past -> dies");
}

static void test_waiting_shifts_later_clicks_fine() {
    std::puts("after a wait, later clicks still line up (the player sees the delayed game)");
    Sim s(1);
    s.e.setTimings({ win(100, -1.5, 1.5), win(200, -0.5, 0.5) }, 2);
    s.clicks = { { 103, true }, { 106, false }, { 200, true }, { 203, false } };
    s.run(300);
    CHECK(s.stepOf(true, false, 0) == 101, "late press applied at %lld", s.stepOf(true, false, 0));
    CHECK(s.stepOf(true, false, 1) == 200, "next one on time at %lld", s.stepOf(true, false, 1));
}

static void test_respawn() {
    std::puts("respawn: clicks behind you are done, no waiting for them");
    Sim s(1);
    s.e.setTimings({ win(100, -1.5, 1.5), win(300, -1.5, 1.5) }, 2);
    s.game = 200;
    s.real = 200;
    s.droppedAt.assign(201, 0.0);
    s.e.reset();
    s.clicks = { { 300, true }, { 303, false } };
    s.run(400);
    CHECK(s.dropped == 0, "dropped %.1f", s.dropped);
    CHECK(s.stepOf(true) == 300, "at %lld", s.stepOf(true));
}

static void test_two_player() {
    std::puts("2-player: a P1 input doesn't count as P2's click");
    Sim s(1);
    s.e.setTimings({ win(100, -1.5, 1.5, true, true) }, 2);
    s.clicks = { { 100, true, false }, { 103, true, true } }; // P1 on time, P2 late by 2
    s.run(150);
    long long p2 = s.stepOf(true, true);
    CHECK(p2 >= 99 && p2 <= 101, "P2 assisted to %lld", p2);
    CHECK(s.stepOf(true, false) == 100, "P1 untouched at %lld", s.stepOf(true, false));
}

static void test_unrelated_inputs_untouched() {
    std::puts("inputs far from any timing pass straight through, no waiting");
    Sim s(1);
    s.e.setTimings({ win(500, -1.5, 1.5) }, 2);
    s.clicks = { { 100, true }, { 110, false }, { 500, true }, { 505, false } };
    s.run(600);
    CHECK(s.stepOf(true, false, 0) == 100, "free click at %lld", s.stepOf(true, false, 0));
    CHECK(s.dropped == 0, "dropped %.1f", s.dropped);
}

static void test_fuzz() {
    std::puts("fuzz: 2000 random windows/offsets/fps follow the rules");
    std::srand(11);
    int checked = 0;
    for (int it = 0; it < 2000; it++) {
        int fps = (std::rand() % 2) ? 1 : 4;
        double X = 1 + std::rand() % 4;
        int lo = -(std::rand() % 4), hi = std::rand() % 4;
        Sim s(fps);
        s.e.setTimings({ win(300, lo - 0.5, hi + 0.5) }, X);
        int off = -12 + std::rand() % 25;
        s.clicks = { { 300.0 + off, true }, { 330.0 + off, false } };
        s.run(450);
        long long st = s.stepOf(true);
        bool in = st >= 300 + lo && st <= 300 + hi;
        // the player's click relative to the window
        int early = (300 + lo) - (300 + off);   // > 0: before the window
        int late = (300 + off) - (300 + hi);    // > 0: after
        if (fps == 1) {
            if (early <= X && late <= X) { CHECK(in, "it %d fps1 X%.0f [%d,%d] off %d -> %lld", it, X, lo, hi, off, st); checked++; }
            if (early > X || late > X + 1) { CHECK(!in, "it %d fps1 X%.0f [%d,%d] off %d -> %lld (should die)", it, X, lo, hi, off, st); checked++; }
        } else {
            if (early > X || late > X + 2 * fps) { CHECK(!in, "it %d fps4 X%.0f [%d,%d] off %d -> %lld (should die)", it, X, lo, hi, off, st); checked++; }
            // 60 fps without CBF: a click made at step c is applied at the
            // first step of the next frame, which can be up to 3 steps
            // EARLIER than c (GD catching up on the frame). If that snapped
            // step is inside the window, or early by <= X, you live.
            if (late <= 0) {
                long long c = 300 + off;
                long long snapped = 4 * ((c + 3) / 4) - 3;
                bool expect = snapped >= 300 + lo - static_cast<long long>(X) && snapped <= 300 + hi;
                CHECK(in == expect, "it %d fps4 X%.0f [%d,%d] off %d -> %lld (snapped %lld, expect %s)",
                    it, X, lo, hi, off, st, snapped, expect ? "live" : "die");
                checked++;
            }
        }
    }
    CHECK(checked > 500, "checked %d", checked);
}

int main() {
    test_offsets_240fps();
    test_offsets_60fps();
    test_picked_gets_no_help();
    test_no_time_lost_when_on_time();
    test_wait_is_bounded();
    test_delay_keeps_hold_length();
    test_release_timings();
    test_waiting_shifts_later_clicks_fine();
    test_respawn();
    test_two_player();
    test_unrelated_inputs_untouched();
    test_fuzz();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
