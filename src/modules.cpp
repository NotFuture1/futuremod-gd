#include "common.hpp"
#include "core.hpp"
#include "display.hpp"
#include "assist_logic.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/loader/SettingV3.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <sstream>

using namespace geode::prelude;

// ===========================================================================
// Cheat modules: click indicator, window extender, LDM instaswap.
//
// All of them are cheats. While any is on, a red CHEATING label sits in the
// bottom-right corner and the level runs in GD's test mode (like a start pos
// run), so nothing is saved as legit progress.
// ===========================================================================

namespace {

bool macroOn() { return Mod::get()->getSettingValue<bool>("macro-enabled"); }
bool indicatorOn() { return macroOn() && Mod::get()->getSettingValue<bool>("mod-click-indicator"); }
bool extenderOn() { return macroOn() && Mod::get()->getSettingValue<bool>("mod-window-extender"); }
bool ldmOn() { return macroOn() && Mod::get()->getSettingValue<bool>("mod-ldm-swap"); }
bool anyCheat() { return indicatorOn() || extenderOn() || ldmOn(); }

// This attempt was played with a cheat module on. Per attempt: once every
// module is off, the next attempt is legit again. (Frame windows on death,
// the replay rings and the analyzer are not cheats and never set this.)
bool g_cheatedSession = false;
bool g_testModeOrig = false; // GD's own test mode (start pos) before we touched it

constexpr int kCheatLabelTag = 0x4D50434C; // 'MPCL'

void markCheating(PlayLayer* pl) {
    if (!pl) return;
    bool on = anyCheat();
    if (on) {
        g_cheatedSession = true;
        pl->m_isTestMode = true; // GD's start-pos mode: progress isn't saved
    }
    auto label = static_cast<CCLabelBMFont*>(pl->getChildByTag(kCheatLabelTag));
    if (!label && g_cheatedSession) {
        label = CCLabelBMFont::create("CHEATING", "bigFont.fnt");
        label->setTag(kCheatLabelTag);
        label->setID("cheating-label"_spr);
        label->setColor({ 255, 60, 60 });
        label->setScale(0.45f);
        label->setAnchorPoint({ 1.f, 0.f });
        auto win = CCDirector::sharedDirector()->getWinSize();
        label->setPosition(win.width - 6.f, 4.f);
        pl->addChild(label, 10002);
    }
    if (label) label->setVisible(g_cheatedSession);
}

// ---------------------------------------------------------------------------
// Click indicator: a vertical line in the level at the dead centre of every
// upcoming click's frame window. It scrolls with the level; your icon crossing
// it is the moment to click. Placed with GD's own time->position conversion,
// RELATIVE to where your icon is now (so a constant offset in that conversion
// cancels out); this works for imported windows and from start positions too.
// ---------------------------------------------------------------------------

constexpr int kLinesTag = 0x4D50434C + 7; // container on the PlayLayer
constexpr int kMaxLines = 48;

CCNode* linesLayer(PlayLayer* pl) {
    auto n = pl->getChildByTag(kLinesTag);
    if (!n) {
        n = CCNode::create();
        n->setTag(kLinesTag);
        n->setID("click-indicator-lines"_spr);
        pl->addChild(n, 9997); // above the level, under rings / HUD
    }
    return n;
}

void hideIndicators(PlayLayer* pl) {
    if (auto n = pl->getChildByTag(kLinesTag)) n->setVisible(false);
}

// one line: a thin full-height bar + its window number at the top
CCNode* lineNode(CCNode* layer, int i, float h) {
    if (auto c = layer->getChildByTag(i)) return c;
    auto node = CCNode::create();
    node->setTag(i);
    auto bar = CCLayerColor::create({ 255, 255, 255, 255 }, 2.f, h);
    bar->ignoreAnchorPointForPosition(false);
    bar->setAnchorPoint({ 0.5f, 0.f });
    bar->setTag(1);
    node->addChild(bar);
    auto l = CCLabelBMFont::create(" ", "bigFont.fnt");
    l->setScale(0.3f);
    l->setAnchorPoint({ 0.5f, 1.f });
    l->setPosition({ 0.f, h - 4.f });
    l->setTag(2);
    node->addChild(l);
    layer->addChild(node);
    return node;
}

void tickIndicator(PlayLayer* pl) {
    auto const& list = fw::core::levelTimings();
    auto p = pl->m_player1;
    if (list.empty() || !pl->m_objectLayer || !p || p->m_isDead) { hideIndicators(pl); return; }
    auto layer = linesLayer(pl);
    layer->setVisible(true);
    auto win = CCDirector::sharedDirector()->getWinSize();
    double now = fw::core::now();
    // where GD's model says "now" is vs where your icon really is
    float baseX = pl->posForTime(static_cast<float>(now / 240.0)).x;
    // the icon in level (object-layer) space, whatever its actual parent is
    CCPoint icon = pl->m_objectLayer->convertToNodeSpace(p->getParent()->convertToWorldSpace(p->getPosition()));
    int used = 0;
    for (auto const& t : list) {
        double centre = t.t + (t.loEdge() + t.hiEdge()) / 2.0;
        if (centre < now - 8.0) continue; // already passed
        float x = icon.x + (pl->posForTime(static_cast<float>(centre / 240.0)).x - baseX);
        CCPoint screen = pl->convertToNodeSpace(pl->m_objectLayer->convertToWorldSpace({ x, icon.y }));
        if (screen.x > win.width + 10.f) break; // further ones are off screen
        if (screen.x < -10.f) continue;
        if (used >= kMaxLines) break;
        auto node = lineNode(layer, used++, win.height);
        node->setVisible(true);
        node->setPosition({ screen.x, 0.f });
        auto col = fw::display::windowColour(t.w);
        auto bar = static_cast<CCLayerColor*>(node->getChildByTag(1));
        bar->setColor(col);
        bar->setOpacity(t.down == 0 ? 110 : 220); // releases fainter
        auto label = static_cast<CCLabelBMFont*>(node->getChildByTag(2));
        label->setString(fw::display::windowText(t.w).c_str());
        label->setColor(col);
    }
    for (int i = used; i < kMaxLines; i++)
        if (auto c = layer->getChildByTag(i)) c->setVisible(false);
}

// ---------------------------------------------------------------------------
// Window extender: every click you did NOT pick gets X frames of extra window
// each side (early inputs held until the window opens; late ones caught by
// the game waiting at the window's last frame). Picked clicks: no help. The
// decisions live in assist_logic.hpp, tested on its own (tests/assist_test.cpp);
// this part feeds it the level's windows and carries out what it says.
// ---------------------------------------------------------------------------

struct Extender {
    std::string key;                // level the picks belong to
    std::vector<double> picked;     // level-clock times of picked presses
    fw::assist::Engine engine;
    std::vector<fw::Timing> const* builtFrom = nullptr;
    size_t builtN = static_cast<size_t>(-1);
    double builtX = -1.0;
    bool dirty = true;
    bool injecting = false;         // our own re-injected inputs pass through
};
Extender g_ext;

std::string pickKey() { return "practice-" + fw::core::baseKey(); }

void loadPicks() {
    auto key = fw::core::baseKey();
    if (g_ext.key == key) return;
    g_ext.key = key;
    g_ext.picked.clear();
    g_ext.dirty = true;
    std::istringstream ss(Mod::get()->getSavedValue<std::string>(pickKey(), ""));
    std::string tok;
    while (std::getline(ss, tok, ',')) if (!tok.empty()) g_ext.picked.push_back(std::strtod(tok.c_str(), nullptr));
    std::sort(g_ext.picked.begin(), g_ext.picked.end());
}

void savePicks() {
    std::string s;
    for (double t : g_ext.picked) s += fmt::format("{}{:.3f}", s.empty() ? "" : ",", t);
    Mod::get()->setSavedValue(pickKey(), s);
    g_ext.dirty = true;
}

bool isPicked(double t) {
    return std::any_of(g_ext.picked.begin(), g_ext.picked.end(), [&](double p) { return std::fabs(p - t) < 0.75; });
}

void setPicked(double t, bool on) {
    auto& v = g_ext.picked;
    v.erase(std::remove_if(v.begin(), v.end(), [&](double p) { return std::fabs(p - t) < 0.75; }), v.end());
    if (on) { v.push_back(t); std::sort(v.begin(), v.end()); }
}

double extraFrames() { return static_cast<double>(Mod::get()->getSettingValue<int64_t>("extender-frames")); }

// rebuild the engine when the level's windows, the picks or X change
void syncExtender() {
    loadPicks();
    auto const& list = fw::core::levelTimings();
    double X = extraFrames();
    if (!g_ext.dirty && g_ext.builtFrom == &list && g_ext.builtN == list.size() && g_ext.builtX == X) return;
    g_ext.dirty = false;
    g_ext.builtFrom = &list;
    g_ext.builtN = list.size();
    g_ext.builtX = X;
    // a picked click = its press and the release that follows it
    std::vector<bool> skip(list.size(), false);
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].down == 0 || !isPicked(list[i].t)) continue;
        skip[i] = true;
        for (size_t j = i + 1; j < list.size(); j++) {
            if (list[j].p2 != list[i].p2) continue;
            if (list[j].down == 0) skip[j] = true;
            break;
        }
    }
    std::vector<fw::assist::Timing> help;
    for (size_t i = 0; i < list.size(); i++) {
        if (skip[i]) continue;
        auto const& t = list[i];
        help.push_back({ t.t, t.loEdge(), t.hiEdge(), t.down != 0, t.p2 });
    }
    g_ext.engine.setTimings(std::move(help), X);
}

