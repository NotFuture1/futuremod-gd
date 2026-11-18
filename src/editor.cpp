#include "common.hpp"
#include "editor.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <cmath>
#include <iterator>
#include <map>
#include <optional>
#include <string>

using namespace geode::prelude;

// ===========================================================================
// Macro editor UI. Pause menu > "Edit macro".
//
// The level stays on screen, frozen at the playhead: what you see is the
// macro's run at that moment, with its path and clicks drawn on the level.
// Along the bottom, the timeline: gamemodes, holds (press -> release) with
// each click's frame window behind it, a ruler, and an overview of the whole
// macro. Drag a click to move it (a CBF macro moves on a sub-frame grid, a
// normal one by whole frames); when you let go, the changed part is played
// again from a save-state, so the path you then see is what the replay really
// does -- and where it dies, if it does.
// ===========================================================================

namespace {

using fw::ed::Edge;
using fw::ed::Model;
namespace rt = fw::edrt;

ccColor4F col(float r, float g, float b, float a) { return { r * a, g * a, b * a, a }; } // premultiplied

ccColor4F modeColor(uint8_t mode, bool mini) {
    float r = 0.6f, g = 0.6f, b = 0.6f;
    switch (mode) {
        case rt::kCube: r = 0.35f; g = 0.65f; b = 1.0f; break;
        case rt::kShip: r = 1.0f; g = 0.45f; b = 0.75f; break;
        case rt::kBall: r = 1.0f; g = 0.45f; b = 0.25f; break;
        case rt::kUfo: r = 1.0f; g = 0.85f; b = 0.25f; break;
        case rt::kWave: r = 0.25f; g = 0.95f; b = 0.95f; break;
        case rt::kRobot: r = 0.85f; g = 0.85f; b = 0.85f; break;
        case rt::kSpider: r = 0.7f; g = 0.45f; b = 1.0f; break;
        case rt::kSwing: r = 0.6f; g = 1.0f; b = 0.35f; break;
    }
    float k = mini ? 0.6f : 1.f;
    return col(r * k, g * k, b * k, 0.9f);
}

ccColor4F windowColor(double w, float a) {
    if (w <= 1.0) return col(1.f, 0.3f, 0.3f, a);
    if (w <= 2.0) return col(1.f, 0.6f, 0.2f, a);
    if (w <= 4.0) return col(1.f, 0.9f, 0.3f, a);
    return col(0.4f, 0.95f, 0.45f, a);
}

char const* speedName(float s) {
    if (s < 0.8f) return "0.5x";
    if (s < 1.0f) return "1x";
    if (s < 1.2f) return "2x";
    if (s < 1.45f) return "3x";
    return "4x";
}

double setting(char const* k) { return Mod::get()->getSettingValue<double>(k); }
bool settingOn(char const* k) { return Mod::get()->getSettingValue<bool>(k); }

double cbfSnap() {
    auto v = Mod::get()->getSettingValue<std::string>("editor-snap-cbf");
    auto slash = v.find('/');
    double d = slash == std::string::npos ? 8.0 : std::atof(v.c_str() + slash + 1);
    return d > 0 ? 1.0 / d : 0.125;
}

void rect(CCDrawNode* n, float x0, float y0, float x1, float y1, ccColor4F c) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    CCPoint v[4] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
    n->drawPolygon(v, 4, c, 0.f, col(0, 0, 0, 0));
}

std::string fmtFrames(double t) {
    if (std::fabs(t - std::round(t)) < 1e-9) return fmt::format("{:.0f}", t);
    return fmt::format("{:.4f}", t);
}

class MacroEditorUI;
MacroEditorUI* g_ui = nullptr;

class MacroEditorUI : public CCLayer {
public:
    static MacroEditorUI* create() {
        auto l = new MacroEditorUI();
        if (l->init()) { l->autorelease(); return l; }
        delete l;
        return nullptr;
    }

    void requestExit() {
        if (m_closing) return;
        if (m_model.dirty() && settingOn("editor-confirm-exit")) {
            createQuickPopup("Macro editor", "You have <cy>unsaved changes</c>.", "Discard", "Save",
                [this](FLAlertLayer*, bool save) {
                    close(save);
                });
            return;
        }
        close(false);
    }

private:
    Model m_model;
    std::vector<Edge> m_applied; // what the runtime currently simulates
    std::map<int, rt::Window> m_winByOrig;
    bool m_cbf = false;
    bool m_closing = false;

    // view: frames shown from m_v0, m_fpp frames per pixel
    double m_v0 = 0.0, m_fpp = 1.0;
    float m_L = 8.f, m_R = 560.f;
    // rows (y ranges) of the timeline panel
    float m_panelY0 = 0, m_panelY1 = 0, m_rulerY0 = 0, m_modeY0 = 0, m_modeY1 = 0;
    float m_p1Y0 = 0, m_p1Y1 = 0, m_p2Y0 = 0, m_p2Y1 = 0, m_ovY0 = 0, m_ovY1 = 0;
    bool m_hasP2 = false;

    int m_sel = 0; // selected edge id
    enum class Drag { None, Marker, Pan, Scrub, Overview } m_drag = Drag::None;
    double m_grab = 0.0;   // where on the marker it was grabbed (frames)
    float m_downX = 0.f;
    double m_downV0 = 0.0;
    bool m_moved = false;
    double m_dragFrom = 0.0;
    std::vector<Edge> m_beforeDrag;

