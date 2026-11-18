#pragma once

// ===========================================================================
// Macro editor: the runtime (in macro.cpp) that the editor's UI (editor.cpp)
// drives. While editing, the level is frozen at the "playhead" step: what you
// see IS the macro's run at that point. Every edit is checked by re-running
// the changed part of the macro from a save-state, so the path shown after a
// moved click is what the replay really does.
// ===========================================================================

#include "editor_logic.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fw::edrt {

struct PathPt {
    float x = 0.f, y = 0.f;
    bool ok = false;
};

// gamemode codes for the strip
enum : uint8_t { kCube, kShip, kBall, kUfo, kWave, kRobot, kSpider, kSwing };

struct StepInfo {
    uint8_t mode = kCube;
    bool mini = false;
    bool upsideDown = false;
    float speed = 0.f; // GD's player speed value (0.7 / 0.9 / 1.1 / 1.3 / 1.6)
    bool ok = false;
};

// an analyzed click's frame window, relative to the click (frames)
struct Window {
    int orig = -1; // index in the macro as loaded
    double lo = 0.0, hi = 0.0, w = 0.0;
};

enum class Busy { None, Building, Seeking, Resim, Playing };

// ---- session ----------------------------------------------------------------
bool canStart(std::string* why);
void start();
// write `edges` as the level's macro (windows kept for clicks that weren't
// moved; returns how many analyzed clicks lost their window)
int save(std::vector<fw::ed::Edge> const& edges);
// leave the editor: `save` writes `edges` first; otherwise the macro is left
// as it was last saved
void finish(bool save, std::vector<fw::ed::Edge> const& edges);
bool active();

// the macro as loaded, its analysis, and some facts about it
std::vector<fw::ed::Edge> loadEdges();
std::vector<Window> windows();
bool cbf();
bool twoPlayer();
int lastStep();  // last input's step
int endLimit();  // furthest step the editor simulates to
std::string title();

// ---- simulation ---------------------------------------------------------------
// call every frame (the editor's own layer does): runs the simulation
void tick(float dt);
Busy busy();
float progress(); // building: 0..1
// show the level at this step
void seek(int step);
int playhead();
// use the edited macro from now on: everything from `firstChange` (frames)
// on is simulated again
void apply(std::vector<fw::ed::Edge> const& edges, double firstChange);
void play(bool on);
bool playing();

// what's known about the CURRENT (edited) macro, per step (object-layer space)
std::vector<PathPt> const& path(int player);
// the macro as it was when the editor opened
std::vector<PathPt> const& basePath(int player);
std::vector<StepInfo> const& info();
int knownTo();   // path is known up to this step
int deathStep(); // -1: no death known
int endStep();   // -1: the end wasn't reached
// changes whenever any of the above does (the UI redraws on change)
uint64_t revision();

} // namespace fw::edrt