bool extenderActive(GJBaseGameLayer* layer) {
    if (!extenderOn() || !fw::core::idle()) return false;
    auto pl = PlayLayer::get();
    if (!pl || static_cast<GJBaseGameLayer*>(pl) != layer || !pl->m_player1 || pl->m_player1->m_isDead) return false;
    syncExtender();
    return g_ext.engine.active();
}

// press / release exactly as GD would for a real input on that player
void injectInput(GJBaseGameLayer* layer, bool down, bool p2) {
    auto pl = PlayLayer::get();
    bool twoP = pl && pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode;
    bool dual = layer->m_gameState.m_isDualMode;
    auto act = [&](PlayerObject* p) {
        if (!p) return;
        if (down) p->pushButton(PlayerButton::Jump);
        else p->releaseButton(PlayerButton::Jump);
    };
    if (twoP) act(p2 ? layer->m_player2 : layer->m_player1);
    else {
        act(layer->m_player1);
        if (dual) act(layer->m_player2);
    }
}

// The picker: every click in the level with its window; tick the ones to
// practice WITHOUT help (everything else gets the extender's help).
class PracticePopup : public geode::Popup {
    struct Row { double t; int n; double w; };
    std::vector<Row> m_rows;
    std::vector<CCMenuItemToggler*> m_toggles;
    CCLabelBMFont* m_count = nullptr;

