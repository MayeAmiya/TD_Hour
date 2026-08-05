#pragma once

#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"

#include "game/object/contracts/ObjectFixedGeometryTypes.h"
#include "game/object/definition/LocomotorTemplate.h"
#include "game/navigation/contracts/NavigationPathContracts.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <limits>

namespace engine {

struct TransformComponent {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float rotation = 0.0f;
};

// Legacy Object::isOffMap() is gameplay state, not renderer visibility.
// Absent means the ordinary on-map state; transport/script systems may add
// this sparse fact when they explicitly remove an object from map queries.
struct ObjectMapStatusComponent final {
    bool offMap = false;
};

// Persistent gameplay coordinates must not inherit DirectXMath's float/SIMD
// representation. Systems convert at the Transform/spatial/render boundary,
// but their authoritative state stays in the fixed-point scalar supplied by
// wwmath. This small value type deliberately has no renderer conversion or
// hidden global precision switch: a component that opts into it is visibly
// deterministic in every build configuration.
// Universal simulation-owned transform. Every live object receives this at
// creation after the external/map/script float ingress has been validated and
// quantized exactly once. TransformComponent is only the legacy/render
// projection; simulation code must not read it back as authoritative state.
struct ObjectFixedTransformComponent final {
    LogicFixedVec3 position{};
    math::q32_32 yawRadians{};
    // Production ObjectLifecycle sets this immediately after the one-time
    // ingress quantization. The false default exists only for bare fixtures
    // that assemble components without using the lifecycle boundary.
    bool authoritative = false;
};

struct ObjectPlayerFormationComponent final {
    uint64_t id = 0;
    math::q32_32 offsetX{};
    math::q32_32 offsetY{};
};

// Cold authoritative metadata for one multi-point Move transaction.  System
// exit paths and player Alt waypoints share the same immutable tick-local SoA
// FollowPath projection, while provenance keeps their admission rules apart.
struct ObjectSystemPathSequenceComponent final {
    ObjectMoveRouteSubtype routeSubtype = ObjectMoveRouteSubtype::Direct;
    ObjectOrderSource source = ObjectOrderSource::System;
    ObjectOrderSystemPurpose systemPurpose =
        ObjectOrderSystemPurpose::Generic;
    // Nonzero freezes the resolver revision while points are appended.  This
    // is required by player privateFollowPathAppend: adding a tail point must
    // not invalidate the in-flight segment or reset its current index.
    uint64_t sequenceRevision = 0;
    // Tail appends advance the mutable ECS queue revision, but they must not
    // replace the already admitted FollowPath head: in-flight navigation and
    // movement feedback are correlated with this frozen identity. Zero is
    // reserved for system routes and pre-admission player assembly.
    uint64_t activeQueueRevision = 0;
    uint64_t activeExternalRevision = 0;
    ObjectId ignoredObstacle = INVALID_OBJECT_ID;
    uint64_t issuedTick = 0;
    uint32_t firstSourceSequence = 0;
    uint32_t queuedOrderCount = 0;
    // Current FollowPath goal index, projected from ObjectAI once per
    // confirmed frame. Presentation starts here so completed nodes are not
    // drawn behind the actor as a false return route.
    uint32_t currentPointIndex = 0;
    ::container::Vector<LogicFixedVec3> points;
};

// Complete deterministic attitude used at structural boundaries where the
// legacy Object carried a Matrix3D.  Translation remains LogicFixedVec3;
// keeping rotation as a quaternion avoids losing launch-bone roll/pitch by
// prematurely projecting it into TransformComponent's yaw-only field.
struct LogicFixedQuaternion final {
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    math::q32_32 w{int32_t{1}};
};

// Flight/physics systems own this runtime fact; KindOf alone is not enough
// because a plane parked on an airfield is a ground target while the exact
// same template can be airborne one frame later.  It is deliberately absent
// from ordinary ground objects.  Transitional content that has no flight
// system may still use the explicit parachute/airborne KindOf fallbacks in
// CombatSystem, but never turns every AIRCRAFT template into a vehicle-air
// target by default.
struct ObjectAirborneComponent final {
    bool isAirborne = false;
};

// Sparse confirmed gameplay fact owned by the insertion dispatcher.  The AI
// state emits Set/Clear commands; ModelCondition extraction and physics only
// observe this component and never infer rappel state from altitude.
struct ObjectRappellingComponent final {
    uint64_t startedTick = 0;
};

// Tracks one queue-backed Command_CombatDrop while the pointer-free ObjectAI
// state owns it.  The order remains at the queue head until the state reaches
// its terminal Idle projection, preventing a completed command from being
// re-admitted on the following tick.
struct ObjectCombatDropOrderRuntimeComponent final {
    ai::AIAsyncOrderIdentity orderIdentity;
    uint64_t queueRevision = 0;
    uint64_t externalRevision = 0;
    uint64_t issuedTick = 0;
    uint32_t sourceSequence = 0;
};

// Per-object mutable counterpart to the immutable LocomotorTemplate. The
// current profile is projected into hot scalar fields while profiles retains
// the complete frozen LocomotorSet in authored order. This reproduces the
// source engine's surface-driven profile switch without retaining pointers
// into a reloadable content store.
enum class ObjectLocomotionState : uint8_t {
    Idle,
    Moving,
    Blocked,
};

struct ObjectLocomotionComponent final {
    ::container::Vector<game::FrozenLocomotorTemplate> profiles;
    ::container::String templateName;
    game::LocomotorSurfaceMask surfaces = 0;
    game::LocomotorAppearance appearance = game::LocomotorAppearance::Other;
    game::LocomotorZAxisBehavior zAxisBehavior = game::LocomotorZAxisBehavior::NoZMotiveForce;
    game::LocomotorGroupPriority groupPriority = game::LocomotorGroupPriority::MovesMiddle;
    math::q32_32 circlingRadius{};
    math::q32_32 extra2DFrictionPerSecond{};
    math::q32_32 maximumThrustAngleRadians{};
    math::q32_32 accelerationPitchLimitRadians{};
    math::q32_32 decelerationPitchLimitRadians{};
    math::q32_32 bounceAngularVelocityRadiansPerSecond{};
    math::q32_32 pitchStiffness{};
    math::q32_32 rollStiffness{};
    math::q32_32 pitchDamping{};
    math::q32_32 rollDamping{};
    math::q32_32 thrustRoll{};
    math::q32_32 thrustWobbleRate{};
    math::q32_32 thrustMinimumWobble{};
    math::q32_32 thrustMaximumWobble{};
    math::q32_32 pitchByZVelocityFactor{};
    math::q32_32 forwardVelocityPitchFactor{};
    math::q32_32 lateralVelocityRollFactor{};
    math::q32_32 forwardAccelerationPitchFactor{};
    math::q32_32 lateralAccelerationRollFactor{};
    math::q32_32 uniformAxialDamping{int32_t{1}};
    math::q32_32 turnPivotOffset{};
    math::q32_32 maximumWheelExtension{};
    math::q32_32 maximumWheelCompression{};
    math::q32_32 frontWheelTurnAngleRadians{};
    math::q32_32 wanderWidthFactor{};
    math::q32_32 wanderLengthFactor{int32_t{1}};
    math::q32_32 wanderAboutPointRadius{};
    math::q32_32 rudderCorrectionDegree{};
    math::q32_32 rudderCorrectionRate{};
    math::q32_32 elevatorCorrectionDegree{};
    math::q32_32 elevatorCorrectionRate{};
    math::q32_32 airborneTargetingHeight{};
    bool hasFiniteAirborneTargetingHeight = false;
    bool closeEnoughDistance3D = false;
    bool stickToGround = false;
    // DeliverPayload's dive temporarily asks locomotion to honor the exact
    // authored Z destination.  Normal ground/hover movement continues to
    // project onto the terrain when this latch is clear.
    bool usePreciseZPosition = false;
    // Locomotor::ULTRA_ACCURATE is transient controller state, not template
    // data. It doubles turn authority and enables precise slide/height
    // convergence for repair drones and other explicitly authored users.
    bool ultraAccurate = false;
    bool canMoveBackwards = false;
    bool locomotorWorksWhenDead = false;
    bool allowMotiveForceWhileAirborne = false;
    bool apply2DFrictionWhenAirborne = false;
    bool downhillOnly = false;
    bool hasSuspension = false;
    bool overWater = false;

