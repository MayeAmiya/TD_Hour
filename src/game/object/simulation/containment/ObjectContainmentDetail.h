#pragma once

#include "game/object/simulation/containment/ObjectContainment.h"

#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"

namespace engine::object_containment_detail {

struct ContainmentUpdateCandidate final {
    ObjectId container = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

struct NetworkPassengerRecord final {
    ObjectContainedObjectRecord record;
    ObjectId entrance = INVALID_OBJECT_ID;
    ecs::entity entranceEntity = ecs::null;
    uint32_t ruleIndex = std::numeric_limits<uint32_t>::max();
};

[[nodiscard]] bool railedTransportDockAllowsContainment(
    const ecs::registry& registry, ecs::entity container) noexcept;
[[nodiscard]] bool railedTransportDockAcceptsObject(
    const ecs::registry& registry, ecs::entity container,
    ecs::entity object) noexcept;
[[nodiscard]] bool hasContainInterface(
    const ecs::registry& registry, ecs::entity entity) noexcept;
[[nodiscard]] bool wouldCreateCycle(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId container, ObjectId object) noexcept;
void bumpRevision(ObjectContainmentComponent& component) noexcept;
[[nodiscard]] container::Vector<container::String> objectKindTokens(
    const ecs::registry& registry, ecs::entity entity);
[[nodiscard]] bool templateMatchesRider(
    const ThingTemplateComponent* passenger,
    container::StringView authoredTemplate) noexcept;
[[nodiscard]] const ObjectContainmentRule* selectContainmentRule(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity container, ecs::entity object, bool force,
    const PlayerRegistry* players = nullptr);
[[nodiscard]] bool isCompletedContainmentEntrance(
    const ecs::registry& registry, ecs::entity entity) noexcept;
[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept;
[[nodiscard]] uint64_t saturatingAddTicks(
    uint64_t tick, uint64_t delay) noexcept;
[[nodiscard]] const ObjectContainmentRule* findContainmentRuleForObject(
    const ecs::registry& registry, ecs::entity container,
    ecs::entity object);
[[nodiscard]] bool sameContainmentNetwork(
    const ecs::registry& registry, ecs::entity source,
    const ObjectContainmentRuntimeComponent& sourceRuntime,
    ObjectContainmentKind kind, ecs::entity candidate,
    const ObjectContainmentRuntimeComponent& candidateRuntime) noexcept;
[[nodiscard]] container::Vector<NetworkPassengerRecord>
collectNetworkPassengers(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity source, const ObjectContainmentRuntimeComponent& sourceRuntime,
    ObjectContainmentKind kind);
void collectNetworkPassengers(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    container::Span<const ContainmentUpdateCandidate> entrances,
    ObjectContainmentKind kind,
    container::Vector<NetworkPassengerRecord>& out);
[[nodiscard]] bool liveTunnelNemesis(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object) noexcept;
[[nodiscard]] bool visibleTunnelNemesis(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object) noexcept;
void applyTransportExitPolicy(
    ecs::registry& registry, ecs::entity container, ecs::entity object,
    const ObjectContainmentRule& rule,
    const ObjectContainmentExitPath* precisePath,
    uint32_t exitPath, uint32_t ruleIndex,
    uint32_t firstCommandSequence, uint32_t secondCommandSequence,
    uint64_t confirmedTick);
void projectContainmentDoorTransition(
    ecs::registry& registry, ecs::entity container, bool opening,
    uint64_t confirmedTick);
[[nodiscard]] size_t behaviorRuleIndex(
    const ObjectContainmentRuntimeComponent& runtime,
    ObjectTransportBehaviorKind kind,
    uint32_t authoredOrder = std::numeric_limits<uint32_t>::max()) noexcept;
[[nodiscard]] ObjectTransportBehaviorKind requestBehaviorKind(
    ObjectTransportBehaviorRequestKind kind) noexcept;
[[nodiscard]] inline uint64_t reserveTransportGameplayOrdinal(
    uint64_t& nextGameplaySubmissionOrdinal) noexcept {
    const uint64_t result = nextGameplaySubmissionOrdinal++;
    if (nextGameplaySubmissionOrdinal == 0) {
        ++nextGameplaySubmissionOrdinal;
    }
    return result;
}

template <typename Event>
Event& pushTransportGameplayTransaction(
    ObjectTransportEventStream& events, Event event,
    uint64_t& nextGameplaySubmissionOrdinal) {
    events.gameplay.push_back({
        .payload = std::move(event),
        .submissionOrdinal = reserveTransportGameplayOrdinal(
            nextGameplaySubmissionOrdinal),
    });
    return std::get<Event>(events.gameplay.back().payload);
}

template <typename Event>
Event& pushTransportPresentationEvent(
    ObjectTransportEventStream& events, Event event) {
    events.presentation.push_back({.payload = std::move(event)});
    return std::get<Event>(events.presentation.back().payload);
}
void pushTransportOclTransaction(
    ObjectTransportEventStream& events, const ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick, uint32_t authoredOrder,
    container::String payload, uint32_t sourcePathfindLayer,
    uint64_t& nextGameplaySubmissionOrdinal);
[[nodiscard]] ObjectTransportOclTransaction freezeTransportOclTransaction(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick, uint32_t authoredOrder,
    container::String payload, uint32_t sourcePathfindLayer);
[[nodiscard]] math::q32_32 deterministicVariance(
    math::q32_32 extent, uint64_t seed) noexcept;
void setHijackerStatus(
    ecs::registry& registry, ecs::entity entity, bool attached,
    uint64_t confirmedTick);
void setSystemMoveOrder(
    ecs::registry& registry, ecs::entity entity, ObjectId target,
    const LogicFixedVec3& position, ObjectOrderSystemPurpose purpose,
    uint32_t instance, uint64_t confirmedTick);
void clearSystemOrder(
    ecs::registry& registry, ecs::entity entity,
    ObjectOrderSystemPurpose purpose, uint32_t instance);
void setSystemAttackOrder(
    ecs::registry& registry, ecs::entity entity, ObjectId target,
    const LogicFixedVec3& position, bool hasPosition, bool attackMove,
    ObjectOrderSystemPurpose purpose, uint32_t instance,
    uint64_t confirmedTick);
// Replaces a system attack intent without pre-empting egress waypoints
// installed by TransportContain/OpenContain.  Assault transports use this
// after detaching passengers so they first clear the host, then acquire the
// original assault target.
void setSystemAttackOrderAfterContainmentExit(
    ecs::registry& registry, ecs::entity entity, ObjectId target,
    const LogicFixedVec3& position, bool hasPosition, bool attackMove,
    ObjectOrderSystemPurpose purpose, uint32_t instance,
    uint64_t confirmedTick);
[[nodiscard]] bool hasKind(
    const ObjectKindOfComponent* kinds, game::ObjectKindOf kind) noexcept;
void pushContainmentEvent(
    container::Vector<ObjectContainmentEvent>& events,
    ObjectContainmentRequestKind kind, ObjectId container, ObjectId object,
    uint64_t confirmedTick, bool accepted,
    bool exposeStealthUnits = false,
    ObjectId parachuteLandingTransport = INVALID_OBJECT_ID);
void synchronizeOne(
    ecs::registry& registry, ecs::entity object,
    ecs::entity container,
    const GameContentSnapshot* content = nullptr) noexcept;

void updateTransportBehaviors(
    const ObjectContainmentSystem& system,
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage,
    const ContainmentUpdateCandidate& candidate,
    ObjectContainmentRuntimeComponent& runtime,
    const ObjectContainmentComponent& contents,
    container::Vector<ObjectContainmentEvent>* containmentEvents,
    ObjectTransportEventStream* behaviorEvents,
    uint64_t& nextGameplaySubmissionOrdinal,
    const PlayerRegistry* players,
    const game::terrain::TerrainLogic* terrain, uint32_t fps);

} // namespace engine::object_containment_detail