    bool init() {
        if (!Popup::init(400.f, 270.f)) return false;
        this->setTitle("Window extender: clicks to practice");
        loadPicks();

        int n = 0;
        for (auto const& w : fw::core::levelTimings()) {
            if (w.down == 0) continue; // releases follow their press
            m_rows.push_back({ w.t, ++n, w.w });
        }

        auto size = m_mainLayer->getContentSize();
        if (m_rows.empty()) {
            auto l = CCLabelBMFont::create("No frame windows for this level yet:\nanalyze it, or Import a file.", "bigFont.fnt");
            l->setScale(0.4f);
            l->setAlignment(kCCTextAlignmentCenter);
            l->setPosition(size / 2.f);
            m_mainLayer->addChild(l);
            return true;
        }

        constexpr float rowH = 22.f;
        auto scroll = ScrollLayer::create({ 360.f, 160.f });
        scroll->setPosition({ (size.width - 360.f) / 2.f, 55.f });
        m_mainLayer->addChild(scroll);
        float total = std::max(160.f, rowH * m_rows.size());
        scroll->m_contentLayer->setContentSize({ 360.f, total });
        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        menu->setContentSize({ 360.f, total });
        scroll->m_contentLayer->addChild(menu);
        for (size_t i = 0; i < m_rows.size(); i++) {
            auto const& r = m_rows[i];
            float y = total - rowH * (i + 0.5f);
            auto tog = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(PracticePopup::onToggle), 0.55f);
            tog->setTag(static_cast<int>(i));
            tog->setPosition({ 20.f, y });
            tog->toggle(isPicked(r.t));
            menu->addChild(tog);
            m_toggles.push_back(tog);
            auto l = CCLabelBMFont::create(fmt::format("#{}   {:.2f}s   {}f", r.n, r.t / 240.0,
                fw::display::windowText(r.w)).c_str(), "bigFont.fnt");
            l->setScale(0.38f);
            l->setAnchorPoint({ 0.f, 0.5f });
            l->setPosition({ 40.f, y });
            l->setColor(fw::display::windowColour(r.w));
            menu->addChild(l);
        }
        scroll->scrollToTop();

