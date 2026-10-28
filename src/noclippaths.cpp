#include "common.hpp"
#include "core.hpp"
#include "paths_logic.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/ui/Popup.hpp>

#include <cstring>
#include <map>

using namespace geode::prelude;

// ===========================================================================
// Noclip paths. While you play a level (normal or practice), your path is
// recorded; every attempt that didn't end in a real death is kept, however
// it ended (reset key, finishing, quitting). With noclip on you never really
// die, so that's every noclip attempt; with it off, deaths are left out.
// In the editor the kept runs are drawn on the level: bright where it's close
// to the middle of all your runs there, fading the further it strays -- so
// you see where you actually go, and how consistently, to buff the level
// around it. Runs that went somewhere else most of the way are left out, and
// brief far-off moments aren't drawn. The averaging is in paths_logic.hpp
// (tested on its own). Not a cheat: it only records and draws.
// ===========================================================================

namespace {

using fw::paths::Pt;
using fw::paths::Run;

bool recordOn() {
    return Mod::get()->getSettingValue<bool>("macro-enabled") && Mod::get()->getSettingValue<bool>("noclip-paths");
}
bool showOn() { return Mod::get()->getSettingValue<bool>("noclip-paths-show"); }
size_t keepRuns() {
    return static_cast<size_t>(std::clamp<int64_t>(Mod::get()->getSettingValue<int64_t>("noclip-paths-keep"), 1, 500));
}

// same key as the macros (online: ID, local: name), without start position
std::string levelKey(GJGameLevel* lvl) {
    if (!lvl) return "";
    int id = lvl->m_levelID.value();
    if (id > 0) return std::to_string(id);
    std::string o;
    for (char c : std::string(lvl->m_levelName)) o += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return "local_" + (o.empty() ? std::string("level") : o);
}

std::filesystem::path fileFor(std::string const& key) {
    auto d = Mod::get()->getSaveDir() / "macros";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d / (key + ".noclip");
}

// file: "FWNP", u32 version, u32 runs, then per run: u32 points, points x (f32 x, f32 y)
std::vector<Run> loadRuns(std::string const& key) {
    std::vector<Run> runs;
    auto res = file::readBinary(fileFor(key));
    if (!res) return runs;
    auto const& b = res.unwrap();
    size_t pos = 0;
    auto take = [&](void* dst, size_t n) {
        if (pos + n > b.size()) return false;
        std::memcpy(dst, b.data() + pos, n);
        pos += n;
        return true;
    };
    char magic[4];
    uint32_t ver = 0, count = 0;
    if (!take(magic, 4) || std::memcmp(magic, "FWNP", 4) != 0 || !take(&ver, 4) || !take(&count, 4)) return runs;
    for (uint32_t r = 0; r < count; r++) {
        uint32_t n = 0;
        if (!take(&n, 4) || n > 10'000'000) break;
        Run run(n);
        if (!take(run.data(), n * sizeof(Pt))) break;
        runs.push_back(std::move(run));
    }
    return runs;
}

void saveRuns(std::string const& key, std::vector<Run> const& runs) {
    std::vector<uint8_t> out;
    auto put = [&](void const* p, size_t n) {
        auto c = static_cast<uint8_t const*>(p);
        out.insert(out.end(), c, c + n);
    };
    put("FWNP", 4);
    uint32_t ver = 1, count = static_cast<uint32_t>(runs.size());
    put(&ver, 4);
    put(&count, 4);
    for (auto const& r : runs) {
        uint32_t n = static_cast<uint32_t>(r.size());
        put(&n, 4);
        put(r.data(), n * sizeof(Pt));
    }
    if (auto res = file::writeBinary(fileFor(key), out); !res)
        log::warn("[paths] save failed: {}", res.unwrapErr());
}

// ---- recording ------------------------------------------------------------

struct Recorder {
    std::string key;
    Run pts;
    int touches = 0;   // hazard touches that didn't kill (noclip), for the log
    bool died = false; // a real death this attempt: not kept
    bool ended = false; // finished: the end animation isn't play
};
Recorder g_rec;

bool recordingAllowed() {
    // your own play only: never the macro's replay or the analyzer's runs
    return recordOn() && (fw::core::idle() || fw::core::recording());
}

constexpr size_t kMinPoints = 240; // 1 s of play (240 steps a second)

// The attempt is over (reset key, respawn, finish, quit). Safe to call more
// than once: the second call has nothing left.
void finishRun(char const* why, bool deadNow) {
    if (!g_rec.key.empty() && !g_rec.pts.empty()) {
        if (g_rec.died || deadNow) {
            FW_LOG("NOCLIP run skipped ({}): ended in a real death", why);
        } else if (g_rec.pts.size() < kMinPoints) {
            FW_VLOG("NOCLIP run skipped ({}): only {} points", why, g_rec.pts.size());
        } else {
            auto runs = loadRuns(g_rec.key);
            runs.push_back(fw::paths::thin(g_rec.pts, 2.0));
            while (runs.size() > keepRuns()) runs.erase(runs.begin());
            saveRuns(g_rec.key, runs);
            FW_LOG("NOCLIP run kept ({}): {} points, x {:.0f}..{:.0f}, {} hazard touches, {} runs for {}",
                why, g_rec.pts.size(), g_rec.pts.front().x, g_rec.pts.back().x, g_rec.touches, runs.size(), g_rec.key);
        }
    }
    g_rec.pts.clear();
    g_rec.touches = 0;
    g_rec.died = false;
}

// ---- editor display ---------------------------------------------------------

constexpr int kViewTag = 0x4D504E50; // 'MPNP'
constexpr double kChunk = 1000.0;    // level units per drawn piece

// Draws the runs around what the editor shows (a few 1000-unit chunks), so
// long levels with many runs stay cheap.
class PathsView : public CCNode {
public:
    static PathsView* create(std::vector<Run> runs) {
        auto v = new PathsView();
        if (v && v->init(std::move(runs))) { v->autorelease(); return v; }
        delete v;
        return nullptr;
    }

