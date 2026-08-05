#pragma once

#include "game/object/component/ObjectComponentsMovementPhysics.h"

#include "game/data/base/PhysicsSimulationRules.h"
#include "game/base/DamageTypes.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <optional>

namespace engine {

// Every WeaponSet slot owns only mutable cooldown/clip state plus a compact
// stable handle into the session's immutable content snapshot.  In
// particular, a unit does not deep-copy strings/FX payloads from a
// WeaponTemplate for every WeaponSet slot, and no confirmed-frame code ever
// follows a pointer into the reloadable global WeaponStore.
struct ObjectWeaponSlotRuntime final {
    game::WeaponContentId content;
    // Zero means an empty finite clip. A weapon with ClipSize == 0 is
    // clipless/infinite and leaves this field unused.
    uint32_t ammoInClip = 0;
    uint64_t nextReadyTick = 0;
    uint64_t reloadCompleteTick = 0;
    uint64_t preAttackCompleteTick = 0;
    uint64_t lastFireTick = 0;
    // RefCode Weapon::m_suspendFXFrame. This is presentation policy state,
    // but remains authoritative because Weapon construction/WeaponSet changes
    // establish the exact confirmed-tick boundary observed by fire events.
    uint64_t suspendFxUntilTick = 0;
    // Zero means this slot has never fired. FireOCLAfterWeaponCooldownUpdate
    // uses the sequence together with lastFireTick so logic tick zero cannot
    // be mistaken for an authored shot.
    uint32_t lastFireSequence = 0;
    // Explicit RefCode Weapon::m_curBarrel lane. Each WeaponSet/slot owns its
    // own state, so returning to a set resumes the exact barrel and cadence.
    uint32_t currentBarrel = 0;
    uint32_t shotsRemainingForCurrentBarrel = 0;
    // Indices into WeaponTemplate::scatterTargets which have not yet been
    // consumed by this clip. RefCode samples without replacement and rebuilds
    // the set only when the private Weapon reloads.
    ::container::Vector<uint32_t> scatterTargetsUnused;
    // Combat runs before ObjectSimulation in the modern frame. Preserve the
    // immediately preceding shot so a weapon firing again this frame cannot
    // overwrite the `now-1` observation used by the legacy cooldown module.
    uint64_t previousFireTick = 0;
    uint32_t previousFireSequence = 0;
    uint64_t preAttackOrderTick = 0;
    uint32_t preAttackOrderSequence = 0;
    uint32_t preAttackSourceScriptId = 0;
    ObjectId preAttackTarget = INVALID_OBJECT_ID;
    uint32_t clipGeneration = 1;
    uint32_t preAttackClipGeneration = 0;
    bool preAttackArmed = false;
    // Only the slot that initiated an automatic reload refills its clip at
    // completion. Other slots may be held by a shared WeaponSet reload gate
    // without having their own authored ammo rewritten.
    bool reloadReplenishesClip = false;
    // RefCode Weapon::m_leechRangeActive. A successful shot from a
    // LeechRangeWeapon keeps only this private slot in range for the current
    // attack order; a new order clears the latch.
    bool leechRangeActive = false;
};

struct ObjectWeaponSetRuntime final {
    game::WeaponSetConditionMask conditions = 0;
    ::container::Array<ObjectWeaponSlotRuntime, game::kWeaponSlotCount> slots;
    bool shareWeaponReloadTime = false;
    bool weaponLockSharedAcrossSets = false;
    // RefCode marks every slot RELOADING_CLIP when one slot exhausts a shared
    // set. Keep that as a set-level gate instead of falsifying siblings' clip
    // counts or leaving a per-slot reload marker permanently armed.
    uint64_t sharedReloadCompleteTick = 0;
};

enum class ObjectWeaponRuntimeState : uint8_t {
    Idle,
    TrackingTarget,
    OutOfRange,
    WindingUp,
    CoolingDown,
    Reloading,
    NoUsableWeapon,
};

enum class ObjectContinuousFireStage : uint8_t {
    None,
    Mean,
    Fast,
    Slow,
};

// Mirrors WeaponLockType without carrying the legacy WeaponSet object. A
// temporary special-attack lock may be released by ordinary order cleanup;
// a permanent LockWeaponCreate lock survives until an explicit full release
// or a non-sharing WeaponSet transition.
enum class ObjectWeaponLockType : uint8_t {
    None,
    Temporary,
    Permanent,
};

enum class ObjectTurretIdlePhase : uint8_t {
    Waiting,
    Scanning,
    Holding,
    Recentering,
};

// Confirmed logic-owned turret pose.  RefCode asks AIUpdate which turret owns
// a WeaponSlot and uses that turret's current yaw/pitch both for the visible
// bones and for projectile launch.  Keeping the pose here prevents render
// extraction from independently re-aiming a barrel after the shot was
// authored.
struct ObjectTurretRuntime final {
    math::q32_32 yawRadians{};
    math::q32_32 pitchRadians{};
    // TurretAIData defaults both rates to 0.01 radians per legacy 30 Hz
    // logic frame.  Runtime rates are stored per second so a non-legacy
    // fixed-step rate preserves the same authored angular velocity.
    math::q32_32 turnRateRadiansPerSecond =
        math::q32_32::from_fraction(3, 10);
    math::q32_32 pitchRateRadiansPerSecond =
        math::q32_32::from_fraction(3, 10);
    // RefCode defaults MinPhysicalPitch to zero.  A negative depression
    // angle exists only when the Turret/AltTurret recipe authors one.
    math::q32_32 minimumPitchRadians{};
    math::q32_32 firePitchRadians{};
    math::q32_32 groundUnitPitchRadians{};
    math::q32_32 naturalYawRadians{};
    math::q32_32 naturalPitchRadians{};
    // Per-WeaponSlot firing sweep, frozen from TurretAI. A recent shot keeps
    // sweep enabled for the same three confirmed frames as RefCode.
    container::Array<math::q32_32, 3> fireAngleSweepRadians{};
    container::Array<math::q32_32, 3> sweepSpeedModifier{
        math::q32_32{int32_t{1}}, math::q32_32{int32_t{1}},
        math::q32_32{int32_t{1}}};
    math::q32_32 minimumIdleScanAngleRadians{};
    math::q32_32 maximumIdleScanAngleRadians{};
    math::q32_32 idleScanTargetYawRadians{};
    uint64_t recenterAtTick = 0;
    uint64_t nextIdleScanTick = 0;
    uint64_t sweepEnabledUntilTick = 0;
    uint64_t continuousFireSoundUntilTick = 0;
    uint32_t recenterMilliseconds = 2000;
    // The legacy no-field default is 9,999,999 logic frames. At ZH's 30 Hz
    // this duration is 333,333,300 ms; angle zero still short-circuits scans.
    uint32_t minimumIdleScanIntervalMilliseconds = 333333300u;
    uint32_t maximumIdleScanIntervalMilliseconds = 333333300u;
    uint8_t controlledWeaponSlots = 0;
    ObjectTurretIdlePhase idlePhase = ObjectTurretIdlePhase::Waiting;
    bool positiveSweep = true;
    // BattlePlanUpdate can disable the Strategy Center turret independently
    // of the WeaponSet. A forced recenter remains allowed while disabled,
    // matching TurretAI::updateTurretAI's RECENTER exception.
    bool enabled = true;
    bool forcedRecentering = false;
    bool allowsPitch = false;
    bool firesWhileTurning = false;
    // TurretAI::friend_turnTowardsAngle owns MODELCONDITION_TURRET_ROTATE (and
    // m_playRotSound): it clears both when the remaining angle is small enough
    // to snap and raises them on every stepping frame. RefCode keeps that bit
    // on the Object, which makes a second TurretAI overwrite the first; keep it
    // per turret here and let the condition authority union the turrets. The
    // value is deliberately sticky when no turret state runs this tick, exactly
    // as the original object flag was.
    bool rotating = false;
};

// The object-level combat controller is deliberately a compact state machine
// rather than a legacy Weapon/WeaponSet virtual hierarchy.  Attack intents
// remain in ObjectOrderQueueComponent; this component resolves the active
// immutable WeaponSet and records only state that changes during simulation.
struct ObjectWeaponComponent final {
    ::container::Vector<ObjectWeaponSetRuntime> sets;
    ::container::Array<ObjectTurretRuntime, 2> turrets;
    bool turretsLinked = false;
    ObjectId target = INVALID_OBJECT_ID;
    // JetAIUpdate::addTargeter bookkeeping. Combat reconciles this edge on
    // Aim enter/exit so the target jet can own one shared lock-on deadline
    // across all current attackers without retaining attacker pointers.
    ObjectId jetLockonTarget = INVALID_OBJECT_ID;
    uint64_t activeOrderTick = 0;
    uint32_t activeOrderSequence = 0;
    uint32_t activeSourceScriptId = 0;
    uint32_t nextShotSequence = 1;
    // WeaponSet::updateWeaponSet recreates the three legacy Weapon instances.
    // Every activation therefore receives a new stream-owner generation so
    // a ProjectileStream cannot reconnect across a set switch.
    uint32_t weaponSetGeneration = 0;
    std::optional<uint32_t> activeWeaponSetIndex;
    // WeaponSet::m_curWeapon equivalent. It persists while idle and is
    // observably separate from the optional lock which selected it.
    std::optional<game::WeaponSlot> currentSlot = game::WeaponSlot::Primary;
    std::optional<game::WeaponSlot> lockedSlot;
    ObjectWeaponLockType lockType = ObjectWeaponLockType::None;
    ObjectWeaponRuntimeState state = ObjectWeaponRuntimeState::Idle;
    // One FiringTracker belongs to the Object, not to an individual Weapon
    // instance. All deadlines are confirmed logic ticks; presentation owns
    // the eventual audio handle.
    uint32_t consecutiveShots = 0;
    ObjectId consecutiveShotVictim = INVALID_OBJECT_ID;
    uint64_t continuousFireCooldownTick = 0;
    uint64_t forceReloadTick = 0;
    uint64_t loopingFireSoundStopTick = 0;
    game::WeaponContentId loopingFireSoundWeapon;
    ObjectContinuousFireStage continuousFireStage =
        ObjectContinuousFireStage::None;
};

// ProjectileObject is a normal ECS object with its own ObjectId, owner,
// KindOf, geometry and render model. This component adds only the mutable
// flight/detonation state; it never stores a legacy Weapon* or Object*.
enum class ObjectProjectileMotion : uint8_t {
    DumbBezier,
    MissileAI,
    NeutronMissile,
};

enum class ObjectMissileProjectileState : uint8_t {
    Launch,
    AttackNoTurn,
    Attack,
    Locked,
    KillSelf,
};

// MissileAIUpdate remains a distinct typed controller. It deliberately does
// not reuse DumbProjectile's cubic path fields: ignition/arming, fuel,
// no-turn distance, lock cheating and target following have different state
// transitions and collision admission in RefCode.
struct ObjectMissileProjectileComponent final {
    using Scalar = math::q32_32;

