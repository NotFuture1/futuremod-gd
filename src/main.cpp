#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/loader/SettingV3.hpp>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// When a level is finishing (the suck-into-the-wall animation), the bound key
// instantly EXITS the level back to where you came from -- skipping the
// animation, the dead-air delay, and the "Level Complete" panel entirely.
//
// The level is still registered as completed (levelComplete() runs to save
// best %, stars, etc.); we just suppress the panel and quit immediately.
//
// playEndAnimationToPos (classic) / playPlatformerEndAnimationToPos
// (platformer) mark the "end window", so the key only acts once a level is
// actually finishing -- making it safe to share with your jump key.
// ---------------------------------------------------------------------------
class $modify(SkipEndPlayLayer, PlayLayer) {
    struct Fields {
        bool m_endActive = false; // has the ending started?
        bool m_exiting = false;   // are we bailing out right now?
    };

    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->m_endActive = false;
        m_fields->m_exiting = false;
    }

    void playEndAnimationToPos(cocos2d::CCPoint position) {
        m_fields->m_endActive = true;
        PlayLayer::playEndAnimationToPos(position);
    }

    void playPlatformerEndAnimationToPos(cocos2d::CCPoint position, bool instant) {
        m_fields->m_endActive = true;
        PlayLayer::playPlatformerEndAnimationToPos(position, instant);
    }

    // Suppress the completion panel while we're bailing out.
    void showCompleteText() {
        if (m_fields->m_exiting) return;
        PlayLayer::showCompleteText();
    }

    // Returns true if it actually exited (so the key press is consumed).
    bool requestExit() {
        if (!m_fields->m_endActive || m_fields->m_exiting) return false;
        m_fields->m_exiting = true;

        // Register the completion (saves best %, stars, etc.) without the panel.
        if (!m_hasCompletedLevel) {
            this->levelComplete();
        }
        // Leave the level, straight back to where you came from.
        this->onQuit();
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
                    if (static_cast<SkipEndPlayLayer*>(pl)->requestExit()) {
                        return true; // consume only when we actually exit
                    }
                }
            }
            return false; // otherwise let the key do its normal thing (jump)
        }
    );
}

$on_mod(Loaded) {
    log::info("Future Mod loaded - bind a key in settings to exit level endings.");
}
