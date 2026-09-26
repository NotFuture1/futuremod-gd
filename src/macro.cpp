#include "common.hpp"
#include "sim/ghost.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
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
#include <limits>

using namespace geode::prelude;

// ===========================================================================
// Macro record/replay + frame window counter.
//
// Record: every input is stored as (physics step, position inside that step).
// With Click Between Frames the position inside the step is real -- CBF
// splits the step at the exact click time -- so it is captured, and replay
// reproduces CBF's split exactly (see "CBF-exact step split" below).
//
// Analyze: replay the run unshifted (baseline) to confirm it's deterministic,
// then for each input shift it -1,+1,-2,+2... ticks until both sides die (or
// the window is wider than the max), and for CBF runs bisect each edge to
// sub-frame precision. The surviving range is that input's frame window, in
// 240 Hz frames. Written to geode.log, a CSV, and NaNDL-compatible JSON.
// ===========================================================================

namespace {

constexpr int kMargin     = 48;  // ticks past the last input the BASELINE / anchor-verify must reach
// PROBE horizon: how far past the tested input we simulate before declaring it
// survived. It ends at the NEXT input that isn't part of the tested gesture --
// a flat long horizon was the "everything is frame-perfect" bug: a 1-tick shift
// compounds across many downstream hazards and *eventually* kills, so every
// window collapsed to 1. Clamped so we still clear an immediate hazard (min)
// and cap a pathological gap with no inputs (max).
constexpr int kMinHorizon = 24;
constexpr int kMaxHorizon = 240;
constexpr int kFF      = 1;   // 1 = no fast-forward (FF corrupts physics -> false deaths)
constexpr int kCap     = 60000; // probe cap per analysis
constexpr int kStall   = 24;  // ticks with no movement at all => the player is dead (fallback)
constexpr int kDeadStall = 4; // ticks with no movement AND m_isDead set => dead (fast path)
constexpr double kSmallest = std::numeric_limits<float>::min(); // CBF's minimum substep factor

// Frame-perfect ding pitch by tier. The tighter the window, the lower the pitch.
constexpr float kDing60  = 1.50f; // easiest  -> highest
constexpr float kDing120 = 1.15f;
constexpr float kDing240 = 0.85f; // hardest  -> lowest

enum class Mode { Idle, Recording, Playing, Analyzing };
enum class Phase { Integer, Refine };

// Per-input confidence. A number we aren't sure of must never silently enter a
// total, so every window carries one of these into the log and the exports.
enum class WStatus : char {
    None    = '-',
    Exact   = 'E', // both edges closed by a death
    Wide    = 'W', // wider than the max window: not a timing
    Capped  = 'C', // an edge ran into the adjacent input of the same button
    Timeout = 'T', // a probe hung and was treated as survived
};

const char* statusName(WStatus s) {
    switch (s) {
        case WStatus::Exact:   return "exact";
        case WStatus::Wide:    return "wide";
        case WStatus::Capped:  return "capped";
        case WStatus::Timeout: return "timeout";
        default:               return "none";
    }
}

struct InputEdge {
    int step = 0;
    int button = 1;
    bool player1 = true;
    bool down = true;
    // Position inside the step's player update, 0..1. Only non-zero for inputs
    // Click Between Frames delivered mid-step; vanilla inputs land between steps.
    double frac = 0.0;
    // Delivered INSIDE the player update (CBF) rather than from GD's input queue
    // at the step boundary. Replay applies it at the same point.
    bool mid = false;
    // Which players GD actually pushed for this input (bit0 = P1, bit1 = P2),
    // captured at record time. 0 = unknown (older macro): fall back to P1/P2.
    int mask = 0;
    double t() const { return step + frac; }
};

// one scheduled input for the current replay/probe (shifted when it's the target)
struct SchedEv {
    int step;
    double frac;
    bool mid;
    size_t idx;
};

struct WinResult {
    double window = 0.0;          // frames at 240 Hz (may be fractional with CBF)
    double lo = 0.0, hi = 0.0;    // surviving offset range, in frames
    WStatus status = WStatus::None;
    bool tap = false;             // press measured together with its release
    bool subframe = false;        // edges bisected below one frame
};

struct SavedWin {
    int idx;
    double window;
    char status;
    // detail for the CSV; absent in macros analyzed by v1.2.0 (NaN = unknown)
    double lo = std::numeric_limits<double>::quiet_NaN();
    double hi = std::numeric_limits<double>::quiet_NaN();
    int tap = -1, sub = -1;
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

std::filesystem::path trackPathForKey(std::string const& key) {
    return macrosDir() / (key + ".track");
}

int settingInt(char const* key) {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>(key));
}

// ---------------------------------------------------------------------------
// Click Between Frames environment. CBF itself is fully supported; only its
// Physics Bypass is not (it derives the physics step from wall-clock time, so
// no two runs are the same).
// ---------------------------------------------------------------------------
struct CbfEnv {
    bool loaded = false;
    bool active = false;        // loaded and not soft-toggled off
    bool physicsBypass = false;
    bool clickOnSteps = false;
};

CbfEnv cbfEnv() {
    CbfEnv e;
    if (auto mod = Loader::get()->getLoadedMod("syzzi.click_between_frames")) {
        e.loaded = true;
        e.active = !mod->getSettingValue<bool>("soft-toggle");
#ifdef GEODE_IS_WINDOWS
        e.physicsBypass = mod->getSettingValue<bool>("physics-bypass");
#endif
        e.clickOnSteps = mod->getSettingValue<bool>("click-on-steps");
    }
    return e;
}

void logEnv(char const* what, GJBaseGameLayer* l) {
    auto c = cbfEnv();
    FW_LOG("ENV {} cbf={} active={} physicsBypass={} clickOnSteps={} gdClickBetweenSteps={} gdClickOnSteps={}",
        what, c.loaded ? 1 : 0, c.active ? 1 : 0, c.physicsBypass ? 1 : 0, c.clickOnSteps ? 1 : 0,
        (l && l->m_clickBetweenSteps) ? 1 : 0, (l && l->m_clickOnSteps) ? 1 : 0);
}

struct Macro {
    Mode mode = Mode::Idle;
    std::vector<InputEdge> inputs;
    int step = 0;
    double gameTime = 0;

    uint64_t seed1 = 0, seed2 = 0;
    bool haveSeed = false;

    // replay schedule (rebuilt per playback / per probe)
    std::vector<SchedEv> sched;
    size_t schedPos = 0;

    // desync detection: the RECORDED player track, and playback drift against it
    std::vector<std::pair<float, float>> track;
    float maxDrift = 0.f;
    int maxDriftStep = -1;
    bool finishedNotified = false;
    bool verdictLogged = false;

    std::unordered_map<CheckpointObject*, int> cpStep;

    // --- analyzer ---
    std::vector<size_t> targets;
    std::vector<WinResult> results;
    size_t targetIdx = 0;
    int maxWindow = 10;   // frames; wider = "wide", not a timing
    int subSteps = 3;     // bisection steps per CBF edge (3 = 1/8 frame)
    Phase phase = Phase::Integer;
    double offset = 0.0;  // current probe shift, in frames
    int minSurv = 0, maxSurv = 0;
    bool negDead = false, posDead = false;     // side closed by a death
    bool negCapped = false, posCapped = false; // side closed by the adjacent input
    double negLimit = -1e9, posLimit = 1e9;    // how far the shift may go before crossing an input
    bool tapMode = false;  // press shifted together with its (close) release
    long pairIdx = -1;     // that release
    bool hadTimeout = false;
    int probeSettleStep = 0;
    // sub-frame refinement (CBF runs): bisect between a surviving and a dying offset
    int refineSide = 0;    // -1 = low edge, +1 = high edge, 0 = done
    double rSurv = 0.0, rDead = 0.0;
    int rLeft = 0;
    double loEdge = 0.0, hiEdge = 0.0;
    bool loRefined = false, hiRefined = false;

    float speed = 1.f;       // analysis game speed (1 = real time)
    bool speedPhase = false; // re-verifying the baseline at `speed`
    int marginEnd = 0, testCount = 0, lastTargetStep = 0;
    int testFrames = 0; // real frames spent on the current test (hang guard)
    bool baseline = false;
    bool testResolved = false, lastSurvived = false;
    int deathStep = -1;
    // death = "player stopped MOVING" (NOT destroyPlayer, which can fire without
    // killing; NOT forward-x progress, which 2.2 reverse gameplay breaks).
    float lastCx = -1.e9f, lastCy = -1.e9f; // player position last step
    int lastProgressStep = 0;               // step the player last moved at
    bool inBeginTest = false; // our own resetLevel, vs GD's death auto-restart
    int c240 = 0, c120 = 0, c60 = 0;
    double tightest = 1e9;

    // the 1x baseline player track -- the GROUND TRUTH. Used to verify any speed-up
    // reproduces 1x tick-for-tick, to validate anchors, and for the probe early-exit.
    struct BasePt { float x, y, vy; };
    std::vector<BasePt> baseTrack;
    bool trackBase = false;    // recording baseTrack (only during the 1x baseline)
    bool checkingSpeed = false; // comparing a sped-up baseline against baseTrack
    bool speedDrift = false;    // the sped-up baseline diverged from 1x
    float baseDriftMax = 0.f;   // 1x baseline vs the RECORDED run
    int baseDriftStep = -1;