    CCDrawNode* m_tl = nullptr;
    // drawn on this layer (screen space), NOT inside the level's object layer:
    // GD and other mods walk that layer's children as level objects
    CCDrawNode* m_ov = nullptr;
    CCLayerColor* m_panel = nullptr;
    CCLayerColor* m_bar = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_info = nullptr;
    CCLabelBMFont* m_titleLbl = nullptr;
    CCMenuItemSpriteExtra* m_playBtn = nullptr;
    CCMenuItemSpriteExtra* m_undoBtn = nullptr;
    CCMenuItemSpriteExtra* m_redoBtn = nullptr;
    std::vector<CCLabelBMFont*> m_rulerLabels;
    std::vector<CCLabelBMFont*> m_speedLabels;
    uint64_t m_lastRev = 0;
    bool m_dirtyDraw = true;
    std::string m_lastStatus, m_lastInfo;
    std::string m_message; // a one-off note (e.g. "can't add a click here")
    float m_messageTime = 0.f;

    // ---- setup -------------------------------------------------------------

    bool init() override {
        if (!CCLayer::init()) return false;
        g_ui = this;
        auto win = CCDirector::sharedDirector()->getWinSize();
        m_L = 8.f;
        m_R = win.width - 8.f;
        this->setID("macro-editor"_spr);

        m_model.setUndoLimit(static_cast<size_t>(Mod::get()->getSettingValue<int64_t>("editor-undo-limit")));
        m_cbf = rt::cbf();
        m_model.load(rt::loadEdges(), m_cbf);
        m_applied = m_model.edges();
        for (auto const& w : rt::windows()) m_winByOrig[w.orig] = w;

        m_ov = CCDrawNode::create();
        this->addChild(m_ov, -1);
        layoutPanel();

        // top bar
        m_bar = CCLayerColor::create({ 0, 0, 0, 170 }, win.width, 24.f);
        m_bar->setPosition({ 0.f, win.height - 24.f });
        this->addChild(m_bar, 1);
        m_titleLbl = CCLabelBMFont::create(fmt::format("Macro editor  {}{}", rt::title(), m_cbf ? "  (CBF)" : "").c_str(),
            "bigFont.fnt");
        m_titleLbl->setScale(0.32f);
        m_titleLbl->setAnchorPoint({ 0.f, 0.5f });
        m_titleLbl->setPosition({ 8.f, win.height - 12.f });
        this->addChild(m_titleLbl, 2);

        auto menu = CCMenu::create();
        auto add = [&](char const* text, SEL_MenuHandler sel, char const* tex = "GJ_button_04.png") {
            auto spr = ButtonSprite::create(text, "bigFont.fnt", tex, 0.8f);
            spr->setScale(0.42f);
            auto item = CCMenuItemSpriteExtra::create(spr, this, sel);
            menu->addChild(item);
            return item;
        };
        m_undoBtn = add("Undo", menu_selector(MacroEditorUI::onUndo));
        m_redoBtn = add("Redo", menu_selector(MacroEditorUI::onRedo));
        add("Add", menu_selector(MacroEditorUI::onAdd));
        add("Delete", menu_selector(MacroEditorUI::onDelete));
        m_playBtn = add("Play", menu_selector(MacroEditorUI::onPlay), "GJ_button_02.png");
        add("-", menu_selector(MacroEditorUI::onZoomOut));
        add("+", menu_selector(MacroEditorUI::onZoomIn));
        add("Fit", menu_selector(MacroEditorUI::onFit));
        add("Settings", menu_selector(MacroEditorUI::onSettings));
        add("Save", menu_selector(MacroEditorUI::onSave), "GJ_button_01.png");
        add("Exit", menu_selector(MacroEditorUI::onExitBtn), "GJ_button_06.png");
        menu->alignItemsHorizontallyWithPadding(3.f);
        float w = 0.f;
        for (auto item : CCArrayExt<CCNode*>(menu->getChildren())) w += item->getScaledContentSize().width + 3.f;
        menu->setPosition({ win.width - w / 2.f - 4.f, win.height - 12.f });
        this->addChild(menu, 3);

        m_status = CCLabelBMFont::create(" ", "chatFont.fnt");
        m_status->setScale(0.62f);
        m_status->setAnchorPoint({ 0.f, 1.f });
        m_status->setPosition({ 8.f, win.height - 27.f });
        this->addChild(m_status, 2);
        m_info = CCLabelBMFont::create(" ", "chatFont.fnt");
        m_info->setScale(0.48f);
        m_info->setAnchorPoint({ 0.f, 0.f });
        this->addChild(m_info, 2);

        // first view: the first ~10 seconds (or all of it)
        double span = std::min<double>(rt::endLimit(), 2400.0);
        m_fpp = std::max(span, 240.0) / (m_R - m_L);
        m_v0 = 0.0;

        this->setTouchEnabled(true);
        this->setKeypadEnabled(true);
        this->setKeyboardEnabled(true);
        this->setMouseEnabled(true);
        this->scheduleUpdate();
        return true;
    }

    void layoutPanel() {
        auto win = CCDirector::sharedDirector()->getWinSize();
        float k = static_cast<float>(std::clamp(setting("editor-timeline-height"), 0.6, 1.8));
        m_hasP2 = false;
        for (auto const& e : m_model.edges()) {
            int mask = e.mask ? e.mask : (e.p1 ? 1 : 2);
            if (mask == 2) m_hasP2 = true;
        }
        float ruler = 14.f * k, mode = 7.f * k, p1 = 22.f * k, p2 = m_hasP2 ? 16.f * k : 0.f, ov = 9.f * k, pad = 3.f;
        m_ovY0 = pad;
        m_ovY1 = m_ovY0 + ov;
        m_p2Y0 = m_ovY1 + pad;
        m_p2Y1 = m_p2Y0 + p2;
        m_p1Y0 = m_p2Y1 + (m_hasP2 ? 2.f : 0.f);
        m_p1Y1 = m_p1Y0 + p1;
        m_modeY0 = m_p1Y1 + 2.f;
        m_modeY1 = m_modeY0 + mode;
        m_rulerY0 = m_modeY1 + 1.f;
        m_panelY0 = 0.f;
        m_panelY1 = m_rulerY0 + ruler;
        if (!m_panel) {
            m_panel = CCLayerColor::create({ 8, 8, 12, 205 }, win.width, m_panelY1);
            this->addChild(m_panel, 0);
            m_tl = CCDrawNode::create();
            this->addChild(m_tl, 1);
        }
        m_panel->setContentSize({ win.width, m_panelY1 });
        if (m_info) m_info->setPosition({ 8.f, m_panelY1 + 3.f });
        m_dirtyDraw = true;
    }