    size_t runCount() const { return m_runs.size(); }

private:
    std::vector<Run> m_runs;
    fw::paths::Stats m_stats;
    std::map<int, CCDrawNode*> m_chunks;

    bool init(std::vector<Run> runs) {
        if (!CCNode::init()) return false;
        runs = fw::paths::removeOutlierRuns(std::move(runs), 3.0);
        m_stats = fw::paths::aggregate(runs, 3.0);
        for (auto& r : runs) m_runs.push_back(fw::paths::thin(r, 3.0));
        this->scheduleUpdate();
        return true;
    }

    CCDrawNode* build(int chunk) {
        auto node = CCDrawNode::create();
        double x0 = chunk * kChunk, x1 = x0 + kChunk;
        for (auto const& r : m_runs) {
            for (size_t i = 1; i < r.size(); i++) {
                auto const& a = r[i - 1];
                auto const& b = r[i];
                double mx = 0.5 * (a.x + b.x);
                if (mx < x0 || mx >= x1) continue;
                Pt mid{ static_cast<float>(mx), 0.5f * (a.y + b.y) };
                if (fw::paths::isOutlierPoint(m_stats, mid)) continue; // a brief far-off moment: not drawn
                float w = fw::paths::weight(m_stats, mid);
                float alpha = 0.06f + 0.84f * w;
                // premultiplied colour (CCDrawNode blends that way)
                ccColor4F col{ 0.35f * alpha, 0.9f * alpha, 1.f * alpha, alpha };
                node->drawSegment({ a.x, a.y }, { b.x, b.y }, 1.3f, col);
            }
        }
        this->addChild(node);
        return node;
    }

    void update(float) override {
        auto parent = this->getParent();
        if (!parent) return;
        auto win = CCDirector::sharedDirector()->getWinSize();
        double left = parent->convertToNodeSpace({ 0.f, 0.f }).x;
        double right = parent->convertToNodeSpace({ win.width, win.height }).x;
        if (left > right) std::swap(left, right);
        int first = static_cast<int>(std::floor(left / kChunk)) - 1;
        int last = static_cast<int>(std::floor(right / kChunk)) + 1;
        for (auto it = m_chunks.begin(); it != m_chunks.end();) {
            if (it->first < first - 2 || it->first > last + 2) {
                it->second->removeFromParent();
                it = m_chunks.erase(it);
            } else ++it;
        }
        for (int c = first; c <= last; c++)
            if (!m_chunks.count(c)) m_chunks[c] = build(c);
    }
};

void refreshView(LevelEditorLayer* ed) {
    if (!ed || !ed->m_objectLayer) return;
    if (auto old = ed->m_objectLayer->getChildByTag(kViewTag)) old->removeFromParent();
    if (!showOn()) return;
    auto runs = loadRuns(levelKey(ed->m_level));
    if (runs.empty()) return;
    if (auto v = PathsView::create(std::move(runs))) {
        v->setTag(kViewTag);
        v->setID("noclip-paths"_spr);
        ed->m_objectLayer->addChild(v, 10000); // above the level's objects
    }
}

class NoclipPathsPopup : public geode::Popup {
    CCLabelBMFont* m_info = nullptr;
    std::string m_key;

