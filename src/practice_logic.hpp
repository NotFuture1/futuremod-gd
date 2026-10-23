#pragma once

// ===========================================================================
// Click practice: the decision logic, with no GD/Geode dependency so it can
// be tested on its own (tests/practice_test.cpp).
//
// The bot plays every recorded input EXCEPT the picked clicks (a picked press
// and its release). Around each picked click there is a window of time in
// which the player's own inputs on that button go through; everywhere else on
// stretches the macro covers, the player's inputs are ignored. Outside what
// the macro covers (before its start, after its end) the player just plays.
//
// Times are on the level clock (240 Hz frames from the level start).
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace fw::practice {

struct Input {
    double t = 0.0;
    int button = 1;
    bool down = true;
    bool player1 = true;
    int mask = 0; // players it moves (bit0 P1, bit1 P2); 0 = from player1
    int players() const { return mask ? mask : (player1 ? 1 : 2); }
};

struct Window {
    double from = 0.0, to = 0.0; // the player has this button here
    int button = 1;
    bool player1 = true;
    int mask = 1;
    size_t press = 0;            // index of the picked press
    bool ended = false;          // end-of-window sync done this attempt
};

struct Sync {
    int button;
    int mask;
};

struct StepOut {
    std::vector<size_t> apply;   // bot inputs to apply now (indices), in order
    std::vector<Sync> release;   // let go of these if the player still holds them
};

class Engine {
public:
    static constexpr double kGap = 0.5; // keep windows this far from neighbour inputs

    // `start` = level time where the macro begins (its start position)
    void setInputs(std::vector<Input> inputs, double start) {
        m_in = std::move(inputs);
        std::stable_sort(m_in.begin(), m_in.end(), [](Input const& a, Input const& b) { return a.t < b.t; });
        m_start = start;
        rebuild();
    }

    // the inputs as the engine holds them (sorted by time): StepOut::apply
    // indices refer to THIS list
    std::vector<Input> const& inputs() const { return m_in; }
    Input const& input(size_t i) const { return m_in[i]; }

    // picked clicks (level times of presses) and how much room around them
    void setSelection(std::vector<double> picked, double margin) {
        m_picked = std::move(picked);
        m_margin = margin;
        rebuild();
    }

    // picks that matched a recorded press (0 = practice does nothing)
    size_t matched() const { return m_windows.size(); }
    bool active() const { return !m_windows.empty() && !m_in.empty(); }
    std::vector<Window> const& windows() const { return m_windows; }

    // a new attempt started (level reset / respawn): the next step re-seeks
    void reset() { m_seek = true; }

    // does the macro cover this moment? (the bot only drives where it does)
    bool covers(double now) const {
        if (m_in.empty()) return false;
        return now >= m_start - kGap && now <= m_in.back().t + m_margin;
    }

    // may the player's real input on `button` (player 1 / 2) go through now?
    bool allowPlayer(double now, int button, bool player1) const {
        if (!active() || !covers(now)) return true;
        for (auto const& w : m_windows)
            if (w.button == button && w.player1 == player1 && now >= w.from && now <= w.to) return true;
        return false;
    }

    // one physics step at level time `now`; `holds(button, mask)` = is any of
    // those players holding that button
    StepOut step(double now, std::function<bool(int, int)> const& holds) {
        StepOut out;
        if (!active()) return out;
        if (m_seek) seek(now, out);
        long long nowStep = std::llround(now);
        while (m_ptr < m_in.size() && std::llround(m_in[m_ptr].t) <= nowStep) {
            if (!m_owned[m_ptr]) out.apply.push_back(m_ptr);
            m_ptr++;
        }
        for (auto& w : m_windows) {
            if (w.ended || now < w.to) continue;
            w.ended = true;
            // the recorded release is in the past now; if you still hold the
            // button, let go so the bot carries on from the recorded state
            if (holds && holds(w.button, w.mask)) out.release.push_back({ w.button, w.mask });
        }
        return out;
    }

private:
    std::vector<Input> m_in;
    std::vector<bool> m_owned; // input belongs to a picked click (the bot skips it)
    std::vector<Window> m_windows;
    std::vector<double> m_picked;
    double m_margin = 30.0;
    double m_start = 0.0;
    size_t m_ptr = 0;
    bool m_seek = true;

    bool same(size_t a, size_t b) const {
        return m_in[a].button == m_in[b].button && m_in[a].player1 == m_in[b].player1;
    }

    void rebuild() {
        m_owned.assign(m_in.size(), false);
        m_windows.clear();
        for (double p : m_picked) {
            // the recorded press closest to the pick (within 0.75 frame)
            size_t best = m_in.size();
            double bestD = 0.75;
            for (size_t i = 0; i < m_in.size(); i++) {
                if (!m_in[i].down) continue;
                double d = std::fabs(m_in[i].t - p);
                if (d < bestD) { bestD = d; best = i; }
            }
            if (best == m_in.size() || m_owned[best]) continue;
            // its release, and the neighbouring inputs of the same button
            size_t rel = m_in.size(), prev = m_in.size(), next = m_in.size();
            for (long j = static_cast<long>(best) - 1; j >= 0; j--)
                if (same(static_cast<size_t>(j), best)) { prev = static_cast<size_t>(j); break; }
            for (size_t j = best + 1; j < m_in.size(); j++) {
                if (!same(j, best)) continue;
                if (rel == m_in.size() && !m_in[j].down) { rel = j; continue; }
                if (rel != m_in.size()) { next = j; break; }
                next = j; // another press before any release (shouldn't happen)
                break;
            }
            Window w;
            w.button = m_in[best].button;
            w.player1 = m_in[best].player1;
            w.mask = m_in[best].players();
            w.press = best;
            double end = rel != m_in.size() ? m_in[rel].t : m_in[best].t;
            w.from = m_in[best].t - m_margin;
            w.to = end + m_margin;
            // never reach over a neighbouring input of the same button: the
            // bot plays those, so the window stops just short of them
            if (prev != m_in.size()) w.from = std::max(w.from, m_in[prev].t + kGap);
            if (next != m_in.size()) w.to = std::min(w.to, m_in[next].t - kGap);
            m_owned[best] = true;
            if (rel != m_in.size()) m_owned[rel] = true;
            m_windows.push_back(w);
        }
        std::sort(m_windows.begin(), m_windows.end(), [](Window const& a, Window const& b) { return a.from < b.from; });
        m_seek = true;
    }

    // jump the bot to `now`: skip what's already past, and if the recording
    // was holding a button at this moment (on a stretch the bot drives),
    // press it so the bot's state matches the recording
    void seek(double now, StepOut& out) {
        m_seek = false;
        m_ptr = static_cast<size_t>(std::lower_bound(m_in.begin(), m_in.end(), now - kGap,
            [](Input const& a, double t) { return a.t < t; }) - m_in.begin());
        for (auto& w : m_windows) w.ended = now >= w.to;
        std::map<std::pair<int, bool>, size_t> last; // last input per button/player before now
        for (size_t i = 0; i < m_ptr; i++) last[{ m_in[i].button, m_in[i].player1 }] = i;
        for (auto const& [key, i] : last) {
            if (!m_in[i].down) continue;
            if (m_owned[i]) continue; // a picked press: that one is yours
            out.apply.push_back(i);
        }
    }
};

} // namespace fw::practice
