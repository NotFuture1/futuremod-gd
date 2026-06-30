#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/Popup.hpp>
#include <unordered_map>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>

using namespace geode::prelude;

// ===========================================================================
// Macro record/replay + frame-perfect analyzer.
//
// Analyzer: replay the run unshifted (baseline) to confirm it's deterministic,
// then for each press shift it +/- k ticks and re-run; the widest unbroken
// surviving range = the timing window. window<=1 tick = frame-perfect@240,
// <=2 = @120, <=4 = @60. Live HUD (top-right) + sound while it runs.
// ===========================================================================

namespace {

constexpr int kMargin = 48;
constexpr int kMaxK   = 8;
constexpr int kFF     = 1;   // 1 = no fast-forward (FF corrupts physics -> false deaths)
constexpr int kCap    = 8000;
constexpr int kStall  = 24;   // steps with no forward progress => the player is dead
constexpr float kBack = 8.f;  // x dropping this far => respawned (also a death)
constexpr int kPre    = kMaxK + 4; // ticks before a jump to snapshot (room to shift early)

enum class Mode { Idle, Recording, Playing, Analyzing };

struct InputEdge {
    int step;
    int button;
    bool player1;
    bool down;
};

// Per-level macro files: macros/<key>.txt under the mod save dir.
std::string sanitizeKey(std::string const& s) {
    std::string o;
    for (char c : s) o += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return o.empty() ? std::string("level") : o;
}

std::string levelKeyFor(GJGameLevel* lvl) {
    if (!lvl) return "unknown";
    int id = lvl->m_levelID.value();
    if (id > 0) return std::to_string(id);                  // online levels
    return "local_" + sanitizeKey(std::string(lvl->m_levelName)); // local levels
}

std::filesystem::path macrosDir() {
    auto d = Mod::get()->getSaveDir() / "macros";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}

std::filesystem::path pathForKey(std::string const& key) {
    return macrosDir() / (key + ".txt");
}

struct Macro {
    Mode mode = Mode::Idle;
    std::vector<InputEdge> inputs;
    size_t playIndex = 0;
    int step = 0;
    double gameTime = 0;

    uint64_t seed1 = 0, seed2 = 0;
    bool haveSeed = false;

    // desync detection (playback)
    std::vector<std::pair<float, float>> track;
    float maxDrift = 0.f;
    int maxDriftStep = -1;
    bool finishedNotified = false;

    std::unordered_map<CheckpointObject*, int> cpStep;

    // --- analyzer ---
    std::vector<size_t> targets;
    std::vector<int> windowTicks;
    size_t targetIdx = 0;
    int offset = 0, dir = 0, minSurv = 0, maxSurv = 0;
    int marginEnd = 0, testCount = 0, lastTargetStep = 0;
    int testFrames = 0; // real frames spent on the current test (hang guard)
    bool baseline = false;
    bool died = false, testResolved = false, lastSurvived = false;
    int deathStep = -1;
    // death = "player stopped advancing", measured from position (NOT destroyPlayer,
    // which can fire without actually killing on some levels/setups).
    float maxX = -1.f;        // furthest x reached this test
    int lastProgressStep = 0; // step at which maxX last increased
    // --- save-state acceleration: snapshot just before each jump so per-jump tests
    //     simulate ~30 ticks instead of replaying the whole level from the start ---
    std::vector<CheckpointObject*> jumpCps; // one snapshot per target jump (retained)
    std::vector<int> jumpCpStep;            // game-step each snapshot was taken at
    size_t nextCpIdx = 0;                   // next snapshot to capture during baseline
    bool useCheckpoints = false;
    bool wasPractice = false;               // restore the user's practice toggle after
    int c240 = 0, c120 = 0, c60 = 0;

    // analysis results that persist for the live playback tally
    std::vector<std::pair<int, int>> savedWindows; // (input index, window ticks)
    // live playback tally: sorted steps of frame-perfect jumps per rate
    std::vector<int> fp240, fp120, fp60;
    size_t pi240 = 0, pi120 = 0, pi60 = 0;

    std::string curKey; // current level's key (set on PlayLayer::init)

    static Macro& get() { static Macro m; return m; }

    std::filesystem::path path() { return pathForKey(curKey); }

