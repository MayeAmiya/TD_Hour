#pragma once

#include "core/container/container_types.h"

#include "game/navigation/runtime/NavigationSystem.h"
#include "game/ai/StrategicAIRuntime.h"
#include "game/object/ai/runtime/ObjectAIRuntime.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/contracts/ObjectTeamRegistry.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
#include <optional>

namespace engine {

struct ObjectAISimulationDigest final {
    uint64_t aiRuntime = 0;
    uint64_t navigation = 0;
    uint64_t movement = 0;
    uint64_t economy = 0;
    uint64_t players = 0;
    uint64_t combined = 0;

    constexpr bool operator==(const ObjectAISimulationDigest&) const noexcept =
        default;
};

// Locomotor templates remain immutable content resources and may retain their
// authored float representation. Rollback/save state stores only stable
// template identities plus mutable fixed-point controller state.
struct ObjectAILocomotionRuntimeSnapshot final {
    container::Vector<container::String> profileNames;
    container::String activeProfileName;
    math::q32_32 closeEnough{};
    math::q32_32 forwardSpeed{};
    math::q32_32 verticalSpeed{};
    math::q32_32 groundOffset{};
    LogicFixedVec3 goal{};
    uint64_t activeOrderTick = 0;
    uint32_t activeOrderSequence = 0;
    uint32_t activeSourceScriptId = 0;
    bool usePreciseZPosition = false;
    bool ultraAccurate = false;
    bool overWater = false;
    bool hasActiveMove = false;
    bool movingBackward = false;
    ObjectLocomotionState state = ObjectLocomotionState::Idle;
};

// Value-only production boundary for the object-AI-owned world slice. It is
// intentionally narrower than a complete GameSession save and contains no
// ecs::entity, service pointer or diagnostic journal.
struct ObjectAIWorldOrderOwnerSnapshot final {
    ObjectId object = INVALID_OBJECT_ID;
    bool objectPresent = false;
    // Authoritative pose only. TransformComponent is a float presentation
    // projection and must never become save/replay/rollback state.
    std::optional<ObjectFixedTransformComponent> fixedTransform;
    std::optional<ObjectAILocomotionRuntimeSnapshot> locomotion;
    std::optional<ObjectAIPathMovementComponent> pathMovement;
    std::optional<ObjectAIMovementObstructionStateComponent>
        movementObstruction;
    std::optional<ObjectTemporaryCollisionIgnoreComponent>
        temporaryCollisionIgnore;
    std::optional<ObjectRepulsorExpiryComponent> repulsorExpiry;
    std::optional<ObjectWaypointCompletionComponent> waypointCompletion;
    std::optional<ObjectOrderQueueComponent> orderQueue;
    std::optional<ObjectSystemPathSequenceComponent> systemPathSequence;
    std::optional<ObjectAirborneComponent> airborne;
    std::optional<ObjectPendingPlayerEvacuationComponent> playerEvacuation;
    std::optional<PrimaryTeamComponent> primaryTeam;
};

struct ObjectAIWorldEconomySnapshot final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectEconomyRuntimeSnapshot runtime;
};

struct ObjectAIStrategicBuildEntrySnapshot final {
    PlayerId player = INVALID_PLAYER_ID;
    container::String objectType;
    math::q32_32 anchorX{};
    math::q32_32 anchorY{};
    math::q32_32 yawRadians{};
    container::String scriptName;
    uint32_t sourceSideOrdinal = UINT32_MAX;
    uint32_t sourceBuildListOrdinal = UINT32_MAX;
    uint32_t sourceSequence = 0;
    uint64_t createdTick = 0;
    uint64_t nextAttemptTick = 0;
    uint32_t attemptCount = 0;
    uint32_t placementSearchOrdinal = 0;
    uint8_t state = 0;
    ObjectId reservedBuilder = INVALID_OBJECT_ID;
    ObjectId constructedObject = INVALID_OBJECT_ID;
    int32_t remainingRebuilds = 0;
    uint64_t strategicPlanId = 0;
    bool authoredBuildList = false;
};

struct ObjectAIWorldSnapshot final {
    static constexpr uint32_t SchemaVersion = 13;

    uint32_t schemaVersion = SchemaVersion;
    uint64_t confirmedTick = 0;
    bool hasConfirmedFrame = false;
    ai::ObjectAIRuntimeSnapshot aiRuntime;
    StrategicAIRuntimeSnapshot strategicAI;
    ObjectTeamRegistrySnapshot objectTeams;
    navigation::NavigationSystemSnapshot navigation;
    container::Vector<ObjectAIWorldOrderOwnerSnapshot> orderOwners;
    container::Vector<ObjectAIWorldEconomySnapshot> economyObjects;
    container::Vector<ObjectAIStrategicBuildEntrySnapshot>
        strategicBuildEntries;
    container::Vector<ai::PathCorrelation> pendingMoveCompletions;
};

enum class ObjectAIWorldSnapshotStatus : uint8_t {
    Success,
    SessionInactive,
    Busy,
    InvalidSchema,
    InvalidTickState,
    AIRuntimeRejected,
    StrategicAIRejected,
    ObjectTeamsRejected,
    RecipeMismatch,
    NavigationRejected,
    InvalidObjectOrder,
    ObjectSetMismatch,
    ComponentPresenceMismatch,
    EconomyRejected,
    InvalidCorrelation,
};

} // namespace engine
