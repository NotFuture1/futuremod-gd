#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/loader/SettingV3.hpp>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Skip the level-ending sequence (suck-into-the-wall animation + the dead-air
// delay) and jump straight to the "Level Complete" panel, on a keypress.
//
// The end sequence is kicked off by playEndAnimationToPos (classic) /
// playPlatformerEndAnimationToPos (platformer). Those mark the "end window".
// While in that window, the bound key calls showCompleteText() to show the
// panel immediately. showCompleteText() is guarded so the panel is only ever
// shown once -- this both prevents a double panel and fixes pressing the key
// after the panel is already up (which previously re-opened it).
// ---------------------------------------------------------------------------
class $modify(SkipEndPlayLayer, PlayLayer) {
    struct Fields {
        bool m_endActive = false; // has the ending started? (safe-with-jump gate)
        bool m_shown = false;     // has the complete panel been shown yet?
    };

    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->m_endActive = false;
        m_fields->m_shown = false;
    }

    void playEndAnimationToPos(cocos2d::CCPoint position) {
        m_fields->m_endActive = true;
        PlayLayer::playEndAnimationToPos(position);
    }

    void playPlatformerEndAnimationToPos(cocos2d::CCPoint position, bool instant) {
        m_fields->m_endActive = true;
        PlayLayer::playPlatformerEndAnimationToPos(position, instant);
    }

    void showCompleteText() {
        if (m_fields->m_shown) return; // only ever show the panel once
        m_fields->m_shown = true;
        PlayLayer::showCompleteText();
    }

    // Returns true if it actually skipped (so the key press is consumed).
    bool requestSkip() {
        if (!m_fields->m_endActive || m_fields->m_shown) return false;
        this->showCompleteText();
        return true;
    }
};

// ---------------------------------------------------------------------------
// Rebindable keybind, handled by Geode 5.x natively (no extra dependency).
// ---------------------------------------------------------------------------
$execute {
    listenForKeybindSettingPresses(
        "skip-end",
        [](Keybind const&, bool down, bool repeat, double) -> bool {
            if (down && !repeat) {
                if (auto pl = PlayLayer::get()) {
                    if (static_cast<SkipEndPlayLayer*>(pl)->requestSkip()) {
                        return true; // consume only when we actually skip
                    }
                }
            }
            return false; // otherwise let the key do its normal thing (jump)
        }
    );
}

$on_mod(Loaded) {
    log::info("Future Mod loaded - bind a key in settings to skip level endings.");
}
