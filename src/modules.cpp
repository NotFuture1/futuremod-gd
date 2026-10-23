#include "common.hpp"
#include "core.hpp"
#include "display.hpp"

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
// Cheat modules: click indicator, click practice, LDM instaswap.
//
// All of them are cheats. While any is on, a red CHEATING label sits in the
// bottom-right corner and the level runs in GD's test mode (like a start pos
// run), so nothing is saved as legit progress.
// ===========================================================================

namespace {

bool macroOn() { return Mod::get()->getSettingValue<bool>("macro-enabled"); }
bool indicatorOn() { return macroOn() && Mod::get()->getSettingValue<bool>("mod-click-indicator"); }
bool practiceOn() { return macroOn() && Mod::get()->getSettingValue<bool>("mod-click-practice"); }
bool ldmOn() { return macroOn() && Mod::get()->getSettingValue<bool>("mod-ldm-swap"); }
bool anyCheat() { return indicatorOn() || practiceOn() || ldmOn(); }

// once a module was on during this level, the whole session is cheated
bool g_cheatedSession = false;

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
// Click practice
// ---------------------------------------------------------------------------

struct Owned {
    double from, to;   // level clock: you're in control in here
    int button;
    int mask;          // players to release at the end if still held
    bool player1;
};

struct Practice {
    std::string key;                 // level the selection belongs to
    std::vector<double> selected;    // level-clock times of picked presses
    std::vector<Owned> owned;
    std::vector<int> endedSync;      // owned[] already synced at their end
    size_t ptr = 0;                  // next bot input to consider
    double lastNow = -1e9;
    bool dirty = true;
};
Practice g_prac;

std::string selKey() { return "practice-" + fw::core::baseKey(); }

void loadSelection() {
    auto key = fw::core::baseKey();
    if (g_prac.key == key) return;
    g_prac = Practice{};
    g_prac.key = key;
    std::istringstream ss(Mod::get()->getSavedValue<std::string>(selKey(), ""));
    std::string tok;
    while (std::getline(ss, tok, ',')) if (!tok.empty()) g_prac.selected.push_back(std::strtod(tok.c_str(), nullptr));
    std::sort(g_prac.selected.begin(), g_prac.selected.end());
}

void saveSelection() {
    std::string s;
    for (double t : g_prac.selected) s += fmt::format("{}{:.3f}", s.empty() ? "" : ",", t);
    Mod::get()->setSavedValue(selKey(), s);
    g_prac.dirty = true;
}

bool isSelected(double t) {
    return std::any_of(g_prac.selected.begin(), g_prac.selected.end(), [&](double s) { return std::fabs(s - t) < 0.75; });
}

void setSelected(double t, bool on) {
    auto& v = g_prac.selected;
    v.erase(std::remove_if(v.begin(), v.end(), [&](double s) { return std::fabs(s - t) < 0.75; }), v.end());
    if (on) { v.push_back(t); std::sort(v.begin(), v.end()); }
}

void rebuildOwned() {
    if (!g_prac.dirty) return;
    g_prac.dirty = false;
    g_prac.owned.clear();
    g_prac.endedSync.clear();
    auto const& ins = fw::core::botInputs();
    double margin = static_cast<double>(Mod::get()->getSettingValue<int64_t>("practice-margin"));
    for (double ts : g_prac.selected) {
        // the recorded press at that time, and its release
        for (size_t i = 0; i < ins.size(); i++) {
            if (!ins[i].down || std::fabs(ins[i].t - ts) >= 0.75) continue;
            double rel = ins[i].t;
            for (size_t j = i + 1; j < ins.size(); j++)
                if (ins[j].button == ins[i].button && ins[j].player1 == ins[i].player1) {
                    if (!ins[j].down) rel = ins[j].t;
                    break;
                }
            int mask = ins[i].mask ? ins[i].mask : (ins[i].player1 ? 1 : 2);
            g_prac.owned.push_back({ ins[i].t - margin, rel + margin, ins[i].button, mask, ins[i].player1 });
            break;
        }
    }
}

Owned const* ownedAt(double t) {
    for (auto const& o : g_prac.owned) if (t >= o.from && t <= o.to) return &o;
    return nullptr;
}

bool practiceActive() {
    if (!practiceOn() || !fw::core::idle()) return false;
    loadSelection();
    return !g_prac.selected.empty() && !fw::core::botInputs().empty();
}

// once per physics step, at the point GD applies inputs: the bot plays every
// input outside your picked clicks; at the end of a picked click, anything you
// still hold is let go so the bot continues from the recorded state
void practiceStep(GJBaseGameLayer* layer) {
    rebuildOwned();
    auto const& ins = fw::core::botInputs();
    double now = fw::core::now();
    if (now < g_prac.lastNow) { // new attempt / respawn
        g_prac.ptr = static_cast<size_t>(std::lower_bound(ins.begin(), ins.end(), now - 0.5,
            [](fw::core::BotInput const& a, double t) { return a.t < t; }) - ins.begin());
        g_prac.endedSync.clear();
    }
    g_prac.lastNow = now;
    while (g_prac.ptr < ins.size() && std::llround(ins[g_prac.ptr].t) <= std::llround(now)) {
        auto const& in = ins[g_prac.ptr++];
        if (!ownedAt(in.t)) fw::core::applyBotInput(layer, in);
    }
    for (size_t i = 0; i < g_prac.owned.size(); i++) {
        auto const& o = g_prac.owned[i];
        if (std::llround(o.to) != std::llround(now)) continue;
        if (std::find(g_prac.endedSync.begin(), g_prac.endedSync.end(), static_cast<int>(i)) != g_prac.endedSync.end()) continue;
        g_prac.endedSync.push_back(static_cast<int>(i));
        PlayerObject* ps[2] = { layer->m_player1, layer->m_player2 };
        for (int k = 0; k < 2; k++) {
            if (!(o.mask & (1 << k)) || !ps[k]) continue;
            auto it = ps[k]->m_holdingButtons.find(o.button);
            if (it != ps[k]->m_holdingButtons.end() && it->second)
                ps[k]->releaseButton(static_cast<PlayerButton>(o.button));
        }
    }
}

// The picker: every recorded press with its window; tick the ones to practice.
class PracticePopup : public geode::Popup {
    struct Row { double t; int n; double w; bool hasW; };
    std::vector<Row> m_rows;
    std::vector<CCMenuItemToggler*> m_toggles;
    CCLabelBMFont* m_count = nullptr;