    bool init() {
        if (!Popup::init(320.f, 170.f)) return false;
        this->setTitle("Noclip paths");
        auto ed = LevelEditorLayer::get();
        m_key = ed ? levelKey(ed->m_level) : "";
        auto size = m_mainLayer->getContentSize();
        m_info = CCLabelBMFont::create(" ", "bigFont.fnt");
        m_info->setScale(0.38f);
        m_info->setAlignment(kCCTextAlignmentCenter);
        m_info->setPosition({ size.width / 2.f, size.height / 2.f + 12.f });
        m_mainLayer->addChild(m_info);

        auto menu = CCMenu::create();
        auto toggle = ButtonSprite::create(showOn() ? "Hide" : "Show");
        toggle->setScale(0.7f);
        menu->addChild(CCMenuItemSpriteExtra::create(toggle, this, menu_selector(NoclipPathsPopup::onToggle)));
        auto clear = ButtonSprite::create("Clear runs");
        clear->setScale(0.7f);
        menu->addChild(CCMenuItemSpriteExtra::create(clear, this, menu_selector(NoclipPathsPopup::onClear)));
        menu->alignItemsHorizontallyWithPadding(10.f);
        menu->setPosition({ size.width / 2.f, 32.f });
        m_mainLayer->addChild(menu);
        refresh();
        return true;
    }

    void refresh() {
        auto runs = m_key.empty() ? std::vector<Run>{} : loadRuns(m_key);
        size_t n = runs.size();
        size_t out = n - fw::paths::removeOutlierRuns(std::move(runs), 3.0).size();
        std::string left = out ? fmt::format(" ({} left out: went elsewhere)", out) : "";
        m_info->setString(n == 0
            ? "No noclip runs for this level yet.\nPlay it with noclip on, then come back."
            : fmt::format("{} noclip run{} recorded{}.\nBright = where you usually go.",
                n, n == 1 ? "" : "s", left).c_str());
    }

    void onToggle(CCObject* sender) {
        Mod::get()->setSettingValue<bool>("noclip-paths-show", !showOn());
        if (auto spr = static_cast<ButtonSprite*>(static_cast<CCMenuItemSpriteExtra*>(sender)->getNormalImage()))
            spr->setString(showOn() ? "Hide" : "Show");
        auto ed = LevelEditorLayer::get();
        refreshView(ed);
        // the editor's own PATH button follows
        if (ed && ed->m_editorUI)
            if (auto btn = typeinfo_cast<CCMenuItemToggler*>(ed->m_editorUI->getChildByIDRecursive("noclip-paths-button"_spr)))
                btn->toggle(showOn());
    }

