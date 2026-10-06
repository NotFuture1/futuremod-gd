#include "display.hpp"
#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>

using namespace geode::prelude;

namespace {

constexpr int kRingLayerTag = 0x4D505247; // 'MPRG'
constexpr int kCounterTag   = 0x4D504643; // 'MPFC'
constexpr size_t kMaxRings  = 160;        // dense spam sections: drop the oldest
constexpr float kRingRadius = 13.f;
constexpr float kRowScale   = 0.5f;
constexpr float kRowPeak    = 0.6f;
constexpr float kRowStep    = 18.f;

// Counter rows are "within N frames": a window of 2.375 counts as 3f, and
// anything at or under one frame (including sub-frame CBF windows) as 1f.
int bucketOf(double w) {
    return std::max(1, static_cast<int>(std::ceil(w - 1e-9)));
}

struct Counts {
    std::map<int, int> total; // bucket -> measured inputs in the macro
    std::map<int, int> hit;   // bucket -> passed this attempt
};
Counts g_counts;

bool ringsOn() { return Mod::get()->getSettingValue<bool>("fw-rings"); }
bool soundOn() { return Mod::get()->getSettingValue<bool>("fw-sound"); }

// Tight = red, loose = green, on a square-root curve so 1/2/3/4 frames are
// clearly different colours (that's where the interesting windows live).
// Sub-frame windows lean pink so CBF timings read as their own thing.
ccColor3B windowColor(double w) {
    float hue;
    if (w < 1.0 - 1e-9) hue = 360.f - 40.f * static_cast<float>(1.0 - std::max(0.0, w));
    else hue = 120.f * std::sqrt(std::clamp(static_cast<float>(w - 1.0) / 9.f, 0.f, 1.f));
    float s = 0.8f, v = 1.f;
    float c = v * s;
    float hp = std::fmod(hue, 360.f) / 60.f;
    float x = c * (1.f - std::fabs(std::fmod(hp, 2.f) - 1.f));
    float r = 0, g = 0, b = 0;
    if (hp < 1)      { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else             { r = c; b = x; }
    float m = v - c;
    auto to8 = [](float f) { return static_cast<GLubyte>(std::clamp(f, 0.f, 1.f) * 255.f + 0.5f); };
    return { to8(r + m), to8(g + m), to8(b + m) };
}

// 1, 2, 1/2, 2 3/8 -- sub-frame bisection gives dyadic windows, which read
// better as the fractions NaN writes than as decimals.
std::string fmtWindow(double w) {
    double r = std::round(w);
    if (std::fabs(w - r) < 1e-6) return std::to_string(static_cast<int>(r));
    for (int den : { 2, 4, 8, 16, 32, 64 }) {
        double n = w * den;
        if (std::fabs(n - std::round(n)) < 1e-6) {
            int ni = static_cast<int>(std::round(n));
            int whole = ni / den, rem = ni % den;
            return whole ? fmt::format("{} {}/{}", whole, rem, den) : fmt::format("{}/{}", rem, den);
        }
    }
    return fmt::format("{:.2f}", w);
}

// ---- sound -----------------------------------------------------------------

// The setting is a GD sound name, a file in the mod's config folder, or an
// absolute path. Anything that can't be found falls back to a built-in click.
std::string resolveSound() {
    static std::string lastSetting, lastResolved;
    auto s = Mod::get()->getSettingValue<std::string>("fw-sound-file");
    if (s == lastSetting && !lastResolved.empty()) return lastResolved;
    lastSetting = s;

    std::string out;
    std::error_code ec;
    if (!s.empty()) {
        std::filesystem::path p(s);
        if (p.is_absolute()) {
            if (std::filesystem::exists(p, ec)) out = p.string();
        } else if (auto c = Mod::get()->getConfigDir() / p; std::filesystem::exists(c, ec)) {
            out = c.string();
        } else {
            auto fu = CCFileUtils::sharedFileUtils();
            auto full = fu->fullPathForFilename(s.c_str(), false);
            if (fu->isFileExist(full)) out = s;
        }
    }
    if (out.empty()) {
        out = "playSound_01.ogg";
        FW_WARN("frame window sound '{}' not found, using {}", s, out);
    }
    lastResolved = out;
    return out;
}

// Pitch follows the window: 1 frame is the lowest note, each extra frame is a
// step up (1.5 semitones), so you can hear how tight a section is.
void playWindowSound(double w) {
    if (!soundOn()) return;
    auto fae = FMODAudioEngine::sharedEngine();
    if (!fae) return;
    double f = std::clamp(w, 0.25, 12.0);
    float pitch = static_cast<float>(0.8 * std::pow(2.0, (f - 1.0) * 1.5 / 12.0));
    float vol = static_cast<float>(Mod::get()->getSettingValue<double>("fw-sound-volume"));
    if (vol <= 0.f) return;
    fae->playEffect(resolveSound(), pitch, 1.f, vol);
}

// ---- rings -----------------------------------------------------------------

// A ring remembers where it was spawned in the OBJECT layer, so it stays on
// that spot in the level while the camera moves, zooms, rotates or mirrors.
class FwRing : public CCNode {
public:
    CCPoint levelPos;

    static FwRing* create(CCPoint levelPos, double window) {
        auto r = new FwRing();
        if (r && r->init(levelPos, window)) { r->autorelease(); return r; }
        delete r;
        return nullptr;
    }

private:
    bool init(CCPoint pos, double window) {
        if (!CCNode::init()) return false;
        levelPos = pos;
        auto col = windowColor(window);

        // inner node carries the pop-in, so camera-zoom scaling on `this` and
        // the animation never fight over setScale
        auto body = CCNode::create();
        this->addChild(body);

        auto circle = CCDrawNode::create();
        CCPoint verts[64];
        for (int i = 0; i < 64; i++) {
            float a = i * 6.2831853f / 64.f;
            verts[i] = CCPoint(kRingRadius * std::cos(a), kRingRadius * std::sin(a));
        }
        ccColor4F clear = { 0.f, 0.f, 0.f, 0.f };
        circle->drawPolygon(verts, 64, clear, 4.f, { 0.f, 0.f, 0.f, 1.f });
        circle->drawPolygon(verts, 64, clear, 2.f, { col.r / 255.f, col.g / 255.f, col.b / 255.f, 1.f });
        body->addChild(circle, 0);

        auto label = CCLabelBMFont::create(fmtWindow(window).c_str(), "bigFont.fnt");
        label->setAnchorPoint({ 1.f, 0.5f });
        label->setPosition({ -18.f, 0.f });
        label->setScale(0.5f);
        label->setColor(col);
        body->addChild(label, 1);

        body->setScale(0.6f);
        body->runAction(CCEaseOut::create(CCScaleTo::create(0.12f, 1.f), 3.f));
        return true;
    }
};

CCNode* ringLayer(PlayLayer* pl, bool create) {
    auto n = pl->getChildByTag(kRingLayerTag);
    if (!n && create) {
        n = CCNode::create();
        n->setTag(kRingLayerTag);
        n->setID("frame-window-rings"_spr);
        pl->addChild(n, 9998);
    }
    return n;
}

// object-layer point -> ring-layer point, and the camera's zoom
void place(PlayLayer* pl, CCNode* layer, FwRing* r) {
    auto ol = pl->m_objectLayer;
    if (!ol) return;
    r->setPosition(layer->convertToNodeSpace(ol->convertToWorldSpace(r->levelPos)));
    r->setScale(std::fabs(ol->getScaleY()));
}

// ---- counter (top-left) ----------------------------------------------------

constexpr int kRowCountBase = 1000; // child tags: count label = base + bucket
constexpr int kRowTotalBase = 2000; //             "/total"   = base + bucket

void layoutRow(CCNode* hud, int b) {
    auto cnt = static_cast<CCLabelBMFont*>(hud->getChildByTag(kRowCountBase + b));
    auto tot = static_cast<CCLabelBMFont*>(hud->getChildByTag(kRowTotalBase + b));
    if (!cnt || !tot) return;
    // measured at rest scale so the pulse doesn't make "/total" jitter
    float w = cnt->getContentSize().width * kRowScale;
    tot->setPositionX(cnt->getPositionX() + w + 2.f);
}

void buildCounter(PlayLayer* pl) {
    if (auto old = pl->getChildByTag(kCounterTag)) old->removeFromParent();
    if (g_counts.total.empty()) return;

    auto hud = CCNode::create();
    hud->setTag(kCounterTag);
    hud->setID("frame-window-counter"_spr);
    auto win = CCDirector::sharedDirector()->getWinSize();
    hud->setPosition(6.f, win.height - 6.f);
    pl->addChild(hud, 10000);

    // the name column is as wide as its widest entry so counts line up
    std::vector<std::pair<int, CCLabelBMFont*>> names;
    float nameW = 0.f;
    for (auto const& [b, _] : g_counts.total) {
        auto l = CCLabelBMFont::create(fmt::format("{}f:", b).c_str(), "bigFont.fnt");
        l->setAnchorPoint({ 0.f, 1.f });
        l->setScale(kRowScale);
        l->setColor(windowColor(b));
        nameW = std::max(nameW, l->getScaledContentSize().width);
        names.push_back({ b, l });
    }
    float y = 0.f;
    for (auto const& [b, name] : names) {
        name->setPosition({ 0.f, y });
        hud->addChild(name);

        auto cnt = CCLabelBMFont::create(std::to_string(g_counts.hit[b]).c_str(), "bigFont.fnt");
        cnt->setTag(kRowCountBase + b);
        // centre-left anchor so the pulse grows in place instead of downward
        cnt->setAnchorPoint({ 0.f, 0.5f });
        cnt->setScale(kRowScale);
        cnt->setColor(windowColor(b));
        float rowH = name->getScaledContentSize().height;
        cnt->setPosition({ nameW + 4.f, y - rowH / 2.f });
        hud->addChild(cnt);

        auto tot = CCLabelBMFont::create(fmt::format("/{}", g_counts.total[b]).c_str(), "bigFont.fnt");
        tot->setTag(kRowTotalBase + b);
        tot->setAnchorPoint({ 0.f, 0.5f });
        tot->setScale(0.35f);
        tot->setColor({ 160, 160, 160 });
        tot->setOpacity(200);
        tot->setPosition({ 0.f, y - rowH / 2.f });
        hud->addChild(tot);
        layoutRow(hud, b);

        y -= kRowStep;
    }
}

void bumpCounter(PlayLayer* pl, int b) {
    auto hud = pl->getChildByTag(kCounterTag);
    if (!hud) return;
    auto cnt = static_cast<CCLabelBMFont*>(hud->getChildByTag(kRowCountBase + b));
    if (!cnt) return;
    cnt->setString(std::to_string(g_counts.hit[b]).c_str());
    layoutRow(hud, b);

    // quick flash-and-settle: up to white and a bit bigger, then back
    constexpr int kScaleAct = 1001, kTintAct = 1002;
    cnt->stopActionByTag(kScaleAct);
    cnt->stopActionByTag(kTintAct);
    auto col = windowColor(b);
    auto sc = CCSequence::create(
        CCEaseOut::create(CCScaleTo::create(0.06f, kRowPeak), 2.f),
        CCEaseOut::create(CCScaleTo::create(0.20f, kRowScale), 2.f), nullptr);
    sc->setTag(kScaleAct);
    cnt->runAction(sc);
    auto tn = CCSequence::create(
        CCTintTo::create(0.06f, 255, 255, 255),
        CCTintTo::create(0.20f, col.r, col.g, col.b), nullptr);
    tn->setTag(kTintAct);
    cnt->runAction(tn);
}

void clearRings(PlayLayer* pl) {
    if (auto l = ringLayer(pl, false)) l->removeAllChildrenWithCleanup(true);
}

} // namespace

namespace fw::display {

void start(PlayLayer* pl, std::vector<double> const& windows) {
    g_counts = Counts{};
    for (double w : windows) g_counts.total[bucketOf(w)]++;
    for (auto const& [b, _] : g_counts.total) g_counts.hit[b] = 0;
    if (!pl) return;
    clearRings(pl);
    if (ringsOn()) buildCounter(pl);
    else if (auto old = pl->getChildByTag(kCounterTag)) old->removeFromParent();
}

void restart(PlayLayer* pl) {
    for (auto& [b, n] : g_counts.hit) n = 0;
    if (!pl) return;
    clearRings(pl);
    if (ringsOn()) buildCounter(pl);
}

void stop(PlayLayer* pl) {
    g_counts = Counts{};
    if (!pl) return;
    clearRings(pl);
    if (auto old = pl->getChildByTag(kCounterTag)) old->removeFromParent();
}

void onInput(PlayLayer* pl, PlayerObject* player, double window) {
    if (!pl || !player) return;
    int b = bucketOf(window);
    g_counts.hit[b]++;
    playWindowSound(window);
    if (!ringsOn()) return;
    bumpCounter(pl, b);

    auto ol = pl->m_objectLayer;
    auto parent = player->getParent();
    if (!ol || !parent) return;
    // in object-layer space, whatever the player's actual parent is
    CCPoint lp = ol->convertToNodeSpace(parent->convertToWorldSpace(player->getPosition()));
    auto ring = FwRing::create(lp, window);
    if (!ring) return;
    auto layer = ringLayer(pl, true);
    layer->addChild(ring);
    place(pl, layer, ring);

    if (auto kids = layer->getChildren(); kids && kids->count() > kMaxRings)
        static_cast<CCNode*>(kids->objectAtIndex(0))->removeFromParent();
}

void tick(PlayLayer* pl) {
    if (!pl) return;
    auto layer = ringLayer(pl, false);
    if (!layer || !layer->getChildrenCount()) return;
    auto win = CCDirector::sharedDirector()->getWinSize();
    constexpr float margin = 300.f;
    std::vector<CCNode*> gone;
    for (auto r : CCArrayExt<FwRing*>(layer->getChildren())) {
        place(pl, layer, r);
        auto p = layer->convertToWorldSpace(r->getPosition());
        if (p.x < -margin || p.x > win.width + margin || p.y < -margin || p.y > win.height + margin)
            gone.push_back(r);
    }
    for (auto n : gone) n->removeFromParent();
}

} // namespace fw::display
