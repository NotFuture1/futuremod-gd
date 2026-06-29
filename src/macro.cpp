#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <unordered_map>
#include <cmath>

using namespace geode::prelude;

// ===========================================================================
// Phase 0 (v2): deterministic macro record/replay, PRACTICE-MODE AWARE.
//
// Inputs are indexed by a physics-step counter we maintain ourselves
// (incremented once per GJBaseGameLayer::processCommands, which runs once per
// 240 TPS physics step). This is independent of game speed, so recording while
// the game is slowed down by other mods still lands clicks on the right steps.
//
// Practice mode: when you place a checkpoint we remember the step it was at;
// when you die and respawn at a checkpoint we rewind our step counter to it and
// DROP the inputs from the failed attempt after that checkpoint. The result is
// a macro stitched only from the segments you actually clear.
// ===========================================================================

namespace {

enum class Mode { Idle, Recording, Playing };

struct InputEdge {
    int step;     // physics step from level start
    int button;   // PlayerButton (round-tripped; 1 = jump)
    bool player1;
    bool down;    // press / release
};

struct Macro {
    Mode mode = Mode::Idle;
    std::vector<InputEdge> inputs;
    size_t playIndex = 0;
    int step = 0;          // current step = round(gameTime * 240), speed-invariant
    double gameTime = 0;   // accumulated game-time (seconds) from level start
    int dtLogBudget = 0;   // diagnostic: log a few dt values while recording
    int injected = 0;      // diagnostic: inputs injected this playback
    bool finishedNotified = false;
    int logBudget = 0;     // diagnostic: limit verbose per-step logging
    std::unordered_map<CheckpointObject*, int> cpStep; // checkpoint -> step (recording)

    static Macro& get() { static Macro m; return m; }
};

void notify(std::string const& msg, NotificationIcon icon) {
    Notification::create(msg, icon)->show();
}

void startRecording() {
    auto pl = PlayLayer::get();
    if (!pl) { notify("Macro: enter a level first", NotificationIcon::Error); return; }
    auto& m = Macro::get();
    m.mode = Mode::Recording;
    m.inputs.clear();
    m.cpStep.clear();
    m.step = 0;
    m.gameTime = 0;
    m.dtLogBudget = 8;
    pl->resetLevel(); // start the attempt fresh from the top
    log::info("[macro] recording started");
    notify("Macro: recording (practice-aware)", NotificationIcon::Info);
}

void stopRecording() {
    auto& m = Macro::get();
    m.mode = Mode::Idle;
    int lo = m.inputs.empty() ? 0 : m.inputs.front().step;
    int hi = m.inputs.empty() ? 0 : m.inputs.back().step;
    log::info("[macro] recording stopped: {} inputs, step range [{}..{}]", m.inputs.size(), lo, hi);
    for (size_t i = 0; i < m.inputs.size(); i++) {
        auto const& in = m.inputs[i];
        log::info("[macro]   rec #{} step={} btn={} down={} p1={}", i, in.step, in.button, in.down, in.player1);
    }
    notify(fmt::format("Macro: recorded {} inputs", m.inputs.size()), NotificationIcon::Success);
}

void startPlaying() {
    auto pl = PlayLayer::get();
    if (!pl) { notify("Macro: enter a level first", NotificationIcon::Error); return; }
    auto& m = Macro::get();
    if (m.inputs.empty()) { notify("Macro: nothing recorded yet", NotificationIcon::Error); return; }
    m.mode = Mode::Playing;
    m.playIndex = 0;
    m.injected = 0;
    m.step = 0;
    m.gameTime = 0;
    m.finishedNotified = false;
    m.logBudget = 45;
    pl->resetLevel(); // restart from the top
    log::info("[macro] playback started: {} inputs queued, first step={}, last step={}",
        m.inputs.size(), m.inputs.front().step, m.inputs.back().step);
    notify(fmt::format("Macro: playing {} inputs", m.inputs.size()), NotificationIcon::Info);
}

void stopPlaying() {
    Macro::get().mode = Mode::Idle;
    notify("Macro: playback stopped", NotificationIcon::Info);
}

} // namespace

