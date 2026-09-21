#include "ghost.hpp"

#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

namespace fw::ghost {

namespace {

bool        g_sim      = false;   // true only inside a ghost stepping loop
bool        g_dead     = false;   // destroyPlayer fired on a ghost this probe
Ref<PlayerObject> g_p1 = nullptr; // the ghost itself
PlayLayer*  g_poolFor  = nullptr; // which PlayLayer the pool belongs to (identity only)

// Scoped simulation flag: every side-effect suppression hook keys off this.
struct SimScope {
    SimScope()  { g_sim = true;  }
    ~SimScope() { g_sim = false; }
};

} // namespace

bool isSim() { return g_sim; }
bool owns(PlayerObject* p) { return p && g_p1 && p == g_p1.data(); }
void markDead(PlayerObject*) { g_dead = true; }
bool deadFlag() { return g_dead; }
void clearDead() { g_dead = false; }
PlayerObject* ghostP1() { return g_p1.data(); }

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------
void PlayerSnapshot::capture(PlayerObject* p) {
    nodePos = p->getPosition();
    nodeRot = p->getRotation();
    nodeScaleX = p->getScaleX();
    nodeScaleY = p->getScaleY();

    position = p->m_position;
    yVelocity = p->m_yVelocity;
    fallSpeed = p->m_fallSpeed;
    gravity = p->m_gravity;
    gravityMod = p->m_gravityMod;
    playerSpeed = p->m_playerSpeed;
    speedMultiplier = p->m_speedMultiplier;
    vehicleSize = p->m_vehicleSize;
    yStart = p->m_yStart;
    platformerXVelocity = p->m_platformerXVelocity;
    maybeReverseSpeed = p->m_maybeReverseSpeed;
    maybeReverseAcceleration = p->m_maybeReverseAcceleration;
    xVelocityRelated = p->m_xVelocityRelated;
    xVelocityRelated2 = p->m_xVelocityRelated2;
    accelerationOrSpeed = p->m_accelerationOrSpeed;
    snapDistance = p->m_snapDistance;

    isShip = p->m_isShip; isBird = p->m_isBird; isBall = p->m_isBall;
    isDart = p->m_isDart; isRobot = p->m_isRobot; isSpider = p->m_isSpider;
    isSwing = p->m_isSwing;
    isUpsideDown = p->m_isUpsideDown; isSideways = p->m_isSideways;
    isGoingLeft = p->m_isGoingLeft;
    isDead = p->m_isDead;
    isOnGround = p->m_isOnGround; isOnGround2 = p->m_isOnGround2;
    isOnGround3 = p->m_isOnGround3; isOnGround4 = p->m_isOnGround4;
    stateOnGround = p->m_stateOnGround;
    gameModeChangedTime = p->m_gameModeChangedTime;

    isOnSlope = p->m_isOnSlope; wasOnSlope = p->m_wasOnSlope;
    maybeUpsideDownSlope = p->m_maybeUpsideDownSlope;
    isCollidingWithSlope = p->m_isCollidingWithSlope;
    isCurrentSlopeTop = p->m_isCurrentSlopeTop;
    slopeFlipGravityRelated = p->m_slopeFlipGravityRelated;
    slopeSlidingMaybeRotated = p->m_slopeSlidingMaybeRotated;
    slopeVelocity = p->m_slopeVelocity;
    slopeAngle = p->m_slopeAngle;
    slopeAngleRadians = p->m_slopeAngleRadians;
    slopeRotation = p->m_slopeRotation;
    currentSlopeYVelocity = p->m_currentSlopeYVelocity;
    yVelocityBeforeSlope = p->m_yVelocityBeforeSlope;
    slopeStartTime = p->m_slopeStartTime;
    slopeEndTime = p->m_slopeEndTime;
    collidingWithSlopeId = p->m_collidingWithSlopeId;

    lastCollisionTop = p->m_lastCollisionTop;
    lastCollisionBottom = p->m_lastCollisionBottom;
    lastCollisionLeft = p->m_lastCollisionLeft;
    lastCollisionRight = p->m_lastCollisionRight;
    unk50C = p->m_unk50C;
    unk510 = p->m_unk510;
    collidedTopMinY = p->m_collidedTopMinY;
    collidedBottomMaxY = p->m_collidedBottomMaxY;
    collidedLeftMaxX = p->m_collidedLeftMaxX;
    collidedRightMinX = p->m_collidedRightMinX;
    maybeIsColliding = p->m_maybeIsColliding;
    isOutOfBounds = p->m_isOutOfBounds;

    jumpBuffered = p->m_jumpBuffered;
    wasJumpBuffered = p->m_wasJumpBuffered;
    stateRingJump = p->m_stateRingJump;
    stateRingJump2 = p->m_stateRingJump2;
    wasRobotJump = p->m_wasRobotJump;
    stateJumpBuffered = p->m_stateJumpBuffered;
    touchedRing = p->m_touchedRing;
    touchedCustomRing = p->m_touchedCustomRing;
    touchedGravityPortal = p->m_touchedGravityPortal;
    touchedPad = p->m_touchedPad;
    holdingLeft = p->m_holdingLeft;
    holdingRight = p->m_holdingRight;
    leftPressedFirst = p->m_leftPressedFirst;
    inputsLocked = p->m_inputsLocked;
    isLocked = p->m_isLocked;
    controlsDisabled = p->m_controlsDisabled;
    hasEverJumped = p->m_hasEverJumped;
    hasEverHitRing = p->m_hasEverHitRing;
    lastJumpTime = p->m_lastJumpTime;
    lastFlipTime = p->m_lastFlipTime;
    lastSpiderFlipTime = p->m_lastSpiderFlipTime;
    lastLandTime = p->m_lastLandTime;

    isDashing = p->m_isDashing;
    dashX = p->m_dashX; dashY = p->m_dashY;
    dashAngle = p->m_dashAngle; dashStartTime = p->m_dashStartTime;

    stateForce = p->m_stateForce;
    stateBoostX = p->m_stateBoostX;
    stateBoostY = p->m_stateBoostY;
    maybeStateForce2 = p->m_maybeStateForce2;
    stateScale = p->m_stateScale;
    stateNoAutoJump = p->m_stateNoAutoJump;
    stateDartSlide = p->m_stateDartSlide;
    stateHitHead = p->m_stateHitHead;
    stateFlipGravity = p->m_stateFlipGravity;
    stateNoStickX = p->m_stateNoStickX;
    stateNoStickY = p->m_stateNoStickY;
    stateForceVector = p->m_stateForceVector;
    affectedByForces = p->m_affectedByForces;
    isSliding = p->m_isSliding;
    isSlidingRight = p->m_isSlidingRight;
    isOnIce = p->m_isOnIce;
    isMoving = p->m_isMoving;
    isPlatformer = p->m_isPlatformer;
    isAccelerating = p->m_isAccelerating;
    platformerMovingLeft = p->m_platformerMovingLeft;
    platformerMovingRight = p->m_platformerMovingRight;
    maybeHasStopped = p->m_maybeHasStopped;
    decreaseBoostSlide = p->m_decreaseBoostSlide;
    maybeIsBoosted = p->m_maybeIsBoosted;
    maybeSlopeForce = p->m_maybeSlopeForce;
    physDeltaRelated = p->m_physDeltaRelated;
    maybeSlidingTime = p->m_maybeSlidingTime;
    maybeSlidingStartTime = p->m_maybeSlidingStartTime;
    changedDirectionsTime = p->m_changedDirectionsTime;

    rotationSpeed = p->m_rotationSpeed;
    rotateSpeed = p->m_rotateSpeed;
    isRotating = p->m_isRotating;
    isBallRotating = p->m_isBallRotating;
    isBallRotating2 = p->m_isBallRotating2;
    shipRotation = p->m_shipRotation;

    totalTime = p->m_totalTime;
    groundYVelocity = p->m_groundYVelocity;
    lastGroundedPos = p->m_lastGroundedPos;
    isSecondPlayer = p->m_isSecondPlayer;
}

void PlayerSnapshot::apply(PlayerObject* p) const {
    p->m_position = position;
    p->m_yVelocity = yVelocity;
    p->m_fallSpeed = fallSpeed;
    p->m_gravity = gravity;
    p->m_gravityMod = gravityMod;
    p->m_playerSpeed = playerSpeed;
    p->m_speedMultiplier = speedMultiplier;
    p->m_vehicleSize = vehicleSize;
    p->m_yStart = yStart;
    p->m_platformerXVelocity = platformerXVelocity;
    p->m_maybeReverseSpeed = maybeReverseSpeed;
    p->m_maybeReverseAcceleration = maybeReverseAcceleration;
    p->m_xVelocityRelated = xVelocityRelated;
    p->m_xVelocityRelated2 = xVelocityRelated2;
    p->m_accelerationOrSpeed = accelerationOrSpeed;
    p->m_snapDistance = snapDistance;

    p->m_isShip = isShip; p->m_isBird = isBird; p->m_isBall = isBall;
    p->m_isDart = isDart; p->m_isRobot = isRobot; p->m_isSpider = isSpider;
    p->m_isSwing = isSwing;
    p->m_isUpsideDown = isUpsideDown; p->m_isSideways = isSideways;
    p->m_isGoingLeft = isGoingLeft;
    p->m_isDead = false;               // a ghost always starts alive
    p->m_isOnGround = isOnGround; p->m_isOnGround2 = isOnGround2;
    p->m_isOnGround3 = isOnGround3; p->m_isOnGround4 = isOnGround4;
    p->m_stateOnGround = stateOnGround;
    p->m_gameModeChangedTime = gameModeChangedTime;

    p->m_isOnSlope = isOnSlope; p->m_wasOnSlope = wasOnSlope;
    p->m_maybeUpsideDownSlope = maybeUpsideDownSlope;
    p->m_isCollidingWithSlope = isCollidingWithSlope;
    p->m_isCurrentSlopeTop = isCurrentSlopeTop;
    p->m_slopeFlipGravityRelated = slopeFlipGravityRelated;
    p->m_slopeSlidingMaybeRotated = slopeSlidingMaybeRotated;
    p->m_slopeVelocity = slopeVelocity;
    p->m_slopeAngle = slopeAngle;
    p->m_slopeAngleRadians = slopeAngleRadians;
    p->m_slopeRotation = slopeRotation;
    p->m_currentSlopeYVelocity = currentSlopeYVelocity;
    p->m_yVelocityBeforeSlope = yVelocityBeforeSlope;
    p->m_slopeStartTime = slopeStartTime;
    p->m_slopeEndTime = slopeEndTime;
    p->m_collidingWithSlopeId = collidingWithSlopeId;

    p->m_lastCollisionTop = lastCollisionTop;
    p->m_lastCollisionBottom = lastCollisionBottom;
    p->m_lastCollisionLeft = lastCollisionLeft;
    p->m_lastCollisionRight = lastCollisionRight;
    p->m_unk50C = unk50C;
    p->m_unk510 = unk510;
    p->m_collidedTopMinY = collidedTopMinY;
    p->m_collidedBottomMaxY = collidedBottomMaxY;
    p->m_collidedLeftMaxX = collidedLeftMaxX;
    p->m_collidedRightMinX = collidedRightMinX;
    p->m_maybeIsColliding = maybeIsColliding;
    p->m_isOutOfBounds = false;

    p->m_jumpBuffered = jumpBuffered;
    p->m_wasJumpBuffered = wasJumpBuffered;
    p->m_stateRingJump = stateRingJump;
    p->m_stateRingJump2 = stateRingJump2;
    p->m_wasRobotJump = wasRobotJump;
    p->m_stateJumpBuffered = stateJumpBuffered;
    p->m_touchedRing = touchedRing;
    p->m_touchedCustomRing = touchedCustomRing;
    p->m_touchedGravityPortal = touchedGravityPortal;
    p->m_touchedPad = touchedPad;
    p->m_holdingLeft = holdingLeft;
    p->m_holdingRight = holdingRight;
    p->m_leftPressedFirst = leftPressedFirst;
    p->m_inputsLocked = inputsLocked;
    p->m_isLocked = isLocked;
    p->m_controlsDisabled = controlsDisabled;
    p->m_hasEverJumped = hasEverJumped;
    p->m_hasEverHitRing = hasEverHitRing;
    p->m_lastJumpTime = lastJumpTime;
    p->m_lastFlipTime = lastFlipTime;
    p->m_lastSpiderFlipTime = lastSpiderFlipTime;
    p->m_lastLandTime = lastLandTime;

    p->m_isDashing = isDashing;
    p->m_dashX = dashX; p->m_dashY = dashY;
    p->m_dashAngle = dashAngle; p->m_dashStartTime = dashStartTime;

    p->m_stateForce = stateForce;
    p->m_stateBoostX = stateBoostX;
    p->m_stateBoostY = stateBoostY;
    p->m_maybeStateForce2 = maybeStateForce2;
    p->m_stateScale = stateScale;
    p->m_stateNoAutoJump = stateNoAutoJump;
    p->m_stateDartSlide = stateDartSlide;
    p->m_stateHitHead = stateHitHead;
    p->m_stateFlipGravity = stateFlipGravity;
    p->m_stateNoStickX = stateNoStickX;
    p->m_stateNoStickY = stateNoStickY;
    p->m_stateForceVector = stateForceVector;
    p->m_affectedByForces = affectedByForces;
    p->m_isSliding = isSliding;
    p->m_isSlidingRight = isSlidingRight;
    p->m_isOnIce = isOnIce;
    p->m_isMoving = isMoving;
    p->m_isPlatformer = isPlatformer;
    p->m_isAccelerating = isAccelerating;
    p->m_platformerMovingLeft = platformerMovingLeft;
    p->m_platformerMovingRight = platformerMovingRight;
    p->m_maybeHasStopped = maybeHasStopped;
    p->m_decreaseBoostSlide = decreaseBoostSlide;
    p->m_maybeIsBoosted = maybeIsBoosted;
    p->m_maybeSlopeForce = maybeSlopeForce;
    p->m_physDeltaRelated = physDeltaRelated;
    p->m_maybeSlidingTime = maybeSlidingTime;
    p->m_maybeSlidingStartTime = maybeSlidingStartTime;
    p->m_changedDirectionsTime = changedDirectionsTime;

    p->m_rotationSpeed = rotationSpeed;
    p->m_rotateSpeed = rotateSpeed;
    p->m_isRotating = isRotating;
    p->m_isBallRotating = isBallRotating;
    p->m_isBallRotating2 = isBallRotating2;
    p->m_shipRotation = shipRotation;

    p->m_totalTime = totalTime;
    p->m_groundYVelocity = groundYVelocity;
    p->m_lastGroundedPos = lastGroundedPos;
    p->m_isSecondPlayer = isSecondPlayer;

    // the ghost owns its own collision dictionaries -- never the real player's
    p->resetCollisionLog(true);

    p->setPosition(nodePos);
    p->setRotation(nodeRot);
    p->setScaleX(nodeScaleX);
    p->setScaleY(nodeScaleY);

    // effects off: a ghost must be seen by physics, never by the player
    p->m_playEffects = false;
    p->m_hasGroundParticles = false;
    p->m_hasShipParticles = false;
    p->setVisible(false);
}

// ---------------------------------------------------------------------------
// Pool
// ---------------------------------------------------------------------------
void ensurePool(PlayLayer* pl) {
    if (!pl || !pl->m_player1 || !pl->m_objectLayer) return;
    if (g_p1 && g_poolFor == pl) return;
    clearPool();

    auto g = PlayerObject::create(1, 1, pl, pl->m_objectLayer, true);
    if (!g) { FW_WARN("ghost pool: PlayerObject::create failed"); return; }

    pl->m_objectLayer->addChild(g);
    // spawnFromPlayer is used ONCE, here, only to get the icon/visual set up.
    // It forces visible, zeroes m_isOnGround and places a wave streak point,
    // which is why it is never used for per-probe seeding.
    g->spawnFromPlayer(pl->m_player1, false);
    g->setVisible(false);
    g->unscheduleUpdate(); // ghosts are stepped by us, never by the scheduler
    g->m_playEffects = false;

    g_p1 = g;
    g_poolFor = pl;
    FW_LOG("ghost pool created (order={})", fw::ghostOrder());
}

void clearPool() {
    if (g_p1) {
        if (g_p1->getParent()) g_p1->removeFromParent();
        g_p1 = nullptr;
    }
    g_poolFor = nullptr;
}

// ---------------------------------------------------------------------------
// One physics tick on a ghost.
//
// Vanilla's real ordering inside processCommands is not readable from the
// bindings, and a half-step phase error is +-0.5 frames -- wider than the
// entire "<1" bucket we want to report. So the order is a runtime setting and
// Phase 1 decides it on hardware instead of by assumption.
// ---------------------------------------------------------------------------
void step(GJBaseGameLayer* layer, PlayerObject* g, float dt, int order) {
    if (!layer || !g) return;
    g->resetCollisionLog(true);
    switch (order) {
        case 1: // update first, then collide
            g->update(dt);
            layer->checkCollisions(g, dt, false);
            g->updateRotation(dt);
            g->updateSpecial(dt);
            break;
        case 2: // collide first, updateSpecial before rotation
            layer->checkCollisions(g, dt, false);
            g->update(dt);
            g->updateSpecial(dt);
            g->updateRotation(dt);
            break;
        default: // 0 -- the order GDMegaOverlay / xdBot / uvbot all use
            layer->checkCollisions(g, dt, false);
            g->update(dt);
            g->updateRotation(dt);
            g->updateSpecial(dt);
            break;
    }
    g->updatePlayerScale();
}

// ===========================================================================
// Phase 1 -- fidelity probe.
//
// Periodically seed a ghost from the real player, simulate N ticks forward
// with the input state HELD, and then compare against what the real player
// actually does over the next N ticks. Any real input edge during the window
// aborts the sample (we cannot know the future inputs, and holding is the
// only honest assumption).
//
// This measures exactly the thing that decides whether sub-frame windows are
// meaningful: does our reconstruction of GD's step reproduce GD's step?
// ===========================================================================
namespace {

constexpr int kFidPeriod = 120; // ticks between samples
constexpr int kFidLen    = 60;  // ticks simulated / compared per sample

struct FidPoint { float x, y; double vy; };

struct FidSample {
    bool active = false;
    int  startTick = 0;
    std::vector<FidPoint> track;
    float maxdx = 0.f, maxdy = 0.f;
    double maxdvy = 0.0;
    int divergeAt = -1;
    bool aborted = false;
    const char* abortReason = "";
};

FidSample        g_s;
std::vector<float> g_cleanMaxDx;   // per-sample maxdx, clean samples only
int g_nSeeds = 0, g_nClean = 0, g_nAborted = 0;
int g_lastEdgeTick = -1;

struct LayerScalars {
    int sci = 0, scc = 0, hci = 0, hcc = 0, aoi = 0, aoc = 0;
    void* p1cb = nullptr;
    void* p2cb = nullptr;
    void capture(GJBaseGameLayer* l) {
        sci = l->m_solidCollisionObjectsIndex;  scc = l->m_solidCollisionObjectsCount;
        hci = l->m_hazardCollisionObjectsIndex; hcc = l->m_hazardCollisionObjectsCount;
        aoi = l->m_activeObjectsIndex;          aoc = l->m_activeObjectsCount;
        p1cb = l->m_player1CollisionBlock;      p2cb = l->m_player2CollisionBlock;
    }
    void restore(GJBaseGameLayer* l) const {
        l->m_solidCollisionObjectsIndex = sci;  l->m_solidCollisionObjectsCount = scc;
        l->m_hazardCollisionObjectsIndex = hci; l->m_hazardCollisionObjectsCount = hcc;
        l->m_activeObjectsIndex = aoi;          l->m_activeObjectsCount = aoc;
        l->m_player1CollisionBlock = static_cast<GameObject*>(p1cb);
        l->m_player2CollisionBlock = static_cast<GameObject*>(p2cb);
    }
    bool operator==(LayerScalars const& o) const {
        return sci == o.sci && scc == o.scc && hci == o.hci && hcc == o.hcc
            && aoi == o.aoi && aoc == o.aoc && p1cb == o.p1cb && p2cb == o.p2cb;
    }
};

void startSample(PlayLayer* pl, int tick) {
    auto g = g_p1.data();
    auto real = pl->m_player1;
    if (!g || !real) return;

    PlayerSnapshot snap;
    snap.capture(real);
    snap.apply(g);

    LayerScalars before, after;
    before.capture(pl);

    g_s = FidSample{};
    g_s.active = true;
    g_s.startTick = tick;
    g_s.track.reserve(kFidLen);

    clearDead();
    {
        SimScope sim;
        const float dt = 1.f / 240.f;
        const int order = fw::ghostOrder();
        for (int i = 0; i < kFidLen; i++) {
            step(pl, g, dt, order);
            if (deadFlag() || g->m_isOutOfBounds) break;
            auto pos = g->getPosition();
            g_s.track.push_back({ pos.x, pos.y, g->m_yVelocity });
        }
    }
    after.capture(pl);
    before.restore(pl); // belt and braces: put back whatever the probe touched

    g_nSeeds++;
    FW_VLOG("LAYERIDX before sci={} hci={} aoi={} p1cb={} after sci={} hci={} aoi={} p1cb={} DELTA={}",
        before.sci, before.hci, before.aoi, fmt::ptr(before.p1cb),
        after.sci, after.hci, after.aoi, fmt::ptr(after.p1cb),
        (before == after) ? 0 : 1);
    if (!(before == after)) {
        FW_WARN("LAYERIDX DELTA=1 @tick {} -- checkCollisions mutates layer state "
                "(sci {}->{} hci {}->{} aoi {}->{})", tick,
                before.sci, after.sci, before.hci, after.hci, before.aoi, after.aoi);
    }
    if (g_s.track.empty()) {
        g_s.aborted = true;
        g_s.abortReason = "ghost-died-immediately";
    }
}

void finishSample() {
    if (!g_s.active) return;
    g_s.active = false;
    if (g_s.aborted) {
        g_nAborted++;
        FW_VLOG("GHOSTFID order={} seed@{} ABORTED ({})",
            fw::ghostOrder(), g_s.startTick, g_s.abortReason);
        return;
    }
    g_nClean++;
    g_cleanMaxDx.push_back(g_s.maxdx);
    FW_LOG("GHOSTFID order={} seed@{} n={} maxdx={:.3f} maxdy={:.3f} maxdvy={:.3f} divergeAt={}",
        fw::ghostOrder(), g_s.startTick, static_cast<int>(g_s.track.size()),
        g_s.maxdx, g_s.maxdy, g_s.maxdvy, g_s.divergeAt);
}

} // namespace

void fidelityOnInputEdge(int tick) {
    g_lastEdgeTick = tick;
    if (g_s.active && !g_s.aborted) {
        g_s.aborted = true;
        g_s.abortReason = "input-edge";
    }
}

void fidelityOnTick(PlayLayer* pl, int tick) {
    if (!pl || !pl->m_player1) return;

    if (g_s.active) {
        const int i = tick - g_s.startTick - 1;
        if (i < 0) return;
        if (pl->m_player1->m_isDead && !g_s.aborted) {
            g_s.aborted = true;
            g_s.abortReason = "real-player-died";
        }
        if (i >= static_cast<int>(g_s.track.size())) { finishSample(); return; }
        if (!g_s.aborted) {
            auto rp = pl->m_player1->getPosition();
            auto const& gp = g_s.track[i];
            const float dx = std::fabs(rp.x - gp.x);
            const float dy = std::fabs(rp.y - gp.y);
            const double dvy = std::fabs(pl->m_player1->m_yVelocity - gp.vy);
            g_s.maxdx = std::max(g_s.maxdx, dx);
            g_s.maxdy = std::max(g_s.maxdy, dy);
            g_s.maxdvy = std::max(g_s.maxdvy, dvy);
            if (g_s.divergeAt < 0 && (dx > fw::ghostEps() || dy > fw::ghostEps()))
                g_s.divergeAt = i;
            FW_VLOG("GHOSTFID.pt seed@{} i={} real=({:.3f},{:.3f}) ghost=({:.3f},{:.3f}) d=({:.3f},{:.3f})",
                g_s.startTick, i, rp.x, rp.y, gp.x, gp.y, dx, dy);
        }
        if (i + 1 >= static_cast<int>(g_s.track.size())) finishSample();
        return;
    }

    // don't seed right next to an input edge -- the hold assumption is weakest there
    if (tick > 60 && tick % kFidPeriod == 0 && tick - g_lastEdgeTick > 4)
        startSample(pl, tick);
}

void fidelityReset() {
    g_s = FidSample{};
    g_lastEdgeTick = -1;
}

void fidelityReport() {
    if (g_nSeeds == 0) return;
    float median = 0.f;
    if (!g_cleanMaxDx.empty()) {
        auto v = g_cleanMaxDx;
        std::sort(v.begin(), v.end());
        median = v[v.size() / 2];
    }
    // PASS threshold comes straight from the plan: a usable order must hold
    // sub-0.05-unit agreement on the clear majority of clean samples.
    int good = 0;
    for (float d : g_cleanMaxDx) if (d < 0.05f) good++;
    const bool pass = !g_cleanMaxDx.empty()
        && good * 100 >= static_cast<int>(g_cleanMaxDx.size()) * 80;
    FW_LOG("PHASE1 order={} seeds={} clean={} aborted={} good={} median_maxdx={:.3f} VERDICT={}",
        fw::ghostOrder(), g_nSeeds, g_nClean, g_nAborted, good, median,
        pass ? "PASS" : "FAIL");
    g_nSeeds = g_nClean = g_nAborted = 0;
    g_cleanMaxDx.clear();
}

} // namespace fw::ghost