    void buildFpLists() {
        fp240.clear(); fp120.clear(); fp60.clear();
        for (auto const& pr : savedWindows) {
            int idx = pr.first, w = pr.second;
            if (idx < 0 || idx >= static_cast<int>(inputs.size())) continue;
            int st = inputs[idx].step;
            if (w <= 1) fp240.push_back(st);
            if (w <= 2) fp120.push_back(st);
            if (w <= 4) fp60.push_back(st);
        }
        std::sort(fp240.begin(), fp240.end());
        std::sort(fp120.begin(), fp120.end());
        std::sort(fp60.begin(), fp60.end());
        pi240 = pi120 = pi60 = 0;
    }

    void save() {
        std::string out = fmt::format("FUTUREMOD_MACRO v2\n{} {}\n", seed1, seed2);
        for (auto const& in : inputs)
            out += fmt::format("{} {} {} {}\n", in.step, in.button, in.player1 ? 1 : 0, in.down ? 1 : 0);
        for (auto const& pr : savedWindows)
            out += fmt::format("W {} {}\n", pr.first, pr.second);
        auto res = file::writeString(path(), out);
        if (!res) log::warn("[macro] save failed: {}", res.unwrapErr());
    }

    // Load the macro saved for a given level key (clears in-memory macro if none).
    void loadForLevel(std::string const& key) {
        curKey = key;
        inputs.clear();
        savedWindows.clear();
        haveSeed = false;
        seed1 = seed2 = 0;
        auto res = file::readString(pathForKey(key));
        if (!res) { log::info("[macro] no macro for level '{}'", key); return; }
        std::istringstream ss(res.unwrap());
        std::string line;
        if (!std::getline(ss, line) || line.rfind("FUTUREMOD_MACRO", 0) != 0) return;
        if (std::getline(ss, line)) {
            std::istringstream s2(line);
            s2 >> seed1 >> seed2;
            haveSeed = (seed1 != 0 || seed2 != 0);
        }
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            if (line[0] == 'W') {
                std::istringstream sw(line);
                std::string tag; int idx, w;
                if (sw >> tag >> idx >> w) savedWindows.push_back({ idx, w });
            } else {
                std::istringstream s3(line);
                int step, btn, p1, dn;
                if (s3 >> step >> btn >> p1 >> dn)
                    inputs.push_back({ step, btn, (bool)p1, (bool)dn });
            }
        }
        log::info("[macro] level '{}': loaded {} inputs, {} windows", key, inputs.size(), savedWindows.size());
    }
};

void notify(std::string const& msg, NotificationIcon icon) {
    Notification::create(msg, icon)->show();
}

void playDing() {
    if (auto fae = FMODAudioEngine::sharedEngine())
        fae->playEffect("achievement_01.ogg");
}

void updateHud() {
    auto& m = Macro::get();
    auto pl = PlayLayer::get();
    if (!pl) return;

    bool s240 = Mod::get()->getSettingValue<bool>("show-240");
    bool s120 = Mod::get()->getSettingValue<bool>("show-120");
    bool s60  = Mod::get()->getSettingValue<bool>("show-60");
    if (!s240 && !s120 && !s60) s240 = true;

    std::string s;
    if (m.mode == Mode::Analyzing) {
        if (s240) s += fmt::format("FP@240: {}\n", m.c240);
        if (s120) s += fmt::format("FP@120: {}\n", m.c120);
        if (s60)  s += fmt::format("FP@60: {}",   m.c60);
    } else { // playback live tally: passed / total
        if (s240) s += fmt::format("FP@240: {}/{}\n", m.pi240, m.fp240.size());
        if (s120) s += fmt::format("FP@120: {}/{}\n", m.pi120, m.fp120.size());
        if (s60)  s += fmt::format("FP@60: {}/{}",    m.pi60,  m.fp60.size());
    }

    // Look the label up by tag on the CURRENT PlayLayer each time -- never hold
    // a raw pointer across levels (the layer frees its children on exit).
    constexpr int kHudTag = 0x4D504650; // 'MPFP'
    auto hud = static_cast<CCLabelBMFont*>(pl->getChildByTag(kHudTag));
    if (!hud) {
        hud = CCLabelBMFont::create(" ", "bigFont.fnt");
        if (!hud) return;
        hud->setTag(kHudTag);
        hud->setAnchorPoint({ 1.f, 1.f });
        auto win = CCDirector::sharedDirector()->getWinSize();
        hud->setPosition(win.width - 6.f, win.height - 6.f);
        hud->setScale(0.45f);
        hud->setZOrder(10000);
        pl->addChild(hud);
    }
    hud->setString(s.c_str());
}

