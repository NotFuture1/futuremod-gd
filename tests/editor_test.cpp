// Host tests for the macro editor's model (no GD needed):
//   clang++ -std=c++20 -I src tests/editor_test.cpp -o /tmp/et && /tmp/et
#include "editor_logic.hpp"

#include <cstdio>
#include <random>
#include <vector>

using namespace fw::ed;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) { g_pass++; }                                                 \
        else { g_fail++; std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
               std::printf(__VA_ARGS__); std::printf("\n"); }                   \
    } while (0)

static Edge E(double t, bool down, int button = 1, bool p1 = true, int mask = 1) {
    Edge e;
    e.t = t; e.down = down; e.button = button; e.p1 = p1; e.mask = mask;
    return e;
}

// three taps on P1: 100-104, 200-230, 300-302
static Model taps(bool cbf) {
    Model m;
    m.load({ E(100, true), E(104, false), E(200, true), E(230, false), E(300, true), E(302, false) }, cbf);
    return m;
}

static int idAt(Model const& m, double t) {
    for (auto const& e : m.edges()) if (std::fabs(e.t - t) < 1e-9) return e.id;
    return 0;
}

// the model's invariants: sorted, and every channel alternates press/release
static bool valid(Model const& m, double gap) {
    auto const& e = m.edges();
    for (size_t i = 1; i < e.size(); i++) if (e[i].t < e[i - 1].t) return false;
    for (size_t i = 0; i < e.size(); i++) {
        if (e[i].t < 0) return false;
        for (size_t k = i + 1; k < e.size(); k++) {
            if (channel(e[k]) != channel(e[i])) continue;
            if (e[k].down == e[i].down) return false;      // must alternate
            if (e[k].t - e[i].t < gap - 1e-9) return false; // and keep the gap
            break;
        }
    }
    return true;
}

static void test_normal_macro_snaps_to_frames() {
    std::puts("normal macro: moves land on whole frames");
    auto m = taps(false);
    int id = idAt(m, 200);
    double v = m.move(id, 203.4, 0.125);
    CHECK(v == 203.0, "got %f", v);
    v = m.move(id, 203.6, 0.125);
    CHECK(v == 204.0, "got %f", v);
    CHECK(!m.edges()[m.indexOf(id)].mid, "a whole-frame edge isn't mid-step");
}

static void test_cbf_macro_snaps_to_step() {
    std::puts("CBF macro: moves land on the sub-frame grid and become mid-step");
    auto m = taps(true);
    int id = idAt(m, 200);
    double v = m.move(id, 200.30, 0.125);
    CHECK(std::fabs(v - 200.25) < 1e-9, "got %f", v);
    CHECK(m.edges()[m.indexOf(id)].mid, "inside a frame = mid-step");
    v = m.move(id, 200.30, 1.0 / 32);
    CHECK(std::fabs(v - 10.0 / 32 - 200) < 1e-9, "1/32 grid: got %f", v);
}

static void test_move_cannot_cross_neighbour() {
    std::puts("a press can't be moved past its own release (or the previous release)");
    auto m = taps(false);
    int id = idAt(m, 200);
    double v = m.move(id, 260, 1);
    CHECK(v == 229.0, "stops a frame before its release: %f", v);
    v = m.move(id, 50, 1);
    CHECK(v == 105.0, "stops a frame after the previous release: %f", v);
    CHECK(valid(m, 1.0), "still valid");
}

static void test_other_players_can_cross() {
    std::puts("clicks of another player / button move freely past each other");
    Model m;
    m.load({ E(100, true), E(110, false), E(105, true, 1, false, 2), E(120, false, 1, false, 2) }, false);
    int p2 = 0;
    for (auto const& e : m.edges()) if (e.mask == 2 && e.down) p2 = e.id;
    double v = m.move(p2, 90, 1);
    CHECK(v == 90.0, "P2's press goes before P1's: %f", v);
    CHECK(m.edges()[0].id == p2, "and sorts first");
}

static void test_drag_is_one_undo() {
    std::puts("a whole drag is one undo step; a drag back to the start is none");
    auto m = taps(false);
    int id = idAt(m, 300);
    m.beginDrag();
    for (double t = 300; t < 310; t += 0.5) m.dragTo(id, t, 1);
    m.endDrag();
    CHECK(m.canUndo(), "undoable");
    m.undo();
    CHECK(m.edges()[m.indexOf(id)].t == 300.0, "undo puts it back");
    CHECK(!m.canUndo(), "one step only");
    m.redo();
    CHECK(m.edges()[m.indexOf(id)].t == 301.0, "redo: %f", m.edges()[m.indexOf(id)].t);
    m.undo();
    m.beginDrag();
    m.dragTo(id, 301, 1);
    m.dragTo(id, 300, 1);
    m.endDrag();
    CHECK(!m.canUndo(), "no change, no undo step");
}