// ===========================================================================
// Side-effect suppression.
//
// Every one of these is gated on isSim(), which is true ONLY inside a ghost
// stepping loop -- no real physics ever runs while it is set, so the gates
// cannot affect normal play.
// ===========================================================================
using namespace fw;

class $modify(GhostPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        // must run before the macro layer's own destroyPlayer hook, so a ghost
        // death never reaches the real game
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::VeryEarly);
        (void)self.setHookPriority("PlayLayer::levelComplete", Priority::VeryEarly);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (ghost::owns(player) || ghost::isSim()) {
            ghost::markDead(player);
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete()                               { if (ghost::isSim()) return; PlayLayer::levelComplete(); }
    // NOTE: PlayLayer::incrementJumps is `win inline` in the 2.2081 broma and
    // CANNOT be hooked on Windows (the Mac build compiles it happily, so this
    // only ever shows up in CI). PlayerObject::incrementJumps is hookable and
    // is upstream of it, so gating that one suppresses the same chain.
    CheckpointObject* markCheckpoint()                 { if (ghost::isSim()) return nullptr; return PlayLayer::markCheckpoint(); }
    void playEndAnimationToPos(cocos2d::CCPoint p)     { if (ghost::isSim()) return; PlayLayer::playEndAnimationToPos(p); }
};