    LogicFixedVec3 velocityUnitsPerSecond{};
    LogicFixedVec3 forward{Scalar{int32_t{1}}, {}, {}};
    LogicFixedVec3 originalTarget{};
    ::container::String ignitionFx;
    ::container::String exhaustParticleSystem;
    Scalar currentSpeedUnitsPerSecond{};
    Scalar maximumSpeedUnitsPerSecond{};
    Scalar accelerationUnitsPerSecondSq{};
    Scalar maximumTurnRateRadiansPerSecond{};
    Scalar preferredHeight{};
    Scalar gravityUnitsPerSecondSq{
        PhysicsSimulationRules::kDefaultGravityUnitsPerSecondSq};
    Scalar aerodynamicFrictionPerSecond{};
    Scalar noTurnDistanceRemaining{};
    Scalar diveDistance{};
    Scalar lockDistance{};
    Scalar distanceScatterWhenJammed{};
    uint64_t ignitionTick = 0;
    uint64_t fuelExpiryTick = UINT64_MAX;
    uint64_t killSelfTick = 0;
    ObjectId countermeasureVictim = INVALID_OBJECT_ID;
    uint64_t countermeasureDiversionTick = 0;
    uint32_t countermeasureRuleIndex = UINT32_MAX;
    uint32_t killSelfDelayFrames = 3;
    ObjectMissileProjectileState state = ObjectMissileProjectileState::Launch;
    bool tryToFollowTarget = true;
    bool useWeaponSpeed = false;
    bool detonateOnNoFuel = false;
    bool armed = false;
    bool trackingTarget = false;
    bool hasLocomotor = false;
    bool fuelExpired = false;
    bool jammed = false;
    bool jamPending = false;
    bool countermeasureDiversionPending = false;
    bool suppressDetonationDamage = false;
    bool divertedToCountermeasure = false;
};

// NeutronMissileUpdate is the authored strategic-launch flight controller,
// not a homing MissileAI variant. RelativeSpeed/ForwardDamping are legacy
// per-logic-frame values and therefore advance once per confirmed tick.
struct ObjectNeutronMissileProjectileComponent final {
    using Scalar = math::q32_32;