static void test_add_click() {
    std::puts("add a click: in a gap works, inside a hold or without room doesn't");
    auto m = taps(false);
    std::string why;
    int id = m.addClick(150, 4, 1, true, 1, 1, &why);
    CHECK(id != 0, "in a gap: %s", why.c_str());
    CHECK(m.edges().size() == 8, "two edges added");
    CHECK(valid(m, 1.0), "valid");
    CHECK(m.addClick(210, 4, 1, true, 1, 1, &why) == 0, "inside the 200-230 hold");
    CHECK(m.addClick(296, 4, 1, true, 1, 1, &why) == 0, "no room before 300: %s", why.c_str());
    CHECK(m.addClick(400, 6, 1, true, 1, 1, &why) != 0, "after the last click");
    m.undo();
    m.undo();
    CHECK(m.edges().size() == 6, "undo removes both adds");
}

static void test_remove_click() {
    std::puts("delete removes the press with its release, from either one");
    auto m = taps(false);
    CHECK(m.removeClick(idAt(m, 230)), "via the release");
    CHECK(m.edges().size() == 4, "pair gone");
    CHECK(idAt(m, 200) == 0, "press gone too");
    CHECK(valid(m, 1.0), "valid");
    m.undo();
    CHECK(m.edges().size() == 6, "undo restores");
}

static void test_dirty_and_first_difference() {
    std::puts("dirty / first difference");
    auto m = taps(true);
    auto before = m.edges();
    CHECK(!m.dirty(), "fresh model");
    m.move(idAt(m, 300), 300.5, 0.125);
    CHECK(m.dirty(), "moved");
    CHECK(Model::firstDifference(m.edges(), before) == 300.0, "first diff 300: %f",
        Model::firstDifference(m.edges(), before));
    m.move(idAt(m, 104), 103, 0.125);
    CHECK(Model::firstDifference(m.edges(), before) == 103.0, "first diff 103");
    m.markSaved();
    CHECK(!m.dirty(), "saved");
    CHECK(Model::firstDifference(m.edges(), m.edges()) == kInf, "no difference");
}

static void test_holds() {
    std::puts("holds pair each press with its release");
    auto m = taps(false);
    auto h = m.holds();
    CHECK(h.size() == 3, "three holds");
    CHECK(h[1].a == 200 && h[1].b == 230, "200-230");
    Model u;
    u.load({ E(10, true), E(20, false), E(30, true) }, false);
    auto hu = u.holds();
    CHECK(hu.size() == 2 && hu[1].b == kInf, "an unreleased hold runs to the end");
}

static void test_rebase_after_save() {
    std::puts("after a save, orig indices / times follow the saved list (undo snapshots too)");
    auto m = taps(false);
    int id = idAt(m, 200);
    m.move(id, 150, 1);   // now the 2nd press sorts... still index 2 (after 104)
    std::string w;
    int added = m.addClick(20, 4, 1, true, 1, 1, &w); // sorts first
    m.rebase();
    CHECK(!m.dirty(), "saved");
    for (size_t i = 0; i < m.edges().size(); i++)
        CHECK(m.edges()[i].orig == static_cast<int>(i) && m.edges()[i].origT == m.edges()[i].t, "edge %zu rebased", i);
    m.undo(); // back before the add: the added click's edges are gone, the rest keep the new indices
    CHECK(m.indexOf(added) < 0, "undo removed the added click");
    int i = m.indexOf(id);
    CHECK(i >= 0 && m.edges()[i].orig == 4 && m.edges()[i].origT == 150, "moved press: orig %d origT %f",
        m.edges()[i].orig, m.edges()[i].origT);
}

static void test_fuzz() {
    std::puts("fuzz: random moves / adds / deletes / undo keep the macro valid");
    std::mt19937 rng(7);
    for (int round = 0; round < 200; round++) {
        bool cbf = round % 2;
        double step = cbf ? 0.125 : 1.0;
        Model m;
        std::vector<Edge> e;
        double t = 10;
        for (int i = 0; i < 20; i++) {
            int mask = (i % 3 == 2) ? 2 : 1;
            e.push_back(E(t, true, 1, mask == 1, mask));
            e.push_back(E(t + 3, false, 1, mask == 1, mask));
            t += 7;
        }
        m.load(e, cbf);
        for (int op = 0; op < 60; op++) {
            auto const& es = m.edges();
            if (es.empty()) break;
            int pick = es[rng() % es.size()].id;
            switch (rng() % 5) {
                case 0: case 1: m.move(pick, std::uniform_real_distribution<double>(0, 160)(rng), step); break;
                case 2: { std::string w; m.addClick(std::uniform_real_distribution<double>(0, 160)(rng), 2, 1, true, 1, step, &w); break; }
                case 3: m.removeClick(pick); break;
                case 4: if (rng() % 2) m.undo(); else m.redo(); break;
            }
            if (!valid(m, m.minGap(step))) { CHECK(false, "round %d op %d broke the rules", round, op); return; }
        }
    }
    CHECK(true, "ok");
}

int main() {
    test_normal_macro_snaps_to_frames();
    test_cbf_macro_snaps_to_step();
    test_move_cannot_cross_neighbour();
    test_other_players_can_cross();
    test_drag_is_one_undo();
    test_add_click();
    test_remove_click();
    test_dirty_and_first_difference();
    test_holds();
    test_rebase_after_save();
    test_fuzz();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