class $modify(GhostBGL, GJBaseGameLayer) {
    void playerTouchedRing(PlayerObject* p, RingObject* o)          { if (ghost::isSim()) return; GJBaseGameLayer::playerTouchedRing(p, o); }
    void playerTouchedTrigger(PlayerObject* p, EffectGameObject* o) { if (ghost::isSim()) return; GJBaseGameLayer::playerTouchedTrigger(p, o); }
    void activateSFXTrigger(SFXTriggerGameObject* o)                { if (ghost::isSim()) return; GJBaseGameLayer::activateSFXTrigger(o); }
    void activateSongEditTrigger(SongTriggerGameObject* o)          { if (ghost::isSim()) return; GJBaseGameLayer::activateSongEditTrigger(o); }
    void gameEventTriggered(GJGameEvent e, int m, int id)           { if (ghost::isSim()) return; GJBaseGameLayer::gameEventTriggered(e, m, id); }
    void flipGravity(PlayerObject* p, bool flip, bool noEffects)    { if (ghost::isSim()) return; GJBaseGameLayer::flipGravity(p, flip, noEffects); }

    bool canBeActivatedByPlayer(PlayerObject* p, EffectGameObject* o) {
        if (ghost::isSim()) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(p, o);
    }
};

class $modify(GhostPlayerObject, PlayerObject) {
    void ringJump(RingObject* o, bool skipCheck)                { if (ghost::isSim()) return; PlayerObject::ringJump(o, skipCheck); }
    void playSpiderDashEffect(cocos2d::CCPoint a, cocos2d::CCPoint b) { if (ghost::isSim()) return; PlayerObject::playSpiderDashEffect(a, b); }
    void placeStreakPoint()                                     { if (ghost::isSim()) return; PlayerObject::placeStreakPoint(); }
    void incrementJumps()                                       { if (ghost::isSim()) return; PlayerObject::incrementJumps(); }
};

class $modify(GhostEffectObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* l, int uid, gd::vector<int> const* keys) {
        if (ghost::isSim()) return;
        EffectGameObject::triggerObject(l, uid, keys);
    }
};

class $modify(GhostGameObject, GameObject) {
    void playShineEffect() { if (ghost::isSim()) return; GameObject::playShineEffect(); }
};

class $modify(GhostHardStreak, HardStreak) {
    void addPoint(cocos2d::CCPoint p) { if (ghost::isSim()) return; HardStreak::addPoint(p); }
};
