#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// PlayLayer: track when a level is finishing and expose a skip() that jumps
// straight to the "Level Complete" screen, killing the end animation + delay.
// ---------------------------------------------------------------------------
class $modify(SkipEndPlayLayer, PlayLayer) {
    struct Fields {
        bool m_completing = false; // are we currently in the end sequence?
        bool m_skipped = false;    // guard so we only skip once per completion
    };

    void levelComplete() {
        PlayLayer::levelComplete();
        // m_hasCompletedLevel is now true and the end animation has started.
        m_fields->m_completing = true;
        m_fields->m_skipped = false;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->m_completing = false;
        m_fields->m_skipped = false;
    }

    void skipEnding() {
        // Only valid while the level is finishing and the screen isn't up yet.
        if (!m_fields->m_completing || m_fields->m_skipped) return;
        if (!m_hasCompletedLevel) return;

        m_fields->m_skipped = true;

        // Cancel the scheduled "show complete text" delay and stop the
        // suck-into-the-wall animation actions, then show the screen now.
        this->unscheduleAllSelectors();
        this->stopAllActions();
        this->showCompleteText();
    }
};

// ---------------------------------------------------------------------------
// Keyboard: when the configured key goes down, skip the ending if a level is
// finishing. Self-contained (no extra dependencies).
// ---------------------------------------------------------------------------
class $modify(SkipEndKeyboard, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double delta) {
        if (down && !repeat) {
            int wanted = Mod::get()->getSettingValue<int64_t>("skip-key");
            if (static_cast<int>(key) == wanted) {
                if (auto pl = PlayLayer::get()) {
                    static_cast<SkipEndPlayLayer*>(pl)->skipEnding();
                }
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, delta);
    }
};

$on_mod(Loaded) {
    log::info("Future Mod loaded - press your skip key to skip level endings.");
}