    // --- experimental fast analysis (mid-level save-states) ---
    struct Anchor {
        Ref<CheckpointObject> cp;
        uint64_t seed1 = 0, seed2 = 0; // RNG state AT the snapshot, not level start
        int step = 0;                  // step the snapshot was actually taken at
    };
    bool fastMode = false;
    bool capturingAnchors = false;
    size_t nextAnchorCapture = 0;
    std::vector<int> anchorStep;
    std::unordered_map<size_t, Anchor> anchors;
    bool verifyingAnchor = false;
    bool anchorDrift = false;
    int nAnchored = 0, nFull = 0;

    // analysis results that persist for the live playback tally
    std::vector<SavedWin> savedWindows;
    std::vector<int> fp240, fp120, fp60;
    size_t pi240 = 0, pi120 = 0, pi60 = 0;
    std::vector<std::pair<int, double>> playWins; // (step, window) of measured timings
    size_t piWin = 0;
    double lastPlayWin = -1.0;

    std::string curKey;

    static Macro& get() { static Macro m; return m; }

    std::filesystem::path path() { return pathForKey(curKey); }

    InputEdge const& target() const { return inputs[targets[targetIdx]]; }

    // step the tested input actually happens at in this probe (shifted when offset<0)
    int effTargetStep() const {
        return static_cast<int>(std::floor(target().t() + std::min(0.0, offset) + 1e-9));
    }

    // a death only counts against the tested input if the run reached the SHIFTED
    // input; anything earlier is upstream desync, not this shift's doing. deathStep
    // is the last step the player still moved (~deathTick-1), hence the -2 slack.
    bool deathCountsAgainstTarget() const {
        return deathStep >= effTargetStep() - 2;
    }

    void buildFpLists() {
        fp240.clear(); fp120.clear(); fp60.clear(); playWins.clear();
        for (auto const& sw : savedWindows) {
            if (sw.idx < 0 || sw.idx >= static_cast<int>(inputs.size())) continue;
            if (sw.status == static_cast<char>(WStatus::Wide) || sw.window <= 0.0) continue;
            int st = inputs[sw.idx].step;
            if (sw.window <= 1.0 + 1e-9) fp240.push_back(st);
            if (sw.window <= 2.0 + 1e-9) fp120.push_back(st);
            if (sw.window <= 4.0 + 1e-9) fp60.push_back(st);
            playWins.push_back({ st, sw.window });
        }
        std::sort(fp240.begin(), fp240.end());
        std::sort(fp120.begin(), fp120.end());
        std::sort(fp60.begin(), fp60.end());
        std::sort(playWins.begin(), playWins.end());
        pi240 = pi120 = pi60 = piWin = 0;
        lastPlayWin = -1.0;
    }

    void save() {
        std::string out = fmt::format("FUTUREMOD_MACRO v3\n{} {}\n", seed1, seed2);
        // the first four fields are the v2 format, so older builds still read it
        for (auto const& in : inputs)
            out += fmt::format("{} {} {} {} {:.9f} {} {}\n", in.step, in.button, in.player1 ? 1 : 0,
                in.down ? 1 : 0, in.frac, in.mid ? 1 : 0, in.mask);
        for (auto const& sw : savedWindows)
            out += fmt::format("W {} {:.4f} {} {:.4f} {:.4f} {} {}\n", sw.idx, sw.window, sw.status,
                sw.lo, sw.hi, sw.tap, sw.sub);
        auto res = file::writeString(path(), out);
        if (!res) log::warn("[macro] save failed: {}", res.unwrapErr());
    }

    void saveTrack() {
        if (track.empty()) return;
        std::string out;
        out.reserve(track.size() * 24);
        for (auto const& p : track) out += fmt::format("{:.3f} {:.3f}\n", p.first, p.second);
        auto res = file::writeString(trackPathForKey(curKey), out);
        if (!res) log::warn("[macro] track save failed: {}", res.unwrapErr());
    }

    // Load the macro saved for a given level key (clears in-memory macro if none).
    void loadForLevel(std::string const& key) {
        curKey = key;
        inputs.clear();
        savedWindows.clear();
        track.clear();
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
        int nMid = 0;
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            if (line[0] == 'W') {
                std::istringstream sw(line);
                std::string tag; int idx; double w; std::string st;
                if (sw >> tag >> idx >> w) {
                    SavedWin v{ idx, w, static_cast<char>(WStatus::Exact) };
                    if (sw >> st && !st.empty()) v.status = st[0];
                    double lo, hi; int tap, sub;
                    if (sw >> lo >> hi >> tap >> sub) { v.lo = lo; v.hi = hi; v.tap = tap; v.sub = sub; }
                    savedWindows.push_back(v);
                }
            } else {
                std::istringstream s3(line);
                InputEdge in;
                int p1, dn;
                if (s3 >> in.step >> in.button >> p1 >> dn) {
                    in.player1 = p1 != 0;
                    in.down = dn != 0;
                    double frac; int mid, mask;
                    if (s3 >> frac >> mid >> mask) {
                        in.frac = std::clamp(frac, 0.0, 0.999999);
                        in.mid = mid != 0;
                        in.mask = mask;
                    }
                    if (in.mid) nMid++;
                    inputs.push_back(in);
                }
            }
        }
        if (auto tr = file::readString(trackPathForKey(key))) {
            std::istringstream ts(tr.unwrap());
            float x, y;
            while (ts >> x >> y) track.push_back({ x, y });
        }
        log::info("[macro] level '{}': loaded {} inputs ({} mid-step/CBF), {} windows, {} track pts",
            key, inputs.size(), nMid, savedWindows.size(), track.size());
    }
};

// ---------------------------------------------------------------------------
// Global step bookkeeping shared by the hooks below.
// ---------------------------------------------------------------------------
uint64_t g_pcSerial = 0; // ++ per processCommands call (one physics step)
int g_inP1Update = 0;    // depth of player-1 PlayerObject::update (outermost hook)

// Recording: how far through the current step's player update we are when an
// input arrives. CBF delivers its inputs between substeps of that update, so
// elapsed/total at that moment IS the click's position inside the step.
struct RecAcc {
    double el[2] = { 0.0, 0.0 };                       // update time so far, P1 / P2
    std::vector<std::pair<size_t, double>> pending;   // (input index, elapsed at input)
} g_acc;

// Recording: which players a real input actually pushed (hooked pushButton).
bool g_capturing = false;
int g_capMask = 0;

// Replay: our reproduction of CBF's split step. Field-for-field the globals CBF
// keeps (p1Split/p2Split/inputThisStep/midStep/rotationDelta/p1Pos/p2Pos), with
// the same lifetime: reset on every player-1 update.
struct SplitState {
    bool midStep = false;
    bool inputThisStep = false;
    bool p1Split = false, p2Split = false;
    float rotationDelta = 0.f;
    CCPoint p1Pos{}, p2Pos{};
} g_split;
float g_shipRotDelta = 0.f;

void resetSplit() { g_split = SplitState{}; g_shipRotDelta = 0.f; }

void finalizePending(Macro& m) {
    double total = std::max(g_acc.el[0], g_acc.el[1]);
    for (auto const& [idx, el] : g_acc.pending) {
        if (idx >= m.inputs.size()) continue;
        double f = total > 0.0 ? el / total : 0.0;
        m.inputs[idx].frac = std::clamp(f, 0.0, 0.999999);
    }
    g_acc.pending.clear();
    g_acc.el[0] = g_acc.el[1] = 0.0;
}

bool driving() {
    auto mode = Macro::get().mode;
    return mode == Mode::Playing || mode == Mode::Analyzing;
}

void notify(std::string const& msg, NotificationIcon icon) {
    Notification::create(msg, icon)->show();
}

// speed doubles as pitch: >1 higher/faster, <1 lower/slower (FMOD frequency).
void playDing(float speed = 1.f) {
    if (auto fae = FMODAudioEngine::sharedEngine())
        fae->playEffect("achievement_01.ogg", speed, 1.f, 1.f);
}

