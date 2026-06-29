#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/loader/SettingV3.hpp>

using namespace geode::prelude;

// ===========================================================================
// Phase 0 of the frame-perfect analyzer: a deterministic macro record/replay
// harness.
//
// Inputs are indexed by the PHYSICS STEP counter (GJBaseGameLayer::m_currentStep),
// NOT by wall-clock time. This is what makes recording while the game is slowed
// down (speedhack / other mods) work correctly: slowing the game only changes
// how fast steps elapse in real time, never the step *sequence*. A click on
// step N is replayed on step N at any playback speed.
//
// Steps are stored RELATIVE to the start of the attempt (a baseline captured
// right after resetLevel), so the macro is independent of any global step
// counter offset.
// ===========================================================================

namespace {

enum class MacroMode { Idle, Recording, Playing };

struct InputEdge {
    int step;     // physics step, relative to attempt start
    int button;   // PlayerButton (1 = jump, 2 = left, 3 = right) -- round-tripped
    bool player1; // which player
    bool down;    // press (true) or release (false)
};

struct Macro {
    MacroMode mode = MacroMode::Idle;
    std::vector<InputEdge> inputs;
    size_t playIndex = 0;
    int baseStep = 0;        // m_currentStep at the start of the attempt
    bool finishedNotified = false;

    static Macro& get() {
        static Macro inst;
        return inst;
    }
};

void notify(std::string const& msg, NotificationIcon icon) {
    Notification::create(msg, icon)->show();
}

void startRecording() {
    auto pl = PlayLayer::get();
    if (!pl) { notify("Macro: enter a level first", NotificationIcon::Error); return; }
    auto& m = Macro::get();
    m.mode = MacroMode::Recording;
    m.inputs.clear();
    pl->resetLevel(); // restart the attempt; resetLevel hook sets baseStep
    notify("Macro: recording (restarts on death)", NotificationIcon::Info);
}

void stopRecording() {
    auto& m = Macro::get();
    m.mode = MacroMode::Idle;
    notify(fmt::format("Macro: recorded {} inputs", m.inputs.size()), NotificationIcon::Success);
}

void startPlaying() {
    auto pl = PlayLayer::get();
    if (!pl) { notify("Macro: enter a level first", NotificationIcon::Error); return; }
    auto& m = Macro::get();
    if (m.inputs.empty()) { notify("Macro: nothing recorded yet", NotificationIcon::Error); return; }
    m.mode = MacroMode::Playing;
    m.playIndex = 0;
    m.finishedNotified = false;
    pl->resetLevel(); // restart from the top; resetLevel hook sets baseStep
    notify(fmt::format("Macro: playing {} inputs", m.inputs.size()), NotificationIcon::Info);
}

void stopPlaying() {
    auto& m = Macro::get();
    m.mode = MacroMode::Idle;
    notify("Macro: playback stopped", NotificationIcon::Info);
}

} // namespace

// ---------------------------------------------------------------------------
// Record real inputs / inject recorded inputs at the physics-step level.
// ---------------------------------------------------------------------------
class $modify(MacroBGL, GJBaseGameLayer) {
    // handleButton is GD's "apply this button" entry point (press/release).
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& m = Macro::get();
        if (m.mode == MacroMode::Recording) {
            m.inputs.push_back({ this->m_currentStep - m.baseStep, button, isPlayer1, down });
            // keep playing while recording -- fall through to apply it
        } else if (m.mode == MacroMode::Playing) {
            return; // ignore the real user's input during playback
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // processCommands runs once per physics step; m_currentStep identifies it.
    // We fire any recorded inputs scheduled for the current step before the
    // step's physics runs, mirroring how real inputs are consumed.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& m = Macro::get();
        if (m.mode == MacroMode::Playing) {
            int cur = this->m_currentStep - m.baseStep;
            while (m.playIndex < m.inputs.size() && m.inputs[m.playIndex].step <= cur) {
                auto const& in = m.inputs[m.playIndex];
                // call the ORIGINAL directly so this injection isn't re-recorded
                GJBaseGameLayer::handleButton(in.down, in.button, in.player1);
                m.playIndex++;
            }
            if (!m.finishedNotified && m.playIndex >= m.inputs.size() && !m.inputs.empty()) {
                m.finishedNotified = true;
                notify("Macro: all inputs played", NotificationIcon::Info);
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ---------------------------------------------------------------------------
// Capture the per-attempt step baseline; restart record/replay on each attempt.
// ---------------------------------------------------------------------------
class $modify(MacroPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        auto& m = Macro::get();
        if (m.mode == MacroMode::Recording) {
            // a fresh attempt -> record it from scratch (captures the run you clear)
            m.inputs.clear();
            m.baseStep = this->m_currentStep;
        } else if (m.mode == MacroMode::Playing) {
            m.playIndex = 0;
            m.baseStep = this->m_currentStep;
            m.finishedNotified = false;
        }
    }
};

// ---------------------------------------------------------------------------
// Keybinds (rebindable, Geode-native): toggle record / toggle play.
// ---------------------------------------------------------------------------
$execute {
    listenForKeybindSettingPresses("macro-record", [](Keybind const&, bool down, bool repeat, double) -> bool {
        if (down && !repeat) {
            if (Macro::get().mode == MacroMode::Recording) stopRecording();
            else startRecording();
            return true;
        }
        return false;
    });
    listenForKeybindSettingPresses("macro-play", [](Keybind const&, bool down, bool repeat, double) -> bool {
        if (down && !repeat) {
            if (Macro::get().mode == MacroMode::Playing) stopPlaying();
            else startPlaying();
            return true;
        }
        return false;
    });
}
