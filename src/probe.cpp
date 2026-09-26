#include "common.hpp"
#include "sim/ghost.hpp"

#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include <chrono>
#include <cmath>

using namespace geode::prelude;

// ===========================================================================
// Diagnostic driver for the frame-window counter.
//
//  * ENV      -- one line at level start saying exactly what we are running in
//  * TSPROBE  -- Phase 6b: is GJBaseGameLayer::m_timestamp a game clock or a
//                wall clock? shouldUseSubstepForButton() compares queued
//                button timestamps against `m_timestamp + dt` where dt is a
//                PHYSICS delta, so if m_timestamp advances by 1/240 per tick
//                then vanilla's own between-steps path can place an input at
//                an exact sub-tick time -- which would give us real sub-frame
//                measurement in the real engine, not just in the ghost.
//  * GHOSTFID -- Phase 1: drives the ghost fidelity probe (see sim/ghost.cpp)
//
// All log-only. Nothing here changes how the game or the existing analyzer
// behaves.
// ===========================================================================

namespace {

constexpr int kTsProbeTicks = 100;

int    g_tick = 0;
double g_gameTime = 0.0;
double g_lastTs = -1.0;
int    g_tsLogged = 0;
std::chrono::steady_clock::time_point g_lastWall;
bool   g_haveWall = false;

bool fidelityEnabled() {
    return Mod::get()->getSettingValue<bool>("ghost-fidelity");
}

bool tsProbeEnabled() {
    return Mod::get()->getSettingValue<bool>("timestamp-probe");
}

void resetTicks() {
    g_tick = 0;
    g_gameTime = 0.0;
    g_lastTs = -1.0;
    g_tsLogged = 0;
    g_haveWall = false;
    fw::ghost::fidelityReset();
}

} // namespace

class $modify(ProbePlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        fw::ghost::clearPool(); // never hold a ghost across levels
        resetTicks();

        const bool cbf = Loader::get()->isModLoaded("syzzi.click_between_frames");
        FW_LOG("ENV mod={} geode={} gd=2.2081 cbf={} order={} eps={:.3f}",
            Mod::get()->getVersion().toNonVString(),
            Loader::get()->getVersion().toNonVString(),
            cbf ? 1 : 0, fw::ghostOrder(), fw::ghostEps());
        FW_LOG("ENV clickBetweenSteps={} clickOnSteps={} isBetweenSteps={} practice={} platformer={} level='{}'",
            this->m_clickBetweenSteps ? 1 : 0,
            this->m_clickOnSteps ? 1 : 0,
            this->m_isBetweenSteps ? 1 : 0,
            this->m_isPracticeMode ? 1 : 0,
            (level && level->isPlatformer()) ? 1 : 0,
            level ? std::string(level->m_levelName) : std::string("?"));
        FW_LOG("ENV probes: fidelity={} timestamp={} verbose={}",
            fidelityEnabled() ? 1 : 0, tsProbeEnabled() ? 1 : 0, fw::verbose() ? 1 : 0);
        if (cbf) {
            // CBF is supported by the macro/analyzer (it reproduces CBF's split
            // step); only the ghost diagnostics below don't model it.
            FW_LOG("ENV Click Between Frames is loaded -- supported by record/replay/analyze; "
                   "the ghost fidelity probe does not model its split steps.");
        }
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (g_tick > 0) fw::ghost::fidelityReport();
        resetTicks();
    }

    void onQuit() {
        fw::ghost::fidelityReport();
        fw::ghost::clearPool();
        resetTicks();
        PlayLayer::onQuit();
    }
};

class $modify(ProbeBGL, GJBaseGameLayer) {
    // Record real input edges so the fidelity probe can discard any sample
    // whose hold-the-buttons assumption was broken. Not called during macro
    // playback (that injects straight into the player), which is exactly why
    // the probe refuses to run while the macro system is busy.
    void handleButton(bool down, int button, bool isPlayer1) {
        if (!fw::ghost::isSim()) fw::ghost::fidelityOnInputEdge(g_tick);
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (fw::ghost::isSim()) return; // our own ghost stepping is not a real tick

        // upcast to compare -- a static downcast would be UB in the editor,
        // where `this` is a LevelEditorLayer and not a PlayLayer at all
        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != static_cast<GJBaseGameLayer*>(this))
            return;

        g_gameTime += dt;
        g_tick = static_cast<int>(std::llround(g_gameTime * 240.0));

        // --- Phase 6b: m_timestamp clock domain -----------------------------
        if (tsProbeEnabled() && g_tsLogged < kTsProbeTicks) {
            const auto now = std::chrono::steady_clock::now();
            double wallDelta = 0.0;
            if (g_haveWall)
                wallDelta = std::chrono::duration<double>(now - g_lastWall).count();
            g_lastWall = now;
            g_haveWall = true;

            const double ts = this->m_timestamp;
            const double tsDelta = (g_lastTs < 0.0) ? 0.0 : (ts - g_lastTs);
            g_lastTs = ts;
            FW_LOG("TSPROBE tick={} m_timestamp={:.6f} delta={:.6f} dt={:.6f} wcdelta={:.6f} queued={}",
                g_tick, ts, tsDelta, dt, wallDelta,
                static_cast<int>(this->m_queuedButtons.size()));
            if (++g_tsLogged == kTsProbeTicks) {
                FW_LOG("TSPROBE done -- if delta is ~{:.6f} every tick it is a GAME clock "
                       "(native sub-step injection is viable); if it tracks wcdelta it is a "
                       "WALL clock (drop that route).", 1.0 / 240.0);
            }
        }

        // --- Phase 1: ghost fidelity ---------------------------------------
        // Skipped while the macro system is recording/replaying/analysing: it
        // injects inputs straight into the PlayerObject, so handleButton never
        // fires and the probe could not tell that its hold assumption broke.
        if (fidelityEnabled() && !fw::macroBusy()) {
            fw::ghost::ensurePool(pl);
            fw::ghost::fidelityOnTick(pl, g_tick);
        }
    }
};