// ---------------------------------------------------------------------------
// Input record / inject at the physics-step level.
// ---------------------------------------------------------------------------
class $modify(MacroBGL, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& m = Macro::get();
        if (m.mode == Mode::Recording) {
            m.inputs.push_back({ m.step, button, isPlayer1, down });
        } else if (m.mode == Mode::Playing) {
            return; // ignore the real user's input during playback
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // GD applies queued inputs here, once per physics step. Injecting at this
    // exact point (like Eclipse's bot) makes the replayed jump land at the same
    // physics moment GD applied it during recording.
    void processQueuedButtons(float dt, bool clearQueue) {
        auto& m = Macro::get();
        if (m.mode == Mode::Playing) {
            while (m.playIndex < m.inputs.size() && m.inputs[m.playIndex].step <= m.step) {
                auto const& in = m.inputs[m.playIndex];
                PlayerObject* p = in.player1 ? m_player1 : m_player2;
                if (p) {
                    auto btn = static_cast<PlayerButton>(in.button);
                    if (in.down) p->pushButton(btn);
                    else p->releaseButton(btn);
                }
                if (m.logBudget > 0) {
                    log::info("[macro] inject #{} recStep={} atMstep={} btn={} down={} player={}",
                        m.playIndex, in.step, m.step, in.button, in.down, p ? 1 : 0);
                    m.logBudget--;
                }
                m.injected++;
                m.playIndex++;
            }
            if (!m.finishedNotified && m.playIndex >= m.inputs.size() && !m.inputs.empty()) {
                m.finishedNotified = true;
                log::info("[macro] all inputs injected ({} total)", m.injected);
                notify(fmt::format("Macro: injected {} inputs", m.injected), NotificationIcon::Info);
            }
        }
        GJBaseGameLayer::processQueuedButtons(dt, clearQueue);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& m = Macro::get();
        if (m.mode != Mode::Idle) {
            // Index by GAME-TIME, not frame count, so recording while slowed
            // down (smaller dt per step) maps to the same step on normal-speed
            // playback. dt is the game-time this step advances (1/240 at 1x).
            m.gameTime += dt;
            m.step = static_cast<int>(std::llround(m.gameTime * 240.0));
            if (m.mode == Mode::Recording && m.dtLogBudget > 0) {
                log::info("[macro] rec dt={} gameTime={} step={}", dt, m.gameTime, m.step);
                m.dtLogBudget--;
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ---------------------------------------------------------------------------
// Per-attempt + practice-checkpoint bookkeeping.
// ---------------------------------------------------------------------------
class $modify(MacroPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        auto& m = Macro::get();
        int cpCount = m_checkpointArray ? m_checkpointArray->count() : 0;
        if (m.mode == Mode::Recording) {
            if (cpCount == 0) {
                // full restart from the top -> fresh recording
                m.inputs.clear();
                m.cpStep.clear();
                m.step = 0;
                m.gameTime = 0;
            }
            // else: practice respawn -> handled in loadFromCheckpoint
        } else if (m.mode == Mode::Playing) {
            log::info("[macro] playback attempt ended: reached step {}, injected {}/{}",
                m.step, m.playIndex, m.inputs.size());
            m.playIndex = 0;
            m.injected = 0;
            m.step = 0;
            m.gameTime = 0;
            m.finishedNotified = false;
            m.logBudget = 45;
        }
    }

    CheckpointObject* markCheckpoint() {
        auto cp = PlayLayer::markCheckpoint();
        auto& m = Macro::get();
        if (m.mode == Mode::Recording && cp) {
            m.cpStep[cp] = m.step;
            log::info("[macro] checkpoint @ step {}", m.step);
        }
        return cp;
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        auto& m = Macro::get();
        if (m.mode == Mode::Recording && cp) {
            auto it = m.cpStep.find(cp);
            if (it != m.cpStep.end()) {
                m.step = it->second;
                m.gameTime = m.step / 240.0; // keep game-time in sync with the rewind
                // drop the failed attempt's inputs recorded after this checkpoint
                while (!m.inputs.empty() && m.inputs.back().step >= m.step) {
                    m.inputs.pop_back();
                }
                log::info("[macro] respawn -> rewound to step {} ({} inputs kept)", m.step, m.inputs.size());
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Rebindable keybinds: toggle record / toggle play.
// ---------------------------------------------------------------------------
$execute {
    listenForKeybindSettingPresses("macro-record", [](Keybind const&, bool down, bool repeat, double) -> bool {
        if (down && !repeat) {
            if (Macro::get().mode == Mode::Recording) stopRecording();
            else startRecording();
            return true;
        }
        return false;
    });
    listenForKeybindSettingPresses("macro-play", [](Keybind const&, bool down, bool repeat, double) -> bool {
        if (down && !repeat) {
            if (Macro::get().mode == Mode::Playing) stopPlaying();
            else startPlaying();
            return true;
        }
        return false;
    });
}