    void onExit() override {
        if (g_ui == this) g_ui = nullptr;
        CCLayer::onExit();
    }

    void registerWithTouchDispatcher() override {
        // after menus (-128) so the buttons get their clicks; before the level
        CCTouchDispatcher::get()->addTargetedDelegate(this, -127, true);
    }

    // ---- timeline geometry ---------------------------------------------------

    float xOf(double t) const { return m_L + static_cast<float>((t - m_v0) / m_fpp); }
    double tOf(float x) const { return m_v0 + (x - m_L) * m_fpp; }
    double viewEnd() const { return tOf(m_R); }

    bool rowOfP2(Edge const& e) const {
        int mask = e.mask ? e.mask : (e.p1 ? 1 : 2);
        return mask == 2 && m_hasP2;
    }

    void clampView() {
        double total = rt::endLimit() + 60.0;
        double maxFpp = total / (m_R - m_L) * 1.05;
        m_fpp = std::clamp(m_fpp, 1.0 / 2048.0, std::max(maxFpp, 1.0 / 256.0));
        double span = (m_R - m_L) * m_fpp;
        m_v0 = std::clamp(m_v0, -30.0, std::max(-30.0, total - span));
        m_dirtyDraw = true;
    }

    void zoomAt(float x, double factor) {
        double t = tOf(x);
        m_fpp *= factor;
        clampView();
        m_v0 = t - (x - m_L) * m_fpp;
        clampView();
    }

    void showFrame(double t) {
        double span = (m_R - m_L) * m_fpp;
        if (t < m_v0 + span * 0.05 || t > m_v0 + span * 0.9) {
            m_v0 = t - span * 0.3;
            clampView();
        }
    }

    // ---- level overlay ---------------------------------------------------------

    // position at time t (frames): between the end of step floor(t)-1 and the end of floor(t)
    static std::optional<CCPoint> posOn(std::vector<rt::PathPt> const& p, double t, int knownTo) {
        int s1 = static_cast<int>(std::floor(t));
        int s0 = s1 - 1;
        if (s0 < 0) s0 = 0;
        if (s1 >= static_cast<int>(p.size()) || s1 > knownTo) return std::nullopt;
        auto const& a = p[s0];
        auto const& b = p[s1];
        if (!a.ok || !b.ok) return std::nullopt;
        float f = static_cast<float>(t - std::floor(t));
        return CCPoint{ a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f };
    }

    std::optional<CCPoint> clickPos(Edge const& e) const {
        int k = rowOfP2(e) ? 1 : 0;
        if (auto p = posOn(rt::path(k), e.t, rt::knownTo())) return p;
        return posOn(rt::basePath(k), e.t, static_cast<int>(rt::basePath(k).size()) - 1);
    }

    void drawOverlay() {
        auto n = m_ov;
        n->clear();
        auto pl = PlayLayer::get();
        if (!pl || !pl->m_objectLayer || !settingOn("editor-show-on-level")) return;
        auto ol = pl->m_objectLayer;
        auto W = [ol](float x, float y) { return ol->convertToWorldSpace({ x, y }); };
        auto Wp = [ol](CCPoint p) { return ol->convertToWorldSpace(p); };
        int ph = rt::playhead();
        int from = std::max(0, ph - 480), to = ph + 1680;
        int known = rt::knownTo();
        // the original path, faint
        if (settingOn("editor-show-original")) {
            for (int k = 0; k < 2; k++) {
                auto const& b = rt::basePath(k);
                for (int s = from + 1; s <= to && s < static_cast<int>(b.size()); s++)
                    if (b[s - 1].ok && b[s].ok)
                        n->drawSegment(W(b[s - 1].x, b[s - 1].y), W(b[s].x, b[s].y), 0.7f, col(1, 1, 1, 0.22f));
            }
        }
        // the edited macro's path where it's known
        if (settingOn("editor-show-path")) {
            for (int k = 0; k < 2; k++) {
                auto const& p = rt::path(k);
                for (int s = from + 1; s <= std::min(to, known) && s < static_cast<int>(p.size()); s++)
                    if (p[s - 1].ok && p[s].ok)
                        n->drawSegment(W(p[s - 1].x, p[s - 1].y), W(p[s].x, p[s].y), 1.1f,
                            k ? col(1.f, 0.55f, 0.8f, 0.9f) : col(0.3f, 0.9f, 1.f, 0.9f));
            }
        }
        // frame windows along the path, and the clicks
        bool showWin = settingOn("editor-show-windows");
        for (auto const& e : m_model.edges()) {
            if (e.t < from - 2 || e.t > to + 2) continue;
            auto pos = clickPos(e);
            if (!pos) continue;
            if (showWin && e.orig >= 0) {
                if (auto w = m_winByOrig.find(e.orig); w != m_winByOrig.end()) {
                    bool stale = std::fabs(e.t - e.origT) > 1e-9;
                    int k = rowOfP2(e) ? 1 : 0;
                    auto const& p = known >= static_cast<int>(e.t) ? rt::path(k) : rt::basePath(k);
                    int last = static_cast<int>(p.size()) - 1;
                    std::optional<CCPoint> prev;
                    for (double t = e.t + w->second.lo; t <= e.t + w->second.hi + 1e-9; t += 0.25) {
                        auto q = posOn(p, t, last);
                        if (q && prev) n->drawSegment(Wp(*prev), Wp(*q), 2.6f, windowColor(w->second.w, stale ? 0.2f : 0.55f));
                        prev = q;
                    }
                }
            }
            bool sel = e.id == m_sel;
            ccColor4F c = e.fix ? col(0.6f, 0.6f, 0.6f, 0.9f)
                : e.down ? col(0.35f, 1.f, 0.45f, 0.95f) : col(1.f, 0.6f, 0.25f, 0.95f);
            auto wp = Wp(*pos);
            if (e.down) n->drawDot(wp, sel ? 4.5f : 3.f, c);
            else n->drawCircle(wp, sel ? 4.5f : 3.f, col(0, 0, 0, 0), 1.2f, c, 16);
            if (sel) n->drawCircle(wp, 8.f, col(0, 0, 0, 0), 1.4f, col(1.f, 0.95f, 0.3f, 1.f), 24);
        }
        // where it dies
        int d = rt::deathStep();
        if (d >= 0 && d < static_cast<int>(rt::path(0).size()) && rt::path(0)[d].ok) {
            auto p = W(rt::path(0)[d].x, rt::path(0)[d].y);
            n->drawSegment({ p.x - 7, p.y - 7 }, { p.x + 7, p.y + 7 }, 1.6f, col(1, 0.2f, 0.2f, 1));
            n->drawSegment({ p.x - 7, p.y + 7 }, { p.x + 7, p.y - 7 }, 1.6f, col(1, 0.2f, 0.2f, 1));
        }
    }

