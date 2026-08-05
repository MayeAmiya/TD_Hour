#pragma once

#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/creation/ObjectCreationListCatalog.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>

namespace engine {

enum class ObjectCreationListCompletionKind : uint8_t {
    None,
    CreateObjectDie,
};

struct ObjectCreationListCompletion final {
    ObjectCreationListCompletionKind kind =
        ObjectCreationListCompletionKind::None;
    ObjectHealthComponent::Scalar previousHealth{};
    ObjectHealthComponent::Scalar maximumHealth{};
    ObjectHealthComponent::Scalar subdualDamage{};
    ObjectId damageSource = INVALID_OBJECT_ID;
    bool transferPreviousHealth = false;
    bool transferSelection = false;
};

// Detached, confirmed-frame request to interpret one immutable OCL recipe.
// It snapshots every source value needed after a lethal Body transaction so
// the executor never retains an EnTT entity or a pointer into a mutable INI
// store.  The source ObjectId is retained for producer/weapon attribution;
// pending-destroy storage may still be queried until the normal lifecycle
// flush, but correctness does not depend on it remaining alive.
struct ObjectCreationListInvocation final {
    game::ObjectCreationListContentId content;
    ObjectId source = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectTeamId primaryTeam = INVALID_OBJECT_TEAM_ID;
    LogicFixedVec3 primaryPosition;
    LogicFixedVec3 secondaryPosition;
    LogicFixedVec3 sourceVelocity;
    ObjectPhysicsComponent::Scalar orientationRadians{};
    ObjectPhysicsComponent::Scalar pitchRadians{};
    ObjectPhysicsComponent::Scalar rollRadians{};
    game::ObjectVeterancyLevel veterancy =
        game::ObjectVeterancyLevel::Regular;
    uint32_t lifetimeOverrideFrames = 0;
    uint32_t authoredOrder = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
    uint32_t sourcePathfindLayer = 0;
    bool hasSecondaryPosition = false;
    bool sourceAirborne = false;
    bool sourceOwnsFullAttitude = false;
    // DeliverPayload only: USE_OWNER_OBJECT reuses `source` as the transport
    // instead of creating the OCL-authored delivery owner. Other nugget
    // families intentionally ignore this compatibility bit.
    bool createDeliveryOwner = true;
    // ObjectCreationUpgrade pauses the authored UpgradeMux walk at this OCL.
    // Completion resumes the same source object before any later authored mux
    // runs, preserving RefCode's synchronous upgradeImplementation ordering.
    bool resumeSourceUpgradeMux = false;
    ObjectCreationListCompletion completion;
};

struct ObjectCreationListExecutionReport final {
    ObjectId firstCreatedObject = INVALID_OBJECT_ID;
    uint32_t visitedNuggets = 0;
    uint32_t createdObjects = 0;
    uint32_t firedWeapons = 0;
    uint32_t appliedPhysicsEffects = 0;
    uint32_t skippedUnsupportedAiNuggets = 0;
};

} // namespace engine