    LogicFixedVec3 velocityPerFrame{};
    LogicFixedVec3 forward{Scalar{int32_t{1}}, {}, {}};
    LogicFixedVec3 target{};
    LogicFixedVec3 intermediateTarget{};
    ::container::String launchFx;
    ::container::String ignitionFx;
    ::container::String exhaustParticleSystem;
    ::container::String deliveryDecalTexture;
    Scalar deliveryDecalRadius{};
    Scalar deliveryDecalMinimumOpacity{int32_t{1}};
    Scalar deliveryDecalMaximumOpacity{int32_t{1}};
    ::container::Array<uint8_t, 4> deliveryDecalColor{0, 0, 0, 0};
    uint32_t deliveryDecalShadowTypeMask = 0x20u;
    uint32_t deliveryDecalAuthoredOrder = 0;
    uint64_t deliveryDecalOpacityThrobFrames = 30;
    bool deliveryDecalUsesPlayerColor = true;
    bool deliveryDecalOnlyVisibleToOwningPlayer = true;
    bool deliveryDecalActive = false;
    // Neutron SpecialJitterDistance is an instance-matrix-only displacement;
    // gameplay position/collision never observes it.
    LogicFixedVec3 presentationOffset{};
    Scalar noTurnDistanceRemaining{};
    Scalar maximumTurnRateRadiansPerFrame{};
    Scalar forwardDamping{};
    Scalar relativeSpeedPerFrame{int32_t{1}};
    Scalar specialSpeedHeight{};
    Scalar specialAccelerationFactor{int32_t{1}};
    Scalar specialJitterDistance{};
    Scalar launchHeight{};
    uint64_t launchTick = 0;
    uint32_t specialSpeedFrames = 0;
    bool reachedIntermediateTarget = true;
    bool armed = false;
};

// Weapon::fireWeaponTemplate permits ProjectileObject templates without a
// ProjectileUpdateInterface (Firestorm/EMP/Neutron helper style). The object
// is placed directly at the frozen destination and its own typed updates own
// the rest of its lifetime. This marker preserves launch attribution without
// making the helper participate in flight collision/detonation.
struct ObjectPlacedProjectileHelperComponent final {
    ObjectId launcher = INVALID_OBJECT_ID;
    ObjectId intendedTarget = INVALID_OBJECT_ID;
    game::WeaponContentId sourceWeapon;
    uint32_t sourceShotSequence = 0;
    uint64_t spawnedTick = 0;
};

// MissileAI-owned route metadata for the one legacy script action that gives
// a freshly fired projectile an AI waypoint path. ObjectAI never observes
// this component and therefore cannot compete for Transform ownership.
struct ObjectProjectileWaypointPathComponent final {
    uint32_t currentWaypointId = UINT32_MAX;
    uint64_t graphRevision = 0;
    uint32_t hopGeneration = 0;
};

struct ObjectProjectileComponent final {
    using Scalar = math::q32_32;