    // ---- timeline ------------------------------------------------------------------

    CCLabelBMFont* label(std::vector<CCLabelBMFont*>& pool, size_t i) {
        while (pool.size() <= i) {
            auto l = CCLabelBMFont::create("", "chatFont.fnt");
            l->setScale(0.42f);
            l->setAnchorPoint({ 0.f, 0.5f });
            this->addChild(l, 2);
            pool.push_back(l);
        }
        pool[i]->setVisible(true);
        return pool[i];
    }

    void drawTimeline() {
        auto n = m_tl;
        n->clear();
        double t0 = m_v0, t1 = viewEnd();
        int known = rt::knownTo();

        // ---- ruler: a tick spacing that leaves room for its labels
        static double const steps[] = { 1.0 / 64, 1.0 / 32, 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1, 2, 5, 10, 20, 30,
                                        60, 120, 240, 480, 1200, 2400, 4800, 9600 };
        double major = steps[std::size(steps) - 1];
        for (double s : steps) if (s / m_fpp >= 55.0) { major = s; break; }
        double minor = major / 5.0;
        size_t li = 0;
        for (double t = std::floor(t0 / minor) * minor; t <= t1; t += minor) {
            float x = xOf(t);
            if (x < m_L || x > m_R) continue;
            bool isMajor = std::fabs(t / major - std::round(t / major)) < 1e-6;
            n->drawSegment({ x, m_panelY1 - (isMajor ? 6.f : 3.f) }, { x, m_panelY1 }, 0.4f,
                col(1, 1, 1, isMajor ? 0.6f : 0.3f));
            if (isMajor) {
                auto l = label(m_rulerLabels, li++);
                l->setString(major >= 60 ? fmt::format("{:.2f}s", t / 240.0).c_str() : fmtFrames(t).c_str());
                l->setPosition({ x + 2.f, (m_rulerY0 + m_panelY1) / 2.f - 1.f });
                n->drawSegment({ x, m_ovY1 }, { x, m_modeY1 }, 0.3f, col(1, 1, 1, 0.07f));
            }
        }
        for (size_t i = li; i < m_rulerLabels.size(); i++) m_rulerLabels[i]->setVisible(false);

        // ---- gamemode strip (+ speed changes)
        size_t si = 0;
        if (settingOn("editor-show-modes") && !rt::info().empty()) {
            auto const& info = rt::info();
            int a = std::max(0, static_cast<int>(std::floor(t0))), b = std::min(static_cast<int>(info.size()) - 1, static_cast<int>(std::ceil(t1)));
            int runStart = a;
            float lastSpeed = -1.f;
            for (int s = a; s <= b + 1; s++) {
                bool cut = s > b || !info[s].ok || !info[runStart].ok || info[s].mode != info[runStart].mode
                    || info[s].mini != info[runStart].mini;
                if (s <= b && info[s].ok && lastSpeed >= 0.f && std::fabs(info[s].speed - lastSpeed) > 0.01f) {
                    float x = xOf(s);
                    n->drawSegment({ x, m_modeY0 - 1.f }, { x, m_modeY1 + 1.f }, 0.6f, col(1, 1, 1, 0.9f));
                    auto l = label(m_speedLabels, si++);
                    l->setString(speedName(info[s].speed));
                    l->setScale(0.32f);
                    l->setPosition({ x + 1.5f, (m_modeY0 + m_modeY1) / 2.f });
                }
                if (s <= b && info[s].ok) lastSpeed = info[s].speed;
                if (!cut) continue;
                if (info[runStart].ok && s > runStart)
                    rect(n, std::max(m_L, xOf(runStart)), m_modeY0, std::min(m_R, xOf(s)), m_modeY1,
                        modeColor(info[runStart].mode, info[runStart].mini));
                runStart = s;
            }
        }
        for (size_t i = si; i < m_speedLabels.size(); i++) m_speedLabels[i]->setVisible(false);

        // ---- rows: background, the part not simulated yet, holds, windows, clicks
        auto rowBg = [&](float y0, float y1) { rect(n, m_L, y0, m_R, y1, col(1, 1, 1, 0.04f)); };
        rowBg(m_p1Y0, m_p1Y1);
        if (m_hasP2) rowBg(m_p2Y0, m_p2Y1);
        if (known + 1 < t1) {
            float x = std::max(m_L, xOf(known + 1));
            rect(n, x, m_p2Y0, m_R, m_p1Y1, col(0, 0, 0, 0.35f));
        }
        if (settingOn("editor-show-holds")) {
            for (auto const& h : m_model.holds()) {
                if (h.b < t0 || h.a > t1) continue;
                bool p2 = (h.ch / 8) == 2 && m_hasP2;
                float y0 = p2 ? m_p2Y0 : m_p1Y0, y1 = p2 ? m_p2Y1 : m_p1Y1;
                float hy0 = y0 + (y1 - y0) * 0.3f, hy1 = y1 - (y1 - y0) * 0.3f;
                rect(n, std::max(m_L, xOf(h.a)), hy0, std::min(m_R, xOf(std::isfinite(h.b) ? h.b : t1)), hy1,
                    p2 ? col(1.f, 0.45f, 0.7f, 0.45f) : col(0.3f, 0.7f, 1.f, 0.45f));
            }
        }
        bool showWin = settingOn("editor-show-windows");
        for (auto const& e : m_model.edges()) {
            if (e.t < t0 - 20 || e.t > t1 + 20) continue;
            bool p2 = rowOfP2(e);
            float y0 = p2 ? m_p2Y0 : m_p1Y0, y1 = p2 ? m_p2Y1 : m_p1Y1;
            if (showWin && e.orig >= 0) {
                if (auto w = m_winByOrig.find(e.orig); w != m_winByOrig.end()) {
                    bool stale = std::fabs(e.t - e.origT) > 1e-9;
                    float a = std::max(m_L, xOf(e.t + w->second.lo)), b = std::min(m_R, xOf(e.t + w->second.hi));
                    if (b > a) rect(n, a, y0 + 1.f, b, y1 - 1.f, windowColor(w->second.w, stale ? 0.12f : 0.3f));
                }
            }
        }
        for (auto const& e : m_model.edges()) {
            float x = xOf(e.t);
            if (x < m_L - 2 || x > m_R + 2) continue;
            bool p2 = rowOfP2(e);
            float y0 = p2 ? m_p2Y0 : m_p1Y0, y1 = p2 ? m_p2Y1 : m_p1Y1;
            bool sel = e.id == m_sel;
            bool moved = e.orig < 0 || std::fabs(e.t - e.origT) > 1e-9;
            ccColor4F c = e.fix ? col(0.6f, 0.6f, 0.6f, 0.9f)
                : e.down ? col(0.4f, 1.f, 0.5f, 1.f) : col(1.f, 0.65f, 0.3f, 1.f);
            if (sel) c = col(1.f, 0.95f, 0.3f, 1.f);
            if (e.down) {
                n->drawSegment({ x, y0 + 1.f }, { x, y1 - 1.f }, sel ? 1.2f : 0.7f, c);
                CCPoint tri[3] = { { x - 3.f, y1 }, { x + 3.f, y1 }, { x, y1 - 4.f } };
                n->drawPolygon(tri, 3, c, 0.f, col(0, 0, 0, 0));
            } else {
                float ym = (y0 + y1) / 2.f;
                n->drawSegment({ x, y0 + 1.f }, { x, ym }, sel ? 1.2f : 0.7f, c);
                CCPoint tri[3] = { { x - 3.f, y0 }, { x + 3.f, y0 }, { x, y0 + 4.f } };
                n->drawPolygon(tri, 3, c, 0.f, col(0, 0, 0, 0));
            }
            if (moved && !sel) n->drawDot({ x, y1 + 1.5f }, 1.2f, col(1.f, 0.95f, 0.3f, 0.9f));
            if (sel && std::fabs(e.t - e.origT) > 1e-9 && e.orig >= 0) {
                float xo = xOf(e.origT);
                if (xo >= m_L && xo <= m_R)
                    n->drawSegment({ xo, y0 + 2.f }, { xo, y1 - 2.f }, 0.5f, col(1.f, 1.f, 1.f, 0.35f));
            }
        }

        // ---- death / end / playhead
        int d = rt::deathStep();
        if (d >= 0 && d >= t0 && d <= t1) {
            float x = xOf(d);
            n->drawSegment({ x, m_p2Y0 }, { x, m_modeY1 }, 1.2f, col(1.f, 0.25f, 0.25f, 1.f));
        }
        int en = rt::endStep();
        if (en >= 0 && en >= t0 && en <= t1) {
            float x = xOf(en);
            n->drawSegment({ x, m_p2Y0 }, { x, m_modeY1 }, 1.2f, col(0.35f, 1.f, 0.45f, 1.f));
        }
        float px = xOf(rt::playhead());
        if (px >= m_L && px <= m_R) {
            n->drawSegment({ px, m_p2Y0 }, { px, m_panelY1 }, 0.6f, col(1, 1, 1, 0.95f));
            CCPoint tri[3] = { { px - 4.f, m_panelY1 }, { px + 4.f, m_panelY1 }, { px, m_panelY1 - 5.f } };
            n->drawPolygon(tri, 3, col(1, 1, 1, 1), 0.f, col(0, 0, 0, 0));
        }

        // ---- overview: the whole macro, the visible part highlighted
        double total = rt::endLimit();
        auto ox = [&](double t) { return m_L + static_cast<float>(t / total) * (m_R - m_L); };
        rect(n, m_L, m_ovY0, m_R, m_ovY1, col(1, 1, 1, 0.06f));
        if (known >= 0) rect(n, m_L, m_ovY0, ox(std::min<double>(known, total)), m_ovY0 + 1.5f, col(0.3f, 0.9f, 1.f, 0.6f));
        rect(n, std::max(m_L, ox(t0)), m_ovY0, std::min(m_R, ox(t1)), m_ovY1, col(1, 1, 1, 0.18f));
        for (auto const& e : m_model.edges())
            if (e.down) n->drawSegment({ ox(e.t), m_ovY0 + 2.f }, { ox(e.t), m_ovY1 - 1.f }, 0.3f, col(0.4f, 1.f, 0.5f, 0.5f));
        if (d >= 0) n->drawSegment({ ox(d), m_ovY0 }, { ox(d), m_ovY1 }, 0.8f, col(1, 0.25f, 0.25f, 1));
        n->drawSegment({ ox(rt::playhead()), m_ovY0 }, { ox(rt::playhead()), m_ovY1 }, 0.6f, col(1, 1, 1, 1));
    }

