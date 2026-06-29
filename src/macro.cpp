#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <unordered_map>
#include <vector>
#include <sstream>
#include <cmath>

using namespace geode::prelude;

// ===========================================================================
// Macro record/replay (Phase 0/1 of the frame-perfect analyzer).
//
// Inputs are indexed by GAME-TIME (step = round(gameTime * 240)) so recording
// while the game is slowed down (speedhack) maps to the same step on normal-
// speed playback. Inputs are injected at PlayerObject::pushButton/releaseButton
// inside processQueuedButtons (the exact point GD applies real input).
//
// This version adds: disk persistence, RNG seed lock (determinism), and a
// desync detector that measures how far a replay drifts from the recording.
// ===========================================================================

namespace {

enum class Mode { Idle, Recording, Playing };

struct InputEdge {
    int step;     // game-time step from level start
    int button;   // PlayerButton (1 = jump)
    bool player1;
    bool down;    // press / release
};

struct Macro {
    Mode mode = Mode::Idle;
    std::vector<InputEdge> inputs;
    size_t playIndex = 0;
    int step = 0;
    double gameTime = 0;

    // determinism
    uint64_t seed1 = 0, seed2 = 0; // m_randomSeed, m_replayRandSeed
    bool haveSeed = false;

    // desync detection: recorded player-1 position per step
    std::vector<std::pair<float, float>> track;
    float maxDrift = 0.f;
    int maxDriftStep = -1;
    int desyncSteps = 0;

    // practice checkpoints
    std::unordered_map<CheckpointObject*, int> cpStep;

    bool finishedNotified = false;

    static Macro& get() { static Macro m; return m; }

    std::filesystem::path path() { return Mod::get()->getSaveDir() / "macro.txt"; }

    void save() {
        std::string out = fmt::format("FUTUREMOD_MACRO v1\n{} {}\n", seed1, seed2);
        for (auto const& in : inputs)
            out += fmt::format("{} {} {} {}\n", in.step, in.button, in.player1 ? 1 : 0, in.down ? 1 : 0);
        auto res = file::writeString(path(), out);
        if (!res) log::warn("[macro] save failed: {}", res.unwrapErr());
    }

    void load() {
        auto res = file::readString(path());
        if (!res) return;
        std::istringstream ss(res.unwrap());
        std::string line;
        if (!std::getline(ss, line) || line.rfind("FUTUREMOD_MACRO", 0) != 0) return;
        inputs.clear();
        if (std::getline(ss, line)) {
            std::istringstream s2(line);
            s2 >> seed1 >> seed2;
            haveSeed = (seed1 != 0 || seed2 != 0);
        }
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            std::istringstream s3(line);
            int step, btn, p1, dn;
            if (s3 >> step >> btn >> p1 >> dn)
                inputs.push_back({ step, btn, (bool)p1, (bool)dn });
        }
        log::info("[macro] loaded {} inputs from disk", inputs.size());
    }
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
    m.track.clear();
    m.step = 0;
    m.gameTime = 0;
    pl->resetLevel();
    notify("Macro: recording (practice-aware)", NotificationIcon::Info);
}

void stopRecording() {
    auto& m = Macro::get();
    m.mode = Mode::Idle;
    m.save();
    log::info("[macro] recorded {} inputs, steps [{}..{}], saved to disk",
        m.inputs.size(), m.inputs.empty() ? 0 : m.inputs.front().step,
        m.inputs.empty() ? 0 : m.inputs.back().step);
    notify(fmt::format("Macro: recorded + saved {} inputs", m.inputs.size()), NotificationIcon::Success);
}

void startPlaying() {
    auto pl = PlayLayer::get();
    if (!pl) { notify("Macro: enter a level first", NotificationIcon::Error); return; }
    auto& m = Macro::get();
    if (m.inputs.empty()) { notify("Macro: nothing recorded yet", NotificationIcon::Error); return; }
    m.mode = Mode::Playing;
    m.playIndex = 0;
    m.step = 0;
    m.gameTime = 0;
    m.finishedNotified = false;
    m.maxDrift = 0.f;
    m.maxDriftStep = -1;
    m.desyncSteps = 0;
    pl->resetLevel();
    notify(fmt::format("Macro: playing {} inputs", m.inputs.size()), NotificationIcon::Info);
}

