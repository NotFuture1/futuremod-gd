#pragma once

// ===========================================================================
// Window extender: the decision logic, with no GD/Geode dependency so it can
// be tested on its own (tests/assist_test.cpp).
//
// You play every click yourself. For every click you did NOT pick to
// practice, the frame window is extended by X frames on each side:
//   - early by <= X frames: your input is held back and applied at the first
//     frame of the window (a press delays its release by the same amount, so
//     your hold length is kept);
//   - late: the game waits at the window's last good frame for up to X frames
//     (plus one rendered frame, the input-polling granularity); an input
//     during the wait is applied on that last good frame;
//   - further off than that: nothing is changed, so you die like normal.
// Picked clicks aren't in the list at all: no help on those.
//
// Steps are physics steps on the level clock (240 Hz). A boundary input
// processed during step s happened "at" s; a window [lo, hi] around a click
// at t accepts inputs at steps ceil(t+lo) .. floor(t+hi).
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

namespace fw::assist {

struct Timing {
    double t = 0.0;      // recorded click (level clock)
    double lo = -0.5;    // survivable range relative to t, frames
    double hi = 0.5;
    bool down = true;    // press / release
    bool p2 = false;     // player 2's click (2-player levels)
};

struct Queued {          // a real input waiting to be processed
    bool down;
    bool p2;
};

struct Inject {          // an input to apply now
    bool down;
    bool p2;
};

struct Plan {
    int steps;           // physics steps to run this frame
    bool carry;          // keep the time of the steps not run (else: dropped = the game waits)
};

class Engine {
public:
    // `extra` = X frames of help each side; `earlyFail` = how far before the
    // extended window an input still counts as a (failed) try at this click
    void setTimings(std::vector<Timing> list, double extra, double earlyFail = 6.0) {
        m_t.clear();
        for (auto const& t : list) {
            T x;
            x.down = t.down;
            x.p2 = t.p2;
            x.first = static_cast<long long>(std::ceil(t.t + t.lo + 1e-9));
            x.last = static_cast<long long>(std::floor(t.t + t.hi - 1e-9));
            if (x.first > x.last) x.first = x.last = std::llround(t.t); // sub-frame: its own step
            m_t.push_back(x);
        }
        std::sort(m_t.begin(), m_t.end(), [](T const& a, T const& b) { return a.last < b.last; });
        m_extra = extra;
        m_earlyFail = earlyFail;
        reset();
    }

    bool active() const { return !m_t.empty(); }

    // new attempt (level reset / respawn): clicks already behind you are done
    void reset() {
        m_needInit = true;
        m_wait = -1;
        m_late = 0.0;
        m_hasOverride = false;
        m_inject.clear();
        m_delay[0] = m_delay[1] = 0;
    }

    // Start of a rendered frame, before any physics: GD wants to run `n`
    // steps; `now` = last step already run; `queued` = real inputs waiting to
    // be processed. (`realSteps` is unused: `n` already is real time, incl.
    // GD's carried remainder.)
    Plan plan(long long now, int n, double /*realSteps*/, std::vector<Queued> const& queued) {
        init(now);
        if (m_wait >= 0) {
            T& w = m_t[m_wait];
            // m_late = how late you were at the end of the last frame. An
            // input seen now arrived after that, so it's at least that late
            // (and at most one rendered frame more: the input-polling
            // granularity, which is where the leniency comes from).
            if (has(queued, w.down, w.p2)) {
                m_wait = -1;
                if (m_late <= m_extra + 1e-9) return { std::min(n, 1), true }; // lands on the last good step
                // too late: it goes in where it really was -> you die
                w.done = true;
                m_override = { w.down, w.p2, w.last + 1 + static_cast<long long>(std::floor(m_late)) };
                m_hasOverride = true;
                return { n, true };
            }
            if (m_late > m_extra + 1e-9) {
                w.done = true; // no input in time: carry on, you'll die like normal
                m_wait = -1;
            } else {
                m_late += n;
                return { 0, false }; // keep waiting (this frame's time is dropped)
            }
        }
        for (;;) {
            int i = earliestPending();
            if (i < 0) return { n, true };
            T& p = m_t[i];
            long long next = now + 1;
            if (p.last < next) { p.done = true; continue; } // passed (e.g. enabled mid-level)
            if (next + n - 1 < p.last) return { n, true };   // this frame doesn't reach it
            if (has(queued, p.down, p.p2)) return { n, true }; // your input lands at `next` <= last
            // Stop just before the last good step and wait there. The steps
            // this frame would have run past it are time you're already late.
            int upTo = static_cast<int>(p.last - next);
            m_wait = i;
            m_late = std::max(0, n - upTo - 1);
            return { upTo, false };
        }
    }