// ---- recording / playback --------------------------------------------------

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
    m.buildFpLists(); // live frame-perfect tally from the last analysis
    pl->resetLevel();
    if (!m.savedWindows.empty()) updateHud();
    notify(fmt::format("Macro: playing {} inputs", m.inputs.size()), NotificationIcon::Info);
}

void stopPlaying() {
    Macro::get().mode = Mode::Idle;
    notify("Macro: playback stopped", NotificationIcon::Info);
}

// ---- analyzer --------------------------------------------------------------

void clearJumpCps() {
    auto& m = Macro::get();
    for (auto cp : m.jumpCps) if (cp) cp->release();
    m.jumpCps.clear();
    m.jumpCpStep.clear();
    m.nextCpIdx = 0;
}

void beginTest() {
    auto& m = Macro::get();
    m.testResolved = false;
    m.died = false;
    m.deathStep = -1;
    m.testFrames = 0;
    m.marginEnd = m.baseline
        ? (m.lastTargetStep + kMargin)
        : (m.inputs[m.targets[m.targetIdx]].step + kMargin);
    auto pl = PlayLayer::get();
    bool fast = !m.baseline && m.useCheckpoints
        && m.targetIdx < m.jumpCps.size() && m.jumpCps[m.targetIdx];
    if (fast && pl) {
        // FAST PATH: restore the snapshot taken just before this jump and replay
        // only the short window around it (the "illusion"), not the whole level.
        pl->loadFromCheckpoint(m.jumpCps[m.targetIdx]);
        m.step = m.jumpCpStep[m.targetIdx];
        m.gameTime = m.step / 240.0;
        m.playIndex = 0;
        while (m.playIndex < m.inputs.size()
            && static_cast<int>(m.inputs[m.playIndex].step) < m.step) m.playIndex++;
        m.maxX = -1.f;
        m.lastProgressStep = m.step;
        if (m.targetIdx == 0) { // log the first jump's restores to verify resume
            float px = pl->m_player1 ? pl->m_player1->getPosition().x : -1.f;
            log::info("[fp] test jump#0 off {} loaded cp@step{} px={:.0f}",
                m.offset, m.step, px);
        }
    } else {
        if (pl) pl->resetLevel();      // baseline (or fallback): full run from start
        m.maxX = -1.f;
        m.lastProgressStep = 0;
    }
}

void finishAnalysis() {
    auto& m = Macro::get();
    m.mode = Mode::Idle;
    m.savedWindows.clear();
    for (size_t i = 0; i < m.targets.size(); i++) {
        log::info("[fp] input #{} step {} window={} ticks",
            m.targets[i], m.inputs[m.targets[i]].step, m.windowTicks[i]);
        m.savedWindows.push_back({ static_cast<int>(m.targets[i]), m.windowTicks[i] });
    }
    log::info("[fp] DONE -> 240:{} 120:{} 60:{}", m.c240, m.c120, m.c60);
    m.save(); // persist windows so the live playback tally survives restarts
    clearJumpCps();
    if (auto pl = PlayLayer::get()) {
        pl->removeAllCheckpoints();
        if (!m.wasPractice) pl->togglePracticeMode(false);
    }
    updateHud();
    playDing();
    notify(fmt::format("Analysis done. 240:{} 120:{} 60:{}", m.c240, m.c120, m.c60), NotificationIcon::Success);
}

void advanceAnalysis(bool survived) {
    auto& m = Macro::get();
    if (++m.testCount > kCap) { log::warn("[fp] hit test cap"); finishAnalysis(); return; }

    bool finalize = false;
    if (m.dir < 0) {
        if (survived) {
            m.minSurv = m.offset;
            m.offset -= 1;
            if (m.offset < -kMaxK) { m.dir = 1; m.offset = 1; }
        } else {
            m.dir = 1; m.offset = 1;
        }
    } else {
        if (survived) {
            m.maxSurv = m.offset;
            m.offset += 1;
            if (m.offset > kMaxK) finalize = true;
        } else {
            finalize = true;
        }
    }

    if (finalize) {
        int w = m.maxSurv - m.minSurv + 1;
        m.windowTicks[m.targetIdx] = w;
        if (w <= 1) m.c240++;
        if (w <= 2) m.c120++;
        if (w <= 4) m.c60++;
        if (w <= 1) playDing();
        updateHud();
        m.targetIdx++;
        if (m.targetIdx >= m.targets.size()) { finishAnalysis(); return; }
        m.dir = -1; m.offset = -1; m.minSurv = 0; m.maxSurv = 0;
    }
    beginTest();
}

