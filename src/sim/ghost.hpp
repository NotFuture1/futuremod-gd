#pragma once

#include "../common.hpp"

// ===========================================================================
// Ghost simulation: throwaway PlayerObjects that are seeded from the real
// player and stepped forward against the LIVE level, so a timing can be
// probed without restarting the level. This is the proven GD pattern
// (GDMegaOverlay / xdBot / uvbot all do exactly this, ~1000-2500 physics
// iterations per frame, at playable framerates).
//
// Phase 1 uses it only to MEASURE ITS OWN FIDELITY against the real player.
// Nothing is reported to the user until that passes on hardware.
// ===========================================================================

namespace fw::ghost {

// ---------------------------------------------------------------------------
// Value-only snapshot of a PlayerObject's physics state.
//
// CRITICAL: this must never hold a pointer owned by the real player.
// m_collisionLogTop/Bottom/Left/Right are CCDictionary*, and
// PlayerObject::resetCollisionLog() calls removeAllObjects() on them -- so
// copying those pointers onto a ghost would make clearing the GHOST's log
// wipe the REAL player's log mid-step. Pointer fields are deliberately
// absent here, not forgotten. Same reasoning for m_touchingRings,
// m_particleSystems, m_playerStreak and the sprite/batch-node members.
//
// spawnFromPlayer() is not used for seeding either: on Windows it is Geode's
// reimplementation, and it forces setVisible(true), zeroes m_isOnGround /
// m_isOnGround2, places a streak point on wave, and defers the rest to
// copyAttributes (which cannot be read). It is only used once, at pool
// creation, to get a usable icon set up.
// ---------------------------------------------------------------------------
struct PlayerSnapshot {
    // node transform
    cocos2d::CCPoint nodePos{};
    float nodeRot = 0.f;
    float nodeScaleX = 1.f, nodeScaleY = 1.f;

    // kinematics
    cocos2d::CCPoint position{};
    double yVelocity = 0.0;
    double fallSpeed = 0.0;
    double gravity = 0.0;
    float gravityMod = 1.f;
    float playerSpeed = 1.f;
    double speedMultiplier = 1.0;
    float vehicleSize = 1.f;
    double yStart = 0.0;
    double platformerXVelocity = 0.0;
    double maybeReverseSpeed = 0.0;
    double maybeReverseAcceleration = 0.0;
    float xVelocityRelated = 0.f;
    float xVelocityRelated2 = 0.f;
    double accelerationOrSpeed = 0.0;
    double snapDistance = 0.0;

    // gamemode
    bool isShip = false, isBird = false, isBall = false, isDart = false;
    bool isRobot = false, isSpider = false, isSwing = false;
    bool isUpsideDown = false, isSideways = false, isGoingLeft = false;
    bool isDead = false, isOnGround = false, isOnGround2 = false;
    bool isOnGround3 = false, isOnGround4 = false;
    int stateOnGround = 0;
    double gameModeChangedTime = 0.0;

    // slopes
    bool isOnSlope = false, wasOnSlope = false, maybeUpsideDownSlope = false;
    bool isCollidingWithSlope = false, isCurrentSlopeTop = false;
    bool slopeFlipGravityRelated = false, slopeSlidingMaybeRotated = false;
    float slopeVelocity = 0.f, slopeAngle = 0.f, slopeAngleRadians = 0.f;
    double slopeRotation = 0.0, currentSlopeYVelocity = 0.0;
    double yVelocityBeforeSlope = 0.0, slopeStartTime = 0.0, slopeEndTime = 0.0;
    int collidingWithSlopeId = 0;

    // collision bookkeeping (ints only -- the CCDictionary logs are NOT copied)
    int lastCollisionTop = 0, lastCollisionBottom = 0;
    int lastCollisionLeft = 0, lastCollisionRight = 0;
    int unk50C = 0, unk510 = 0;
    double collidedTopMinY = 0.0, collidedBottomMaxY = 0.0;
    double collidedLeftMaxX = 0.0, collidedRightMinX = 0.0;
    bool maybeIsColliding = false, isOutOfBounds = false;

    // input latch / buffering -- these decide whether a click even registers,
    // so they matter more here than anywhere else
    bool jumpBuffered = false, wasJumpBuffered = false, stateRingJump = false;
    bool stateRingJump2 = false, wasRobotJump = false;
    unsigned char stateJumpBuffered = 0;
    bool touchedRing = false, touchedCustomRing = false;
    bool touchedGravityPortal = false, touchedPad = false;
    bool holdingLeft = false, holdingRight = false, leftPressedFirst = false;
    bool inputsLocked = false, isLocked = false, controlsDisabled = false;
    bool hasEverJumped = false, hasEverHitRing = false;
    double lastJumpTime = 0.0, lastFlipTime = 0.0, lastSpiderFlipTime = 0.0;
    double lastLandTime = 0.0;

    // dash
    bool isDashing = false;
    double dashX = 0.0, dashY = 0.0, dashAngle = 0.0, dashStartTime = 0.0;

    // forces / platformer
    int stateForce = 0, stateBoostX = 0, stateBoostY = 0, maybeStateForce2 = 0;
    int stateScale = 0, stateNoAutoJump = 0, stateDartSlide = 0;
    int stateHitHead = 0, stateFlipGravity = 0;
    unsigned char stateNoStickX = 0, stateNoStickY = 0;
    cocos2d::CCPoint stateForceVector{};
    bool affectedByForces = false;
    bool isSliding = false, isSlidingRight = false, isOnIce = false;
    bool isMoving = false, isPlatformer = false, isAccelerating = false;
    bool platformerMovingLeft = false, platformerMovingRight = false;
    bool maybeHasStopped = false, decreaseBoostSlide = false, maybeIsBoosted = false;
    double maybeSlopeForce = 0.0, physDeltaRelated = 0.0;
    int maybeSlidingTime = 0;
    double maybeSlidingStartTime = 0.0, changedDirectionsTime = 0.0;

    // rotation
    float rotationSpeed = 0.f, rotateSpeed = 0.f;
    bool isRotating = false, isBallRotating = false, isBallRotating2 = false;
    cocos2d::CCPoint shipRotation{};

    // misc timing
    double totalTime = 0.0;
    double groundYVelocity = 0.0;
    cocos2d::CCPoint lastGroundedPos{};
    bool isSecondPlayer = false;

    void capture(PlayerObject* p);
    void apply(PlayerObject* p) const;
};

// ---------------------------------------------------------------------------
// Simulation guard. isSim() is true ONLY inside a ghost stepping loop, which
// is what makes it safe for the side-effect suppression hooks to key off it:
// no real physics ever runs while it is set.
// ---------------------------------------------------------------------------
bool isSim();
bool owns(PlayerObject* p);
void markDead(PlayerObject* p);
bool deadFlag();
void clearDead();

// Pool lifetime. Ghosts are children of m_objectLayer, which frees its
// children on level exit -- so the pool is always torn down in PlayLayer::init
// and onQuit, never held across levels.
void ensurePool(PlayLayer* pl);
void clearPool();
PlayerObject* ghostP1();

// One full 1/240 tick on a ghost. `order` selects the step reconstruction.
void step(GJBaseGameLayer* layer, PlayerObject* g, float dt, int order);

// --- Phase 1: fidelity probe (log-only) ------------------------------------
void fidelityOnTick(PlayLayer* pl, int tick);
void fidelityOnInputEdge(int tick);
void fidelityReset();
void fidelityReport();

} // namespace fw::ghost
