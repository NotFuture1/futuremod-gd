#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/loader/SettingV3.hpp>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// PlayLayer: skip straight to the "Level Complete" screen, cutting the
// suck-into-the-wall animation AND the dead-air delay before the panel.
// ---------------------------------------------------------------------------
class $modify(SkipEndPlayLayer, PlayLayer) {
    struct Fields {
        bool m_skipped = false; // only skip once per completion
    };

    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->m_skipped = false;
    }

    void skipEnding() {
        // Only act once the level is actually finishing. This makes the key
        // safe to share with your jump key: during normal play it does
        // nothing, but the moment the ending starts it takes you out.
        if (!m_hasCompletedLevel || m_fields->m_skipped) return;
        m_fields->m_skipped = true;

        // Kill the end animation (on the layer and on the player icon)...
        this->stopAllActions();
        if (m_player1) m_player1->stopAllActions();
        if (m_player2) m_player2->stopAllActions();
        // ...cancel the scheduled delay that waits before the panel...
        this->unscheduleAllSelectors();
        // ...and show the Level Complete panel right now.
        this->showCompleteText();
    }
};

// ---------------------------------------------------------------------------
// Listen for the rebindable keybind (configured in the mod's settings).
// Geode 5.x handles keybinds natively, so no external dependency is needed.
// ---------------------------------------------------------------------------
$execute {
    listenForKeybindSettingPresses(
        "skip-end",
        [](Keybind const&, bool down, bool repeat, double) -> bool {
            if (down && !repeat) {
                if (auto pl = PlayLayer::get()) {
                    if (pl->m_hasCompletedLevel) {
                        static_cast<SkipEndPlayLayer*>(pl)->skipEnding();
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