void onTestResolved() {
    auto& m = Macro::get();
    if (m.baseline) {
        m.baseline = false;
        if (!m.lastSurvived) {
            m.mode = Mode::Idle;
            clearJumpCps();
            if (auto pl = PlayLayer::get()) {
                pl->removeAllCheckpoints();
                if (!m.wasPractice) pl->togglePracticeMode(false);
            }
            log::warn("[fp] baseline stopped progressing @ step {} (maxX {:.0f})",
                m.deathStep, m.maxX);
            std::string msg;
            if (m.deathStep < 0)
                msg = "Can't analyze: the replay didn't reach the end in time (timed out).\n"
                      "If you're using speedhack, set the game speed to 1x for analysis.";
            else if (m.deathStep <= 30)
                msg = fmt::format("Can't analyze: the replay stalls right at the start (step {}).\n"
                              "The recorded run isn't replaying cleanly from the beginning.\n"
                              "Try re-recording a fresh run, then analyze.", m.deathStep);
            else
                msg = fmt::format("Can't analyze: the replay drifts off at step {} and stops\n"
                              "progressing. The replay isn't reproducing your run exactly --\n"
                              "re-record a clean run and try again.", m.deathStep);
            notify(msg, NotificationIcon::Error);
            return;
        }
        // baseline clean -> start probing the first target
        size_t got = 0;
        for (auto cp : m.jumpCps) if (cp) got++;
        m.useCheckpoints = (got == m.jumpCps.size() && got > 0);
        log::info("[fp] baseline OK (reached step {}, maxX {:.0f}) -> probing {} jumps, "
            "save-states {}/{} ({})", m.step, m.maxX, m.targets.size(),
            got, m.jumpCps.size(), m.useCheckpoints ? "FAST" : "from-start fallback");
        m.targetIdx = 0; m.dir = -1; m.offset = -1; m.minSurv = 0; m.maxSurv = 0;
        beginTest();
        return;
    }
    advanceAnalysis(m.lastSurvived);
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
    m.windowTicks.assign(m.targets.size(), 0);
    m.lastTargetStep = m.inputs[m.targets.back()].step;
    m.testCount = 0;
    m.c240 = m.c120 = m.c60 = 0;
    // schedule a save-state kPre ticks before each jump; captured during the baseline.
    clearJumpCps();
    m.jumpCps.assign(m.targets.size(), nullptr);
    m.jumpCpStep.assign(m.targets.size(), 0);
    for (size_t i = 0; i < m.targets.size(); i++)
        m.jumpCpStep[i] = std::max(0, static_cast<int>(m.inputs[m.targets[i]].step) - kPre);
    m.useCheckpoints = false; // enabled after baseline confirms all snapshots captured
    // clear practice checkpoints so every test starts from the LEVEL START,
    // not from a mid-level checkpoint (which would put the run into a wall).
    int cps = pl->m_checkpointArray ? pl->m_checkpointArray->count() : 0;
    log::info("[fp] startAnalysis: {} inputs, {} checkpoints (clearing)", m.inputs.size(), cps);
    // analysis runs in practice mode so loadFromCheckpoint cleanly respawns+resumes
    // the player (outside practice it reloads into a dead/frozen state).
    m.wasPractice = pl->m_isPracticeMode;
    if (!m.wasPractice) pl->togglePracticeMode(true);
    pl->removeAllCheckpoints();
    m.baseline = true;       // first run is the unshifted determinism check
    m.mode = Mode::Analyzing;
    updateHud();
    notify(fmt::format("Analyzing {} jumps... TURN OFF speedhack/noclip", m.targets.size()), NotificationIcon::Info);
    beginTest(); // baseline run
}

} // namespace