    ObjectId launcher = INVALID_OBJECT_ID;
    uint32_t sourcePathfindLayer = 0;
    ObjectId intendedTarget = INVALID_OBJECT_ID;
    game::WeaponContentId detonationWeapon;
    // Captured once at launch. Detonation ORs this with the projectile
    // object's own current conditions, so a launcher losing an upgrade while
    // the shell is in flight cannot retroactively change its warhead.
    game::WeaponBonusConditionMask launcherWeaponBonusConditions = 0;
    uint32_t projectileStreamOwnerGeneration = 0;
    // Frozen at fire admission. Render extraction must not infer this from a
    // later WeaponSet selection because the launcher may switch sets/slots
    // while the projectile is still alive.
    uint8_t launchSlot = 0;
    // Monotonic per launcher/Weapon/slot target run. This is the explicit
    // INVALID_ID hole from ProjectileStreamUpdate expressed as a value, so a
    // prior run cannot reconnect when the intervening projectile dies.
    uint32_t projectileStreamChainIdentity = 0;
    uint32_t sourceShotSequence = 0;
    // One-based explicit authored barrel selected when the shot was admitted.
    // Stateful weapons have already applied the active Draw barrel count.
    uint32_t sourceBarrelSequenceOrdinal = 0;
    uint64_t spawnedTick = 0;
    uint64_t expiryTick = 0;