    // ---- status / info text --------------------------------------------------------

    void updateText(float dt) {
        std::string st;
        auto busy = rt::busy();
        int d = rt::deathStep(), en = rt::endStep();
        if (busy == rt::Busy::Building) st = fmt::format("Loading the macro... {:.0f}%", rt::progress() * 100.f);
        else if (busy == rt::Busy::Resim) st = "Re-simulating the change...";
        else if (d >= 0) st = fmt::format("Dies at frame {} ({:.2f} s)", d, d / 240.0);
        else if (en >= 0) st = fmt::format("Reaches the end (frame {})", en);
        else st = fmt::format("Simulated to frame {}", std::max(0, rt::knownTo()));
        st += fmt::format("   |   frame {}{}", rt::playhead(), m_model.dirty() ? "   |   unsaved" : "");
        if (m_messageTime > 0.f) {
            m_messageTime -= dt;
            st += "   |   " + m_message;
        }
        if (st != m_lastStatus) {
            m_lastStatus = st;
            m_status->setString(st.c_str());
            if (d >= 0 && busy != rt::Busy::Building) m_status->setColor({ 255, 110, 110 });
            else if (en >= 0) m_status->setColor({ 120, 255, 140 });
            else m_status->setColor({ 255, 255, 255 });
        }

        std::string info;
        int i = m_model.indexOf(m_sel);
        if (i >= 0) {
            auto const& e = m_model.edges()[i];
            bool p2 = rowOfP2(e);
            info = fmt::format("{} {} {}  ·  frame {}", e.fix ? "Respawn fix" : (e.down ? "Press" : "Release"),
                p2 ? "P2" : "P1", e.button == 1 ? "" : (e.button == 2 ? "(left)" : "(right)"), fmtFrames(e.t));
            if (e.orig < 0) info += "  ·  added";
            else if (std::fabs(e.t - e.origT) > 1e-9) info += fmt::format("  ·  moved {:+.4f} f", e.t - e.origT);
            if (e.orig >= 0)
                if (auto w = m_winByOrig.find(e.orig); w != m_winByOrig.end())
                    info += fmt::format("  ·  window {:.2f} f ({:+.2f} .. {:+.2f}){}", w->second.w, w->second.lo,
                        w->second.hi, std::fabs(e.t - e.origT) > 1e-9 ? " - measured before the move" : "");
        } else {
            info = "Drag a click to move it (Shift: finer) · click empty: playhead · drag empty: pan · wheel: zoom · "
                   "Space play · A add · Del delete · Ctrl+Z/Y";
        }
        if (info != m_lastInfo) {
            m_lastInfo = info;
            m_info->setString(info.c_str());
        }
        if (auto spr = typeinfo_cast<ButtonSprite*>(m_playBtn->getNormalImage()))
            spr->setString(rt::playing() ? "Pause" : "Play");
    }