// Analysis speed-up = plain scheduler time scale (the same mechanism as a
// speedhack): the engine itself splits the bigger dt into 1/240 substeps.
// ALWAYS restore to 1 on every analyzer exit path.
void setAnalyzeSpeed(float s) {
    CCDirector::sharedDirector()->getScheduler()->setTimeScale(s);
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
        s += fmt::format("FW {}/{}\n", std::min(m.targetIdx, m.targets.size()), m.targets.size());
        if (s240) s += fmt::format("FP@240: {}\n", m.c240);
        if (s120) s += fmt::format("FP@120: {}\n", m.c120);
        if (s60)  s += fmt::format("FP@60: {}\n",  m.c60);
        if (m.tightest < 1e8) s += fmt::format("min: {:.2f}f", m.tightest);
    } else { // playback live tally: passed / total
        if (s240) s += fmt::format("FP@240: {}/{}\n", m.pi240, m.fp240.size());
        if (s120) s += fmt::format("FP@120: {}/{}\n", m.pi120, m.fp120.size());
        if (s60)  s += fmt::format("FP@60: {}/{}\n",  m.pi60,  m.fp60.size());
        if (m.lastPlayWin > 0.0) s += fmt::format("last: {:.2f}f", m.lastPlayWin);
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

// master switch: when off, all macro features are fully disabled.
bool macroEnabled() { return Mod::get()->getSettingValue<bool>("macro-enabled"); }

// ---------------------------------------------------------------------------
// Replay injection
// ---------------------------------------------------------------------------

// Push/release on exactly the players the real input reached (captured at
// record time). Older macros without the capture fall back to P1/P2.
void applyInput(GJBaseGameLayer* l, InputEdge const& in) {
    auto btn = static_cast<PlayerButton>(in.button);
    auto act = [&](PlayerObject* p) {
        if (!p) return;
        if (in.down) p->pushButton(btn);
        else p->releaseButton(btn);
    };
    int mask = in.mask ? in.mask : (in.player1 ? 1 : 2);
    if (mask & 1) act(l->m_player1);
    if (mask & 2) act(l->m_player2);
}

// Build the input schedule for a replay. When `shifted`, the analyzer's target
// (and its paired release in tap mode) is moved by `offset` frames.
void buildSchedule(Macro& m, bool shifted) {
    m.sched.clear();
    m.sched.reserve(m.inputs.size());
    long tgt = -1, pair = -1;
    if (shifted && m.targetIdx < m.targets.size()) {
        tgt = static_cast<long>(m.targets[m.targetIdx]);
        pair = m.tapMode ? m.pairIdx : -1;
    }
    for (size_t i = 0; i < m.inputs.size(); i++) {
        auto const& in = m.inputs[i];
        long li = static_cast<long>(i);
        if ((li == tgt || li == pair) && m.offset != 0.0) {
            double T = std::max(0.0, in.t() + m.offset);
            int st = static_cast<int>(std::floor(T + 1e-9));
            double fr = T - st;
            if (fr < 1e-9) fr = 0.0;
            m.sched.push_back({ st, fr, in.mid || fr > 0.0, i });
        } else {
            m.sched.push_back({ in.step, in.frac, in.mid, i });
        }
    }
    // step order; within a step, boundary (queue) inputs before mid-step ones,
    // then by position; ties keep recorded order (stable).
    std::stable_sort(m.sched.begin(), m.sched.end(), [](SchedEv const& a, SchedEv const& b) {
        if (a.step != b.step) return a.step < b.step;
        if (a.mid != b.mid) return !a.mid;
        return a.frac < b.frac;
    });
    m.schedPos = 0;
}

// ---- recording / playback --------------------------------------------------

void startRecording() {
    auto pl = PlayLayer::get();
    if (!pl) { notify("Macro: enter a level first", NotificationIcon::Error); return; }
    auto& m = Macro::get();
    auto cbf = cbfEnv();
    logEnv("record", pl);
    if (cbf.active && cbf.physicsBypass) {
        notify("CBF Physics Bypass is ON: runs can't be replayed exactly.\n"
               "Turn it off in CBF's settings (CBF itself is fine).", NotificationIcon::Warning);
    } else if (!cbf.active && pl->m_clickBetweenSteps) {
        notify("GD's own 'Click between steps' is on: replays may be off.\n"
               "Use Click Between Frames, or turn that option off.", NotificationIcon::Warning);
    }
    m.mode = Mode::Recording;
    m.inputs.clear();
    m.cpStep.clear();
    m.track.clear();
    m.step = 0;
    m.gameTime = 0;
    g_acc = RecAcc{};
    pl->resetLevel();
    notify(cbf.active ? "Macro: recording (CBF sub-frame timing on)" : "Macro: recording",
        NotificationIcon::Info);
}

void stopRecording() {
    auto& m = Macro::get();
    finalizePending(m);
    m.mode = Mode::Idle;
    // an empty recording must not clobber a previously saved (analyzed) macro
    if (m.inputs.empty()) {
        notify("Macro: nothing recorded, keeping the saved macro", NotificationIcon::Info);
        return;
    }
    m.savedWindows.clear(); // old windows belong to the old run
    m.save();
    m.saveTrack();
    int nMid = 0;
    for (auto const& in : m.inputs) if (in.mid) nMid++;
    FW_LOG("RECORDED inputs={} midStep={} lastStep={}", m.inputs.size(), nMid, m.inputs.back().step);
    notify(fmt::format("Macro: recorded + saved {} inputs", m.inputs.size()), NotificationIcon::Success);
}

void logPlaybackVerdict(char const* why) {
    auto& m = Macro::get();
    if (m.verdictLogged) return;
    m.verdictLogged = true;
    char const* v = m.track.empty() ? "NOTRACK"
        : m.maxDrift < 0.01f ? "EXACT"
        : m.maxDrift < 1.0f ? "CLOSE" : "DRIFT";
    FW_LOG("PLAYBACK end={} step={} maxDrift={:.3f}u @step {} VERDICT={}",
        why, m.step, m.maxDrift, m.maxDriftStep, v);
}

void startPlaying() {
    auto pl = PlayLayer::get();
    if (!pl) { notify("Macro: enter a level first", NotificationIcon::Error); return; }
    auto& m = Macro::get();
    if (m.inputs.empty()) { notify("Macro: nothing recorded yet", NotificationIcon::Error); return; }
    m.mode = Mode::Playing;
    m.offset = 0.0;
    buildSchedule(m, false);
    resetSplit();
    m.step = 0;
    m.gameTime = 0;
    m.finishedNotified = false;
    m.verdictLogged = false;
    m.maxDrift = 0.f;
    m.maxDriftStep = -1;
    m.buildFpLists(); // live frame-perfect tally from the last analysis
    logEnv("play", pl);
    pl->resetLevel();
    if (!m.savedWindows.empty()) updateHud();
    notify(fmt::format("Macro: playing {} inputs", m.inputs.size()), NotificationIcon::Info);
}

void stopPlaying() {
    logPlaybackVerdict("stopped");
    Macro::get().mode = Mode::Idle;
    notify("Macro: playback stopped", NotificationIcon::Info);
}

// ---- analyzer --------------------------------------------------------------

// How far the target may move before it would cross the adjacent input of the
// same button. A press whose release follows within a few frames is a TAP and
// is shifted together with that release (the gesture moves as a unit, as it
// does for a human) -- shifting the press alone would collide with its own
// release and cap every short tap's window, inflating the frame-perfect count.
void computeLimits(Macro& m) {
    size_t ti = m.targets[m.targetIdx];
    auto const& in = m.inputs[ti];
    auto same = [&](size_t j) {
        return m.inputs[j].button == in.button && m.inputs[j].player1 == in.player1;
    };
    long prev = -1, next = -1;
    for (long j = static_cast<long>(ti) - 1; j >= 0; j--) if (same(j)) { prev = j; break; }
    for (size_t j = ti + 1; j < m.inputs.size(); j++) if (same(j)) { next = static_cast<long>(j); break; }

    m.tapMode = false;
    m.pairIdx = -1;
    if (in.down && next >= 0 && !m.inputs[next].down
        && m.inputs[next].t() - in.t() <= m.maxWindow + 2) {
        m.tapMode = true;
        m.pairIdx = next;
    }

    m.negLimit = -in.t(); // never before the level start
    if (prev >= 0) m.negLimit = std::max(m.negLimit, m.inputs[prev].t() - in.t());

    m.posLimit = 1e9;
    if (m.tapMode) {
        for (size_t j = static_cast<size_t>(m.pairIdx) + 1; j < m.inputs.size(); j++)
            if (same(j)) { m.posLimit = m.inputs[j].t() - m.inputs[m.pairIdx].t(); break; }
    } else if (next >= 0) {
        m.posLimit = m.inputs[next].t() - in.t();
    }
}

void beginTest() {
    auto& m = Macro::get();
    m.testResolved = false;
    m.deathStep = -1;
    m.testFrames = 0;
    m.anchorDrift = false;
    resetSplit();

    bool probing = !m.baseline;
    int tstep = probing ? m.target().step : 0;
    if (m.baseline) {
        m.marginEnd = m.lastTargetStep + kMargin;
    } else {
        // Probe AND anchor-verify use the SAME horizon: it ends at the next input
        // after the tested gesture (clamped). The verify (offset 0 from the anchor)
        // must reproduce the 1x baseline over this whole window, which is exactly
        // the window probes use.
        size_t ti = m.targets[m.targetIdx];
        size_t after = m.tapMode ? static_cast<size_t>(m.pairIdx) : ti;
        int nextStep = (after + 1 < m.inputs.size()) ? m.inputs[after + 1].step
                                                     : tstep + kMaxHorizon;
        m.marginEnd = tstep + std::clamp(nextStep - tstep, kMinHorizon, kMaxHorizon);
        int shifted = static_cast<int>(std::floor(m.target().t() + m.offset + 1e-9));
        m.marginEnd = std::max(m.marginEnd, shifted + kMinHorizon);
        int lastOrig = m.tapMode ? m.inputs[m.pairIdx].step : tstep;
        m.probeSettleStep = std::max(lastOrig, static_cast<int>(
            std::floor(m.inputs[m.tapMode ? m.pairIdx : ti].t() + std::max(0.0, m.offset)))) + 2;
    }
    buildSchedule(m, probing);

    auto pl = PlayLayer::get();
    bool restored = false;
    m.inBeginTest = true; // our resets are not death signals (see resetLevel hook)

    // Fast mode: restore the snapshot taken just before this input instead of
    // replaying from the level start. Guarded so any missing/removed/suspect
    // anchor silently falls back to the normal full-replay path.
    if (probing && m.fastMode && pl) {
        auto it = m.anchors.find(m.targetIdx);
        if (it != m.anchors.end() && it->second.cp
            && it->second.step <= tstep - m.maxWindow - 2 // even the widest -shift lands after it
            && pl->m_checkpointArray && pl->m_checkpointArray->containsObject(it->second.cp)) {
            auto const& a = it->second;
            // resetLevel FIRST: it cancels any auto-respawn GD still has pending
            // from the previous probe's death; the checkpoint then overrides it.
            pl->resetLevel();
            pl->loadFromCheckpoint(a.cp);
            m.step = a.step;
            m.gameTime = a.step / 240.0;
            // inputs AT the anchor step are already baked into the snapshot
            m.schedPos = 0;
            while (m.schedPos < m.sched.size() && m.sched[m.schedPos].step <= a.step) m.schedPos++;
            pl->m_randomSeed = a.seed1;
            pl->m_replayRandSeed = a.seed2;
            // re-seed the movement-based death detector at the restore point, so a
            // mid-level restore is NOT misread as a death.
            auto ppos = pl->m_player1 ? pl->m_player1->getPosition() : CCPoint{0.f, 0.f};
            m.lastCx = ppos.x;
            m.lastCy = ppos.y;
            m.lastProgressStep = a.step;
            restored = true;
            if (!m.verifyingAnchor) m.nAnchored++;
        }
    }
    if (probing && !m.verifyingAnchor && !restored) m.nFull++;
    if (!restored && pl) {
        // resetLevelFromStart() ALWAYS restarts at the level start -- unlike
        // resetLevel(), which in practice mode respawns at the last checkpoint.
        pl->resetLevelFromStart();
        m.step = 0;
        m.gameTime = 0;
        m.schedPos = 0;
        if (m.haveSeed) { pl->m_randomSeed = m.seed1; pl->m_replayRandSeed = m.seed2; }
        m.lastCx = m.lastCy = -1.e9f;
        m.lastProgressStep = 0;
    }
    resetSplit();
    m.inBeginTest = false;
}

// next whole-frame probe, expanding the narrower open side: -1, +1, -2, +2, ...
bool pickNextInteger(Macro& m) {
    for (;;) {
        int w = m.maxSurv - m.minSurv + 1;
        if (w > m.maxWindow) return false; // wide: not a timing, stop spending probes
        bool negOpen = !m.negDead && !m.negCapped;
        bool posOpen = !m.posDead && !m.posCapped;
        if (!negOpen && !posOpen) return false;
        bool neg = negOpen && (!posOpen || -m.minSurv <= m.maxSurv);
        int cand = neg ? m.minSurv - 1 : m.maxSurv + 1;
        if (neg && cand < m.negLimit - 1e-9) { m.negCapped = true; continue; }
        if (!neg && cand > m.posLimit + 1e-9) { m.posCapped = true; continue; }
        m.phase = Phase::Integer;
        m.offset = cand;
        return true;
    }
}

// Sub-frame edges. Only for inputs CBF delivered mid-step: a vanilla click can
// only land on a step boundary, so its window is a whole number of frames and
// probing between them would measure physics the player couldn't use.
void setupRefine(Macro& m) {
    m.loRefined = m.hiRefined = false;
    m.refineSide = 0;
    bool sub = m.target().mid && m.subSteps > 0
        && (m.maxSurv - m.minSurv + 1) <= m.maxWindow;
    if (!sub) return;
    if (m.negDead) {
        m.refineSide = -1;
        m.rSurv = m.minSurv; m.rDead = m.minSurv - 1; m.rLeft = m.subSteps;
    } else if (m.posDead) {
        m.refineSide = 1;
        m.rSurv = m.maxSurv; m.rDead = m.maxSurv + 1; m.rLeft = m.subSteps;
    }
}

bool pickNextRefine(Macro& m) {
    for (;;) {
        if (m.refineSide == 0) return false;
        if (m.rLeft > 0) {
            m.phase = Phase::Refine;
            m.offset = 0.5 * (m.rSurv + m.rDead);
            return true;
        }
        double edge = 0.5 * (m.rSurv + m.rDead);
        if (m.refineSide < 0) {
            m.loEdge = edge; m.loRefined = true;
            if (m.posDead) {
                m.refineSide = 1;
                m.rSurv = m.maxSurv; m.rDead = m.maxSurv + 1; m.rLeft = m.subSteps;
                continue;
            }
        } else {
            m.hiEdge = edge; m.hiRefined = true;
        }
        m.refineSide = 0;
        return false;
    }
}

void finishAnalysis();

// Record the current target's window and move targetIdx on. Never starts the
// next target itself (nextTargets() loops), so a long run of inputs that need
// no probes can't recurse.
void finalizeTarget() {
    auto& m = Macro::get();
    WinResult r;
    int wInt = m.maxSurv - m.minSurv + 1;
    // Whole-frame convention: N surviving frames = a window of N frames, i.e. the
    // edges sit half a frame outside the outermost survivors. Refined (CBF) edges
    // replace that with the bisected boundary.
    r.lo = m.loRefined ? m.loEdge : m.minSurv - 0.5;
    r.hi = m.hiRefined ? m.hiEdge : m.maxSurv + 0.5;
    r.tap = m.tapMode;
    r.subframe = m.loRefined || m.hiRefined;
    if (wInt > m.maxWindow) {
        r.status = WStatus::Wide;
        r.window = wInt; // lower bound
    } else {
        r.window = r.hi - r.lo;
        r.status = m.hadTimeout ? WStatus::Timeout
            : (m.negCapped || m.posCapped) ? WStatus::Capped
            : WStatus::Exact;
    }
    m.results[m.targetIdx] = r;

    if (r.status != WStatus::Wide) {
        m.tightest = std::min(m.tightest, r.window);
        if (r.window <= 1.0 + 1e-9) { m.c240++; playDing(kDing240); }
        if (r.window <= 2.0 + 1e-9) m.c120++;
        if (r.window <= 4.0 + 1e-9) m.c60++;
    }
    auto const& in = m.target();
    FW_LOG("WIN #{} {} p{} step={} frac={:.3f} window={:.3f} lo={:+.3f} hi={:+.3f} status={} tap={} sub={}",
        m.targets[m.targetIdx], in.down ? "press" : "release", in.player1 ? 1 : 2, in.step, in.frac,
        r.window, r.lo, r.hi, statusName(r.status), r.tap ? 1 : 0, r.subframe ? 1 : 0);

    m.targetIdx++;
    updateHud();
}

// start the next probe for the current target; false = the target is measured
bool stepTarget() {
    auto& m = Macro::get();
    if (m.phase == Phase::Integer) {
        if (pickNextInteger(m)) { beginTest(); return true; }
        setupRefine(m);
    }
    if (pickNextRefine(m)) { beginTest(); return true; }
    return false;
}

// set up targets in order until one needs a probe (or all are done)
void nextTargets() {
    auto& m = Macro::get();
    while (m.targetIdx < m.targets.size()) {
        m.minSurv = m.maxSurv = 0;
        m.negDead = m.posDead = false;
        m.negCapped = m.posCapped = false;
        m.hadTimeout = false;
        m.refineSide = 0;
        m.loRefined = m.hiRefined = false;
        m.phase = Phase::Integer;
        m.offset = 0.0;
        computeLimits(m);
        // fast mode: verify this input's anchor first
        m.verifyingAnchor = m.fastMode && m.anchors.count(m.targetIdx) > 0;
        if (m.verifyingAnchor) { beginTest(); return; }
        if (stepTarget()) return;
        finalizeTarget();
    }
    finishAnalysis();
}

void continueTarget() {
    if (stepTarget()) return;
    finalizeTarget();
    nextTargets();
}

std::string fmtNum(double v) {
    auto s = fmt::format("{:.4f}", v);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? "0" : s;
}

// CSV (everything, with status) + NaNDL calculator JSON. Two JSON flavours:
// .nandl.json has every analyzed input, non-timings as "-" (NaNDL's own
// "ignored" marker); .fwc.json drops them, because the frame-window-counter
// mod's importer reads "-" as a 1-frame window.
// Built from the SAVED analysis, so the files can be regenerated at any time
// (pause menu > Files) without re-running it.
void writeExports() {
    auto& m = Macro::get();
    auto wins = m.savedWindows;
    std::sort(wins.begin(), wins.end(), [](SavedWin const& a, SavedWin const& b) { return a.idx < b.idx; });

    std::string csv = "input,edge,player,step,frac,time_s,window_frames,lo,hi,status,tap,subframe\n";
    std::vector<std::string> rowsAll, rowsFwc;
    auto opt = [](double v) { return std::isnan(v) ? std::string() : fmt::format("{:.4f}", v); };
    auto optI = [](int v) { return v < 0 ? std::string() : std::to_string(v); };
    for (auto const& sw : wins) {
        if (sw.idx < 0 || sw.idx >= static_cast<int>(m.inputs.size())) continue;
        auto const& in = m.inputs[sw.idx];
        auto st = static_cast<WStatus>(sw.status);
        csv += fmt::format("{},{},{},{},{:.4f},{:.5f},{:.4f},{},{},{},{},{}\n",
            sw.idx, in.down ? "press" : "release", in.player1 ? 1 : 2, in.step, in.frac,
            in.t() / 240.0, sw.window, opt(sw.lo), opt(sw.hi), statusName(st), optI(sw.tap), optI(sw.sub));
        bool wide = st == WStatus::Wide;
        std::string win = wide ? "\"-\"" : fmtNum(sw.window);
        auto row = [&](size_t n) {
            return fmt::format("    {{ \"input\": {}, \"timePosition\": {}, \"frameWindow\": {}, \"isPlayer2\": {} }}",
                n, fmtNum(in.t()), win, in.player1 ? "false" : "true");
        };
        rowsAll.push_back(row(rowsAll.size() + 1));
        if (!wide) rowsFwc.push_back(row(rowsFwc.size() + 1));
    }
    auto json = [](std::vector<std::string> const& rows) {
        std::string joined;
        for (size_t k = 0; k < rows.size(); k++) {
            if (k > 0) joined += ",\n";
            joined += rows[k];
        }
        return fmt::format(
            "{{\n  \"format\": \"nandl-calculator\",\n  \"version\": 1,\n"
            "  \"settings\": {{ \"gameFps\": 240, \"windowFps\": 240, \"respawnSeconds\": 0, \"timeUnit\": \"frames\" }},\n"
            "  \"frameWindows\": [\n{}\n  ]\n}}\n", joined);
    };
    auto dir = macrosDir();
    auto w = [](std::filesystem::path const& p, std::string const& s) {
        auto res = file::writeString(p, s);
        if (!res) log::warn("[fw] export failed ({}): {}", p.string(), res.unwrapErr());
    };
    w(dir / (m.curKey + ".windows.csv"), csv);
    if (!rowsAll.empty()) w(dir / (m.curKey + ".nandl.json"), json(rowsAll));
    if (!rowsFwc.empty()) w(dir / (m.curKey + ".fwc.json"), json(rowsFwc));
    FW_LOG("EXPORT dir='{}' key={} nandlRows={} fwcRows={}",
        dir.string(), m.curKey, rowsAll.size(), rowsFwc.size());
}

void finishAnalysis() {
    auto& m = Macro::get();
    setAnalyzeSpeed(1.f);
    // don't leave anchor checkpoints behind (practice mode would respawn at them)
    if (auto pl = PlayLayer::get()) pl->removeAllCheckpoints();
    m.anchors.clear();
    m.savedWindows.clear();

    int buckets[11] = {};
    int nWide = 0, nCapped = 0, nTimeout = 0, nSub = 0;
    for (size_t i = 0; i < m.targets.size(); i++) {
        auto const& r = m.results[i];
        if (r.status == WStatus::None) continue;
        SavedWin sw{ static_cast<int>(m.targets[i]), r.window, static_cast<char>(r.status) };
        sw.lo = r.lo; sw.hi = r.hi; sw.tap = r.tap ? 1 : 0; sw.sub = r.subframe ? 1 : 0;
        m.savedWindows.push_back(sw);
        if (r.status == WStatus::Wide) { nWide++; continue; }
        if (r.status == WStatus::Capped) nCapped++;
        if (r.status == WStatus::Timeout) nTimeout++;
        if (r.subframe) nSub++;
        int b = std::clamp(static_cast<int>(std::ceil(r.window - 1e-9)), 1, 10);
        buckets[b]++;
    }
    std::string bs;
    for (int b = 1; b <= 10; b++) bs += fmt::format(" <={}:{}", b, buckets[b]);
    FW_LOG("SUMMARY inputs={} timings={} wide={} capped={} timeout={} subframe={} probes={} |{}",
        m.targets.size(), static_cast<int>(m.targets.size()) - nWide, nWide, nCapped, nTimeout,
        nSub, m.testCount, bs);
    FW_LOG("SUMMARY FP@240={} FP@120={} FP@60={} tightest={:.3f}",
        m.c240, m.c120, m.c60, m.tightest < 1e8 ? m.tightest : -1.0);
    if (m.fastMode)
        FW_LOG("fast mode: {} probes from a save-state, {} full replays", m.nAnchored, m.nFull);

    m.save(); // persist windows so the live playback tally survives restarts
    writeExports();
    updateHud(); // while still Analyzing, so the HUD shows the final counts
    m.mode = Mode::Idle;
    playDing();
    notify(fmt::format("Analysis done: {} timings, 240:{} 120:{} 60:{}\nSaved to the macros folder",
        static_cast<int>(m.targets.size()) - nWide, m.c240, m.c120, m.c60), NotificationIcon::Success);
}

void advanceAnalysis(bool survived) {
    auto& m = Macro::get();
    if (++m.testCount > kCap) {
        log::warn("[fw] hit probe cap");
        for (size_t i = m.targetIdx; i < m.targets.size(); i++) m.results[i].status = WStatus::None;
        finishAnalysis();
        return;
    }
    if (m.phase == Phase::Integer) {
        int o = static_cast<int>(std::lround(m.offset));
        if (o < 0) { if (survived) m.minSurv = o; else m.negDead = true; }
        else       { if (survived) m.maxSurv = o; else m.posDead = true; }
    } else {
        if (survived) m.rSurv = m.offset; else m.rDead = m.offset;
        m.rLeft--;
    }
    continueTarget();
}

void startProbing() {
    auto& m = Macro::get();
    m.capturingAnchors = false;
    m.trackBase = false;
    log::info("[fw] probing {} inputs at {}x ({} anchors, {} tracked steps)",
        m.targets.size(), m.speed, m.anchors.size(), m.baseTrack.size());
    m.targetIdx = 0;
    nextTargets();
}

void onTestResolved() {
    auto& m = Macro::get();
    if (m.baseline) {
        if (m.speedPhase) {
            // Sped-up baseline: keep this speed ONLY if it reproduced the 1x
            // trajectory tick-for-tick. If it diverges, halve and re-verify.
            if (m.lastSurvived && !m.speedDrift) {
                log::info("[fw] {}x verified against the 1x baseline", m.speed);
                m.speedPhase = false;
                m.checkingSpeed = false;
                m.baseline = false;
                notify(fmt::format("Analyzing at {}x (verified deterministic)", (int)m.speed),
                    NotificationIcon::Info);
                startProbing();
                return;
            }
            int lower = std::max(1, static_cast<int>(m.speed) / 2);
            log::warn("[fw] {}x diverges from 1x ({}) -> retry at {}x",
                m.speed, m.speedDrift ? "trajectory drift" : "death", lower);
            m.speed = static_cast<float>(lower);
            setAnalyzeSpeed(m.speed);
            m.speedDrift = false;
            m.anchors.clear(); // captured at the bad speed -> recapture at the new one
            if (lower <= 1) {
                m.speedPhase = false;
                m.checkingSpeed = false;
                m.baseline = false;
                notify("Speed-up wasn't deterministic; analyzing at 1x", NotificationIcon::Info);
                startProbing();
                return;
            }
            if (m.fastMode) { m.capturingAnchors = true; m.nextAnchorCapture = 0; }
            beginTest();
            return;
        }
        if (!m.lastSurvived) {
            m.mode = Mode::Idle;
            setAnalyzeSpeed(1.f);
            FW_WARN("BASELINE FAIL stopped moving @ step {} (x {:.0f}) driftVsRecording={:.3f}u @step {}",
                m.deathStep, m.lastCx, m.baseDriftMax, m.baseDriftStep);
            std::string msg;
            if (m.deathStep < 0)
                msg = "Can't analyze: the replay didn't reach the end in time (timed out).\n"
                      "If you're using speedhack, set the game speed to 1x for analysis.";
            else if (m.deathStep <= 30)
                msg = fmt::format("Can't analyze: the replay stalls right at the start (step {}).\n"
                              "Try re-recording a fresh run, then analyze.", m.deathStep);
            else
                msg = fmt::format("Can't analyze: the replay drifts off at step {} and dies.\n"
                              "It isn't reproducing your run -- re-record and try again,\n"
                              "and send geode.log if it keeps happening.", m.deathStep);
            notify(msg, NotificationIcon::Error);
            return;
        }
        char const* v = m.track.empty() ? "NOTRACK"
            : m.baseDriftMax < 0.01f ? "EXACT"
            : m.baseDriftMax < 1.0f ? "CLOSE" : "DRIFT";
        FW_LOG("BASELINE OK reached step {} x {:.0f} tracked={} driftVsRecording={:.3f}u @step {} VERDICT={}",
            m.step, m.lastCx, m.baseTrack.size(), m.baseDriftMax, m.baseDriftStep, v);
        if (!m.track.empty() && m.baseDriftMax >= 1.0f)
            notify(fmt::format("Note: the replay differs from your recording by {:.1f} units\n"
                "(step {}). Windows describe the replayed run.", m.baseDriftMax, m.baseDriftStep),
                NotificationIcon::Warning);
        if (m.speed > 1.f) {
            m.speedPhase = true;
            m.checkingSpeed = true; // compare this pass against baseTrack, don't overwrite it
            m.speedDrift = false;
            m.trackBase = false;
            setAnalyzeSpeed(m.speed);
            if (m.fastMode) { m.capturingAnchors = true; m.nextAnchorCapture = 0; m.anchors.clear(); }
            beginTest();
            return;
        }
        m.baseline = false;
        startProbing();
        return;
    }
    if (m.verifyingAnchor) {
        // offset-0 re-run from the anchor: it must retrace the baseline exactly.
        m.verifyingAnchor = false;
        if (m.anchorDrift || !m.lastSurvived) {
            log::warn("[fw] anchor for input #{} failed verification ({}) -> full replay",
                m.targets[m.targetIdx], m.anchorDrift ? "drift" : "death");
            m.anchors.erase(m.targetIdx);
        }
        continueTarget();
        return;
    }
    advanceAnalysis(m.lastSurvived);
}

void startAnalysis() {
    auto pl = PlayLayer::get();
    if (!pl) { notify("Analyze: enter a level first", NotificationIcon::Error); return; }
    auto& m = Macro::get();
    if (m.inputs.empty()) { notify("Analyze: record a run first", NotificationIcon::Error); return; }
    auto cbf = cbfEnv();
    logEnv("analyze", pl);
    if (cbf.active && cbf.physicsBypass) {
        notify("Analyze: turn OFF 'Physics Bypass' in Click Between Frames first.\n"
               "It makes every run different. (CBF itself is fine.)", NotificationIcon::Error);
        return;
    }
    bool alsoReleases = Mod::get()->getSettingValue<bool>("analyze-releases");
    m.maxWindow = std::clamp(settingInt("fw-max-window"), 2, 30);
    m.subSteps = std::clamp(settingInt("fw-subframe"), 0, 6);
    m.targets.clear();
    int nMid = 0;
    for (size_t i = 0; i < m.inputs.size(); i++) {
        if (m.inputs[i].mid) nMid++;
        if (m.inputs[i].down || alsoReleases) m.targets.push_back(i);
    }
    if (m.targets.empty()) { notify("Analyze: no press inputs", NotificationIcon::Error); return; }
    m.results.assign(m.targets.size(), WinResult{});
    m.lastTargetStep = m.inputs[m.targets.back()].step;
    m.testCount = 0;
    m.c240 = m.c120 = m.c60 = 0;
    m.tightest = 1e9;
    m.nAnchored = m.nFull = 0;
    m.offset = 0.0;
    m.speed = std::max(1.f, static_cast<float>(settingInt("analyze-speed")));
    m.speedPhase = false;
    setAnalyzeSpeed(1.f); // the first baseline always verifies at 1x
    int cps = pl->m_checkpointArray ? pl->m_checkpointArray->count() : 0;
    FW_LOG("ANALYZE start inputs={} targets={} midStep={} maxWindow={} subframeSteps={} releases={} checkpoints={} (clearing)",
        m.inputs.size(), m.targets.size(), nMid, m.maxWindow, m.subSteps, alsoReleases ? 1 : 0, cps);
    pl->removeAllCheckpoints();
    m.fastMode = Mod::get()->getSettingValue<bool>("analyze-fast");
    m.anchors.clear();
    m.nextAnchorCapture = 0;
    m.anchorStep.assign(m.targets.size(), 0);
    for (size_t i = 0; i < m.targets.size(); i++)
        m.anchorStep[i] = std::max(0, m.inputs[m.targets[i]].step - m.maxWindow - 2);
    m.capturingAnchors = m.fastMode && (m.speed <= 1.f);
    m.baseTrack.clear();
    m.trackBase = true;
    m.checkingSpeed = false;
    m.speedDrift = false;
    m.baseDriftMax = 0.f;
    m.baseDriftStep = -1;
    m.verifyingAnchor = false;
    m.anchorDrift = false;
    m.tapMode = false;
    m.baseline = true;       // first run is the unshifted determinism check
    m.targetIdx = 0;
    m.mode = Mode::Analyzing;
    updateHud();
    notify(fmt::format("Analyzing {} inputs... hands off, no speedhack/noclip", m.targets.size()),
        NotificationIcon::Info);
    beginTest(); // baseline run
}

} // namespace