    void onClear(CCObject*) {
        if (m_key.empty()) return;
        std::error_code ec;
        std::filesystem::remove(fileFor(m_key), ec);
        refresh();
        refreshView(LevelEditorLayer::get());
    }

public:
    static NoclipPathsPopup* create() {
        auto p = new NoclipPathsPopup();
        if (p->init()) { p->autorelease(); return p; }
        delete p;
        return nullptr;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------

class $modify(NpBGL, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this || !recordingAllowed() || g_rec.ended) return;
        auto p = m_player1;
        if (p && p->m_isDead) g_rec.died = true;
        if (!p || p->m_isDead || !p->getParent() || !m_objectLayer) return;
        auto pos = m_objectLayer->convertToNodeSpace(p->getParent()->convertToWorldSpace(p->getPosition()));
        g_rec.pts.push_back({ pos.x, pos.y });
    }
};

class $modify(NpPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        // outermost: noclip (e.g. Mega Hack) stops destroyPlayer from killing
        // you; we only look at whether you're still alive after the call
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::First);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_rec = Recorder{};
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        g_rec.key = levelKey(level);
        return true;
    }

    void destroyPlayer(PlayerObject* p, GameObject* obj) {
        PlayLayer::destroyPlayer(p, obj);
        if (!p) return;
        if (p->m_isDead) { g_rec.died = true; return; } // a real death (either player)
        // touched a hazard and still alive = a noclip touch (only for the
        // log; GD's own anti-cheat spike doesn't count)
        if (obj && obj != m_anticheatSpike) g_rec.touches++;
    }

    bool deadNow() {
        return (m_player1 && m_player1->m_isDead) || (m_player2 && m_player2->m_isDead && m_gameState.m_isDualMode);
    }

    // Every way an attempt can end: the reset key / pause > restart (either
    // kind), the respawn after a death, finishing, quitting. Whichever runs
    // first keeps the run; the rest find nothing left.
    void resetLevel() {
        finishRun("restart", deadNow());
        g_rec.ended = false;
        PlayLayer::resetLevel();
    }

    void resetLevelFromStart() {
        finishRun("restart", deadNow());
        g_rec.ended = false;
        PlayLayer::resetLevelFromStart();
    }

    void fullReset() {
        finishRun("restart", deadNow());
        g_rec.ended = false;
        PlayLayer::fullReset();
    }

    void levelComplete() {
        finishRun("complete", false);
        g_rec.ended = true;
        PlayLayer::levelComplete();
    }

    void onQuit() {
        finishRun("quit", deadNow());
        PlayLayer::onQuit();
    }
};

class $modify(NpEditor, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        if (!LevelEditorLayer::init(level, noUI)) return false;
        refreshView(this);
        return true;
    }
};

// A toggle in the editor itself (top-left, next to undo / redo): cyan while
// the paths are shown, gray while hidden.
class $modify(NpEditorUI, EditorUI) {
    static CCNode* pathsIcon(bool on) {
        auto label = CCLabelBMFont::create("PATH", "bigFont.fnt");
        label->setScale(0.35f);
        auto spr = EditorButtonSprite::create(label, on ? EditorBaseColor::Cyan : EditorBaseColor::Gray);
        spr->setScale(0.8f);
        return spr;
    }

    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) return false;
        if (!Mod::get()->getSettingValue<bool>("macro-enabled")) return true;
        auto toggle = CCMenuItemToggler::create(pathsIcon(false), pathsIcon(true), this,
            menu_selector(NpEditorUI::onTogglePaths));
        toggle->setID("noclip-paths-button"_spr);
        toggle->toggle(showOn());
        if (auto menu = this->getChildByID("undo-menu")) {
            // Node IDs lays this row out: it lands right after hide-UI
            menu->addChild(toggle);
            menu->updateLayout();
        } else {
            // without Node IDs: our own little menu under the undo buttons
            auto menu2 = CCMenu::create();
            menu2->addChild(toggle);
            auto win = CCDirector::sharedDirector()->getWinSize();
            menu2->setPosition({ 26.f, win.height - 80.f });
            this->addChild(menu2, 10);
        }
        return true;
    }

    void onTogglePaths(CCObject*) {
        // (the toggler flips its own look after this call)
        bool show = !showOn();
        Mod::get()->setSettingValue<bool>("noclip-paths-show", show);
        refreshView(m_editorLayer);
        if (show && m_editorLayer && loadRuns(levelKey(m_editorLayer->m_level)).empty())
            Notification::create("No noclip runs for this level yet: play it with noclip on first",
                NotificationIcon::Info)->show();
    }
};

class $modify(NpEditorPause, EditorPauseLayer) {
    void customSetup() {
        EditorPauseLayer::customSetup();
        if (!Mod::get()->getSettingValue<bool>("macro-enabled")) return;
        auto menu = CCMenu::create();
        auto spr = ButtonSprite::create("Noclip paths");
        spr->setScale(0.55f);
        menu->addChild(CCMenuItemSpriteExtra::create(spr, this, menu_selector(NpEditorPause::onNoclipPaths)));
        auto win = CCDirector::sharedDirector()->getWinSize();
        menu->setPosition({ win.width / 2.f, 22.f });
        this->addChild(menu, 100);
    }

    void onNoclipPaths(CCObject*) {
        if (auto p = NoclipPathsPopup::create()) p->show();
    }
};
