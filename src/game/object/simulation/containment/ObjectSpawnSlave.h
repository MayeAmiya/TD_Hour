#pragma once

#include "core/container/container_types.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <optional>

#include "game/object/plan/containment/ObjectSpawnSlavePlanTypes.h"
namespace engine {

class PlayerRegistry;
class GameContentSnapshot;
class SimulationRandom;
class ObjectSpatialIndex;
struct ObjectDefectionRequest;
struct ObjectDeleteDestroyRequest;

// Shared set switch used by typed controllers which retain the immutable
// ThingTemplate LocomotorSet recipe while changing only the live hot profile.
[[nodiscard]] bool applyObjectLocomotorSet(
    ObjectLocomotionComponent& locomotion,
    const game::ThingTemplate& objectTemplate,
    const GameContentSnapshot& content,
    game::LocomotorSetSlot slot);

struct ObjectSpawnRuntime final {
    struct PendingRequest final {
        uint64_t requestId = 0;
        container::String templateName;
        uint32_t emissionSequence = 0;
        uint64_t lastAttemptTick = 0;
    };

    // Per-occurrence ownership in successful spawn order. RefCode keeps the
    // SpawnBehavior roster as an insertion-ordered list and consumes that
    // order from onDie/onDelete; ObjectId order is not authored occurrence
    // order. A template name or generic producer edge is not a SpawnBehavior
    // identity: two occurrences may use the same child template and
    // OCL-created Slaved objects must remain unrelated.
    container::Vector<ObjectId> children;
    container::Vector<uint64_t> replacementReadyTicks;
    container::Vector<PendingRequest> pendingRequests;
    uint64_t nextUpdateTick = 0;
    uint64_t nextRequestId = 1;
    uint32_t nextTemplateIndex = 0;
    uint32_t emissionSequence = 1;
    uint64_t successfulSpawnCount = 0;
    uint32_t successfulInitialProducerExitCount = 0;
    // MobMemberSlavedUpdate owns the decision to begin/end a self task, while
    // SpawnBehavior owns the per-occurrence admission ratio.  Keep only the
    // stable child identities here so the two systems share no AI state. This
    // auxiliary membership set remains sorted independently of children.
    container::Vector<ObjectId> selfTaskingChildren;
    // SpawnBehavior observes the master's explicit command generation so a
    // Stop is forwarded exactly once to every owned slave.  Attack commands
    // are synchronized by value below; this counter exists only because an
    // empty queue otherwise cannot distinguish Stop from ordinary idleness.
    uint64_t observedMasterExternalOrderRevision = 0;
    uint32_t observedMasterDisabledMask = 0;
    bool lastAttackCommandWasAi = false;
    bool initialized = false;
    bool oneShotCompleted = false;
    uint64_t revision = 0;
};

struct ObjectHordeRuntime final {
    uint64_t nextUpdateTick = 0;
    bool inHorde = false;
    bool trueHordeMember = false;
    bool hasFlag = false;
    uint64_t revision = 0;
};

struct ObjectTensileRuntime final {
    container::Array<ObjectId, 4> links{};
    container::Array<LogicFixedVec3, 4> tensors{};
    LogicFixedVec3 inertia{};
    math::q32_32 lowestSlideElevation{int32_t{255}};
    uint32_t lifeTicks = 0;
    bool enabled = false;
    bool linksInitialized = false;
    bool activationPublished = false;
    bool terminal = false;
    bool movingCondition = false;
    bool freefallCondition = false;
    uint64_t revision = 0;
};

enum class ObjectTensileFormationEventKind : uint8_t {
    NavigationWallCreate,
    NavigationWallRemove,
    CrackSound,
    TerminalRubble,
};

struct ObjectTensileFormationEvent final {
    ObjectTensileFormationEventKind kind =
        ObjectTensileFormationEventKind::NavigationWallCreate;
    ObjectId object = INVALID_OBJECT_ID;
    container::String resource;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
    uint64_t submissionOrdinal = 0;
};

enum class ObjectSlaveRepairState : uint8_t {
    None,
    Unpacking,
    Ready,
    Extending,
    Welding,
    Retracting,
};

struct ObjectSlaveRuntime final {
    ObjectId master = INVALID_OBJECT_ID;
    LogicFixedVec3 guardOffset{};
    LogicFixedVec3 repairDestination{};
    uint32_t outsideCatchUpFrames = 0;
    uint32_t nextCommandSequence = 1;
    uint64_t nextDecisionTick = 0;
    uint64_t repairPhaseDueTick = 0;
    ObjectId primaryVictim = INVALID_OBJECT_ID;
    ObjectSlaveRepairState repairState = ObjectSlaveRepairState::None;
    bool returningToMaster = false;
    bool requireMaster = false;
    bool guardOffsetInitialized = false;
    bool repairDestinationValid = false;
    bool slavedEffectsApplied = false;
    bool decisionClockInitialized = false;
    bool selfTasking = false;
    uint64_t revision = 0;
};

enum class ObjectSlaveRepairPresentationEventKind : uint8_t {
    WeldingStarted,
};

// Renderer/audio-safe replacement for the ParticleSystem and MiscAudio calls
// made when SlavedUpdate first enters its welding phase.
struct ObjectSlaveRepairPresentationEvent final {
    ObjectSlaveRepairPresentationEventKind kind =
        ObjectSlaveRepairPresentationEventKind::WeldingStarted;
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId master = INVALID_OBJECT_ID;
    container::String particleSystem;
    container::String boneName;
    uint64_t lifetimeTicks = 0;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

// DroneSpotting is a condition on the master with potentially several live
// SlavedUpdate producers. Rebuilding this stable source set once per tick
// avoids the retail order-dependent "last drone clears everybody" behavior
// and guarantees that deleting the final drone removes the bonus.
struct ObjectSlaveRangeBonusSourcesComponent final {
    container::Vector<ObjectId> sources;
    uint64_t revision = 0;
};

struct ObjectSpawnSlaveComponent final {
    container::SharedPtr<const game::ObjectSpawnSlavePlan> plan;
    container::Vector<ObjectSpawnRuntime> spawns;
    container::Vector<ObjectSpawnRuntime> mobNexus;
    container::Vector<ObjectHordeRuntime> hordes;
    container::Vector<ObjectTensileRuntime> tensileFormations;
    container::Vector<ObjectSlaveRuntime> slaved;
    container::Vector<ObjectSlaveRuntime> mobMemberSlaved;
};

struct ObjectSpawnChildrenComponent final {
    container::Vector<ObjectId> children;
    uint64_t revision = 0;
};

struct ObjectSpawnedByRuntimeComponent final {
    enum class Kind : uint8_t {
        SpawnBehavior,
        MobNexus,
    };

