#pragma once

// ===========================================================================
// Practice fix: GD's practice checkpoints don't bring the player back exactly
// as they were. PlayerCheckpoint stores a subset of PlayerObject, and the
// position only as a float (the game runs on double m_positionX/Y). After a
// respawn the run continues from a slightly different state than the one a
// straight replay reaches at that point, so a macro recorded in practice mode
// drifts at every checkpoint -- worst for precise clicks right after one.
//
// PlayerSnap copies a player's physics state (every plain field of
// PlayerObject in the 2.2081 bindings, plus the level-object pointers GD
// itself keeps, minus looks, identity and GD's own checkpoint-placement
// timers) and writes it back after GD's own restore. Held buttons are part of
// the state; the caller decides what to do about them (see macro.cpp).
// ===========================================================================

#include <Geode/Geode.hpp>

namespace fw::snap {

// clang-format off
#define FW_PLAYER_FIELDS(X) \
    X(m_wasTeleported) X(m_fixGravityBug) X(m_reverseSync) X(m_yVelocityBeforeSlope) X(m_dashX) \
    X(m_dashY) X(m_dashAngle) X(m_dashStartTime) X(m_dashRing) X(m_slopeStartTime) \
    X(m_maybeLastGroundObject) X(m_lastCollisionBottom) X(m_lastCollisionTop) X(m_lastCollisionLeft) \
    X(m_lastCollisionRight) X(m_unk50C) X(m_unk510) X(m_currentSlope2) X(m_preLastGroundObject) \
    X(m_slopeAngle) X(m_slopeSlidingMaybeRotated) X(m_quickCheckpointMode) X(m_collidedObject) \
    X(m_lastGroundObject) X(m_collidingWithLeft) X(m_collidingWithRight) X(m_maybeSavedPlayerFrame) \
    X(m_scaleXRelated2) X(m_groundYVelocity) X(m_yVelocityRelated) X(m_scaleXRelated3) \
    X(m_scaleXRelated4) X(m_scaleXRelated5) X(m_isCollidingWithSlope) X(m_isBallRotating) \
    X(m_unk669) X(m_currentPotentialSlope) X(m_currentSlope) X(m_collidingWithSlopeId) \
    X(m_slopeFlipGravityRelated) X(m_slopeAngleRadians) X(m_rotateObjectsRelated) \
    X(m_potentialSlopeMap) X(m_rotationSpeed) X(m_rotateSpeed) X(m_isRotating) X(m_isBallRotating2) \
    X(m_isHidden) X(m_speedMultiplier) X(m_yStart) X(m_gravity) X(m_unk648) X(m_gameModeChangedTime) \
    X(m_padRingRelated) X(m_maybeIsFalling) X(m_maybeCanRunIntoBlocks) X(m_isOnGround3) \
    X(m_lastJumpTime) X(m_lastFlipTime) X(m_lastSpiderFlipTime) X(m_unkBool5) \
    X(m_accelerationOrSpeed) X(m_snapDistance) X(m_ringJumpRelated) X(m_ringRelatedSet) \
    X(m_objectSnappedTo) X(m_maybeSpriteRelated) X(m_slopeRotation) X(m_currentSlopeYVelocity) \
    X(m_unk3d0) X(m_blackOrbRelated) X(m_unk3e0) X(m_unk3e1) X(m_isAccelerating) \
    X(m_isCurrentSlopeTop) X(m_collidedTopMinY) X(m_collidedBottomMaxY) X(m_collidedLeftMaxX) \
    X(m_collidedRightMinX) X(m_maybeIsColliding) X(m_jumpBuffered) X(m_stateRingJump) \
    X(m_wasJumpBuffered) X(m_wasRobotJump) X(m_stateJumpBuffered) X(m_stateRingJump2) \
    X(m_touchedRing) X(m_touchedCustomRing) X(m_touchedGravityPortal) \
    X(m_maybeTouchedBreakableBlock) X(m_touchedPad) X(m_yVelocity) X(m_fallSpeed) X(m_isOnSlope) \
    X(m_wasOnSlope) X(m_slopeVelocity) X(m_maybeUpsideDownSlope) X(m_isShip) X(m_isBird) X(m_isBall) \
    X(m_isDart) X(m_isRobot) X(m_isSpider) X(m_isUpsideDown) X(m_isOnGround) X(m_isGoingLeft) \
    X(m_isSideways) X(m_isSwing) X(m_reverseRelated) X(m_maybeReverseSpeed) \
    X(m_maybeReverseAcceleration) X(m_xVelocityRelated2) X(m_isDashing) X(m_groundObjectMaterial) \
    X(m_vehicleSize) X(m_playerSpeed) X(m_shipRotation) X(m_lastPortalPos) X(m_unkUnused3) \
    X(m_isOnGround2) X(m_lastLandTime) X(m_platformerVelocityRelated) X(m_maybeIsBoosted) \
    X(m_scaleXRelatedTime) X(m_decreaseBoostSlide) X(m_unkA29) X(m_isLocked) X(m_controlsDisabled) \
    X(m_lastGroundedPos) X(m_touchedRings) X(m_lastActivatedPortal) X(m_hasEverJumped) \
    X(m_hasEverHitRing) X(m_position) X(m_unkA99) X(m_totalTime) X(m_isBeingSpawnedByDualPortal) \
    X(m_unkAngle1) X(m_yVelocityRelated3) X(m_followRelated) X(m_playerFollowFloats) X(m_unk838) \
    X(m_stateOnGround) X(m_stateUnk) X(m_stateNoStickX) X(m_stateNoStickY) X(m_stateUnk2) \
    X(m_stateBoostX) X(m_stateBoostY) X(m_maybeStateForce2) X(m_stateScale) X(m_platformerXVelocity) \
    X(m_holdingRight) X(m_holdingLeft) X(m_leftPressedFirst) X(m_scaleXRelated) X(m_maybeHasStopped) \
    X(m_xVelocityRelated) X(m_maybeGoingCorrectSlopeDirection) X(m_isSliding) X(m_maybeSlopeForce) \
    X(m_isOnIce) X(m_physDeltaRelated) X(m_isOnGround4) X(m_maybeSlidingTime) \
    X(m_maybeSlidingStartTime) X(m_changedDirectionsTime) X(m_slopeEndTime) X(m_isMoving) \
    X(m_platformerMovingLeft) X(m_platformerMovingRight) X(m_isSlidingRight) \
    X(m_maybeChangedDirectionAngle) X(m_unkUnused2) X(m_stateNoAutoJump) X(m_stateDartSlide) \
    X(m_stateHitHead) X(m_stateFlipGravity) X(m_gravityMod) X(m_stateForce) X(m_stateForceVector) \
    X(m_affectedByForces) X(m_jumpPadRelated) X(m_lastMovedTime) X(m_playerSpeedAC) \
    X(m_fixRobotJump) X(m_holdingButtons) X(m_gv0123) X(m_unkUnused) X(m_isOutOfBounds) \
    X(m_fallStartY) X(m_disablePlayerSqueeze) X(m_ignoreDamage) X(m_enable22Changes) \
    X(m_enableImpulseFix)
// clang-format on

struct PlayerSnap {
#define FW_SNAP_DECL(f) decltype(PlayerObject::f) f{};
    FW_PLAYER_FIELDS(FW_SNAP_DECL)
#undef FW_SNAP_DECL
    // GameObject side: the node position and rotation, the exact (double)
    // position the physics runs on, and the previous step's position
    cocos2d::CCPoint nodePos;
    float rotation = 0.f;
    double positionX = 0.0, positionY = 0.0;
    cocos2d::CCPoint lastPosition;
};

inline PlayerSnap take(PlayerObject* p) {
    PlayerSnap s;
#define FW_SNAP_SAVE(f) s.f = p->f;
    FW_PLAYER_FIELDS(FW_SNAP_SAVE)
#undef FW_SNAP_SAVE
    s.nodePos = p->getPosition();
    s.rotation = p->getRotation();
    s.positionX = p->m_positionX;
    s.positionY = p->m_positionY;
    s.lastPosition = p->m_lastPosition;
    return s;
}

inline void apply(PlayerObject* p, PlayerSnap const& s) {
#define FW_SNAP_LOAD(f) p->f = s.f;
    FW_PLAYER_FIELDS(FW_SNAP_LOAD)
#undef FW_SNAP_LOAD
    p->setPosition(s.nodePos);
    p->setRotation(s.rotation);
    // after setPosition, which sets them from the float position
    p->m_positionX = s.positionX;
    p->m_positionY = s.positionY;
    p->m_lastPosition = s.lastPosition;
}

// is this button held on this player (1 = jump, 2 = left, 3 = right)
inline bool held(PlayerObject* p, int button) {
    if (!p) return false;
    auto it = p->m_holdingButtons.find(button);
    bool h = it != p->m_holdingButtons.end() && it->second;
    if (button == 2) h = h || p->m_holdingLeft;
    if (button == 3) h = h || p->m_holdingRight;
    return h;
}

} // namespace fw::snap