    // Quantized immutable/session runtime parameters. These are populated
    // whenever a Locomotor profile is selected; logic frames consume only
    // these values and never reparse the float template projection.
    math::q32_32 maximumSpeed{};
    math::q32_32 damagedMaximumSpeed{};
    math::q32_32 maximumTurnRate{};
    math::q32_32 damagedMaximumTurnRate{};
    math::q32_32 acceleration{};
    math::q32_32 damagedAcceleration{};
    math::q32_32 lift{};
    math::q32_32 damagedLift{};
    math::q32_32 braking{};
    math::q32_32 minimumSpeed{};
    math::q32_32 minimumTurnSpeed{};
    math::q32_32 preferredHeightFixed{};
    math::q32_32 preferredHeightDampingFixed{int32_t{1}};
    math::q32_32 speedLimitZ{};
    math::q32_32 closeEnough{};
    math::q32_32 slideIntoPlace{};
    bool accelerationIsInfinite = false;
    bool damagedAccelerationIsInfinite = false;
    bool brakingIsInfinite = false;
    bool hasFiniteBraking = true;
    bool hasFiniteSpeedLimitZ = false;
    bool preferredHeightIsLowest = false;

    // Authoritative runtime state. Presentation converts these values only
    // while extracting a render frame.
    math::q32_32 forwardSpeed{};
    math::q32_32 verticalSpeed{};
    math::q32_32 groundOffsetFixed{};
    LogicFixedVec3 goal{};
    bool fixedRuntimeInitialized = false;
    uint64_t activeOrderTick = 0;
    uint32_t activeOrderSequence = 0;
    uint32_t activeSourceScriptId = 0;
    bool hasActiveMove = false;
    bool movingBackward = false;
    ObjectLocomotionState state = ObjectLocomotionState::Idle;
};

// Movement-owned continuation for an AI PathHandle. AI states retain only
// value correlation and never write Transform/locomotion directly; this ECS
// sidecar lets the existing movement owner continue one immutable path across
// ticks and therefore participates in authoritative world snapshots.
struct ObjectAIPathMovementComponent final {
    ai::PathCorrelation correlation;
    ai::PathHandle path;
    ObjectId ignoredObstacle = INVALID_OBJECT_ID;
    uint64_t pathRevision = 0;
    uint32_t nextPointIndex = 0;
    uint32_t blockedTicks = 0;
    int64_t alongPathDistanceRaw = 0;
    int64_t speedLimitRaw = 0;
    int64_t extraDistanceRaw = 0;
    ai::AIMovementMode mode = ai::AIMovementMode::Normal;
    bool panicking = false;
    bool allowPathThroughUnits = false;
};

// Pair-stable deadlock ledger for two already-moving allies. It overlays the
// existing parent path/order, so no AI state or payload is replaced.
struct ObjectAIMovementObstructionStateComponent final {
    ObjectId blocker = INVALID_OBJECT_ID;
    ObjectId previousBlocker = INVALID_OBJECT_ID;
    uint64_t lastContactTick = 0;
    uint32_t consecutiveTicks = 0;
};

// Frozen SET_NORMAL_UPGRADED candidate for LocomotorSetUpgrade.  The source
// AIUpdate destroys its current Locomotor instances and creates the upgraded
// set from immutable templates.  Keeping the selected session-snapshot value
// beside the entity reproduces that rare transition without consulting the
// reloadable LocomotorStore or retaining a content pointer during a match.
//
struct ObjectLocomotorSetUpgradeComponent final {
    ::container::Vector<game::FrozenLocomotorTemplate> upgraded;
    uint64_t revision = 0;
    bool active = false;
};

// Typed, fixed-point projection of RefCode PhysicsBehavior.  This is not a
// replacement for ground locomotion: the Stage-1 physics pass owns only free
// rigid bodies, while existing Stage-0 locomotors remain explicit transform
// writers until they are migrated to force/intent output.
enum class ObjectPhysicsMotionState : uint8_t {
    Grounded,
    Airborne,
    Bounced,
};

struct ObjectPhysicsComponent final {
    using Scalar = math::q32_32;