// ---------------------------------------------------------------------------
class $modify(MacroBGL, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& m = Macro::get();
        if (m.mode == Mode::Recording) {
            m.inputs.push_back({ m.step, button, isPlayer1, down });
        } else if (m.mode == Mode::Playing || m.mode == Mode::Analyzing) {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processQueuedButtons(float dt, bool clearQueue) {
        auto& m = Macro::get();
        if (m.mode == Mode::Playing || m.mode == Mode::Analyzing) {
            int target = (m.mode == Mode::Analyzing && !m.baseline && !m.targets.empty())
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
                // the drift track only exists for a macro recorded this session
                if (m.track.empty())
                    notify("Macro playback finished", NotificationIcon::Info);
                else
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
        } else if (m.mode == Mode::Playing) {
            if (m.step >= 1 && m.step <= 6 && m_player1) {
                auto pos = m_player1->getPosition();
                log::info("[fp] PLAY step={} x={:.0f} y={:.0f}", m.step, pos.x, pos.y);
            }
            if (m_player1 && m.step >= 0 && m.step < static_cast<int>(m.track.size())) {
                auto pos = m_player1->getPosition();
                auto rec = m.track[m.step];
                float dx = pos.x - rec.first, dy = pos.y - rec.second;
                float drift = std::sqrt(dx * dx + dy * dy);
                if (drift > m.maxDrift) { m.maxDrift = drift; m.maxDriftStep = m.step; }
            }
            // live frame-perfect tally: count as we pass each FP click, ding only for
            // the rates the user has selected to display.
            bool a240 = false, a120 = false, a60 = false;
            while (m.pi240 < m.fp240.size() && m.fp240[m.pi240] <= m.step) { m.pi240++; a240 = true; }
            while (m.pi120 < m.fp120.size() && m.fp120[m.pi120] <= m.step) { m.pi120++; a120 = true; }
            while (m.pi60  < m.fp60.size()  && m.fp60[m.pi60]   <= m.step) { m.pi60++;  a60  = true; }
            bool s240 = Mod::get()->getSettingValue<bool>("show-240");
            bool s120 = Mod::get()->getSettingValue<bool>("show-120");
            bool s60  = Mod::get()->getSettingValue<bool>("show-60");
            if (!s240 && !s120 && !s60) s240 = true; // mirror the HUD default
            if ((s240 && a240) || (s120 && a120) || (s60 && a60)) playDing();
            if (a240 || a120 || a60) updateHud();
        } else if (m.mode == Mode::Analyzing && !m.testResolved) {
            if (m.baseline && m.step >= 1 && m.step <= 6 && m_player1) {
                auto pos = m_player1->getPosition();
                float tx = (m.step < static_cast<int>(m.track.size())) ? m.track[m.step].first : -1.f;
                log::info("[fp] base step={} x={:.0f} trackx={:.0f}", m.step, pos.x, tx);
            }
            // During the single baseline pass, snapshot the game state just before each
            // jump so per-jump tests can resume from there instead of from the start.
            if (m.baseline) {
                while (m.nextCpIdx < m.jumpCpStep.size()
                    && m.step >= m.jumpCpStep[m.nextCpIdx]) {
                    if (auto pl = PlayLayer::get()) {
                        auto cp = pl->markCheckpoint(); // real practice snapshot (full state)
                        if (cp) cp->retain();
                        m.jumpCps[m.nextCpIdx] = cp;
                    }
                    m.nextCpIdx++;
                }
            }
            // Death = the player stopped advancing. This ignores destroyPlayer entirely
            // (it can fire without killing), so a player that keeps moving is "alive".
            float cx = m_player1 ? m_player1->getPosition().x : m.maxX;
            // forward progress resets the hang-guard, so long levels never time out;
            // the guard only trips when the run is genuinely stuck (no progress).
            if (cx > m.maxX + 0.2f) { m.maxX = cx; m.lastProgressStep = m.step; m.testFrames = 0; }
            bool respawned = (cx < m.maxX - kBack);                  // x jumped backward
            bool frozen = (m.step - m.lastProgressStep) >= kStall;   // x stuck in place
            bool dead = respawned || frozen;
            if (dead) m.deathStep = m.lastProgressStep;

            if (m.baseline) {
                if (dead) { m.testResolved = true; m.lastSurvived = false; }
                else if (m.step >= m.marginEnd) { m.testResolved = true; m.lastSurvived = true; }
            } else if (dead) {
                m.testResolved = true;
                // a death only counts against this jump if we actually reached it;
                // an earlier death is upstream desync, not the shift's doing.
                int tstep = m.inputs[m.targets[m.targetIdx]].step;
                m.lastSurvived = (m.deathStep < tstep);
            } else if (m.step >= m.marginEnd) {
                m.testResolved = true;
                m.lastSurvived = true;
            }
        }
    }

    void update(float dt) {
        auto& m = Macro::get();
        if (m.mode == Mode::Analyzing) {
            for (int i = 0; i < kFF && !m.testResolved; i++)
                GJBaseGameLayer::update(dt);
            // hang guard: a test that never resolves (stuck/desync) is treated
            // as "survived" so it can't fabricate a frame-perfect, then we move on.
            if (!m.testResolved && ++m.testFrames > 2400) {
                log::warn("[fp] test timeout (target {}, offset {})", m.targetIdx, m.offset);
                m.testResolved = true;
                m.lastSurvived = !m.baseline; // baseline that never settles = desync, not a pass
            }
            if (m.testResolved) {
                m.testResolved = false;
                onTestResolved();
            }
        } else {
            GJBaseGameLayer::update(dt);
        }
    }
};

// ---------------------------------------------------------------------------
class $modify(MacroPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        // auto-load this level's saved macro (clears if none)
        Macro::get().loadForLevel(levelKeyFor(level));
        return true;
    }

    void onExit() {
        // leaving the level: drop any analysis snapshots while their objects are
        // still alive, and stop analyzing so we never load a stale checkpoint.
        auto& m = Macro::get();
        if (m.mode == Mode::Analyzing) { m.mode = Mode::Idle; clearJumpCps(); }
        PlayLayer::onExit();
    }

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
            m.pi240 = m.pi120 = m.pi60 = 0; // restart the live tally
            if (m.haveSeed) { m_randomSeed = m.seed1; m_replayRandSeed = m.seed2; }
        } else if (m.mode == Mode::Analyzing) {
            m.playIndex = 0;
            m.step = 0;
            m.gameTime = 0;
            m.died = false;
            m.deathStep = -1;
            // NOTE: deliberately do NOT reset maxX here -- after a death-respawn the
            // replay restarts at x~0, which stays below maxX and reads as "no progress"
            // (i.e. a death), which is exactly what we want to detect.
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
        // intentionally does nothing special: death is detected by loss of forward
        // progress in processCommands, since destroyPlayer can fire without killing.
        PlayLayer::destroyPlayer(p, o);
    }

    void levelComplete() {
        auto& m = Macro::get();
        if (m.mode == Mode::Analyzing) {
            m.testResolved = true;
            m.lastSurvived = true;
            return; // don't actually end the level mid-analysis
        }
        PlayLayer::levelComplete();
    }
};