    // A real input reaches the game during step `s`. true = let it through
    // now; false = it was taken and will come back via due() later.
    bool onInput(long long s, bool down, bool p2) {
        init(s - 1);
        int k = p2 ? 1 : 0;
        long long eff = s;
        if (!down) { eff += m_delay[k]; m_delay[k] = 0; } // keep the hold length of a delayed press
        long long target = eff;
        if (m_hasOverride && m_override.down == down && m_override.p2 == p2) {
            // the input that came too late during a wait: where it really was
            m_hasOverride = false;
            target = std::max(eff, m_override.at);
            if (down) m_delay[k] = target - s;
            if (target <= s) return true;
            m_inject.push_back({ target, { down, p2 } });
            return false;
        }
        int i = claim(eff, down, p2);
        if (i >= 0) {
            T& c = m_t[i];
            c.done = true;
            if (m_wait == i) m_wait = -1;
            if (eff < c.first && eff >= c.first - static_cast<long long>(std::floor(m_extra + 1e-9)))
                target = c.first; // early, within X: held until the window opens
        }
        if (down) m_delay[k] = target - s;
        if (target <= s) return true;
        m_inject.push_back({ target, { down, p2 } });
        return false;
    }

    // inputs to apply during step `s` (held-back ones that are due)
    std::vector<Inject> due(long long s) {
        std::vector<Inject> out;
        while (!m_inject.empty() && m_inject.front().first <= s) {
            out.push_back(m_inject.front().second);
            m_inject.pop_front();
        }
        return out;
    }

    bool waiting() const { return m_wait >= 0; }

private:
    struct T {
        long long first = 0, last = 0; // accepted steps
        bool down = true, p2 = false;
        bool done = false;
    };
    std::vector<T> m_t;
    double m_extra = 2.0, m_earlyFail = 6.0;
    bool m_needInit = true;
    int m_wait = -1;
    double m_late = 0.0; // while waiting: how late you are (steps past the last good one)
    struct Override { bool down; bool p2; long long at; };
    Override m_override{ true, false, 0 };
    bool m_hasOverride = false;
    std::deque<std::pair<long long, Inject>> m_inject; // kept in time order
    long long m_delay[2] = { 0, 0 };

    void init(long long now) {
        if (!m_needInit) return;
        m_needInit = false;
        for (auto& t : m_t) t.done = t.last <= now;
    }

    static bool has(std::vector<Queued> const& q, bool down, bool p2) {
        for (auto const& x : q) if (x.down == down && x.p2 == p2) return true;
        return false;
    }

    int earliestPending() const {
        for (size_t i = 0; i < m_t.size(); i++) if (!m_t[i].done) return static_cast<int>(i);
        return -1;
    }

    // the click an input at `eff` is a try at: the earliest pending one of
    // this kind whose (extended) window hasn't passed and has started
    int claim(long long eff, bool down, bool p2) const {
        long long reach = static_cast<long long>(std::floor(m_extra + m_earlyFail + 1e-9));
        for (size_t i = 0; i < m_t.size(); i++) {
            auto const& t = m_t[i];
            if (t.done || t.down != down || t.p2 != p2) continue;
            if (t.last < eff) continue;
            if (eff < t.first - reach) return -1; // way before it: not about this click
            return static_cast<int>(i);
        }
        return -1;
    }
};

} // namespace fw::assist
