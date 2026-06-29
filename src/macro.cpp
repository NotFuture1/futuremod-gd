#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <unordered_map>
#include <vector>
#include <string>
#include <sstream>
#include <cmath>

using namespace geode::prelude;

// ===========================================================================
// Macro record/replay + frame-perfect analyzer.
//
// - Inputs indexed by GAME-TIME (step = round(gameTime*240)) so recording while
//   slowed down maps to the same step at normal speed.
// - Injected via PlayerObject::pushButton/releaseButton in processQueuedButtons.
// - Disk persistence, RNG seed lock, desync detector.
// - Analyzer: for each press, shift it +/- k ticks and re-run (fast-forwarded);
//   the widest unbroken surviving range = the timing window. window<=1 tick is
//   frame-perfect at 240, <=2 at 120, <=4 at 60.
// ===========================================================================

namespace {

constexpr int kMargin = 48;  // ticks past the input to confirm "survived"
constexpr int kMaxK   = 8;   // max offset tested each direction
constexpr int kFF     = 6;   // analysis fast-forward factor
constexpr int kCap    = 6000; // safety cap on total test runs

enum class Mode { Idle, Recording, Playing, Analyzing };

struct InputEdge {
    int step;
    int button;
    bool player1;
    bool down;
};

struct Macro {
    Mode mode = Mode::Idle;
    std::vector<InputEdge> inputs;
    size_t playIndex = 0;
    int step = 0;
    double gameTime = 0;

    // determinism
    uint64_t seed1 = 0, seed2 = 0;
    bool haveSeed = false;

    // desync detection
    std::vector<std::pair<float, float>> track;
    float maxDrift = 0.f;
    int maxDriftStep = -1;
    int desyncSteps = 0;
    bool finishedNotified = false;

    // practice checkpoints
    std::unordered_map<CheckpointObject*, int> cpStep;

