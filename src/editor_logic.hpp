#pragma once

// ===========================================================================
// Macro editor: the editing model. No GD/Geode dependency, tested on its own
// (tests/editor_test.cpp).
//
// A macro is a list of edges (press / release of one button) on the level
// clock in frames (240 a second). A CBF macro's edges can sit anywhere inside
// a frame (t = step + fraction); a normal macro's only on whole frames, so
// moves snap to whole frames there.
//
// Rules the model keeps:
//   - edges of the same "channel" (the player(s) it drives + the button) keep
//     their order and alternate press / release, at least one snap step apart
//     -- a move can't jump over its neighbour, and a press can't be added
//     inside a hold;
//   - clicks are added and deleted as press + release pairs;
//   - every change is undoable (whole-list snapshots: macros are small).
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace fw::ed {

constexpr double kInf = std::numeric_limits<double>::infinity();

struct Edge {
    double t = 0.0;      // level-clock frames from the macro's step 0
    bool down = true;    // press / release
    int button = 1;      // 1 jump, 2 left, 3 right
    bool p1 = true;      // GD's "player 1" side of the input
    int mask = 0;        // players it pushed (bit0 P1, bit1 P2); 0 = unknown
    bool mid = false;    // CBF: applied inside the step (vs at its start)
    bool fix = false;    // practice-respawn hold fix (not a real click)
    int orig = -1;       // index in the macro as loaded; -1 = added here
    double origT = 0.0;  // its time as loaded
    int id = 0;          // stable identity across sorts / undo
};

// who an edge drives: the same channel = the same hold
inline int channel(Edge const& e) {
    int m = e.mask ? e.mask : (e.p1 ? 1 : 2);
    return m * 8 + e.button;
}

struct Hold {
    int ch = 0;
    double a = 0.0, b = kInf; // press .. release (kInf: never released)
    int pressId = 0, releaseId = 0;
};

class Model {
public:
    void load(std::vector<Edge> edges, bool cbf) {
        m_cbf = cbf;
        m_e = std::move(edges);
        m_nextId = 1;
        for (size_t i = 0; i < m_e.size(); i++) {
            m_e[i].orig = static_cast<int>(i);
            m_e[i].origT = m_e[i].t;
            m_e[i].id = m_nextId++;
        }
        sortEdges();
        m_undo.clear();
        m_redo.clear();
        m_saved = m_e;
    }

    std::vector<Edge> const& edges() const { return m_e; }
    bool cbf() const { return m_cbf; }

    int indexOf(int id) const {
        for (size_t i = 0; i < m_e.size(); i++) if (m_e[i].id == id) return static_cast<int>(i);
        return -1;
    }

    // Snap a time to the editing grid: whole frames for a normal macro,
    // `step` frames for a CBF one (step <= 0: no snapping).
    double snap(double t, double step) const {
        if (!m_cbf) return std::round(t);
        if (step <= 0.0) return t;
        return std::round(t / step) * step;
    }

    // smallest distance between two edges of one channel
    double minGap(double step) const { return m_cbf ? std::max(step, 1.0 / 64.0) : 1.0; }

    // Where edge `id` may be moved to: strictly after the previous edge of its
    // channel and before the next one, `gap` apart; never before frame 0.
    std::pair<double, double> moveRange(int id, double gap) const {
        int i = indexOf(id);
        if (i < 0) return { 0.0, 0.0 };
        int ch = channel(m_e[i]);
        double lo = 0.0, hi = kInf;
        for (int k = i - 1; k >= 0; k--)
            if (channel(m_e[k]) == ch) { lo = m_e[k].t + gap; break; }
        for (size_t k = i + 1; k < m_e.size(); k++)
            if (channel(m_e[k]) == ch) { hi = m_e[k].t - gap; break; }
        return { lo, hi };
    }

    // Drag: begin() records one undo point, to() moves (clamped, no history),
    // end() drops the undo point again if nothing changed.
    void beginDrag() {
        m_dragStart = m_e;
        pushUndo();
    }
    // returns the time actually set
    double dragTo(int id, double t, double step) {
        int i = indexOf(id);
        if (i < 0) return 0.0;
        auto [lo, hi] = moveRange(id, minGap(step));
        double v = snap(t, step);
        if (m_cbf && step > 0.0) {
            // the clamp keeps the snap grid: nearest grid point inside the range
            if (v < lo) v = std::ceil(lo / step - 1e-9) * step;
            if (v > hi) v = std::floor(hi / step + 1e-9) * step;
        } else if (!m_cbf) {
            if (v < lo) v = std::ceil(lo - 1e-9);
            if (v > hi) v = std::floor(hi + 1e-9);
        }
        v = std::clamp(v, lo, std::max(lo, hi));
        m_e[i].t = v;
        if (!m_cbf) m_e[i].mid = false;
        else if (std::fabs(v - std::floor(v)) > 1e-9) m_e[i].mid = true; // inside a frame = CBF-delivered
        sortEdges();
        return v;
    }
    void endDrag() {
        if (sameTimes(m_e, m_dragStart)) { m_undo.pop_back(); return; }
        m_redo.clear();
    }

    // one-shot move (keyboard nudge, typed time)
    double move(int id, double t, double step) {
        beginDrag();
        double v = dragTo(id, t, step);
        endDrag();
        return v;
    }

    // Add a click: press at `t`, release `hold` frames later, on one channel.
    // Fails (with the reason) inside a hold or without room before the next
    // click of that channel. Returns the press's id (0 on failure).
    int addClick(double t, double hold, int button, bool p1, int mask, double step, std::string* why) {
        double gap = minGap(step);
        t = snap(std::max(0.0, t), step);
        hold = std::max(hold, gap);
        double r = snap(t + hold, step);
        if (r < t + gap) r = t + gap;
        Edge press;
        press.t = t; press.down = true; press.button = button; press.p1 = p1; press.mask = mask;
        press.mid = m_cbf && std::fabs(t - std::floor(t)) > 1e-9;
        Edge rel = press;
        rel.t = r; rel.down = false;
        rel.mid = m_cbf && std::fabs(r - std::floor(r)) > 1e-9;
        int ch = channel(press);
        // the channel's state just before t, and its next edge
        Edge const* prev = nullptr;
        Edge const* next = nullptr;
        for (auto const& e : m_e) {
            if (channel(e) != ch) continue;
            if (e.t <= t) prev = &e;
            else { next = &e; break; }
        }
        if (prev && prev->down) { if (why) *why = "That's inside a hold: let go first"; return 0; }
        if (prev && t - prev->t < gap) { if (why) *why = "Too close to the previous click"; return 0; }
        if (next && next->t - r < gap) {
            if (why) *why = "Not enough room before the next click";
            return 0;
        }
        pushUndo();
        m_redo.clear();
        press.id = m_nextId++;
        rel.id = m_nextId++;
        m_e.push_back(press);
        m_e.push_back(rel);
        sortEdges();
        return press.id;
    }

    // Delete a click: the press together with its release (selecting either).
    bool removeClick(int id) {
        int i = indexOf(id);
        if (i < 0) return false;
        int ch = channel(m_e[i]);
        int partner = -1;
        if (m_e[i].down) {
            for (size_t k = i + 1; k < m_e.size(); k++)
                if (channel(m_e[k]) == ch) { if (!m_e[k].down) partner = static_cast<int>(k); break; }
        } else {
            for (int k = i - 1; k >= 0; k--)
                if (channel(m_e[k]) == ch) { if (m_e[k].down) partner = k; break; }
        }
        pushUndo();
        m_redo.clear();
        int a = std::max(i, partner), b = std::min(i, partner);
        m_e.erase(m_e.begin() + a);
        if (b >= 0) m_e.erase(m_e.begin() + b);
        return true;
    }

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    bool undo() {
        if (m_undo.empty()) return false;
        m_redo.push_back(std::move(m_e));
        m_e = std::move(m_undo.back());
        m_undo.pop_back();
        return true;
    }
    bool redo() {
        if (m_redo.empty()) return false;
        m_undo.push_back(std::move(m_e));
        m_e = std::move(m_redo.back());
        m_redo.pop_back();
        return true;
    }
    void setUndoLimit(size_t n) { m_limit = std::max<size_t>(n, 1); }

    // After a save the saved list is the new "as loaded": every edge's orig
    // becomes its index in it and origT its time (in the undo / redo
    // snapshots too, by id; edges not in the saved list become "added").
    void rebase() {
        auto fix = [&](std::vector<Edge>& list) {
            for (auto& e : list) {
                int i = indexOf(e.id);
                if (i < 0) { e.orig = -1; e.origT = e.t; }
                else { e.orig = i; e.origT = m_e[i].t; }
            }
        };
        for (size_t i = 0; i < m_e.size(); i++) { m_e[i].orig = static_cast<int>(i); m_e[i].origT = m_e[i].t; }
        for (auto& u : m_undo) fix(u);
        for (auto& r : m_redo) fix(r);
        m_saved = m_e;
    }

    // different from what was loaded / last saved
    bool dirty() const { return !sameTimes(m_e, m_saved) || m_e.size() != m_saved.size(); }
    void markSaved() { m_saved = m_e; }

    // earliest frame where the macro differs from `ref` (kInf: identical)
    static double firstDifference(std::vector<Edge> const& a, std::vector<Edge> const& ref) {
        double first = kInf;
        auto key = [](Edge const& e) { return e.id; };
        // edges only in one list, or at different times
        for (auto const& e : a) {
            auto it = std::find_if(ref.begin(), ref.end(), [&](Edge const& r) { return key(r) == key(e); });
            if (it == ref.end()) first = std::min(first, e.t);
            else if (std::fabs(it->t - e.t) > 1e-9) first = std::min({ first, e.t, it->t });
        }
        for (auto const& r : ref) {
            auto it = std::find_if(a.begin(), a.end(), [&](Edge const& e) { return key(e) == key(r); });
            if (it == a.end()) first = std::min(first, r.t);
        }
        return first;
    }

    // all holds (press -> its release), for the hold bars
    std::vector<Hold> holds() const {
        std::vector<Hold> out;
        std::vector<std::pair<int, size_t>> open; // channel -> index in out
        for (auto const& e : m_e) {
            int ch = channel(e);
            auto it = std::find_if(open.begin(), open.end(), [&](auto const& o) { return o.first == ch; });
            if (e.down) {
                if (it != open.end()) continue; // already holding (double press): one hold
                Hold h;
                h.ch = ch; h.a = e.t; h.pressId = e.id;
                out.push_back(h);
                open.push_back({ ch, out.size() - 1 });
            } else if (it != open.end()) {
                out[it->second].b = e.t;
                out[it->second].releaseId = e.id;
                open.erase(it);
            }
        }
        return out;
    }

private:
    std::vector<Edge> m_e, m_saved, m_dragStart;
    std::vector<std::vector<Edge>> m_undo, m_redo;
    size_t m_limit = 200;
    bool m_cbf = false;
    int m_nextId = 1;

    void sortEdges() {
        std::stable_sort(m_e.begin(), m_e.end(), [](Edge const& a, Edge const& b) { return a.t < b.t; });
    }
    void pushUndo() {
        m_undo.push_back(m_e);
        if (m_undo.size() > m_limit) m_undo.erase(m_undo.begin());
    }
    static bool sameTimes(std::vector<Edge> const& a, std::vector<Edge> const& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (a[i].id != b[i].id || std::fabs(a[i].t - b[i].t) > 1e-9) return false;
        return true;
    }
};

} // namespace fw::ed