    // Authoritative free-body state. TransformComponent is a float projection
    // for legacy consumers and rendering, never the integration accumulator.
    LogicFixedVec3 position{};
    LogicFixedVec3 lastPublishedPosition{};
    // Frozen before any confirmed-tick physics synchronization/integration.
    // Collision broad/narrow phases consume this together with the final pose
    // so fast movers cannot tunnel through a contact in one logic frame.
    LogicFixedVec3 collisionStartPosition{};
    // The same confirmed-tick freeze for angular CCD.  `lastPublishedYaw`
    // is the previous authoritative projection even when Movement or a
    // projectile controller has already written this tick's final pose.
    Scalar collisionStartYaw{};
    Scalar lastPublishedYaw{};
    uint64_t collisionStartTick = 0;
    LogicFixedVec3 velocityUnitsPerSecond{};
    // ObjectSimulation's ordered ObjectPhysicsRequest ingress accumulates
    // force here. Physics divides it by mass once during the next confirmed
    // tick and publishes the resulting acceleration as a value, so gameplay
    // code never needs a legacy PhysicsBehavior pointer.
    LogicFixedVec3 pendingForce{};
    LogicFixedVec3 previousAcceleration{};

    Scalar mass{int32_t{1}};
    // RefCode's unauthored defaults are per 30 Hz logic frame. Store their
    // per-second equivalents here so direct typed construction has the same
    // semantics as the immutable recipe projection.
    Scalar forwardFrictionPerSecond{4.5f};
    Scalar lateralFrictionPerSecond{4.5f};
    Scalar zFrictionPerSecond{24.0f};
    Scalar aerodynamicFrictionPerSecond{};
    Scalar shockResistance{};
    Scalar shockMaxYaw{0.05f};
    Scalar shockMaxPitch{0.025f};
    Scalar shockMaxRoll{0.025f};
    Scalar centerOfMassOffset{};
    Scalar minFallSpeedUnitsPerSecond{};
    Scalar fallHeightDamageFactor{int32_t{1}};
    Scalar pitchRollYawFactor{int32_t{2}};
    Scalar yaw{};
    Scalar pitch{};
    Scalar roll{};
    // Authoritative Z-up local axes.  PhysicsBehavior post-multiplies its
    // existing Matrix3D by local X/Y/Z rotations; retaining this basis avoids
    // the information loss caused by rebuilding a matrix from Euler angles
    // every frame.  The Euler values above remain compatibility projections
    // for effects and legacy boundaries.
    LogicFixedVec3 orientationX{
        Scalar{int32_t{1}}, Scalar{}, Scalar{}};
    LogicFixedVec3 orientationY{
        Scalar{}, Scalar{int32_t{1}}, Scalar{}};
    LogicFixedVec3 orientationZ{
        Scalar{}, Scalar{}, Scalar{int32_t{1}}};
    Scalar orientationProjectionYaw{};
    Scalar orientationProjectionPitch{};
    Scalar orientationProjectionRoll{};
    Scalar yawRate{};
    Scalar pitchRate{};
    Scalar rollRate{};
    // Chassis suspension spring for a grounded ground locomotor.  RefCode
    // computes this on the client (Drawable::calcPhysicsXformTreads /
    // calcPhysicsXformWheels / calcPhysicsXformMotorcycle, which spring
    // pitch/roll towards the horizontal projection of the terrain normal).
    // This port keeps the whole attitude in Q32.32 simulation state instead,
    // so the spring accumulators live beside the Euler projection they drive
    // and remain identical on every peer.
    //
    // These are deliberately separate from pitchRate/rollRate above: those
    // are per-second free-body tumble rates consumed by
    // integratePhysicsOrientation, while the values below are the authored
    // per-logic-frame quantities RefCode's Locomotor coefficients expect.
    Scalar chassisPitch{};
    Scalar chassisPitchRate{};
    Scalar chassisRoll{};
    Scalar chassisRollRate{};
    // Separate recoil/acceleration term, clamped by the authored
    // Acceleration/DecelerationPitchLimit exactly like RefCode.  Lateral
    // acceleration is not published for locomotion-owned objects, so only the
    // forward (pitch) axis is driven.
    Scalar chassisAccelerationPitch{};
    Scalar chassisAccelerationPitchRate{};
    Scalar chassisPreviousForwardSpeed{};

