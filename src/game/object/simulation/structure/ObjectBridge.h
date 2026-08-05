#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "core/ecs/ObjectId.h"
#include "game/object/creation/ObjectCreationListRuntime.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <limits>
#include <optional>

#include "game/object/plan/structure/ObjectBridgePlanTypes.h"
namespace engine {

class GameContentSnapshot;
struct ObjectDamageRequest;
struct ObjectSimulationRules;

namespace detail {
inline constexpr size_t ObjectRailedTransportMaximumPaths = 32;
}

} // namespace engine

namespace game::terrain {
class TerrainLogic;
}

namespace engine {

struct ObjectBridgeComponent final {
    container::SharedPtr<const game::ObjectBridgeRailPlan> plan;
    // Resolved once at object initialization. Runtime death processing never
    // performs a string/catalog lookup in the simulation hot path.
    container::Vector<container::Vector<game::ObjectCreationListContentId>>
        dieOclContentByRule;
    // Stable identities of the currently erected repair scaffold generation.
    // The bridge owns this association; scaffold behavior never searches for
    // a nearby bridge or retains an EnTT entity.
    container::Vector<ObjectId> scaffoldObjects;
    uint64_t scaffoldRequestSequence = 0;
    uint64_t scaffoldRevision = 0;
    bool scaffoldingPresent = false;
    bool navigationStatePublished = false;
    bool lastNavigationActive = false;
};

enum class ObjectBridgeScaffoldMotion : uint8_t {
    Still,
    Rise,
    BuildAcross,
    TearDownAcross,
    Sink,
};

enum class ObjectBridgeScaffoldRequestKind : uint8_t {
    // Applies all bridge/style-derived values to an already-created scaffold
    // ObjectId and starts the original Rise -> BuildAcross -> Still chain.
    CreateAndBuild,
    Reverse,
};

// Detached, replay-safe boundary between bridge layout generation / object
// creation and this runtime. No EnTT entity, TerrainRoad pointer or mutable
// module data crosses it. Dynamic layout generation remains a GameSession /
// terrain-content responsibility because it requires BridgeInfo corners and
// scaffold/support template geometry which ObjectBridgeRailPlan does not own.
struct ObjectBridgeScaffoldMotionRequest final {
    ObjectBridgeScaffoldRequestKind kind =
        ObjectBridgeScaffoldRequestKind::CreateAndBuild;
    ObjectId scaffold = INVALID_OBJECT_ID;
    ObjectId bridge = INVALID_OBJECT_ID;
    LogicFixedVec3 createPosition{};
    LogicFixedVec3 risePosition{};
    LogicFixedVec3 buildPosition{};
    math::q32_32 orientationRadians{};
    math::q32_32 lateralSpeedPerFrame{int32_t{1}};
    math::q32_32 verticalSpeedPerFrame{int32_t{1}};
    uint64_t sequence = 0;
    uint64_t confirmedTick = 0;
};

// Immutable terrain/content input to the legacy BridgeBehavior tiling
// algorithm. GameSession resolves the exact map provenance, Roads.ini style,
// frozen template geometry and owner/team before constructing this value.
// Consequently the pure planner needs neither TerrainLogic nor ECS access.
struct ObjectBridgeScaffoldLayoutRequest final {
    ObjectId bridge = INVALID_OBJECT_ID;
    container::String scaffoldTemplateName;
    container::String scaffoldSupportTemplateName;
    LogicFixedVec3 fromPosition{};
    LogicFixedVec3 toPosition{};
    LogicFixedVec3 bridgeCenter{};
    math::q32_32 scaffoldSpacing{};
    math::q32_32 scaffoldHeight{};
    math::q32_32 scaffoldSupportHeight{};
    math::q32_32 lateralSpeedPerFrame{};
    math::q32_32 verticalSpeedPerFrame{};
    uint64_t requestSequence = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectBridgeScaffoldSpawnSpec final {
    container::String templateName;
    ObjectBridgeScaffoldMotionRequest motion;
};

struct ObjectBridgeScaffoldSpawnPlan final {
    container::Vector<ObjectBridgeScaffoldSpawnSpec> objects;
};

// Reproduces RefCode BridgeBehavior::createScaffolding/setScaffoldData using
// Q32.32 persistent values. Returns no plan for incomplete/degenerate content
// rather than manufacturing style names, geometry, endpoints or a bridge.
[[nodiscard]] std::optional<ObjectBridgeScaffoldSpawnPlan>
buildObjectBridgeScaffoldSpawnPlan(
    const ObjectBridgeScaffoldLayoutRequest& request);

struct ObjectBridgeScaffoldComponent final {
    container::SharedPtr<const game::ObjectBridgeRailPlan> plan;
    ObjectId bridge = INVALID_OBJECT_ID;
    LogicFixedVec3 createPosition{};
    LogicFixedVec3 risePosition{};
    LogicFixedVec3 buildPosition{};
    LogicFixedVec3 position{};
    math::q32_32 lateralSpeedPerFrame{int32_t{1}};
    math::q32_32 verticalSpeedPerFrame{int32_t{1}};
    ObjectBridgeScaffoldMotion motion = ObjectBridgeScaffoldMotion::Still;
    bool bridgeActive = false;
    bool configured = false;
    bool destroyRequested = false;
    uint64_t acceptedRequestSequence = 0;
    uint64_t revision = 0;
};

struct ObjectBridgeTowerComponent final {
    container::SharedPtr<const game::ObjectBridgeRailPlan> plan;
    ObjectId bridge = INVALID_OBJECT_ID;
    bool bridgeActive = false;
    bool effectivelyDead = false;
    uint64_t revision = 0;
};

enum class ObjectRailroadConductorState : uint8_t {
    Accelerating,
    Braking,
    WaitingAtStation,
    Coasting,
    EndOfLine,
};

struct ObjectRailroadRuntime final {
    container::Vector<uint32_t> waypointIds;
    container::Vector<LogicFixedVec3> trackPoints;
    math::q32_32 trackLength{};
    math::q32_32 trackDistance{};
    math::q32_32 hitchDistance{};
    math::q32_32 speed{};
    int32_t direction = 1;
    uint64_t waitUntilTick = 0;
    ObjectId locomotive = INVALID_OBJECT_ID;
    ObjectId puller = INVALID_OBJECT_ID;
    ObjectId trailer = INVALID_OBJECT_ID;
    ObjectId chainTail = INVALID_OBJECT_ID;
    uint32_t currentSegment = std::numeric_limits<uint32_t>::max();
    uint32_t nextCarriageTemplateIndex = 0;
    uint32_t pendingCarriageTemplateIndex =
        std::numeric_limits<uint32_t>::max();
    uint32_t nextSpawnSequence = 1;
    uint32_t pendingSpawnSequence = 0;
    uint32_t unpulledTicks = 0;
    ObjectRailroadConductorState state =
        ObjectRailroadConductorState::Accelerating;
    bool trackDataLoaded = false;
    bool looping = false;
    bool held = false;
    bool carriagesInitialized = false;
    bool proximityChain = false;
    bool hasEverBeenHitched = false;
    bool leadCarriage = false;
    bool waitingInWings = true;
    bool endOfLine = false;
    bool disembarkAtStop = false;
    bool pendingCarriageSpawn = false;
    bool runningSoundActive = false;
    bool stationWhistlePlayed = false;
    bool collisionWhistleActive = false;
    uint64_t revision = 0;
};

struct ObjectRailroadComponent final {
    container::SharedPtr<const game::ObjectBridgeRailPlan> plan;
    container::Vector<ObjectRailroadRuntime> instances;
};

// RailroadBehavior owns the train graph and requests only the actual object
// allocation from GameSession.  The request is replay-safe and contains no
// EnTT entity or mutable template pointer; acknowledgeCarriageSpawn validates
// the sequence before publishing the stable ObjectId pull edge.
struct ObjectRailroadCarriageSpawnRequest final {
    ObjectId locomotive = INVALID_OBJECT_ID;
    ObjectId puller = INVALID_OBJECT_ID;
    container::String templateName;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectTeamId primaryTeam = INVALID_OBJECT_TEAM_ID;
    ObjectFixedTransformComponent transform{.authoritative = true};
    uint32_t railroadRuleIndex = 0;
    uint32_t carriageTemplateIndex = 0;
    uint32_t requestSequence = 0;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectRailroadDisembarkRequest final {
    ObjectId carriage = INVALID_OBJECT_ID;
    uint32_t railroadRuleIndex = 0;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

enum class ObjectRailroadPresentationEventKind : uint8_t {
    RunningLoopStarted,
    RunningLoopStopped,
    ClicketyClack,
    Whistle,
};

struct ObjectRailroadPresentationEvent final {
    ObjectRailroadPresentationEventKind kind =
        ObjectRailroadPresentationEventKind::ClicketyClack;
    ObjectId object = INVALID_OBJECT_ID;
    container::String eventName;
    math::q32_32 volumeScale{int32_t{1}};
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectRailedTransportWaypointPath final {
    uint32_t startWaypointId = std::numeric_limits<uint32_t>::max();
    uint32_t endWaypointId = std::numeric_limits<uint32_t>::max();
    LogicFixedVec3 startPosition{};
    LogicFixedVec3 endPosition{};
};

struct ObjectRailedTransportDockAttachRequest final {
    ObjectId container = INVALID_OBJECT_ID;
    ObjectId object = INVALID_OBJECT_ID;
    // UINT32_MAX selects RefCode's first RailedTransportDockUpdate interface.
    // A concrete value is retained on completion/acknowledgement so rollback
    // never mutates a different authored occurrence.
    uint32_t dockRuleIndex = std::numeric_limits<uint32_t>::max();
    uint32_t containmentRuleIndex = std::numeric_limits<uint32_t>::max();
    bool destroyWithContainer = false;
    bool enclosing = true;
    bool followsContainerTransform = true;
    uint32_t logicFramesPerSecond = 30;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectRailedTransportDockAttachCompletion final {
    ObjectRailedTransportDockAttachRequest request;
    bool accepted = false;
};

enum class ObjectRailedTransportDockAdmission : uint8_t {
    NotApplicable,
    Rejected,
    Deferred,
};

struct ObjectRailedTransportRuntime final {
    container::Array<ObjectRailedTransportWaypointPath,
                     detail::ObjectRailedTransportMaximumPaths>
        paths{};
    size_t pathCount = 0;
    size_t currentPath = detail::ObjectRailedTransportMaximumPaths;
    uint64_t observedExternalOrderRevision = 0;
    uint64_t observedContainmentRevision = 0;
    uint64_t transitionEndsTick = 0;
    uint32_t nextCommandSequence = 1;
    size_t lastContainedCount = 0;
    container::Vector<ObjectId> containedObjects;
    container::Vector<ObjectId> pendingUnloadObjects;
    ObjectId dockingObject = INVALID_OBJECT_ID;
    ObjectId unloadingObject = INVALID_OBJECT_ID;
    // Pull-in is a two-phase transaction.  The original pose remains frozen
    // until ObjectContainment commits the prepared edge, so a rejected commit
    // can restore the authoritative object state instead of stranding the
    // docker at the ferry centre.
    LogicFixedVec3 dockingStartPosition{};
    math::q32_32 dockingStartRotation{};
    uint32_t dockingContainmentRuleIndex =
        std::numeric_limits<uint32_t>::max();
    uint32_t dockingLogicFramesPerSecond = 30;
    LogicFixedVec3 dockEndLocal{};
    LogicFixedVec3 dockWaiting07Local{};
    LogicFixedVec3 unloadDestination{};
    math::q32_32 pushOutsideDistancePerFrame{};
    math::q32_32 pullInsideDistancePerFrame{};
    bool dockingDestroyWithContainer = false;
    bool dockingEnclosing = true;
    bool dockingFollowsContainerTransform = true;
    bool dockingAwaitingCommit = false;
    bool waypointDataLoaded = false;
    bool containmentSnapshotInitialized = false;
    bool dockEndValid = false;
    bool dockWaiting07Valid = false;
    bool inTransit = false;
    bool executeRequested = false;
    bool dockOpen = true;
    bool loadingOrUnloading = false;
    uint64_t revision = 0;
};

struct ObjectRailedTransportRuntimeComponent final {
    container::SharedPtr<const game::ObjectBridgeRailPlan> plan;
    container::Vector<ObjectRailedTransportRuntime> instances;
};

struct ObjectBridgeStateEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    bool active = false;
    bool deathOccurrence = false;
    uint32_t authoredOrder = 0;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

enum class ObjectBridgeDeathEffectKind : uint8_t {
    FxList,
    ObjectCreationList,
};

// Detached BridgeBehavior::update continuation. Bridge objects may cross the
// deferred destruction boundary before an authored delay expires, so every
// OCL inheritance input and every world anchor is frozen at onDie time.
struct ObjectBridgeDeathEffectRuntime final {
    ObjectBridgeDeathEffectKind kind = ObjectBridgeDeathEffectKind::FxList;
    ObjectCreationListInvocation invocation;
    container::String fxList;
    LogicFixedVec3 position{};
    math::q32_32 orientationRadians{};
    uint32_t sourcePathfindLayer = 0;
    uint32_t authoredOrder = 0;
    uint64_t emissionSequence = 0;
    uint64_t dueTick = 0;
};

struct ObjectStructureEffectEvent;

class ObjectBridgeSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const GameContentSnapshot* content = nullptr) const;
    [[nodiscard]] bool applyScaffoldMotionRequest(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectBridgeScaffoldMotionRequest& request) const;
    [[nodiscard]] ObjectRailedTransportDockAdmission
    beginRailedTransportDockAttach(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectRailedTransportDockAttachRequest& request) const;
    [[nodiscard]] bool requestRailedTransportExecute(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId transport, uint64_t confirmedTick) const;
    [[nodiscard]] bool acknowledgeCarriageSpawn(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectRailroadCarriageSpawnRequest& request,
        ObjectId spawnedCarriage, bool accepted) const;
    void acknowledgeRailedTransportDockAttach(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId container, ObjectId object, uint32_t dockRuleIndex,
        bool accepted,
        uint64_t confirmedTick) const;
    // Object destruction must sever railroad and railed-dock structural
    // edges before authored BehaviorModule::onDelete callbacks run.  This is
    // deliberately not a Bridge authored callback.
    void detachObjectRelationships(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick,
        container::Vector<ObjectRailedTransportDockAttachCompletion>&
            cancelledDockAttach) const;
    // Bridge/BridgeTower damage callbacks in RefCode propagate the authored
    // percentage, not the post-armor HP delta.  Keep that callback semantic
    // behind the central Body value barrier instead of mutating sibling HP.
    void propagateHealthRequest(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectDamageRequest& request, math::q32_32 authoredAmount,
        container::Vector<ObjectDamageRequest>& outDamage) const;
    // A tower death kills its bridge; bridge death then kills all associated
    // towers.  These are force-kill Body requests, never raw entity deletion.
    void propagateDeath(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick,
        container::Vector<ObjectDamageRequest>& outDamage) const;
    void beginDeathOccurrence(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const game::terrain::TerrainLogic* terrain,
        const GameContentSnapshot* content,
        const ObjectSimulationRules& rules, ObjectId object,
        uint32_t authoredOrder, uint64_t sessionSeed,
        uint64_t confirmedTick, uint64_t& nextEmissionSequence,
        container::Vector<ObjectBridgeDeathEffectRuntime>& pending,
        container::Vector<ObjectStructureEffectEvent>& effects,
        container::Vector<ObjectCreationListInvocation>& invocations) const;
    void updateDeathEffects(
        uint64_t confirmedTick,
        container::Vector<ObjectBridgeDeathEffectRuntime>& pending,
        container::Vector<ObjectStructureEffectEvent>& effects,
        container::Vector<ObjectCreationListInvocation>& invocations) const;
    void update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                const game::terrain::TerrainLogic& terrain,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick,
                container::Vector<ObjectBridgeStateEvent>& events) const;
    void update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                const game::terrain::TerrainLogic& terrain,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick,
                container::Vector<ObjectBridgeStateEvent>& events,
                container::Vector<ObjectRailedTransportDockAttachCompletion>&
                    dockCompletions) const;
    void update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                const game::terrain::TerrainLogic& terrain,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick,
                container::Vector<ObjectBridgeStateEvent>& events,
                uint64_t& nextGameplaySubmissionOrdinal,
                container::Vector<ObjectRailedTransportDockAttachCompletion>&
                    dockCompletions,
                container::Vector<ObjectRailroadCarriageSpawnRequest>&
                    carriageSpawns,
                container::Vector<ObjectRailroadDisembarkRequest>&
                    disembarks,
                container::Vector<ObjectDamageRequest>& railroadDamage,
                container::Vector<ObjectRailroadPresentationEvent>&
                    presentationEvents) const;
};

} // namespace engine