        auto quick = CCMenu::create();
        auto add = [&](char const* text, SEL_MenuHandler sel) {
            auto spr = ButtonSprite::create(text, "bigFont.fnt", "GJ_button_04.png", 0.6f);
            spr->setScale(0.55f);
            quick->addChild(CCMenuItemSpriteExtra::create(spr, this, sel));
        };
        add("1f", menu_selector(PracticePopup::onTight1));
        add("2f", menu_selector(PracticePopup::onTight2));
        add("4f", menu_selector(PracticePopup::onTight4));
        add("Died on", menu_selector(PracticePopup::onDiedOn));
        add("Clear", menu_selector(PracticePopup::onClear));
        quick->alignItemsHorizontallyWithPadding(6.f);
        quick->setPosition({ size.width / 2.f, 32.f });
        m_mainLayer->addChild(quick);

        m_count = CCLabelBMFont::create(" ", "goldFont.fnt");
        m_count->setScale(0.42f);
        m_count->setPosition({ size.width / 2.f, 12.f });
        m_mainLayer->addChild(m_count);
        refresh();
        return true;
    }

    void refresh() {
        for (size_t i = 0; i < m_toggles.size(); i++) m_toggles[i]->toggle(isPicked(m_rows[i].t));
        if (m_count) m_count->setString(fmt::format("{} picked (no help) - the other {} get +-{} frames",
            g_ext.picked.size(), m_rows.size() - std::min(m_rows.size(), g_ext.picked.size()),
            static_cast<int>(extraFrames())).c_str());
        savePicks();
    }

    void onToggle(CCObject* sender) {
        auto i = static_cast<size_t>(static_cast<CCNode*>(sender)->getTag());
        if (i >= m_rows.size()) return;
        setPicked(m_rows[i].t, !isPicked(m_rows[i].t));
        Loader::get()->queueInMainThread([self = Ref(this)] { self->refresh(); });
    }
    void pickTight(double maxW) {
        g_ext.picked.clear();
        for (auto const& r : m_rows) if (r.w <= maxW + 1e-9) g_ext.picked.push_back(r.t);
        refresh();
    }
    void onTight1(CCObject*) { pickTight(1.0); }
    void onTight2(CCObject*) { pickTight(2.0); }
    void onTight4(CCObject*) { pickTight(4.0); }
    void onDiedOn(CCObject*) {
        double d = fw::core::lastDiedOn();
        if (std::isnan(d)) {
            Notification::create("No death on a timing yet this level", NotificationIcon::Info)->show();
            return;
        }
        double best = NAN; // the died-on timing may be a release: pick its press
        for (auto const& r : m_rows) if (r.t <= d + 0.75) best = r.t;
        if (!std::isnan(best)) setPicked(best, true);
        refresh();
    }
    void onClear(CCObject*) { g_ext.picked.clear(); refresh(); }