    // Frozen source Object position from the shot that most recently created
    // this stream point. It is intentionally distinct from `start`, which is
    // the weapon launch-bone position used by projectile flight.
    LogicFixedVec3 projectileStreamOwnerAnchorPosition{};

    // These values are the authoritative DumbProjectile trajectory. The
    // presentation Transform is merely a float projection of `position`.
    LogicFixedVec3 target{};
    LogicFixedVec3 start{};
    LogicFixedQuaternion launchOrientation{};
    LogicFixedVec3 controlOne{};
    LogicFixedVec3 controlTwo{};
    LogicFixedVec3 position{};
    Scalar speedUnitsPerSecond{};
    Scalar firstHeight{};
    Scalar secondHeight{};
    Scalar firstPercentIndent{};
    Scalar secondPercentIndent{};
    // `FlightPathAdjustDistPerSecond` is a following cap, not homing AI.
    Scalar targetAdjustDistancePerSecond{};
    // The current authored Bezier tangent is a simulation fact.  In
    // particular, a directional detonation must not read Transform.rotation:
    // that yaw is only a 2D presentation projection and may still describe
    // the preceding segment when a swept collision occurs.
    LogicFixedVec3 flightPathForward{};
    uint32_t pathSegments = 1;
    uint32_t currentStep = 0;
    uint32_t pathfindLayer = 0;
    game::ObjectKindOfMask garrisonHitRequiredKindMask{};
    game::ObjectKindOfMask garrisonHitForbiddenKindMask{};
    ::container::String garrisonHitFx;
    uint32_t garrisonHitKillCount = 0;
    ObjectProjectileMotion motion = ObjectProjectileMotion::DumbBezier;
    bool orientToFlightPath = true;
    // This affects only visual attitude: DumbBezier remains the sole owner
    // of XYZ path/collision/lifespan.  When a PhysicsBehavior is present it
    // supplies deterministic pitch/yaw/roll rates at launch.
    bool tumbleRandomly = false;
    // Weapon::positionProjectileForLaunch installs the complete launch-bone
    // matrix before the projectile controller advances.  Preserve that
    // first confirmed pose even though later flight owns its own tangent.
    bool hasLaunchOrientation = false;
    // RefCode's MaxLifespan is a real duration, including an explicitly
    // authored zero-frame lifetime.  Keep a separate presence bit so tick 0
    // is not accidentally reinterpreted as "live forever".
    bool hasExpiryTick = true;
    // When set, detonation feeds an UNRESISTABLE/DETONATED transaction back
    // through the projectile's own Body/Die path instead of directly
    // reclaiming the ECS entity.
    bool detonateCallsKill = false;
    bool hasFlightPathForward = false;
    bool detonated = false;
};

// A resolved armor set is copied into gameplay-owned scalar arrays at spawn.
// It is deliberately independent of ArmorStore and GameContentSnapshot after
// initialization: a match may never observe a menu/editor reload halfway
// through a damage transaction.
struct ObjectArmorSetRuntime final {
    game::ArmorSetConditionMask conditions = 0;
    ::container::String armorTemplateName;
    ::container::String damageFxName;
    ::container::Array<math::q32_32, game::DAMAGE_TYPE_COUNT>
        damageMultipliersFixed{};

    ObjectArmorSetRuntime() {
        damageMultipliersFixed.fill(math::q32_32{int32_t{1}});
    }
};

struct ObjectArmorComponent final {
    ::container::Vector<ObjectArmorSetRuntime> sets;
    ::container::String selectedArmorTemplateName;
    ::container::String selectedDamageFxName;
    ::container::Array<math::q32_32, game::DAMAGE_TYPE_COUNT>
        damageMultipliersFixed{};

    ObjectArmorComponent() {
        damageMultipliersFixed.fill(math::q32_32{int32_t{1}});
    }
};

} // namespace engine
