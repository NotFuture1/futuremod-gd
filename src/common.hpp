#pragma once

#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>

using namespace geode::prelude;

// ===========================================================================
// Shared helpers for the frame-window counter.
//
// Everything the analyzer reports goes through the [fw] prefix: the user's GD
// runs on a different machine, so `geode.log` is the ONLY instrument we have.
// Every phase must end in a line that can be read without interpretation.
// ===========================================================================

namespace fw {

inline bool verbose() {
    return Mod::get()->getSettingValue<bool>("log-verbose");
}

// ghost-order selects which reconstruction of GD's physics step the ghost
// simulation uses. Vanilla's real ordering inside processCommands is not
// readable from the bindings, so it is an on-hardware A/B, not a guess.
inline int ghostOrder() {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>("ghost-order"));
}

inline double ghostEps() {
    return Mod::get()->getSettingValue<double>("ghost-eps");
}

// True while the macro system is recording, replaying or analysing. Defined in
// macro.cpp. Diagnostics that assume real, human input must sit this out:
// playback injects straight into the PlayerObject, so handleButton never fires.
bool macroBusy();

} // namespace fw

#define FW_LOG(...)  ::geode::log::info("[fw] {}", fmt::format(__VA_ARGS__))
#define FW_WARN(...) ::geode::log::warn("[fw] {}", fmt::format(__VA_ARGS__))
#define FW_VLOG(...)                                                           \
    do {                                                                       \
        if (::fw::verbose()) FW_LOG(__VA_ARGS__);                              \
    } while (0)
