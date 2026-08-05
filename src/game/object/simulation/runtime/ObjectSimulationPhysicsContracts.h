#pragma once

#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>

namespace engine {

enum class ObjectPhysicsEventKind : uint8_t {
    Landed,
    Bounced,
    RestingDestroyed,
};

struct ObjectPhysicsEvent final {
    ObjectPhysicsEventKind kind = ObjectPhysicsEventKind::Landed;
    ObjectId object = INVALID_OBJECT_ID;
    LogicFixedVec3 position{};
    math::q32_32 verticalSpeedUnitsPerSecond{};
    uint64_t confirmedTick = 0;
};

// Immutable participant facts for one locomotor-vs-ally contact. These are
// sampled at the collision boundary so the later typed transaction never
// reconstructs ZH's blockedBy()/needToRotate()/path-priority predicates from
// ECS state that may already have been changed by an earlier transaction.
struct ObjectAIMovementObstructionParticipant final {
    ObjectId object = INVALID_OBJECT_ID;
    LogicFixedVec3 position{};
    LogicFixedVec3 direction{};
    uint32_t blockedTicks = 0;
    bool hasPath = false;
    bool moving = false;
    bool doingGroundMovement = false;
    bool movingBackward = false;
    bool needsRotation = true;
    bool nearFinalGoal = false;
    bool effectivelyDead = false;
    bool infantry = false;
    bool vehicle = false;
    bool dozer = false;

    constexpr bool operator==(
        const ObjectAIMovementObstructionParticipant&) const noexcept =
        default;
};

// Stable directed contact sampled after kinematics. Physics freezes facts;
// GameSession performs the deterministic, typed MoveAside/order mutation.
struct ObjectAIMovementObstructionEvent final {
    ObjectAIMovementObstructionParticipant mover;
    ObjectAIMovementObstructionParticipant blocker;
    // Authoritative mover pose at the swept time of impact. The transaction
    // may restore this pose only when blockedBy() accepts the contact; allowed
    // crossings (infantry crossing, near-goal overlap, crush) keep the final
    // movement pose sampled in mover.position.
    LogicFixedVec3 moverContactPosition{};
    math::q32_32 pathfindCellSize{int32_t{10}};
    bool moverCanCrushBlocker = false;
    uint64_t confirmedTick = 0;
    uint64_t submissionOrdinal = 0;

    constexpr bool operator==(
        const ObjectAIMovementObstructionEvent&) const noexcept = default;
};

struct ObjectPhysicsCrashCommand final {
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    game::WeaponContentId weapon;
    LogicFixedVec3 impactPosition;
    uint32_t sourceSequence = 0;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
    bool targetIsBuilding = false;
    bool destroySource = false;
};

enum class ObjectPhysicsRequestKind : uint8_t {
    ApplyForce,
    ApplyMotiveForce,
    ApplyShock,
    AddVelocity,
    SetAngularRates,
    AddAngularRates,
    ScrubVelocity2D,
    ScrubVelocityZ,
    SetStunned,
    SetFreeFall,
};

struct ObjectPhysicsRequest final {
    ObjectId target = INVALID_OBJECT_ID;
    ObjectId source = INVALID_OBJECT_ID;
    uint32_t sourceSequence = 0;
    LogicFixedVec3 linear{};
    ObjectPhysicsComponent::Scalar yawRate{};
    ObjectPhysicsComponent::Scalar pitchRate{};
    ObjectPhysicsComponent::Scalar rollRate{};
    ObjectPhysicsComponent::Scalar magnitudeLimit{};
    bool enabled = true;
    ObjectPhysicsRequestKind kind = ObjectPhysicsRequestKind::ApplyForce;
    uint64_t confirmedTick = 0;
};

} // namespace engine