// ---------------------------------------------------------------------------
// In-level "Macros" menu (in the pause screen): load this level's saved macro.
// ---------------------------------------------------------------------------
class $modify(MacroPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto spr = ButtonSprite::create("Macros");
        spr->setScale(0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(MacroPauseLayer::onMacros));
        auto menu = CCMenu::create();
        menu->addChild(btn);
        auto win = CCDirector::sharedDirector()->getWinSize();
        menu->setPosition(60.f, win.height - 22.f); // top-left corner of the pause screen
        menu->setZOrder(100);
        this->addChild(menu);
    }

    void onMacros(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl || !pl->m_level) {
            createQuickPopup("Macros", "Not in a level.", "OK", nullptr, [](auto, bool) {});
            return;
        }
        std::string key = levelKeyFor(pl->m_level);
        auto res = file::readString(pathForKey(key));
        if (!res) {
            createQuickPopup("Macros",
                "No macro saved for this level yet.\nRecord one with your <cy>Macro: Record</c> key.",
                "OK", nullptr, [](auto, bool) {});
            return;
        }
        // count inputs / analyzed jumps in the file
        int nin = 0, nwin = 0;
        {
            std::istringstream ss(res.unwrap());
            std::string l;
            std::getline(ss, l); // header
            std::getline(ss, l); // seeds
            while (std::getline(ss, l)) {
                if (l.empty()) continue;
                if (l[0] == 'W') nwin++; else nin++;
            }
        }
        createQuickPopup("Macros",
            fmt::format("Saved macro for this level:\n<cy>{}</c> inputs, <cg>{}</c> analyzed jumps.\n\nLoad it?", nin, nwin),
            "Cancel", "Load",
            [key](FLAlertLayer*, bool load) {
                if (!load) return;
                Macro::get().loadForLevel(key);
                Notification::create(
                    fmt::format("Loaded macro: {} inputs", Macro::get().inputs.size()),
                    NotificationIcon::Success)->show();
            });
    }
};

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
    listenForKeybindSettingPresses("macro-analyze", [](Keybind const&, bool down, bool repeat, double) -> bool {
        if (down && !repeat) {
            if (Macro::get().mode == Mode::Analyzing) { Macro::get().mode = Mode::Idle; notify("Analyze: cancelled", NotificationIcon::Info); }
            else startAnalysis();
            return true;
        }
        return false;
    });
}