public:
    static PracticePopup* create() {
        auto p = new PracticePopup();
        if (p->init()) { p->autorelease(); return p; }
        delete p;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// LDM instaswap
// ---------------------------------------------------------------------------

std::string keyOfLevel(GJGameLevel* lvl) {
    if (!lvl) return "";
    int id = lvl->m_levelID.value();
    if (id > 0) return std::to_string(id);
    std::string o;
    for (char c : std::string(lvl->m_levelName)) o += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return "local_" + o;
}

// the binding is stored both ways, so either level swaps to the other
std::string bindingOf(std::string const& key) { return Mod::get()->getSavedValue<std::string>("ldm-bind-" + key, ""); }

struct SwapState {
    Ref<GJGameLevel> origin; // the level you swapped away from (played as-is when swapping back)
    Ref<GJGameLevel> copy;   // the LDM version we built (with your start positions)
    std::optional<CCPoint> wantStartPos;
};
SwapState g_swap;

GJGameLevel* resolveTarget(std::string const& spec) {
    if (spec.rfind("id:", 0) == 0) {
        int id = std::atoi(spec.c_str() + 3);
        return id > 0 ? GameLevelManager::sharedState()->getSavedLevel(id) : nullptr;
    }
    if (spec.rfind("name:", 0) == 0) {
        auto name = spec.substr(5);
        auto llm = LocalLevelManager::sharedState();
        if (llm && llm->m_localLevels)
            for (auto lvl : CCArrayExt<GJGameLevel*>(llm->m_localLevels))
                if (std::string(lvl->m_levelName) == name) return lvl;
    }
    return nullptr;
}

std::string decodeLevel(gd::string const& s) {
    std::string raw = ZipUtils::decompressString(s, false, 0);
    return raw.empty() ? std::string(s) : raw;
}

// is this object string ("1,31,2,x,3,y,...") a start position (object id 31)?
bool isStartPos(std::string const& obj) {
    // key/value pairs separated by commas; key "1" = object id
    size_t i = 0;
    while (i < obj.size()) {
        size_t c1 = obj.find(',', i);
        if (c1 == std::string::npos) return false;
        size_t c2 = obj.find(',', c1 + 1);
        auto key = obj.substr(i, c1 - i);
        auto val = obj.substr(c1 + 1, (c2 == std::string::npos ? obj.size() : c2) - c1 - 1);
        if (key == "1") return val == "31";
        if (c2 == std::string::npos) return false;
        i = c2 + 1;
    }
    return false;
}

// split a decoded level string into its header and object strings
void splitLevel(std::string const& raw, std::string& header, std::vector<std::string>& objs) {
    objs.clear();
    size_t pos = raw.find(';');
    header = raw.substr(0, pos);
    while (pos != std::string::npos && pos + 1 < raw.size()) {
        size_t end = raw.find(';', pos + 1);
        auto obj = raw.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
        if (!obj.empty()) objs.push_back(obj);
        pos = end;
    }
}

// The LDM/ULDM copy: the target level with ALL of its start positions removed
// and replaced by the ones of the level you're on (your full-detail version),
// so every start pos you use is there, at the same spots.
GJGameLevel* buildCopy(GJGameLevel* from, GJGameLevel* target) {
    std::string header, srcHeader;
    std::vector<std::string> objs, srcObjs;
    splitLevel(decodeLevel(target->m_levelString), header, objs);
    splitLevel(decodeLevel(from->m_levelString), srcHeader, srcObjs);
    std::string raw = header + ";";
    int removed = 0, added = 0;
    for (auto const& o : objs) {
        if (isStartPos(o)) { removed++; continue; }
        raw += o + ";";
    }
    for (auto const& o : srcObjs)
        if (isStartPos(o)) { raw += o + ";"; added++; }
    auto lvl = GJGameLevel::create();
    lvl->m_levelName = target->m_levelName;
    lvl->m_levelString = ZipUtils::compressString(raw, false, 0);
    lvl->m_audioTrack = target->m_audioTrack;
    lvl->m_songID = target->m_songID;
    lvl->m_songIDs = target->m_songIDs;
    lvl->m_sfxIDs = target->m_sfxIDs;
    lvl->m_twoPlayerMode = target->m_twoPlayerMode;
    lvl->m_levelType = GJLevelType::Editor; // a local copy: nothing saved to the online level
    FW_LOG("LDM swap -> '{}': removed {} of its start positions, added {} from '{}' ({} objects)",
        std::string(target->m_levelName), removed, added, std::string(from->m_levelName), objs.size());
    if (added == 0)
        Notification::create("The level you're on has no start positions to carry over", NotificationIcon::Info)->show();
    return lvl;
}

void swapLevel() {
    auto pl = PlayLayer::get();
    if (!pl || !pl->m_level || !ldmOn() || !fw::core::idle()) return;
    if (pl->m_startPosObject) g_swap.wantStartPos = pl->m_startPosObject->getPosition();
    else g_swap.wantStartPos.reset();

    GJGameLevel* next = nullptr;
    if (g_swap.copy && pl->m_level == g_swap.copy && g_swap.origin) {
        next = g_swap.origin; // back to the level you came from
    } else {
        auto spec = bindingOf(keyOfLevel(pl->m_level));
        if (spec.empty()) {
            Notification::create("No LDM version bound to this level (pause menu > LDM)", NotificationIcon::Info)->show();
            return;
        }
        auto target = resolveTarget(spec);
        if (!target) {
            Notification::create("Couldn't find the bound level. If it's online, open it once so it's downloaded.",
                NotificationIcon::Error)->show();
            return;
        }
        if (std::string(target->m_levelString).empty()) {
            Notification::create("The bound level isn't downloaded yet: open it once (play it), then swap.",
                NotificationIcon::Error)->show();
            return;
        }
        g_swap.origin = pl->m_level;
        g_swap.copy = buildCopy(pl->m_level, target);
        next = g_swap.copy;
    }
    CCDirector::sharedDirector()->replaceScene(PlayLayer::scene(next, false, false));
}

class LdmPopup : public geode::Popup {
    TextInput* m_input = nullptr;
    CCLabelBMFont* m_state = nullptr;
    std::string m_key;

    bool init() {
        if (!Popup::init(340.f, 190.f)) return false;
        this->setTitle("LDM instaswap");
        auto pl = PlayLayer::get();
        GJGameLevel* cur = nullptr;
        if (pl) cur = (g_swap.copy && pl->m_level == g_swap.copy.data()) ? g_swap.origin.data() : pl->m_level;
        m_key = keyOfLevel(cur);
        auto size = m_mainLayer->getContentSize();

        m_state = CCLabelBMFont::create(" ", "bigFont.fnt");
        m_state->setScale(0.35f);
        m_state->setPosition({ size.width / 2.f, size.height - 55.f });
        m_mainLayer->addChild(m_state);

        m_input = TextInput::create(260.f, "LDM level ID, or local level name");
        m_input->setPosition({ size.width / 2.f, size.height / 2.f - 5.f });
        m_mainLayer->addChild(m_input);

        auto menu = CCMenu::create();
        auto bind = ButtonSprite::create("Bind");
        bind->setScale(0.7f);
        menu->addChild(CCMenuItemSpriteExtra::create(bind, this, menu_selector(LdmPopup::onBind)));
        auto unbind = ButtonSprite::create("Unbind");
        unbind->setScale(0.7f);
        menu->addChild(CCMenuItemSpriteExtra::create(unbind, this, menu_selector(LdmPopup::onUnbind)));
        menu->alignItemsHorizontallyWithPadding(10.f);
        menu->setPosition({ size.width / 2.f, 32.f });
        m_mainLayer->addChild(menu);
        refresh();
        return true;
    }

    void refresh() {
        auto b = bindingOf(m_key);
        m_state->setString(b.empty() ? "Not bound yet"
            : fmt::format("Bound to {}", b.rfind("id:", 0) == 0 ? "level ID " + b.substr(3) : "\"" + b.substr(5) + "\"").c_str());
    }

    void onBind(CCObject*) {
        auto text = std::string(m_input->getString());
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
        if (text.empty() || m_key.empty()) return;
        bool digits = std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
        auto spec = digits ? "id:" + text : "name:" + text;
        auto target = resolveTarget(spec);
        if (!target) {
            Notification::create(digits ? "Level not found: open it once (online) so GD has it saved"
                                        : "No local level with that exact name", NotificationIcon::Error)->show();
            return;
        }
        auto pl = PlayLayer::get();
        GJGameLevel* self = g_swap.copy && pl && pl->m_level == g_swap.copy ? g_swap.origin.data() : (pl ? pl->m_level : nullptr);
        Mod::get()->setSavedValue("ldm-bind-" + m_key, spec);
        // and back, so the LDM version swaps to this level
        if (self) {
            int id = self->m_levelID.value();
            Mod::get()->setSavedValue("ldm-bind-" + keyOfLevel(target),
                id > 0 ? fmt::format("id:{}", id) : "name:" + std::string(self->m_levelName));
        }
        refresh();
    }

    void onUnbind(CCObject*) {
        auto b = bindingOf(m_key);
        if (auto t = resolveTarget(b)) Mod::get()->setSavedValue("ldm-bind-" + keyOfLevel(t), std::string());
        Mod::get()->setSavedValue("ldm-bind-" + m_key, std::string());
        refresh();
    }

public:
    static LdmPopup* create() {
        auto p = new LdmPopup();
        if (p->init()) { p->autorelease(); return p; }
        delete p;
        return nullptr;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------

class $modify(FwModBGL, GJBaseGameLayer) {
    void update(float dt) {
        GJBaseGameLayer::update(dt);
        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return;
        markCheating(pl);
        if (indicatorOn() && fw::core::idle()) tickIndicator(pl);
        else hideIndicators(pl);
    }

    static void onModify(auto& self) {
        // inside Click Between Frames' hook (VeryEarly): CBF then sees our
        // step count, and the input queue it drains is still there for us
        (void)self.setHookPriority("GJBaseGameLayer::getModifiedDelta", Priority::Late);
    }

    // Window extender: how many physics steps run this frame. Holding the
    // game at a window's last good frame (0 steps, time dropped) is how a
    // late input can still land in the window.
    double getModifiedDelta(float dt) {
        double mod = GJBaseGameLayer::getModifiedDelta(dt);
        if (!extenderActive(this)) return mod;
        double stepLen = std::max(m_gameState.m_timeWarp, 1.f) / 240.0;
        int n = static_cast<int>(std::lround(mod / stepLen));
        std::vector<fw::assist::Queued> queued;
        for (auto const& q : m_queuedButtons)
            if (q.m_button == PlayerButton::Jump) queued.push_back({ q.m_isPush, q.m_isPlayer2 });
        auto plan = g_ext.engine.plan(static_cast<long long>(std::llround(fw::core::now())), n,
            static_cast<double>(dt) * 240.0, queued);
        if (plan.steps >= n) return mod;
        if (plan.carry) m_extraDelta += (n - plan.steps) * stepLen;
        return plan.steps * stepLen;
    }

    // held-back inputs come back at the step they're due
    void processQueuedButtons(float dt, bool clearQueue) {
        if (extenderActive(this)) {
            for (auto const& inj : g_ext.engine.due(static_cast<long long>(std::llround(fw::core::now())))) {
                g_ext.injecting = true;
                injectInput(this, inj.down, inj.p2);
                g_ext.injecting = false;
            }
        }
        GJBaseGameLayer::processQueuedButtons(dt, clearQueue);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!g_ext.injecting && button == static_cast<int>(PlayerButton::Jump) && extenderActive(this)
            && !g_ext.engine.onInput(static_cast<long long>(std::llround(fw::core::now())), down, !isPlayer1))
            return; // held back: due() brings it back inside the window
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(FwModPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_cheatedSession = false;
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        g_testModeOrig = m_isTestMode;
        markCheating(this);
        g_ext.engine.reset();
        // LDM swap: back on the start position you were on
        if (g_swap.wantStartPos) {
            auto want = *g_swap.wantStartPos;
            g_swap.wantStartPos.reset();
            StartPosObject* found = nullptr;
            if (m_objects)
                for (auto obj : CCArrayExt<GameObject*>(m_objects))
                    if (obj->m_objectID == 31 && ccpDistance(obj->getPosition(), want) < 2.f) {
                        found = static_cast<StartPosObject*>(obj);
                        break;
                    }
            FW_LOG("LDM swap: start pos at ({:.0f}, {:.0f}) {}", want.x, want.y, found ? "found" : "NOT found");
            if (found) {
                Loader::get()->queueInMainThread([self = Ref(this), found] {
                    if (PlayLayer::get() != self.data()) return;
                    self->m_startPosObject = found;
                    self->resetLevel();
                    self->startMusic();
                });
            }
        }
        return true;
    }

    void resetLevel() {
        // a new attempt: legit again if no cheat module is on now
        if (!anyCheat() && g_cheatedSession) {
            g_cheatedSession = false;
            m_isTestMode = g_testModeOrig;
        }
        PlayLayer::resetLevel();
        markCheating(this);
        g_ext.engine.reset(); // new attempt: clicks behind you are done
    }
};

// cheated runs never save progress, even outside test mode
class $modify(FwModLevel, GJGameLevel) {
    void savePercentage(int percent, bool isPracticeMode, int clicks, int attempts, bool isChkValid) {
        if (g_cheatedSession && PlayLayer::get()) return;
        GJGameLevel::savePercentage(percent, isPracticeMode, clicks, attempts, isChkValid);
    }
};

class $modify(FwModPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!extenderOn() && !ldmOn()) return;
        auto menu = CCMenu::create();
        if (extenderOn()) {
            auto spr = ButtonSprite::create("Extender");
            spr->setScale(0.6f);
            menu->addChild(CCMenuItemSpriteExtra::create(spr, this, menu_selector(FwModPause::onPractice)));
        }
        if (ldmOn()) {
            auto spr = ButtonSprite::create("LDM");
            spr->setScale(0.6f);
            menu->addChild(CCMenuItemSpriteExtra::create(spr, this, menu_selector(FwModPause::onLdm)));
        }
        menu->alignItemsHorizontallyWithPadding(6.f);
        auto win = CCDirector::sharedDirector()->getWinSize();
        menu->setPosition(90.f, win.height - 50.f); // under the macro menu
        menu->setZOrder(100);
        this->addChild(menu);
    }

    void onPractice(CCObject*) { if (auto p = PracticePopup::create()) p->show(); }
    void onLdm(CCObject*) { if (auto p = LdmPopup::create()) p->show(); }
};

$execute {
    listenForKeybindSettingPresses("ldm-swap-key", [](Keybind const&, bool down, bool repeat, double) -> bool {
        if (!down || repeat || !ldmOn() || !PlayLayer::get()) return false;
        swapLevel();
        return true;
    });
}