    bool init() {
        if (!Popup::init(400.f, 270.f)) return false;
        this->setTitle("Click practice");
        loadSelection();

        auto const& ins = fw::core::botInputs();
        auto const& wins = fw::core::levelTimings();
        int n = 0;
        for (auto const& in : ins) {
            if (!in.down) continue;
            n++;
            Row r{ in.t, n, 0.0, false };
            for (auto const& w : wins)
                if (std::fabs(w.t - in.t) < 0.75 && w.p2 == !in.player1) { r.w = w.w; r.hasW = true; break; }
            m_rows.push_back(r);
        }

        auto size = m_mainLayer->getContentSize();
        if (m_rows.empty()) {
            auto l = CCLabelBMFont::create("Record (and analyze) this level first.", "bigFont.fnt");
            l->setScale(0.4f);
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
            tog->toggle(isSelected(r.t));
            menu->addChild(tog);
            m_toggles.push_back(tog);
            auto l = CCLabelBMFont::create(fmt::format("#{}   {:.2f}s   {}", r.n, r.t / 240.0,
                r.hasW ? fw::display::windowText(r.w) + "f" : std::string("not analyzed")).c_str(), "bigFont.fnt");
            l->setScale(0.38f);
            l->setAnchorPoint({ 0.f, 0.5f });
            l->setPosition({ 40.f, y });
            if (r.hasW && r.w <= 2.0 + 1e-9) l->setColor({ 255, 120, 120 });
            menu->addChild(l);
        }
        scroll->scrollToTop();

        // quick picks
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
        m_count->setScale(0.45f);
        m_count->setPosition({ size.width / 2.f, 12.f });
        m_mainLayer->addChild(m_count);
        refresh();
        return true;
    }