    void note(std::string msg) {
        m_message = std::move(msg);
        m_messageTime = 3.f;
    }

    void update(float dt) override {
        if (!rt::active()) { if (!m_closing) { m_closing = true; this->removeFromParent(); } return; }
        if (rt::playing()) showFrame(rt::playhead());
        uint64_t rev = rt::revision();
        if (rev != m_lastRev || m_dirtyDraw) {
            m_lastRev = rev;
            m_dirtyDraw = false;
            drawTimeline();
            drawOverlay();
        }
        updateText(dt);
        m_undoBtn->setOpacity(m_model.canUndo() ? 255 : 90);
        m_redoBtn->setOpacity(m_model.canRedo() ? 255 : 90);
    }

    // ---- edits -----------------------------------------------------------------------

    // tell the runtime about the model's current macro (re-sims the changed part)
    void commit(double viewAt = -1.0) {
        double first = Model::firstDifference(m_model.edges(), m_applied);
        m_applied = m_model.edges();
        m_dirtyDraw = true;
        if (!std::isfinite(first)) return;
        rt::apply(m_applied, first);
        if (viewAt >= 0.0) {
            int lead = static_cast<int>(Mod::get()->getSettingValue<int64_t>("editor-preview-lead"));
            rt::seek(std::max(0, static_cast<int>(std::floor(viewAt)) - lead));
        }
    }

    double snapStep(bool fine) const {
        if (!m_cbf) return 1.0;
        double s = cbfSnap();
        return fine ? s / 8.0 : s;
    }

    void nudge(int dir, bool fine, bool coarse) {
        int i = m_model.indexOf(m_sel);
        if (i < 0) {
            rt::seek(rt::playhead() + dir * (coarse ? 10 : 1));
            return;
        }
        double t = m_model.edges()[i].t;
        double step = coarse ? (m_cbf ? 1.0 : 10.0) : snapStep(fine);
        m_model.move(m_sel, t + dir * step, fine ? snapStep(true) : snapStep(false));
        commit(m_model.edges()[m_model.indexOf(m_sel)].t);
    }