    ::container::String crashIntoBuildingWeapon;
    ::container::String crashIntoNonBuildingWeapon;
    game::WeaponContentId crashIntoBuildingWeaponContent;
    game::WeaponContentId crashIntoNonBuildingWeaponContent;
    // RefCode retains one stable ObjectID for temporary collision immunity
    // (notably a missile and its launcher) plus the latest real contact.
    ObjectId ignoreCollisionWith = INVALID_OBJECT_ID;
    ObjectId lastCollidee = INVALID_OBJECT_ID;
    ObjectId currentOverlap = INVALID_OBJECT_ID;
    ObjectId previousOverlap = INVALID_OBJECT_ID;
    uint64_t overlapLedgerTick = 0;
    uint64_t motiveForceExpiresTick = 0;
    bool stunned = false;
    bool inFreeFall = false;
    bool sleeping = false;
    bool physicsEverUpdated = false;
    ObjectPhysicsMotionState state = ObjectPhysicsMotionState::Grounded;
    bool allowBouncing = false;
    bool originalAllowBouncing = false;
    bool allowCollideForce = true;
    bool killWhenRestingOnGround = false;
    bool stickToGround = false;
    bool allowToFall = false;
    bool applyFriction2DWhenAirborne = false;
    // Explicit handoff from a retained locomotor recipe to PhysicsBehavior's
    // free-body lane. SlowDeath fling is the first producer: RefCode keeps
    // AI/Locomotor modules installed while Physics owns the dead object's XYZ.
    bool forceFreeBodyTranslation = false;
    // Ground locomotion and ordinary DumbBezier flight continue to own their
    // legacy Transform yaw.  Set this only when the Physics pass owns a
    // complete 3D attitude (free-body rotation or an explicit tumble), so
    // render extraction never replaces a live locomotor/path yaw with the
    // spawn-time Physics snapshot.
    bool ownsAttitude = false;
    // Narrow counterpart of ownsAttitude for terrain-conforming ground
    // vehicles.  Locomotion still owns XYZ and yaw; Physics owns only the
    // pitch/roll pair produced by the chassis spring above.  Render
    // extraction needs this exact distinction: widening ownsAttitude here
    // would let the Physics snapshot replace a live locomotor yaw.
    bool conformsToTerrain = false;
    bool orientationBasisValid = false;
    bool hasAuthoritativePosition = false;
    bool wasAirborneLastFrame = false;
};

// Durable named-script override consumed when either StructureToppleUpdate
// or the generic ToppleUpdate chooses its direction.  It does not activate a
// topple by itself; it replaces only the later producer-selected vector.
struct ObjectScriptToppleDirectionComponent final {
    LogicFixedVec3 direction{};
    uint64_t revision = 0;
};

// Gameplay-owned presentation intent used by any effect which submerges or
// otherwise removes an object's shadow.  Render extraction only observes the
// fact and never exposes a renderer handle back to simulation.
struct ObjectShadowSuppressionComponent final {
    uint64_t confirmedTick = 0;
};

} // namespace engine
