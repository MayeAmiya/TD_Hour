#pragma once

#include "core/container/container_types.h"
#include "game/base/DamageTypes.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectDeathReaction.h"

#include <cstdint>
#include <optional>

namespace engine {

enum class ObjectDeathEventKind : uint8_t {
    // A currently migrated Die action matched the exact DieMux filters.
    ReactionApplied,
    // An original Die module matched, but its behavior is deliberately not
    // yet migrated. It remains observable instead of silently acting like
    // DestroyDie.
    UnsupportedReaction,
    // Programmatic fixtures/helpers that were created without a frozen
    // ObjectArchetype retain the pre-DieMux safe destruction behavior. Final
    // content recipes never rely on this path: their inherited DestroyDie is
    // compiled into the shared plan.
    UnprofiledFallbackDestroy,
    // Fixed Object::onDie suffix after every authored Die interface has
    // closed. Consumers such as reconstructing-hole transfer must not infer
    // this edge from the presence of a particular Die module.
    Postamble,
};

struct ObjectDeathEvent final {
    ObjectDeathEventKind kind = ObjectDeathEventKind::ReactionApplied;
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId source = INVALID_OBJECT_ID;
    game::ObjectDeathReactionKind reaction = game::ObjectDeathReactionKind::Unsupported;
    uint32_t authoredOrder = 0;
    game::DamageType damageType = game::DamageType::EXPLOSION;
    game::DeathType deathType = game::DeathType::NORMAL;
    uint64_t confirmedTick = 0;
};

// Authoritative ECS-to-script fact emitted by SpecialPowerCompletionDie.
// The creator is an historical stable ID and is deliberately not resolved
// back to a live entity: RefCode still reports completion after the creator
// has died. The controlling player is snapshotted from the completing object
// before deferred lifecycle destruction.
struct ObjectSpecialPowerCompletionEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId creator = INVALID_OBJECT_ID;
    PlayerId player = INVALID_PLAYER_ID;
    container::String specialPowerTemplate;
    uint32_t authoredOrder = 0;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

// Session-owned ownership transfer requested by NeutronBlastBehavior after
// the authoritative Die callback has disabled and idled a surviving vehicle.
// The event carries only stable IDs; ObjectSimulation never reaches into the
// Team/selection presentation owners directly.
struct ObjectVehicleNeutralizationRequest final {
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectPilotVehicleTakeoverRequest final {
    ObjectId pilot = INVALID_OBJECT_ID;
    ObjectId vehicle = INVALID_OBJECT_ID;
    PlayerId newOwner = INVALID_PLAYER_ID;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectCrushDieEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId source = INVALID_OBJECT_ID;
    game::ObjectCrushType crushType = game::ObjectCrushType::None;
    uint32_t authoredOrder = 0;
    bool frontCrushed = false;
    bool backCrushed = false;
    std::optional<container::String> audioEvent;
    // Frozen authoritative anchor. Audio projects it after the death
    // transaction has committed.
    LogicFixedVec3 position{};
    uint64_t confirmedTick = 0;
};

// InstantDeathBehavior selects one entry from each of its FX/OCL/Weapon lists
// before it requests deferred destruction. This is a value-only confirmed-
// frame command; no renderer handle, legacy Object pointer, or EnTT entity
// escapes here.
struct ObjectInstantDeathEffectEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId source = INVALID_OBJECT_ID;
    uint32_t sourceSequence = 0;
    game::DamageType damageType = game::DamageType::EXPLOSION;
    game::DeathType deathType = game::DeathType::NORMAL;
    LogicFixedVec3 position{};
    math::q32_32 rotationRadians{};
    PlayerId owner = INVALID_PLAYER_ID;
    uint32_t sourcePathfindLayer = 0;
    uint32_t authoredOrder = 0;
    std::optional<container::String> fx;
    std::optional<container::String> ocl;
    std::optional<container::String> weapon;
    uint64_t fxEmissionSequence = 0;
    uint64_t confirmedTick = 0;
};

enum class ObjectDeathOclEventKind : uint8_t {
    CreateObject,
    EjectPilot,
};

struct ObjectCreateObjectDieEvent final {
    ObjectDeathOclEventKind kind = ObjectDeathOclEventKind::CreateObject;
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId damageSource = INVALID_OBJECT_ID;
    container::String objectCreationList;
    ObjectHealthComponent::Scalar previousHealth{};
    ObjectHealthComponent::Scalar maximumHealth{};
    ObjectHealthComponent::Scalar subdualDamage{};
    uint32_t sourcePathfindLayer = 0;
    uint32_t authoredOrder = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
    bool transferPreviousHealth = false;
    bool transferSelection = false;
    container::String voiceEject;
    container::String soundEject;
};

struct ObjectCreateCrateDieEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId damageSource = INVALID_OBJECT_ID;
    container::String crateObjectTemplate;
    PlayerId makerOwner = INVALID_PLAYER_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    LogicFixedVec3 sourcePosition;
    uint32_t sourcePathfindLayer = 0;
    bool sourceAirborne = false;
    math::q32_32 nearSearchAngleRadians{};
    math::q32_32 wideSearchAngleRadians{};
    math::q32_32 orientationRadians{};
    uint32_t authoredOrder = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
};

enum class FxInvocationAnchorKind : uint8_t {
    ObjectAttachment,
    WorldPosition,
};

struct FxInvocationAnchorSnapshot final {
    LogicFixedVec3 position{};
    math::q32_32 rollRadians{};
    math::q32_32 pitchRadians{};
    math::q32_32 yawRadians{};
};

struct ObjectFxListDieEffectEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId source = INVALID_OBJECT_ID;
    uint32_t sourceSequence = 0;
    game::DamageType damageType = game::DamageType::EXPLOSION;
    game::DeathType deathType = game::DeathType::NORMAL;
    FxInvocationAnchorSnapshot primary;
    std::optional<FxInvocationAnchorSnapshot> secondary;
    PlayerId owner = INVALID_PLAYER_ID;
    container::String fx;
    FxInvocationAnchorKind anchor = FxInvocationAnchorKind::ObjectAttachment;
    uint32_t authoredOrder = 0;
    uint64_t fxEmissionSequence = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectSlowDeathPhaseEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId source = INVALID_OBJECT_ID;
    uint32_t sourceSequence = 0;
    uint32_t authoredOrder = 0;
    uint32_t sourcePathfindLayer = 0;
    game::ObjectSlowDeathPhase phase = game::ObjectSlowDeathPhase::Initial;
    std::optional<container::String> fx;
    std::optional<container::String> ocl;
    std::optional<container::String> weapon;
    // Helicopter/JetSlowDeathBehavior's explicit FinalRubbleObject is a
    // typed object replacement, not an ordinary OCL payload. Keep it
    // separate so consumers never infer rubble from `source` alone.
    std::optional<container::String> rubbleObject;
    // Aircraft FinalRubble creation observes the dying object at emission
    // time. Freeze these values so earlier sibling OCL/Weapon reactions
    // cannot rewrite ownership or pose before the continuation runs.
    PlayerId rubbleOwner = INVALID_PLAYER_ID;
    ObjectTeamId rubblePrimaryTeam = INVALID_OBJECT_TEAM_ID;
    ObjectFixedTransformComponent rubbleTransform{};
    bool hasRubbleSpawnState = false;
    uint64_t fxEmissionSequence = 0;
    uint64_t confirmedTick = 0;
};

} // namespace engine