    ObjectId master = INVALID_OBJECT_ID;
    Kind kind = Kind::SpawnBehavior;
    uint32_t ruleIndex = 0;
    uint64_t requestId = 0;
    bool requireMaster = false;
    uint64_t revision = 0;
};

struct ObjectSpawnSlaveRequest final {
    ObjectSpawnedByRuntimeComponent::Kind kind =
        ObjectSpawnedByRuntimeComponent::Kind::SpawnBehavior;
    ObjectId spawner = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectTeamId primaryTeam = INVALID_OBJECT_TEAM_ID;
    container::String templateName;
    LogicFixedVec3 position{};
    math::q32_32 yawRadians{};
    LogicFixedVec3 exitTarget{};
    std::optional<uint32_t> initialPathfindLayer;
    // Reservation ownership is frozen into the request so the central spawn
    // transaction can commit or release the exact ExitInterface host without
    // retaining a component/entity pointer across the value boundary.
    ObjectId exitHost = INVALID_OBJECT_ID;
    ObjectProductionExitReservation exitReservation;
    // CanReclaimOrphans does not create a new object. The simulation freezes
    // the selected live orphan here, and GameSession feeds it through the
    // normal acknowledgement path so SpawnedBy/children/slave-master state is
    // still owned by ObjectSpawnSlaveSystem.
    ObjectId reclaimedObject = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;
    uint32_t emissionSequence = 0;
    uint32_t ruleIndex = 0;
    uint64_t requestId = 0;
    uint64_t submissionOrdinal = 0;
    // SpawnBehavior calls Player::onUnitCreated; MobNexus initial payload
    // creation does not. Freeze that score callback distinction at emission.
    bool scoreAsBuilt = false;
    bool hasExitTarget = false;
    // SpawnPointProductionExitUpdate owns a stationary world slot. RefCode
    // places the child at that slot and marks it DISABLED_HELD instead of
    // issuing the ordinary production-exit rally move.
    bool holdAfterSpawn = false;
    bool exitByBudding = false;
    bool usedInitialProducerExit = false;
    bool containInSpawner = false;
    uint32_t logicFramesPerSecond = 1;
    uint64_t confirmedTick = 0;
};

struct ObjectSpawnVeterancyRequest final {
    ObjectId object = INVALID_OBJECT_ID;
    game::ObjectVeterancyLevel level =
        game::ObjectVeterancyLevel::Regular;
};

enum class ObjectHiveDamageRoute : uint8_t {
    ApplyToHive,
    RoutedToSlave,
    Swallowed,
};

class ObjectSpawnSlaveSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;
    void update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                const PlayerRegistry* players,
                const GameContentSnapshot* content,
                const ObjectSpatialIndex* spatialIndex,
                const game::terrain::TerrainLogic* terrain,
                SimulationRandom* random,
                const ObjectSimulationRules& rules, uint64_t confirmedTick,
                uint64_t& nextGameplaySubmissionOrdinal,
                container::Vector<ObjectSpawnSlaveRequest>& spawnRequests,
                container::Vector<ObjectDamageRequest>& damageRequests,
                container::Vector<ObjectSpawnVeterancyRequest>&
                    veterancyRequests,
                container::Vector<ObjectBodyHealthProjection>&
                    bodyHealthProjections,
                container::Vector<ObjectDeleteDestroyRequest>&
                    destroyRequests,
                container::Vector<ObjectDefectionRequest>&
                    defectionRequests,
                container::Vector<ObjectSlaveRepairPresentationEvent>&
                    repairPresentationEvents,
                container::Vector<ObjectTensileFormationEvent>&
                    tensileNavigationEvents,
                container::Vector<ObjectTensileFormationEvent>&
                    tensilePresentationEvents) const;
    // GameSession acknowledges the complete spawn/contain transaction. A
    // failed attempt stays pending with the same request/template identity
    // and is retried at the next sparse update; no one-shot/deadline state is
    // consumed by an allocation or exit failure.
    [[nodiscard]] bool acknowledgeSpawn(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSpawnSlaveRequest& request, ObjectId spawned,
        bool accepted) const;
    // Generic OCL calls SlavedUpdateInterface::onEnslave(source) without
    // adding the object to any SpawnBehavior roster. Keep that relation
    // explicit so producer provenance, Spawn occurrence ownership and slave
    // master identity never collapse back into one heuristic edge.
    [[nodiscard]] bool bindOclSlaveMaster(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, ObjectId master) const noexcept;
    // Value-only replacements for SpawnBehaviorInterface queries.  They
    // inspect the authored occurrence roster and never expose component or
    // entity pointers to Command/Combat/AI consumers.
    [[nodiscard]] ObjectId closestSpawnChild(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId spawner, const LogicFixedVec3& position) const noexcept;
    [[nodiscard]] container::Vector<ObjectId> spawnChildren(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId spawner) const;
    [[nodiscard]] bool maySpawnSelfTaskAI(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId spawner, uint32_t ruleIndex,
        math::q32_32 maximumSelfTaskersRatio) const noexcept;
    // Narrow ingress for the MobMemberSlavedUpdate owner.  It changes only
    // SpawnBehavior's ratio-accounting fact, not the child's AI state.
    [[nodiscard]] bool setSpawnChildSelfTasking(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId spawner, uint32_t ruleIndex, ObjectId child,
        bool selfTasking) const noexcept;
    [[nodiscard]] ObjectHiveDamageRoute routeHiveDamage(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectDamageRequest& request) const;
    // Authored SpawnBehavior::onDie for one occurrence. It stops the first
    // SlavedUpdate interface, clears producer provenance immediately and
    // emits Body kills only when SpawnedRequireSpawner is authored.
    [[nodiscard]] bool onSpawnerDie(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId spawner, uint32_t authoredOrder, uint64_t confirmedTick,
        container::Vector<ObjectDamageRequest>& outDamage) const;
    // SpawnBehavior::onDelete differs from onDie: direct deletion destroys
    // required children structurally instead of applying Body damage.
    void onSpawnerDelete(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectId spawner, uint32_t authoredOrder,
        uint64_t confirmedTick,
        container::Vector<ObjectDeleteDestroyRequest>& outDestroy) const;
    // Fixed Object::onDie postamble callback from a spawned child to the exact
    // SpawnBehavior occurrence that owns it. This removes the child and arms
    // replacement before any later confirmed-frame system can observe a stale
    // roster; MobNexus and generic Producer provenance are not conflated.
    void onSpawnedObjectDie(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules, ObjectId child,
        uint64_t confirmedTick) const;
};

} // namespace engine