    // --- analyzer ---
    std::vector<size_t> targets;   // input indices to probe (presses)
    std::vector<int> windowTicks;  // result window per target
    size_t targetIdx = 0;
    int offset = 0, dir = 0, minSurv = 0, maxSurv = 0;
    int marginEnd = 0, testCount = 0;
    bool died = false, testResolved = false, lastSurvived = false;

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

// ---- recording / playback control ----------------------------------------

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

// ---- analyzer --------------------------------------------------------------

void beginTest() {
    auto& m = Macro::get();
    m.testResolved = false;
    m.died = false;
    int tstep = m.inputs[m.targets[m.targetIdx]].step;
    m.marginEnd = tstep + kMargin;
    if (auto pl = PlayLayer::get()) pl->resetLevel();
}

void finishAnalysis() {
    auto& m = Macro::get();
    m.mode = Mode::Idle;
    bool s240 = Mod::get()->getSettingValue<bool>("show-240");
    bool s120 = Mod::get()->getSettingValue<bool>("show-120");
    bool s60  = Mod::get()->getSettingValue<bool>("show-60");
    if (!s240 && !s120 && !s60) s240 = true;

    int c240 = 0, c120 = 0, c60 = 0;
    for (size_t i = 0; i < m.targets.size(); i++) {
        int w = m.windowTicks[i];
        if (w <= 1) c240++;
        if (w <= 2) c120++;
        if (w <= 4) c60++;
        log::info("[fp] input #{} step {} window={} ticks", m.targets[i], m.inputs[m.targets[i]].step, w);
    }
    std::string disp;
    if (s240) disp += fmt::format("240:{}  ", c240);
    if (s120) disp += fmt::format("120:{}  ", c120);
    if (s60)  disp += fmt::format("60:{}", c60);
    log::info("[fp] frame perfects -> {}", disp);
    notify(fmt::format("Frame perfects  {}", disp), NotificationIcon::Success);
}

void advanceAnalysis(bool survived) {
    auto& m = Macro::get();
    if (++m.testCount > kCap) { log::warn("[fp] hit test cap"); finishAnalysis(); return; }

    bool finalize = false;
    if (m.dir < 0) { // probing earlier
        if (survived) {
            m.minSurv = m.offset;
            m.offset -= 1;
            if (m.offset < -kMaxK) { m.dir = 1; m.offset = 1; }
        } else {
            m.dir = 1; m.offset = 1; // earlier boundary found
        }
    } else {          // probing later
        if (survived) {
            m.maxSurv = m.offset;
            m.offset += 1;
            if (m.offset > kMaxK) finalize = true;
        } else {
            finalize = true;
        }
    }

    if (finalize) {
        m.windowTicks[m.targetIdx] = m.maxSurv - m.minSurv + 1;
        m.targetIdx++;
        if (m.targetIdx >= m.targets.size()) { finishAnalysis(); return; }
        m.dir = -1; m.offset = -1; m.minSurv = 0; m.maxSurv = 0;
    }
    beginTest();
}

void startAnalysis() {
    auto pl = PlayLayer::get();
    if (!pl) { notify("Analyze: enter a level first", NotificationIcon::Error); return; }
    auto& m = Macro::get();
    if (m.inputs.empty()) { notify("Analyze: record a run first", NotificationIcon::Error); return; }
    m.targets.clear();
    for (size_t i = 0; i < m.inputs.size(); i++)
        if (m.inputs[i].down) m.targets.push_back(i);
    if (m.targets.empty()) { notify("Analyze: no press inputs", NotificationIcon::Error); return; }
    m.windowTicks.assign(m.targets.size(), 1);
    m.targetIdx = 0; m.dir = -1; m.offset = -1; m.minSurv = 0; m.maxSurv = 0; m.testCount = 0;
    m.mode = Mode::Analyzing;
    notify(fmt::format("Analyzing {} inputs... (turn OFF noclip/speedhack)", m.targets.size()), NotificationIcon::Info);
    beginTest();
}

} // namespace

// ---------------------------------------------------------------------------
class $modify(MacroBGL, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& m = Macro::get();
        if (m.mode == Mode::Recording) {
            m.inputs.push_back({ m.step, button, isPlayer1, down });
        } else if (m.mode == Mode::Playing || m.mode == Mode::Analyzing) {
            return; // ignore the real user during playback/analysis
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processQueuedButtons(float dt, bool clearQueue) {
        auto& m = Macro::get();
        if (m.mode == Mode::Playing || m.mode == Mode::Analyzing) {
            int target = (m.mode == Mode::Analyzing && !m.targets.empty())
                ? static_cast<int>(m.targets[m.targetIdx]) : -1;
            while (m.playIndex < m.inputs.size()) {
                int eff = m.inputs[m.playIndex].step
                    + (static_cast<int>(m.playIndex) == target ? m.offset : 0);
                if (eff > m.step) break;
                auto const& in = m.inputs[m.playIndex];
                PlayerObject* p = in.player1 ? m_player1 : m_player2;
                if (p) {
                    auto btn = static_cast<PlayerButton>(in.button);
                    if (in.down) p->pushButton(btn);
                    else p->releaseButton(btn);
                }
                m.playIndex++;
            }
            if (m.mode == Mode::Playing && !m.finishedNotified
                && m.playIndex >= m.inputs.size() && !m.inputs.empty()) {
                m.finishedNotified = true;
                notify(fmt::format("Macro done. Max drift {:.1f}u @ step {}",
                    m.maxDrift, m.maxDriftStep), NotificationIcon::Info);
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
        } else if (m.mode == Mode::Analyzing && !m.testResolved) {
            if (m.died) { m.testResolved = true; m.lastSurvived = false; }
            else if (m.step >= m.marginEnd) { m.testResolved = true; m.lastSurvived = true; }
        }
    }

    void update(float dt) {
        auto& m = Macro::get();
        if (m.mode == Mode::Analyzing) {
            for (int i = 0; i < kFF && !m.testResolved; i++)
                GJBaseGameLayer::update(dt);
            if (m.testResolved) {
                m.testResolved = false;
                advanceAnalysis(m.lastSurvived);
            }
        } else {
            GJBaseGameLayer::update(dt);
        }
    }
};

// ---------------------------------------------------------------------------
class $modify(MacroPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        auto& m = Macro::get();
        int cpCount = m_checkpointArray ? m_checkpointArray->count() : 0;
        if (m.mode == Mode::Recording) {
            if (cpCount == 0) {
                m.inputs.clear();
                m.cpStep.clear();
                m.track.clear();
                m.step = 0;
                m.gameTime = 0;
                m.seed1 = m_randomSeed;
                m.seed2 = m_replayRandSeed;
                m.haveSeed = true;
            }
        } else if (m.mode == Mode::Playing) {
            m.playIndex = 0;
            m.step = 0;
            m.gameTime = 0;
            m.finishedNotified = false;
            m.maxDrift = 0.f;
            m.maxDriftStep = -1;
            m.desyncSteps = 0;
            if (m.haveSeed) { m_randomSeed = m.seed1; m_replayRandSeed = m.seed2; }
        } else if (m.mode == Mode::Analyzing) {
            m.playIndex = 0;
            m.step = 0;
            m.gameTime = 0;
            m.died = false;
            if (m.haveSeed) { m_randomSeed = m.seed1; m_replayRandSeed = m.seed2; }
        }
    }

    void startGame() {
        PlayLayer::startGame();
        auto& m = Macro::get();
        if ((m.mode == Mode::Playing || m.mode == Mode::Analyzing) && m.haveSeed) {
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

    void destroyPlayer(PlayerObject* p, GameObject* o) {
        PlayLayer::destroyPlayer(p, o);
        if (Macro::get().mode == Mode::Analyzing) Macro::get().died = true;
    }

    void levelComplete() {
        auto& m = Macro::get();
        if (m.mode == Mode::Analyzing) {
            // count as "survived"; don't actually end the level mid-analysis
            m.testResolved = true;
            m.lastSurvived = true;
            return;
        }
        PlayLayer::levelComplete();
    }
};

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
    listenForKeybindSettingPresses("macro-analyze", [](Keybind const&, bool down, bool repeat, double) -> bool {
        if (down && !repeat) {
            if (Macro::get().mode == Mode::Analyzing) { Macro::get().mode = Mode::Idle; notify("Analyze: cancelled", NotificationIcon::Info); }
            else startAnalysis();
            return true;
        }
        return false;
    });
}