    void selectNext(int dir) {
        auto const& e = m_model.edges();
        if (e.empty()) return;
        int i = m_model.indexOf(m_sel);
        if (i < 0) {
            // the first click after (or before) the playhead
            int ph = rt::playhead();
            i = dir > 0 ? 0 : static_cast<int>(e.size()) - 1;
            for (int k = 0; k < static_cast<int>(e.size()); k++)
                if (dir > 0 ? e[k].t >= ph : e[k].t <= ph) { i = k; if (dir > 0) break; }
        } else {
            i = std::clamp(i + dir, 0, static_cast<int>(e.size()) - 1);
        }
        m_sel = e[i].id;
        showFrame(e[i].t);
        int lead = static_cast<int>(Mod::get()->getSettingValue<int64_t>("editor-preview-lead"));
        rt::seek(std::max(0, static_cast<int>(std::floor(e[i].t)) - lead));
        m_dirtyDraw = true;
    }

    void onUndo(CCObject*) { if (m_model.undo()) { commit(); note("Undone"); } }
    void onRedo(CCObject*) { if (m_model.redo()) { commit(); note("Redone"); } }

    void onAdd(CCObject*) {
        // same player / button as the selected click, else the closest one before the playhead
        int button = 1, mask = 1;
        bool p1 = true;
        double t = rt::playhead() + 1;
        Edge const* like = nullptr;
        if (int i = m_model.indexOf(m_sel); i >= 0) like = &m_model.edges()[i];
        else for (auto const& e : m_model.edges()) if (e.t <= t && !e.fix) like = &e;
        if (like) { button = like->button; p1 = like->p1; mask = like->mask; }
        std::string why;
        double hold = setting("editor-default-hold");
        int id = m_model.addClick(t, hold, button, p1, mask, snapStep(false), &why);
        if (!id) { note(why); return; }
        m_sel = id;
        commit(t);
        note("Click added");
    }

    void onDelete(CCObject*) {
        if (!m_model.removeClick(m_sel)) { note("Select a click first"); return; }
        m_sel = 0;
        commit();
        note("Click deleted");
    }

    void onPlay(CCObject*) { rt::play(!rt::playing()); }
    void onZoomIn(CCObject*) { zoomAt(xOf(rt::playhead()), 1.0 / 1.6); }
    void onZoomOut(CCObject*) { zoomAt(xOf(rt::playhead()), 1.6); }
    void onFit(CCObject*) {
        m_v0 = -10.0;
        m_fpp = (rt::endLimit() + 20.0) / (m_R - m_L);
        clampView();
    }
    void onSettings(CCObject*) { openSettingsPopup(Mod::get(), false); }

    void onSave(CCObject*) {
        int lost = rt::save(m_model.edges());
        m_model.rebase();
        m_applied = m_model.edges();
        m_winByOrig.clear();
        for (auto const& w : rt::windows()) m_winByOrig[w.orig] = w;
        m_dirtyDraw = true;
        note(lost > 0 ? fmt::format("Saved. {} moved clicks need a new analysis", lost) : std::string("Saved"));
    }

    void onExitBtn(CCObject*) { requestExit(); }

    void close(bool save) {
        if (m_closing) return;
        m_closing = true;
        if (auto pl = PlayLayer::get(); pl && pl->m_uiLayer) pl->m_uiLayer->setVisible(true);
        rt::finish(save, m_model.edges());
        this->removeFromParent();
    }

    // ---- input ---------------------------------------------------------------------------

    bool inPanel(CCPoint p) const { return p.y >= m_panelY0 && p.y <= m_panelY1 && p.x >= 0.f; }

    int hitClick(CCPoint p) const {
        int best = 0;
        float bestD = 7.f;
        for (auto const& e : m_model.edges()) {
            bool p2 = rowOfP2(e);
            float y0 = p2 ? m_p2Y0 : m_p1Y0, y1 = p2 ? m_p2Y1 : m_p1Y1;
            if (p.y < y0 - 3.f || p.y > y1 + 3.f) continue;
            float d = std::fabs(xOf(e.t) - p.x);
            if (d < bestD || (d <= bestD && e.id == m_sel)) { bestD = d; best = e.id; }
        }
        return best;
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        if (m_closing) return false;
        auto p = touch->getLocation();
        if (!inPanel(p)) return false;
        m_downX = p.x;
        m_downV0 = m_v0;
        m_moved = false;
        if (p.y >= m_rulerY0) {
            m_drag = Drag::Scrub;
            rt::seek(static_cast<int>(std::lround(tOf(p.x))));
        } else if (p.y >= m_ovY0 && p.y <= m_ovY1) {
            m_drag = Drag::Overview;
            jumpOverview(p.x);
        } else if (int id = hitClick(p)) {
            if (rt::busy() == rt::Busy::Building) return true;
            m_drag = Drag::Marker;
            m_sel = id;
            auto const& e = m_model.edges()[m_model.indexOf(id)];
            m_grab = tOf(p.x) - e.t;
            m_dragFrom = e.t;
            m_beforeDrag = m_model.edges();
            m_model.beginDrag();
            m_dirtyDraw = true;
        } else {
            m_drag = Drag::Pan;
        }
        return true;
    }

    void jumpOverview(float x) {
        double total = rt::endLimit();
        double t = (x - m_L) / (m_R - m_L) * total;
        double span = (m_R - m_L) * m_fpp;
        m_v0 = t - span / 2.0;
        clampView();
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        auto p = touch->getLocation();
        if (std::fabs(p.x - m_downX) > 2.f) m_moved = true;
        switch (m_drag) {
            case Drag::Scrub: rt::seek(static_cast<int>(std::lround(tOf(p.x)))); m_dirtyDraw = true; break;
            case Drag::Overview: jumpOverview(p.x); break;
            case Drag::Pan:
                m_v0 = m_downV0 - (p.x - m_downX) * m_fpp;
                clampView();
                break;
            case Drag::Marker:
                if (m_moved) {
                    bool fine = CCKeyboardDispatcher::get()->getShiftKeyPressed();
                    m_model.dragTo(m_sel, tOf(p.x) - m_grab, snapStep(fine));
                    m_dirtyDraw = true;
                }
                break;
            default: break;
        }
    }