    void refresh() {
        for (size_t i = 0; i < m_toggles.size(); i++) m_toggles[i]->toggle(isSelected(m_rows[i].t));
        if (m_count) m_count->setString(fmt::format("{} of {} clicks picked - the bot plays the rest",
            g_prac.selected.size(), m_rows.size()).c_str());
        saveSelection();
    }

    void onToggle(CCObject* sender) {
        auto i = static_cast<size_t>(static_cast<CCNode*>(sender)->getTag());
        if (i >= m_rows.size()) return;
        // the toggler flips after this callback: read the current state, invert
        setSelected(m_rows[i].t, !isSelected(m_rows[i].t));
        Loader::get()->queueInMainThread([self = Ref(this)] { self->refresh(); });
    }
    void pickTight(double maxW) {
        g_prac.selected.clear();
        for (auto const& r : m_rows) if (r.hasW && r.w <= maxW + 1e-9) g_prac.selected.push_back(r.t);
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
        // the died-on timing may be a release: pick its press
        double best = NAN;
        for (auto const& r : m_rows) if (r.t <= d + 0.75) best = r.t;
        if (!std::isnan(best)) setSelected(best, true);
        refresh();
    }
    void onClear(CCObject*) { g_prac.selected.clear(); refresh(); }

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

// object strings ("1,31,2,x,3,y,...") of every start position in a level string
std::vector<std::string> startPosObjects(std::string const& raw) {
    std::vector<std::string> out;
    size_t pos = raw.find(';'); // skip the level header
    while (pos != std::string::npos && pos + 1 < raw.size()) {
        size_t end = raw.find(';', pos + 1);
        auto obj = raw.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
        if (obj.rfind("1,31,", 0) == 0) out.push_back(obj);
        pos = end;
    }
    return out;
}

GJGameLevel* buildCopy(GJGameLevel* from, GJGameLevel* target) {
    auto raw = decodeLevel(target->m_levelString);
    auto existing = startPosObjects(raw);
    int added = 0;
    for (auto const& sp : startPosObjects(decodeLevel(from->m_levelString))) {
        if (std::find(existing.begin(), existing.end(), sp) != existing.end()) continue;
        if (!raw.empty() && raw.back() != ';') raw += ';';
        raw += sp + ';';
        added++;
    }
    auto lvl = GJGameLevel::create();
    lvl->m_levelName = target->m_levelName;
    lvl->m_levelString = ZipUtils::compressString(raw, false, 0);
    lvl->m_audioTrack = target->m_audioTrack;
    lvl->m_songID = target->m_songID;
    lvl->m_songIDs = target->m_songIDs;
    lvl->m_sfxIDs = target->m_sfxIDs;
    lvl->m_twoPlayerMode = target->m_twoPlayerMode;
    lvl->m_levelType = GJLevelType::Editor; // a local copy: nothing saved to the online level
    FW_LOG("LDM swap -> '{}' (+{} start positions from '{}')", std::string(target->m_levelName), added,
        std::string(from->m_levelName));
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

    void processQueuedButtons(float dt, bool clearQueue) {
        auto pl = PlayLayer::get();
        if (pl && static_cast<GJBaseGameLayer*>(pl) == this && practiceActive()) practiceStep(this);
        GJBaseGameLayer::processQueuedButtons(dt, clearQueue);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // click practice: outside your picked clicks the bot plays; ignore you
        auto pl = PlayLayer::get();
        if (pl && static_cast<GJBaseGameLayer*>(pl) == this && practiceActive()) {
            rebuildOwned();
            if (!ownedAt(fw::core::now())) return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(FwModPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_cheatedSession = false;
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        markCheating(this);
        g_prac.dirty = true;
        g_prac.lastNow = -1e9;
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
        PlayLayer::resetLevel();
        markCheating(this);
        g_prac.lastNow = -1e9;
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
        if (!practiceOn() && !ldmOn()) return;
        auto menu = CCMenu::create();
        if (practiceOn()) {
            auto spr = ButtonSprite::create("Practice");
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