// Exposed for the frame-window diagnostics (src/probe.cpp): they must not run
// while the macro system is driving the player.
namespace fw {
bool macroBusy() { return Macro::get().mode != Mode::Idle; }
} // namespace fw

// ---------------------------------------------------------------------------
class $modify(MacroBGL, GJBaseGameLayer) {
    static void onModify(auto& self) {
        // the step counter must advance before ANY other mod's per-step code runs
        // (CBF's click-on-steps delivers inputs at the top of processCommands)
        (void)self.setHookPriority("GJBaseGameLayer::processCommands", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::update", Priority::First);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& m = Macro::get();
        if (m.mode == Mode::Recording && !fw::ghost::isSim()) {
            InputEdge e;
            e.step = m.step;
            e.button = button;
            e.player1 = isPlayer1;
            e.down = down;
            if (g_inP1Update > 0) {
                // delivered inside the player update = a CBF mid-step input; its
                // position inside the step is finalized once the step's total
                // update time is known (next processCommands)
                e.mid = true;
                g_acc.pending.push_back({ m.inputs.size(), std::max(g_acc.el[0], g_acc.el[1]) });
            }
            g_capturing = true;
            g_capMask = 0;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            g_capturing = false;
            e.mask = g_capMask;
            m.inputs.push_back(e);
            return;
        }
        if (m.mode == Mode::Playing || m.mode == Mode::Analyzing) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processQueuedButtons(float dt, bool clearQueue) {
        auto& m = Macro::get();
        if (m.mode == Mode::Playing || m.mode == Mode::Analyzing) {
            // boundary inputs (and any overdue mid-step ones) go in here, at the
            // point vanilla applies real inputs; this step's mid-step inputs wait
            // for the player update split
            while (m.schedPos < m.sched.size()) {
                auto const& ev = m.sched[m.schedPos];
                if (ev.step > m.step) break;
                if (ev.mid && ev.step == m.step) break;
                applyInput(this, m.inputs[ev.idx]);
                m.schedPos++;
            }
            if (m.mode == Mode::Playing && !m.finishedNotified
                && m.schedPos >= m.sched.size() && !m.inputs.empty()) {
                m.finishedNotified = true;
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
        if (!fw::ghost::isSim()) {
            g_pcSerial++;
            if (m.mode == Mode::Recording) finalizePending(m);
        }
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
            if (m_player1 && m.step >= 0 && m.step < static_cast<int>(m.track.size())) {
                auto pos = m_player1->getPosition();
                auto rec = m.track[m.step];
                float dx = pos.x - rec.first, dy = pos.y - rec.second;
                float drift = std::sqrt(dx * dx + dy * dy);
                if (drift > m.maxDrift) { m.maxDrift = drift; m.maxDriftStep = m.step; }
            }
            // live frame-perfect tally: count as we pass each FP click, ding only for
            // the rates the user has selected to display.
            bool a240 = false, a120 = false, a60 = false, aw = false;
            while (m.pi240 < m.fp240.size() && m.fp240[m.pi240] <= m.step) { m.pi240++; a240 = true; }
            while (m.pi120 < m.fp120.size() && m.fp120[m.pi120] <= m.step) { m.pi120++; a120 = true; }
            while (m.pi60  < m.fp60.size()  && m.fp60[m.pi60]   <= m.step) { m.pi60++;  a60  = true; }
            while (m.piWin < m.playWins.size() && m.playWins[m.piWin].first <= m.step) {
                m.lastPlayWin = m.playWins[m.piWin].second; m.piWin++; aw = true;
            }
            bool s240 = Mod::get()->getSettingValue<bool>("show-240");
            bool s120 = Mod::get()->getSettingValue<bool>("show-120");
            bool s60  = Mod::get()->getSettingValue<bool>("show-60");
            if (!s240 && !s120 && !s60) s240 = true; // mirror the HUD default
            bool d240 = s240 && a240, d120 = s120 && a120, d60 = s60 && a60;
            if (d240 || d120 || d60)
                playDing(d240 ? kDing240 : d120 ? kDing120 : kDing60);
            if (a240 || a120 || a60 || aw) updateHud();
        } else if (m.mode == Mode::Analyzing && !m.testResolved) {
            // 1x baseline: record the ground-truth reference track, and compare it
            // to the RECORDED run (does the replay reproduce what you played?)
            if (m.baseline && m.trackBase && m_player1) {
                auto pos = m_player1->getPosition();
                if (static_cast<int>(m.baseTrack.size()) <= m.step)
                    m.baseTrack.resize(m.step + 1, { 0.f, 0.f, 0.f });
                m.baseTrack[m.step] = { pos.x, pos.y,
                    static_cast<float>(m_player1->m_yVelocity) };
                if (m.step >= 0 && m.step < static_cast<int>(m.track.size())) {
                    auto rec = m.track[m.step];
                    float dx = pos.x - rec.first, dy = pos.y - rec.second;
                    float d = std::sqrt(dx * dx + dy * dy);
                    if (d > m.baseDriftMax) { m.baseDriftMax = d; m.baseDriftStep = m.step; }
                }
            }
            // speed-up verification: the sped-up baseline must retrace the 1x track.
            if (m.baseline && m.checkingSpeed && m_player1
                && m.step < static_cast<int>(m.baseTrack.size())) {
                auto pos = m_player1->getPosition();
                auto const& bt = m.baseTrack[m.step];
                if (std::fabs(pos.x - bt.x) > 0.25f || std::fabs(pos.y - bt.y) > 0.25f) {
                    m.speedDrift = true;
                    m.testResolved = true;
                    m.lastSurvived = false;
                }
            }
            // fast mode: snapshot state just before each input as the baseline reaches it
            if (m.capturingAnchors && m.baseline) {
                if (auto plc = PlayLayer::get()) {
                    while (m.nextAnchorCapture < m.targets.size()
                           && m.step >= m.anchorStep[m.nextAnchorCapture]) {
                        if (auto cp = plc->markCheckpoint())
                            m.anchors[m.nextAnchorCapture] =
                                { cp, plc->m_randomSeed, plc->m_replayRandSeed, m.step };
                        m.nextAnchorCapture++;
                    }
                }
            }
            // Death = the player stopped MOVING.
            float cx = m.lastCx, cy = m.lastCy;
            bool deadFlag = false;
            if (m_player1) {
                auto pos = m_player1->getPosition();
                cx = pos.x; cy = pos.y;
                deadFlag = m_player1->m_isDead;
            }
            bool moved = std::fabs(cx - m.lastCx) > 0.05f || std::fabs(cy - m.lastCy) > 0.05f;
            if (moved) { m.lastProgressStep = m.step; m.testFrames = 0; }
            m.lastCx = cx; m.lastCy = cy;
            int still = m.step - m.lastProgressStep;
            bool dead = (deadFlag && still >= kDeadStall) || still >= kStall;
            if (dead) m.deathStep = m.lastProgressStep;

            if (m.baseline) {
                if (dead) { m.testResolved = true; m.lastSurvived = false; }
                else if (m.step >= m.marginEnd) { m.testResolved = true; m.lastSurvived = true; }
            } else if (m.verifyingAnchor) {
                if (!dead && m_player1 && m.step < static_cast<int>(m.baseTrack.size())) {
                    auto const& bt = m.baseTrack[m.step];
                    if (std::fabs(cx - bt.x) > 0.1f || std::fabs(cy - bt.y) > 0.1f) {
                        m.anchorDrift = true;
                        m.testResolved = true;
                        m.lastSurvived = false;
                    }
                }
                if (!m.testResolved) {
                    if (dead) { m.testResolved = true; m.lastSurvived = false; }
                    else if (m.step >= m.marginEnd) { m.testResolved = true; m.lastSurvived = true; }
                }
            } else if (dead) {
                m.testResolved = true;
                m.lastSurvived = !m.deathCountsAgainstTarget();
            } else if (m.step >= m.marginEnd) {
                m.testResolved = true;
                m.lastSurvived = true;
            } else if (m_player1 && !deadFlag && m_player1->m_isOnGround
                       && (!m_player2 || !m_player2->isVisible())
                       && m.step > m.probeSettleStep
                       && m.step < static_cast<int>(m.baseTrack.size())) {
                // early exit: grounded at the baseline's exact x/y/vy after the whole
                // shifted gesture has fired = the trajectory re-joined the baseline.
                auto const& bt = m.baseTrack[m.step];
                if (std::fabs(cx - bt.x) < 0.1f && std::fabs(cy - bt.y) < 0.1f
                    && std::fabs(static_cast<float>(m_player1->m_yVelocity) - bt.vy) < 0.1f) {
                    m.testResolved = true;
                    m.lastSurvived = true;
                }
            }
        }
    }

    void update(float dt) {
        auto& m = Macro::get();
        // While the macro drives the player, real clicks must not reach the game.
        // GD queues them here with timestamps; Click Between Frames reads this
        // queue to split steps, so clearing it keeps CBF from splitting (and
        // perturbing) a replay when you touch the mouse.
        if ((m.mode == Mode::Playing || m.mode == Mode::Analyzing)
            && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == this)
            m_queuedButtons.clear();

        if (m.mode == Mode::Analyzing) {
            for (int i = 0; i < kFF && !m.testResolved; i++)
                GJBaseGameLayer::update(dt);
            // hang guard: a test that never resolves (stuck/desync) is treated
            // as "survived" so it can't fabricate a frame-perfect, then we move on.
            if (!m.testResolved && ++m.testFrames > 2400) {
                log::warn("[fw] test timeout (target {}, offset {:.3f})", m.targetIdx, m.offset);
                m.testResolved = true;
                m.lastSurvived = !m.baseline && !m.verifyingAnchor;
                if (!m.baseline && !m.verifyingAnchor) m.hadTimeout = true;
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
// Player hooks. Three layers, by priority:
//   First    -- marks "inside player 1's update" (recording: a handleButton
//               in here was delivered mid-step by CBF)
//   Late     -- replay: CBF-exact step split (runs inside CBF's own hook, which
//               passes through untouched because the queue is empty on replay)
//   VeryLate -- recording: accumulates how much of the step has been simulated
// ---------------------------------------------------------------------------
class $modify(FwRegionPlayer, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::update", Priority::First);
    }

    void update(float dt) {
        auto pl = PlayLayer::get();
        bool p1 = pl && this == pl->m_player1 && !fw::ghost::isSim();
        if (p1) g_inP1Update++;
        PlayerObject::update(dt);
        if (p1) g_inP1Update--;
    }
};

// CBF's decomp of PlayerObject::resetCollisionLog (inlined on Windows).
static void fwResetCollisionLog(PlayerObject* p) {
    p->m_collisionLogTop->removeAllObjects();
    p->m_collisionLogBottom->removeAllObjects();
    p->m_collisionLogLeft->removeAllObjects();
    p->m_collisionLogRight->removeAllObjects();
    p->m_lastCollisionLeft = -1;
    p->m_lastCollisionRight = -1;
    p->m_lastCollisionBottom = -1;
    p->m_lastCollisionTop = -1;
}

// ===========================================================================
// CBF-exact step split (replay side).
//
// This reproduces Click Between Frames v1.5.0's PlayerObject::update split
// line for line (theyareonit/Click-Between-Frames, src/main.cpp): the player
// is simulated up to the click's position inside the step, collisions and
// rotation are resolved there, the input is applied, and the rest of the step
// is simulated. Whether a player splits at all ("not buffering"), the
// on-ground fix, the slope/dart collision delta, the leftover rotation and
// m_lastPosition restore, P2's update being folded into P1's, and the ship
// rotation workaround all match CBF -- so a CBF run replays as CBF played it,
// and a shifted probe lands exactly where a CBF click at that time would.
// Works with or without CBF loaded: the physics is ours, not CBF's.
// ===========================================================================
class $modify(FwSplitPlayer, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::update", Priority::Late);
        (void)self.setHookPriority("PlayerObject::updateRotation", Priority::Late);
#ifdef GEODE_IS_WINDOWS
        (void)self.setHookPriority("PlayerObject::updateShipRotation", Priority::Late);
#endif
    }

    void fwSplitStep(PlayLayer* pl, float stepDelta, std::vector<std::pair<size_t, double>> const& evs) {
        auto& m = Macro::get();
        PlayerObject* p2 = pl->m_player2;
        bool isDual = pl->m_gameState.m_isDualMode;
        bool p1StartedOnGround = this->m_isOnGround;
        bool p2StartedOnGround = p2 ? p2->m_isOnGround : false;

        auto notBuffering = [](PlayerObject* p, bool grounded) {
            return grounded
                || (p->m_touchingRings && p->m_touchingRings->count())
                || p->m_isDashing
                || (p->m_isDart || p->m_isBird || p->m_isShip || p->m_isSwing);
        };

        g_split.p1Pos = this->getPosition();
        g_split.p2Pos = p2 ? p2->getPosition() : CCPoint{};
        g_split.p1Split = notBuffering(this, p1StartedOnGround);
        g_split.p2Split = p2 && isDual && notBuffering(p2, p2StartedOnGround);
        g_split.inputThisStep = true;

        // substep factors, exactly as CBF's buildStepQueue computes them
        std::vector<double> factors;
        double elapsed = 0.0;
        for (auto const& e : evs) {
            factors.push_back(std::clamp(e.second - elapsed, kSmallest, 1.0));
            elapsed = e.second;
        }
        factors.push_back(std::max(kSmallest, 1.0 - elapsed));

        g_split.midStep = true;
        bool firstLoop = true;
        for (size_t k = 0; k < factors.size(); k++) {
            bool endStep = (k + 1 == factors.size());
            // CBF applies each input when the NEXT substep is popped
            if (k > 0) applyInput(pl, m.inputs[evs[k - 1].first]);
            const float substepDelta = stepDelta * factors[k];
            g_split.rotationDelta = substepDelta;

            if (g_split.p1Split) {
                PlayerObject::update(substepDelta);
                if (!endStep) {
                    if (firstLoop && ((this->m_yVelocity < 0) ^ this->m_isUpsideDown))
                        this->m_isOnGround = p1StartedOnGround;
                    if (!this->m_isOnSlope || this->m_isDart) pl->checkCollisions(this, 0.0f, true);
                    else pl->checkCollisions(this, stepDelta, true);
                    PlayerObject::updateRotation(substepDelta);
                    fwResetCollisionLog(this);
                }
            } else if (endStep) {
                PlayerObject::update(stepDelta);
            }

            if (g_split.p2Split) {
                p2->update(substepDelta);
                if (!endStep) {
                    if (firstLoop && ((p2->m_yVelocity < 0) ^ p2->m_isUpsideDown))
                        p2->m_isOnGround = p2StartedOnGround;
                    if (!p2->m_isOnSlope || p2->m_isDart) pl->checkCollisions(p2, 0.0f, true);
                    else pl->checkCollisions(p2, stepDelta, true);
                    p2->updateRotation(substepDelta);
                    fwResetCollisionLog(p2);
                }
            } else if (endStep && p2) {
                p2->update(stepDelta);
            }
            firstLoop = false;
        }
        g_split.midStep = false;
    }

    void update(float dt) {
        auto pl = PlayLayer::get();
        if (!pl || !driving() || fw::ghost::isSim() || g_split.midStep) {
            PlayerObject::update(dt);
            return;
        }
        if (this == pl->m_player2) {
            // already stepped inside player 1's split this step (CBF semantics)
            if (g_split.inputThisStep) return;
            PlayerObject::update(dt);
            return;
        }
        if (this != pl->m_player1) {
            PlayerObject::update(dt);
            return;
        }
        // every player-1 update starts a fresh step for the split bookkeeping
        g_split.inputThisStep = false;
        g_split.p1Split = g_split.p2Split = false;

        auto& m = Macro::get();
        std::vector<std::pair<size_t, double>> evs;
        while (m.schedPos < m.sched.size()) {
            auto const& ev = m.sched[m.schedPos];
            if (ev.step > m.step) break;
            if (!ev.mid && ev.step == m.step) break; // boundary input: processQueuedButtons' job
            evs.push_back({ ev.idx, ev.step < m.step ? 0.0 : ev.frac });
            m.schedPos++;
        }
        if (evs.empty()) {
            PlayerObject::update(dt);
            return;
        }
        fwSplitStep(pl, dt, evs);
    }

    void updateRotation(float t) {
        auto pl = PlayLayer::get();
        if (pl && driving() && !g_split.midStep && !fw::ghost::isSim()) {
            // finish the rotation left incomplete by the split, and restore the
            // pre-step position move triggers / spider read (CBF does the same)
            if (this == pl->m_player1 && g_split.p1Split) {
                PlayerObject::updateRotation(g_split.rotationDelta);
                this->m_lastPosition = g_split.p1Pos;
                return;
            }
            if (this == pl->m_player2 && g_split.p2Split) {
                PlayerObject::updateRotation(g_split.rotationDelta);
                this->m_lastPosition = g_split.p2Pos;
                return;
            }
        }
        PlayerObject::updateRotation(t);
    }

#ifdef GEODE_IS_WINDOWS
    void updateShipRotation(float t) {
        auto pl = PlayLayer::get();
        if (pl && driving() && g_split.inputThisStep && !fw::ghost::isSim()
            && (this == pl->m_player1 || this == pl->m_player2)) {
            g_shipRotDelta = t;
            PlayerObject::updateShipRotation(1.0f / 1024); // CBF's workaround, see fwSlerp2D
            g_shipRotDelta = 0.f;
            return;
        }
        PlayerObject::updateShipRotation(t);
    }
#endif
};

class $modify(FwAccumPlayer, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::update", Priority::VeryLate);
    }

    void update(float dt) {
        if (Macro::get().mode == Mode::Recording && !fw::ghost::isSim()) {
            if (auto pl = PlayLayer::get()) {
                if (this == pl->m_player1) g_acc.el[0] += dt;
                else if (this == pl->m_player2) g_acc.el[1] += dt;
            }
        }
        PlayerObject::update(dt);
    }

    // which players a real input reached (recording only)
    void fwCapture() {
        if (!g_capturing) return;
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (this == pl->m_player1) g_capMask |= 1;
        else if (this == pl->m_player2) g_capMask |= 2;
    }

    bool pushButton(PlayerButton b) {
        fwCapture();
        return PlayerObject::pushButton(b);
    }

    bool releaseButton(PlayerButton b) {
        fwCapture();
        return PlayerObject::releaseButton(b);
    }
};

#ifdef GEODE_IS_WINDOWS
// CBF's ship-rotation workaround: during an input step CBF calls
// updateShipRotation with 1/1024 and rescales Slerp2D's factor back to the real
// delta. Same address and math as CBF v1.5.0 (GD 2.2081 Windows). Only active
// while our replay split sets g_shipRotDelta; otherwise a pure pass-through.
static float fwSlerp2D(float p0, float p1, float p2) {
    auto orig = reinterpret_cast<float (*)(float, float, float)>(geode::base::get() + 0x71ef0);
    if (g_shipRotDelta != 0.f) {
        g_shipRotDelta *= p2 * 1024;
        return orig(p0, p1, g_shipRotDelta);
    }
    return orig(p0, p1, p2);
}

$on_mod(Loaded) {
    auto res = Mod::get()->hook(
        reinterpret_cast<void*>(geode::base::get() + 0x71ef0),
        &fwSlerp2D, "Slerp2D (futuremod CBF replay)",
        tulip::hook::TulipConvention::Default);
    if (!res) log::warn("[fw] Slerp2D hook failed: {}", res.unwrapErr());
}
#endif

// ---------------------------------------------------------------------------
class $modify(MacroPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        auto& m = Macro::get();
        // a fresh level never starts inside a macro op: a leaked Playing /
        // Analyzing mode would silently block all input here
        if (m.mode == Mode::Analyzing) setAnalyzeSpeed(1.f);
        m.mode = Mode::Idle;
        m.anchors.clear(); // never keep checkpoint refs from another level
        resetSplit();
        g_acc = RecAcc{};
        if (macroEnabled()) m.loadForLevel(levelKeyFor(level));
        return true;
    }

    void onQuit() {
        auto& m = Macro::get();
        if (m.mode == Mode::Analyzing) setAnalyzeSpeed(1.f);
        if (m.mode == Mode::Playing) logPlaybackVerdict("quit");
        if (m.mode == Mode::Recording) stopRecording(); // leaving must not lose the run
        m.mode = Mode::Idle;
        m.anchors.clear();
        resetSplit();
        PlayLayer::onQuit();
    }

    void resetLevel() {
        auto& m = Macro::get();
        if (m.mode == Mode::Playing && m.step > 0) logPlaybackVerdict("died");
        PlayLayer::resetLevel();
        int cpCount = m_checkpointArray ? m_checkpointArray->count() : 0;
        resetSplit();
        if (m.mode == Mode::Recording) {
            g_acc = RecAcc{};
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
            m.schedPos = 0;
            m.step = 0;
            m.gameTime = 0;
            m.finishedNotified = false;
            m.verdictLogged = false;
            m.maxDrift = 0.f;
            m.maxDriftStep = -1;
            m.pi240 = m.pi120 = m.pi60 = m.piWin = 0; // restart the live tally
            m.lastPlayWin = -1.0;
            if (m.haveSeed) { m_randomSeed = m.seed1; m_replayRandSeed = m.seed2; }
        } else if (m.mode == Mode::Analyzing) {
            if (!m.inBeginTest && !m.testResolved) {
                // GD reset the level on its own (the post-death auto-restart):
                // an authoritative death signal.
                m.deathStep = m.lastProgressStep;
                m.testResolved = true;
                if (m.baseline || m.verifyingAnchor) m.lastSurvived = false;
                else m.lastSurvived = !m.deathCountsAgainstTarget();
                log::debug("[fw] external reset -> death @ step {} (target {}, offset {:.3f})",
                    m.deathStep, m.targetIdx, m.offset);
            }
            m.schedPos = 0;
            m.step = 0;
            m.gameTime = 0;
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
            g_acc = RecAcc{};
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
        // intentionally does nothing special: death is detected by loss of
        // movement in processCommands, since destroyPlayer can fire without killing.
        PlayLayer::destroyPlayer(p, o);
    }

    void levelComplete() {
        auto& m = Macro::get();
        if (m.mode == Mode::Analyzing) {
            m.testResolved = true;
            m.lastSurvived = true;
            return; // don't actually end the level mid-analysis
        }
        if (m.mode == Mode::Playing) logPlaybackVerdict("complete");
        bool wasRecording = m.mode == Mode::Recording;
        PlayLayer::levelComplete();
        // a completed run is the run you wanted: save it without needing J
        if (wasRecording && m.mode == Mode::Recording) stopRecording();
    }
};

// ---------------------------------------------------------------------------
// In-level "Macros" menu (in the pause screen): load this level's saved macro,
// and open the folder with the macros + window exports.
// ---------------------------------------------------------------------------
class $modify(MacroPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!macroEnabled()) return; // master switch off: no in-level macro menu
        auto menu = CCMenu::create();
        auto spr = ButtonSprite::create("Macros");
        spr->setScale(0.6f);
        menu->addChild(CCMenuItemSpriteExtra::create(spr, this, menu_selector(MacroPauseLayer::onMacros)));
        auto spr2 = ButtonSprite::create("Files");
        spr2->setScale(0.6f);
        menu->addChild(CCMenuItemSpriteExtra::create(spr2, this, menu_selector(MacroPauseLayer::onFiles)));
        menu->alignItemsHorizontallyWithPadding(6.f);
        auto win = CCDirector::sharedDirector()->getWinSize();
        menu->setPosition(90.f, win.height - 22.f); // top-left corner of the pause screen
        menu->setZOrder(100);
        this->addChild(menu);
    }

    void onFiles(CCObject*) {
        // refresh this level's exports from its saved analysis first, so the
        // files are always current (and a fixed exporter needs no re-analysis)
        auto& m = Macro::get();
        if (PlayLayer::get() && !m.savedWindows.empty() && m.mode == Mode::Idle) writeExports();
        file::openFolder(macrosDir());
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
            fmt::format("Saved macro for this level:\n<cy>{}</c> inputs, <cg>{}</c> analyzed.\n\nLoad it?", nin, nwin),
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
        if (!macroEnabled()) return false;
        if (down && !repeat) {
            if (Macro::get().mode == Mode::Recording) stopRecording();
            else startRecording();
            return true;
        }
        return false;
    });
    listenForKeybindSettingPresses("macro-play", [](Keybind const&, bool down, bool repeat, double) -> bool {
        if (!macroEnabled()) return false;
        if (down && !repeat) {
            if (Macro::get().mode == Mode::Playing) stopPlaying();
            else startPlaying();
            return true;
        }
        return false;
    });
    listenForKeybindSettingPresses("macro-analyze", [](Keybind const&, bool down, bool repeat, double) -> bool {
        if (!macroEnabled()) return false;
        if (down && !repeat) {
            if (Macro::get().mode == Mode::Analyzing) {
                Macro::get().mode = Mode::Idle;
                setAnalyzeSpeed(1.f);
                resetSplit();
                if (auto pl = PlayLayer::get()) pl->removeAllCheckpoints();
                Macro::get().anchors.clear();
                notify("Analyze: cancelled", NotificationIcon::Info);
            }
            else startAnalysis();
            return true;
        }
        return false;
    });
}