    void ccTouchEnded(CCTouch* touch, CCEvent*) override {
        auto p = touch->getLocation();
        auto d = m_drag;
        m_drag = Drag::None;
        switch (d) {
            case Drag::Marker: {
                m_model.endDrag();
                int i = m_model.indexOf(m_sel);
                double t = i >= 0 ? m_model.edges()[i].t : m_dragFrom;
                if (std::fabs(t - m_dragFrom) > 1e-9) {
                    commit(std::min(t, m_dragFrom));
                    note(fmt::format("Moved {:+.4f} f", t - m_dragFrom));
                } else {
                    // a click on a marker: select it and look at it
                    int lead = static_cast<int>(Mod::get()->getSettingValue<int64_t>("editor-preview-lead"));
                    rt::seek(std::max(0, static_cast<int>(std::floor(t)) - lead));
                }
                break;
            }
            case Drag::Pan:
                if (!m_moved) { // a click on empty: deselect, playhead there
                    m_sel = 0;
                    rt::seek(static_cast<int>(std::lround(tOf(p.x))));
                }
                break;
            default: break;
        }
        m_dirtyDraw = true;
    }

    void ccTouchCancelled(CCTouch* touch, CCEvent* ev) override { ccTouchEnded(touch, ev); }

    void scrollWheel(float y, float x) override {
        auto mp = getMousePos();
        if (!inPanel(mp)) return;
        auto kb = CCKeyboardDispatcher::get();
        // GD's wheel deltas differ between mice / platforms: only the direction counts
        float dir = (y + x) > 0.f ? 1.f : ((y + x) < 0.f ? -1.f : 0.f);
        if (dir == 0.f) return;
        if (kb->getShiftKeyPressed()) {
            m_v0 += dir * 0.12 * (m_R - m_L) * m_fpp;
            clampView();
        } else {
            zoomAt(mp.x, dir > 0.f ? 1.18 : 1.0 / 1.18);
        }
    }

    void keyBackClicked() override { requestExit(); }

    void keyDown(enumKeyCodes key, double) override {
        auto kb = CCKeyboardDispatcher::get();
        bool ctrl = kb->getControlKeyPressed() || kb->getCommandKeyPressed();
        bool shift = kb->getShiftKeyPressed();
        switch (key) {
            case KEY_Space: onPlay(nullptr); break;
            case KEY_Delete: case KEY_Backspace: onDelete(nullptr); break;
            case KEY_Z: if (ctrl) { if (shift) onRedo(nullptr); else onUndo(nullptr); } break;
            case KEY_Y: if (ctrl) onRedo(nullptr); break;
            case KEY_S: if (ctrl) onSave(nullptr); break;
            case KEY_A: if (!ctrl) onAdd(nullptr); break;
            case KEY_F: onFit(nullptr); break;
            case KEY_Left: nudge(-1, shift, ctrl); break;
            case KEY_Right: nudge(1, shift, ctrl); break;
            case KEY_Tab: selectNext(shift ? -1 : 1); break;
            case KEY_Home: rt::seek(0); showFrame(0); break;
            case KEY_End: rt::seek(rt::endLimit()); showFrame(rt::endLimit()); break;
            case KEY_OEMPlus: case KEY_Add: onZoomIn(nullptr); break;
            case KEY_OEMMinus: case KEY_Subtract: onZoomOut(nullptr); break;
            default: break;
        }
    }
};

void openEditor() {
    std::string why;
    if (!rt::canStart(&why)) { Notification::create(why, NotificationIcon::Error)->show(); return; }
    auto pl = PlayLayer::get();
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    if (!pl || !scene) return;
    rt::start();
    if (!rt::active()) return;
    if (pl->m_uiLayer) pl->m_uiLayer->setVisible(false);
    if (auto ui = MacroEditorUI::create()) scene->addChild(ui, 1000);
}

} // namespace

// Pause menu: "Edit macro" (under the Macros / Files / NaNDL / Import row).
class $modify(MacroEditPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!Mod::get()->getSettingValue<bool>("macro-enabled")) return;
        auto menu = CCMenu::create();
        auto spr = ButtonSprite::create("Edit macro", "bigFont.fnt", "GJ_button_02.png", 0.8f);
        spr->setScale(0.5f);
        menu->addChild(CCMenuItemSpriteExtra::create(spr, this, menu_selector(MacroEditPauseLayer::onEditMacro)));
        auto win = CCDirector::sharedDirector()->getWinSize();
        menu->setPosition({ 70.f, win.height - 46.f });
        menu->setZOrder(100);
        menu->setID("macro-editor-menu"_spr);
        this->addChild(menu);
    }

    void onEditMacro(CCObject*) {
        std::string why;
        if (!fw::edrt::canStart(&why)) { Notification::create(why, NotificationIcon::Error)->show(); return; }
        this->onResume(nullptr);
        // after the pause screen is gone
        Loader::get()->queueInMainThread([] { openEditor(); });
    }
};

// While editing, GD's own pause (Esc, losing focus) is the editor's exit.
class $modify(MacroEditPlayLayer, PlayLayer) {
    void pauseGame(bool unfocused) {
        if (fw::edrt::active()) {
            if (!unfocused && g_ui) g_ui->requestExit();
            return;
        }
        PlayLayer::pauseGame(unfocused);
    }
};