void stopPlaying() {
    Macro::get().mode = Mode::Idle;
    notify("Macro: playback stopped", NotificationIcon::Info);
}

} // namespace

// ---------------------------------------------------------------------------
// Record / inject inputs; track positions for desync detection.
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
                m.playIndex++;
            }
            if (!m.finishedNotified && m.playIndex >= m.inputs.size() && !m.inputs.empty()) {
                m.finishedNotified = true;
                notify(fmt::format("Macro done. Max drift {:.1f}u @ step {}",
                    m.maxDrift, m.maxDriftStep), NotificationIcon::Info);
                log::info("[macro] playback complete: maxDrift={:.2f} @ step {}, desyncSteps={}",
                    m.maxDrift, m.maxDriftStep, m.desyncSteps);
            }
        }
        GJBaseGameLayer::processQueuedButtons(dt, clearQueue);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& m = Macro::get();
        if (m.mode != Mode::Idle) {
            m.gameTime += dt;
            m.step = static_cast<int>(std::llround(m.gameTime * 240.0));
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        // desync track: record P1 position while recording, compare while playing
        if (m.mode == Mode::Recording && m_player1) {
            auto pos = m_player1->getPosition();
            if (static_cast<int>(m.track.size()) <= m.step)
                m.track.resize(m.step + 1, { pos.x, pos.y });
            m.track[m.step] = { pos.x, pos.y };
        } else if (m.mode == Mode::Playing && m_player1 &&
                   m.step >= 0 && m.step < static_cast<int>(m.track.size())) {
            auto pos = m_player1->getPosition();
            auto rec = m.track[m.step];
            float dx = pos.x - rec.first, dy = pos.y - rec.second;
            float drift = std::sqrt(dx * dx + dy * dy);
            if (drift > 1.f) m.desyncSteps++;
            if (drift > m.maxDrift) { m.maxDrift = drift; m.maxDriftStep = m.step; }
        }
    }
};

// ---------------------------------------------------------------------------
// Per-attempt + practice-checkpoint bookkeeping; RNG seed capture/restore.
// ---------------------------------------------------------------------------
class $modify(MacroPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        auto& m = Macro::get();
        int cpCount = m_checkpointArray ? m_checkpointArray->count() : 0;
        if (m.mode == Mode::Recording) {
            if (cpCount == 0) {
                // full restart -> fresh recording; capture this attempt's seeds
                m.inputs.clear();
                m.cpStep.clear();
                m.track.clear();
                m.step = 0;
                m.gameTime = 0;
                m.seed1 = m_randomSeed;
                m.seed2 = m_replayRandSeed;
                m.haveSeed = true;
            }
            // else: practice respawn -> handled in loadFromCheckpoint
        } else if (m.mode == Mode::Playing) {
            m.playIndex = 0;
            m.step = 0;
            m.gameTime = 0;
            m.finishedNotified = false;
            m.maxDrift = 0.f;
            m.maxDriftStep = -1;
            m.desyncSteps = 0;
            // restore the recorded seeds so RNG-driven triggers match
            if (m.haveSeed) {
                m_randomSeed = m.seed1;
                m_replayRandSeed = m.seed2;
            }
        }
    }

    void startGame() {
        PlayLayer::startGame();
        auto& m = Macro::get();
        // re-apply seeds in case startGame re-seeds after resetLevel
        if (m.mode == Mode::Playing && m.haveSeed) {
            m_randomSeed = m.seed1;
            m_replayRandSeed = m.seed2;
        }
    }

    CheckpointObject* markCheckpoint() {
        auto cp = PlayLayer::markCheckpoint();
        auto& m = Macro::get();
        if (m.mode == Mode::Recording && cp) m.cpStep[cp] = m.step;
        return cp;
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        auto& m = Macro::get();
        if (m.mode == Mode::Recording && cp) {
            auto it = m.cpStep.find(cp);
            if (it != m.cpStep.end()) {
                m.step = it->second;
                m.gameTime = m.step / 240.0;
                while (!m.inputs.empty() && m.inputs.back().step >= m.step) m.inputs.pop_back();
                if (static_cast<int>(m.track.size()) > m.step) m.track.resize(m.step);
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Keybinds + load any saved macro on startup.
// ---------------------------------------------------------------------------
$execute {
    Macro::get().load();

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
