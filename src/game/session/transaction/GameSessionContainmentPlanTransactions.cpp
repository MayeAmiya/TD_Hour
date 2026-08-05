#include "game/session/transaction/GameSessionContainmentPlanTransactions.h"

#include "game/session/state/GameSessionDomainState.h"
#include "game/session/transaction/GameSessionObjectDamageTransactions.h"

#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace engine {
namespace {

[[nodiscard]] bool hasObjectKind(
    const ObjectKindOfComponent* kinds, game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

} // namespace

GameSessionContainmentPlanTransactions::GameSessionContainmentPlanTransactions(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation,
    GameSessionObjectDamageTransactions* damage) noexcept
    : m_content(content),
      m_world(world),
      m_ai(ai),
      m_presentation(presentation),
      m_damage(damage) {}

bool GameSessionContainmentPlanTransactions::acceptsConfirmedTick(
    uint64_t confirmedTick) const noexcept {
    return m_content.m_active && m_presentation.m_hasConfirmedFrame &&
        confirmedTick == m_presentation.m_confirmedTick;
}

GameSessionContainmentTransactions
GameSessionContainmentPlanTransactions::atomic() noexcept {
    return {
        m_world.m_registry,
        m_world.m_objects,
        m_world.m_objectSimulation,
        m_world.m_spatialIndex,
        m_world.m_objectTeams,
        m_content.m_players,
        m_content.m_contentSnapshot,
    };
}

size_t GameSessionContainmentPlanTransactions::requestTeamCaptureNearestUnmanned(
    ObjectTeamId team, uint32_t sourceSequence, uint64_t confirmedTick) {
    if (!acceptsConfirmedTick(confirmedTick) || !m_world.m_objectTeams.find(team)) {
        return 0;
    }
    const std::optional<PlayerId> owner = m_world.m_objectTeams.teamOwner(team);
    if (!owner || !m_content.m_players.get(*owner)) return 0;

    const container::Span<const ObjectId> members =
        m_world.m_objectTeams.legacyMembers(team);
    if (members.empty()) return 0;

    math::q32_32 centerX{};
    math::q32_32 centerY{};
    size_t centerCount = 0;
    const auto accumulateCenter = [&](bool aiOnly) {
        for (const ObjectId member : members) {
            const std::optional<ecs::entity> entity =
                m_world.m_objects.entityFromId(member);
            if (!entity || m_world.m_objects.isPendingDestroy(member) ||
                isObjectDisabledBy(
                    m_world.m_registry, *entity, ObjectDisabledReason::Held,
                    confirmedTick) ||
                (aiOnly && !m_ai.m_objectAI.find(member))) {
                continue;
            }
            const TransformComponent* transform =
                ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
            if (!transform) continue;
            const LogicFixedVec3 position = readAuthoritativeObjectPosition(
                m_world.m_registry, *entity, *transform);
            centerX += position.x;
            centerY += position.y;
            ++centerCount;
        }
    };

    // AIGroup::getCenter first considers only non-Held objects which expose
    // AIUpdateInterface. A structure-only group falls back to every non-Held
    // member rather than producing a different search origin.
    accumulateCenter(true);
    if (centerCount == 0) {
        centerX = {};
        centerY = {};
        accumulateCenter(false);
    }
    if (centerCount == 0) return 0;
    const math::q32_32 centerDivisor{static_cast<int32_t>(std::min<size_t>(
        centerCount, static_cast<size_t>(std::numeric_limits<int32_t>::max())))};
    centerX /= centerDivisor;
    centerY /= centerDivisor;

    ObjectId selected = INVALID_OBJECT_ID;
    math::q32_32 bestDistanceSquared = math::q32_32::from_raw(
        std::numeric_limits<int64_t>::max());
    for (const ObjectSpatialRecord& record : m_world.m_spatialIndex.records()) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(record.object);
        if (!entity || m_world.m_objects.isPendingDestroy(record.object)) continue;
        const ObjectMapStatusComponent* mapStatus =
            ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, *entity);
        if ((mapStatus && mapStatus->offMap) ||
            ecs::try_get<ObjectContainedByComponent>(m_world.m_registry, *entity) ||
            !isObjectDisabledBy(
                m_world.m_registry, *entity, ObjectDisabledReason::Unmanned,
                confirmedTick)) {
            continue;
        }
        const PlayerRelationship relationship =
            relationshipBetweenPlayerAndObject(
                m_world.m_registry, m_content.m_players, *owner, *entity);
        if (relationship != PlayerRelationship::Enemies &&
            relationship != PlayerRelationship::Neutral) {
            continue;
        }

        const math::q32_32 dx = record.position.x - centerX;
        const math::q32_32 dy = record.position.y - centerY;
        const math::q32_32 distanceSquared = dx * dx + dy * dy;
        if (distanceSquared < bestDistanceSquared ||
            (distanceSquared == bestDistanceSquared &&
             (!selected || record.object < selected))) {
            selected = record.object;
            bestDistanceSquared = distanceSquared;
        }
    }
    if (!selected) return 0;

    // RefCode selects the nearest affiliation/unmanned/on-map Object before
    // groupEnter. Do not make target selection depend on current capacity or
    // canContain(); the established Enter path performs per-member admission.
    return atomic().requestTeamEnter(
        team, selected, false, sourceSequence, confirmedTick);
}

size_t GameSessionContainmentPlanTransactions::requestTeamGarrisonNearest(
    ObjectTeamId team, uint32_t sourceSequence, uint64_t confirmedTick) {
    if (!acceptsConfirmedTick(confirmedTick) || !m_world.m_objectTeams.find(team))
        return 0;
    const container::Span<const ObjectId> members =
        m_world.m_objectTeams.legacyMembers(team);
    if (members.empty()) return 0;
    const std::optional<ecs::entity> leader =
        m_world.m_objects.entityFromId(members.front());
    const TransformComponent* leaderTransform = leader
        ? ecs::try_get<TransformComponent>(m_world.m_registry, *leader)
        : nullptr;
    const ObjectKindOfComponent* leaderKinds = leader
        ? ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *leader)
        : nullptr;
    const ObjectMapStatusComponent* leaderMap = leader
        ? ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, *leader)
        : nullptr;
    if (!leaderTransform) return 0;
    const LogicFixedVec3 leaderPosition = readAuthoritativeObjectPosition(
        m_world.m_registry, *leader, *leaderTransform);
    const bool hackerTeam = hasObjectKind(
        leaderKinds, game::ObjectKindOf::MoneyHacker);

    struct Candidate final {
        ObjectId building = INVALID_OBJECT_ID;
        uint32_t remaining = 0;
        math::q32_32 distanceSquared{};
    };
    container::HashMap<ObjectId, uint64_t> pendingByBuilding;
    const auto pendingView = ecs::view<
        const ObjectScriptContainmentEnterComponent>(m_world.m_registry);
    pendingByBuilding.reserve(pendingView.size());
    for (const ecs::entity pendingEntity : pendingView) {
        const ObjectScriptContainmentEnterComponent& pending =
            pendingView.template get<
                const ObjectScriptContainmentEnterComponent>(pendingEntity);
        if (pending.target) {
            pendingByBuilding[pending.target] +=
                std::max<uint32_t>(1u, pending.reservedCapacity);
        }
    }
    container::Vector<Candidate> candidates;
    for (const ObjectSpatialRecord& record : m_world.m_spatialIndex.records()) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(record.object);
        const ObjectKindOfComponent* kinds = entity
            ? ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *entity)
            : nullptr;
        const ObjectContainmentRuntimeComponent* runtime = entity
            ? ecs::try_get<ObjectContainmentRuntimeComponent>(
                  m_world.m_registry, *entity)
            : nullptr;
        const TransformComponent* transform = entity
            ? ecs::try_get<TransformComponent>(m_world.m_registry, *entity)
            : nullptr;
        const ObjectMapStatusComponent* candidateMap = entity
            ? ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, *entity)
            : nullptr;
        if (!entity || !transform || !runtime || !runtime->plan ||
            !hasObjectKind(kinds, game::ObjectKindOf::Structure) ||
            ((leaderMap && leaderMap->offMap) !=
             (candidateMap && candidateMap->offMap)) ||
            (hackerTeam != hasObjectKind(
                kinds, game::ObjectKindOf::FsInternetCenter)))
            continue;
        uint32_t capacity = 0;
        size_t garrisonRuleIndex = std::numeric_limits<size_t>::max();
        for (size_t index = 0; index < runtime->plan->rules.size(); ++index) {
            const ObjectContainmentRule& rule = runtime->plan->rules[index];
            if (rule.kind == ObjectContainmentKind::Garrison) {
                capacity = rule.containMax;
                garrisonRuleIndex = index;
                break;
            }
        }
        const ObjectContainmentComponent* contents =
            ecs::try_get<ObjectContainmentComponent>(m_world.m_registry, *entity);
        uint64_t used = 0;
        if (contents &&
            garrisonRuleIndex != std::numeric_limits<size_t>::max()) {
            for (const ObjectContainedObjectRecord& occupant :
                 contents->objects) {
                const std::optional<ecs::entity> occupantEntity =
                    m_world.m_objects.entityFromId(occupant.object);
                const ObjectContainedByComponent* edge = occupantEntity
                    ? ecs::try_get<ObjectContainedByComponent>(
                          m_world.m_registry, *occupantEntity)
                    : nullptr;
                if (edge && edge->containmentRuleIndex ==
                                garrisonRuleIndex) ++used;
            }
        }
        const auto pending = pendingByBuilding.find(record.object);
        const uint64_t reserved = pending == pendingByBuilding.end()
            ? 0u : pending->second;
        const uint64_t unavailable = used + reserved;
        const math::q32_32 dx = record.position.x - leaderPosition.x;
        const math::q32_32 dy = record.position.y - leaderPosition.y;
        const math::q32_32 dz = record.position.z - leaderPosition.z;
        candidates.push_back({
            .building = record.object,
            .remaining = unavailable >= capacity ? 0u
                : static_cast<uint32_t>(
                    static_cast<uint64_t>(capacity) - unavailable),
            .distanceSquared = dx * dx + dy * dy + dz * dz,
        });
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.distanceSquared != right.distanceSquared
                ? left.distanceSquared < right.distanceSquared
                : left.building < right.building;
        });

    auto commits = atomic();
    size_t accepted = 0;
    uint32_t sequence = sourceSequence == 0 ? 1u : sourceSequence;
    for (const ObjectId member : members) {
        const std::optional<ecs::entity> memberEntity =
            m_world.m_objects.entityFromId(member);
        const ObjectKindOfComponent* memberKinds = memberEntity
            ? ecs::try_get<ObjectKindOfComponent>(
                  m_world.m_registry, *memberEntity)
            : nullptr;
        // RefCode's team-nearest action advances every member but only spends
        // building capacity for infantry which are not NO_GARRISON.
        if (!memberEntity ||
            !hasObjectKind(memberKinds, game::ObjectKindOf::Infantry) ||
            hasObjectKind(memberKinds, game::ObjectKindOf::NoGarrison)) {
            if (sequence != std::numeric_limits<uint32_t>::max()) ++sequence;
            continue;
        }
        const ObjectScriptContainmentEnterComponent* previous =
            ecs::try_get<ObjectScriptContainmentEnterComponent>(
                m_world.m_registry, *memberEntity);
        Candidate* previousCandidate = nullptr;
        uint32_t previousReservation = 0;
        if (previous) {
            const auto found = std::find_if(
                candidates.begin(), candidates.end(),
                [previous](const Candidate& candidate) {
                    return candidate.building == previous->target;
                });
            if (found != candidates.end()) {
                previousCandidate = &*found;
                previousReservation = std::max<uint32_t>(
                    1u, previous->reservedCapacity);
                const uint64_t restored =
                    static_cast<uint64_t>(found->remaining) +
                    previousReservation;
                found->remaining = static_cast<uint32_t>(std::min<uint64_t>(
                    restored, std::numeric_limits<uint32_t>::max()));
            }
        }
        bool assigned = false;
        for (Candidate& candidate : candidates) {
            if (candidate.remaining == 0) continue;
            if (commits.requestObjectGarrison(
                    member, candidate.building, sequence,
                    confirmedTick)) {
                --candidate.remaining;
                ++accepted;
                assigned = true;
                break;
            }
        }
        if (!assigned && previousCandidate) {
            previousCandidate->remaining = previousCandidate->remaining >
                    previousReservation
                ? previousCandidate->remaining - previousReservation
                : 0u;
        }
        if (sequence != std::numeric_limits<uint32_t>::max()) ++sequence;
    }
    return accepted;
}

size_t GameSessionContainmentPlanTransactions::requestPlayerGarrisonAll(
    PlayerId player, uint32_t sourceSequence, uint64_t confirmedTick) {
    if (!acceptsConfirmedTick(confirmedTick) || !m_content.m_players.get(player))
        return 0;
    struct Candidate final {
        ObjectId building = INVALID_OBJECT_ID;
        uint32_t remaining = 0;
    };
    container::HashMap<ObjectId, uint64_t> pendingByBuilding;
    const auto pendingView = ecs::view<
        const ObjectScriptContainmentEnterComponent>(m_world.m_registry);
    pendingByBuilding.reserve(pendingView.size());
    for (const ecs::entity pendingEntity : pendingView) {
        const ObjectScriptContainmentEnterComponent& pending =
            pendingView.template get<
                const ObjectScriptContainmentEnterComponent>(pendingEntity);
        if (pending.target) {
            pendingByBuilding[pending.target] +=
                std::max<uint32_t>(1u, pending.reservedCapacity);
        }
    }
    container::Vector<Candidate> candidates;
    const auto buildings = ecs::view<
        const ObjectIdentityComponent,
        const ObjectKindOfComponent,
        const ObjectContainmentRuntimeComponent>(m_world.m_registry);
    for (const ecs::entity entity : buildings) {
        const ObjectIdentityComponent& identity = buildings.template get<
            const ObjectIdentityComponent>(entity);
        const ObjectKindOfComponent& kinds = buildings.template get<
            const ObjectKindOfComponent>(entity);
        const ObjectContainmentRuntimeComponent& runtime =
            buildings.template get<
                const ObjectContainmentRuntimeComponent>(entity);
        if (!identity.id ||
            !hasObjectKind(&kinds, game::ObjectKindOf::Structure) ||
            !runtime.plan) continue;
        uint32_t capacity = 0;
        size_t garrisonRuleIndex = std::numeric_limits<size_t>::max();
        for (size_t index = 0; index < runtime.plan->rules.size(); ++index) {
            const ObjectContainmentRule& rule = runtime.plan->rules[index];
            if (rule.kind == ObjectContainmentKind::Garrison) {
                capacity = rule.containMax;
                garrisonRuleIndex = index;
                break;
            }
        }
        const ObjectContainmentComponent* contents =
            ecs::try_get<ObjectContainmentComponent>(m_world.m_registry, entity);
        uint64_t used = 0;
        if (contents &&
            garrisonRuleIndex != std::numeric_limits<size_t>::max()) {
            for (const ObjectContainedObjectRecord& occupant :
                 contents->objects) {
                const std::optional<ecs::entity> occupantEntity =
                    m_world.m_objects.entityFromId(occupant.object);
                const ObjectContainedByComponent* edge = occupantEntity
                    ? ecs::try_get<ObjectContainedByComponent>(
                          m_world.m_registry, *occupantEntity)
                    : nullptr;
                if (edge && edge->containmentRuleIndex ==
                                garrisonRuleIndex) ++used;
            }
        }
        const auto pending = pendingByBuilding.find(identity.id);
        const uint64_t reserved = pending == pendingByBuilding.end()
            ? 0u : pending->second;
        const uint64_t unavailable = used + reserved;
        if (capacity == 0) continue;
        candidates.push_back({
            .building = identity.id,
            .remaining = unavailable >= capacity ? 0u
                : static_cast<uint32_t>(
                    static_cast<uint64_t>(capacity) - unavailable),
        });
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.building < right.building;
        });

    auto commits = atomic();
    const container::Span<const ObjectId> owned =
        m_world.m_ownership.objects(player);
    container::Vector<ObjectId> actors(owned.begin(), owned.end());
    std::sort(actors.begin(), actors.end());
    size_t accepted = 0;
    uint32_t sequence = sourceSequence == 0 ? 1u : sourceSequence;
    for (const ObjectId object : actors) {
        const std::optional<ecs::entity> actor =
            m_world.m_objects.entityFromId(object);
        const TransformComponent* actorTransform = actor
            ? ecs::try_get<TransformComponent>(m_world.m_registry, *actor)
            : nullptr;
        if (!actorTransform) continue;
        const LogicFixedVec3 actorPosition = readAuthoritativeObjectPosition(
            m_world.m_registry, *actor, *actorTransform);
        const ObjectScriptContainmentEnterComponent* previous =
            ecs::try_get<ObjectScriptContainmentEnterComponent>(
                m_world.m_registry, *actor);
        Candidate* previousCandidate = nullptr;
        uint32_t previousReservation = 0;
        if (previous) {
            const auto found = std::find_if(
                candidates.begin(), candidates.end(),
                [previous](const Candidate& candidate) {
                    return candidate.building == previous->target;
                });
            if (found != candidates.end()) {
                previousCandidate = &*found;
                previousReservation = std::max<uint32_t>(
                    1u, previous->reservedCapacity);
                const uint64_t restored =
                    static_cast<uint64_t>(found->remaining) +
                    previousReservation;
                found->remaining = static_cast<uint32_t>(std::min<uint64_t>(
                    restored, std::numeric_limits<uint32_t>::max()));
            }
        }
        container::Vector<size_t> order;
        order.reserve(candidates.size());
        for (size_t index = 0; index < candidates.size(); ++index) {
            if (candidates[index].remaining != 0) order.push_back(index);
        }
        std::sort(order.begin(), order.end(), [&](size_t left, size_t right) {
            const auto distance = [&](size_t index) {
                const std::optional<ecs::entity> building =
                    m_world.m_objects.entityFromId(candidates[index].building);
                const TransformComponent* transform = building
                    ? ecs::try_get<TransformComponent>(
                          m_world.m_registry, *building)
                    : nullptr;
                if (!transform) {
                    return math::q32_32::from_raw(
                        std::numeric_limits<int64_t>::max());
                }
                const LogicFixedVec3 buildingPosition =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry, *building, *transform);
                const math::q32_32 dx =
                    buildingPosition.x - actorPosition.x;
                const math::q32_32 dy =
                    buildingPosition.y - actorPosition.y;
                const math::q32_32 dz =
                    buildingPosition.z - actorPosition.z;
                return dx * dx + dy * dy + dz * dz;
            };
            const math::q32_32 leftDistance = distance(left);
            const math::q32_32 rightDistance = distance(right);
            return leftDistance != rightDistance
                ? leftDistance < rightDistance
                : candidates[left].building < candidates[right].building;
        });
        bool assigned = false;
        for (const size_t index : order) {
            if (commits.requestObjectGarrison(
                    object, candidates[index].building, sequence,
                    confirmedTick)) {
                --candidates[index].remaining;
                ++accepted;
                assigned = true;
                break;
            }
        }
        if (!assigned && previousCandidate) {
            previousCandidate->remaining = previousCandidate->remaining >
                    previousReservation
                ? previousCandidate->remaining - previousReservation
                : 0u;
        }
        if (sequence != std::numeric_limits<uint32_t>::max()) ++sequence;
    }
    return accepted;
}

size_t GameSessionContainmentPlanTransactions::requestTeamLoadTransports(
    ObjectTeamId team, uint32_t sourceSequence, uint64_t confirmedTick) {
    if (!acceptsConfirmedTick(confirmedTick) || !m_world.m_objectTeams.find(team))
        return 0;
    const auto slotCount = [&](ecs::entity entity) {
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, entity);
        return type && type->archetype
            ? std::max<uint32_t>(
                  1u, type->archetype->templateData.transportSlotCount)
            : 1u;
    };
    struct Transport final {
        ObjectId object = INVALID_OBJECT_ID;
        uint32_t remaining = 0;
    };
    container::Vector<Transport> transports;
    container::Vector<ObjectId> passengers;
    container::HashMap<ObjectId, uint64_t> pendingByTransport;
    const auto pendingView = ecs::view<
        const ObjectScriptContainmentEnterComponent>(m_world.m_registry);
    pendingByTransport.reserve(pendingView.size());
    for (const ecs::entity pendingEntity : pendingView) {
        const ObjectScriptContainmentEnterComponent& pending =
            pendingView.template get<
                const ObjectScriptContainmentEnterComponent>(pendingEntity);
        if (pending.target) {
            pendingByTransport[pending.target] +=
                std::max<uint32_t>(1u, pending.reservedCapacity);
        }
    }
    for (const ObjectId member : m_world.m_objectTeams.legacyMembers(team)) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(member);
        const ObjectKindOfComponent* kinds = entity
            ? ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *entity)
            : nullptr;
        if (!entity) continue;
        if (!hasObjectKind(kinds, game::ObjectKindOf::Transport)) {
            passengers.push_back(member);
            continue;
        }
        const ObjectContainmentRuntimeComponent* runtime =
            ecs::try_get<ObjectContainmentRuntimeComponent>(
                m_world.m_registry, *entity);
        if (!runtime || !runtime->plan) continue;
        uint32_t capacity = 0;
        size_t transportRuleIndex = std::numeric_limits<size_t>::max();
        for (size_t index = 0; index < runtime->plan->rules.size(); ++index) {
            const ObjectContainmentRule& rule = runtime->plan->rules[index];
            if (rule.kind == ObjectContainmentKind::Transport) {
                capacity = rule.containMax;
                transportRuleIndex = index;
                break;
            }
        }
        uint64_t used = 0;
        if (const ObjectContainmentComponent* contents =
                ecs::try_get<ObjectContainmentComponent>(
                    m_world.m_registry, *entity)) {
            for (const ObjectContainedObjectRecord& record :
                 contents->objects) {
                const std::optional<ecs::entity> occupant =
                    m_world.m_objects.entityFromId(record.object);
                const ObjectContainedByComponent* edge = occupant
                    ? ecs::try_get<ObjectContainedByComponent>(
                          m_world.m_registry, *occupant)
                    : nullptr;
                if (occupant && edge && edge->containmentRuleIndex ==
                                            transportRuleIndex)
                    used += slotCount(*occupant);
            }
        }
        const auto pending = pendingByTransport.find(member);
        if (pending != pendingByTransport.end()) used += pending->second;
        transports.push_back({
            .object = member,
            .remaining = used >= capacity
                ? 0u : capacity - static_cast<uint32_t>(used),
        });
    }

    // RefCode's PartitionSolver orders both sides largest-first before its
    // stable first-fit pass. Preserve legacy Team order for equal sizes.
    std::stable_sort(transports.begin(), transports.end(),
        [](const Transport& left, const Transport& right) {
            return left.remaining > right.remaining;
        });
    std::stable_sort(passengers.begin(), passengers.end(),
        [&](ObjectId left, ObjectId right) {
            const std::optional<ecs::entity> leftEntity =
                m_world.m_objects.entityFromId(left);
            const std::optional<ecs::entity> rightEntity =
                m_world.m_objects.entityFromId(right);
            const uint32_t leftSlots = leftEntity ? slotCount(*leftEntity) : 0u;
            const uint32_t rightSlots = rightEntity ? slotCount(*rightEntity) : 0u;
            return leftSlots > rightSlots;
        });

    auto commits = atomic();
    size_t accepted = 0;
    uint32_t sequence = sourceSequence == 0 ? 1u : sourceSequence;
    for (const ObjectId passenger : passengers) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(passenger);
        if (!entity) continue;
        const uint32_t required = slotCount(*entity);
        const ObjectScriptContainmentEnterComponent* previous =
            ecs::try_get<ObjectScriptContainmentEnterComponent>(
                m_world.m_registry, *entity);
        Transport* previousTransport = nullptr;
        uint32_t previousReservation = 0;
        if (previous) {
            const auto found = std::find_if(
                transports.begin(), transports.end(),
                [previous](const Transport& transport) {
                    return transport.object == previous->target;
                });
            if (found != transports.end()) {
                previousTransport = &*found;
                previousReservation = std::max<uint32_t>(
                    1u, previous->reservedCapacity);
                const uint64_t restored =
                    static_cast<uint64_t>(found->remaining) +
                    previousReservation;
                found->remaining = static_cast<uint32_t>(std::min<uint64_t>(
                    restored, std::numeric_limits<uint32_t>::max()));
            }
        }
        bool assigned = false;
        for (Transport& transport : transports) {
            if (transport.remaining < required) continue;
            if (commits.requestObjectEnter(
                    passenger, transport.object, sequence,
                    confirmedTick, required)) {
                transport.remaining -= required;
                ++accepted;
                assigned = true;
                break;
            }
        }
        if (!assigned && previousTransport) {
            previousTransport->remaining = previousTransport->remaining >
                    previousReservation
                ? previousTransport->remaining - previousReservation
                : 0u;
        }
        if (sequence != std::numeric_limits<uint32_t>::max()) ++sequence;
    }
    return accepted;
}

size_t GameSessionContainmentPlanTransactions::killContainedObjects(
    ObjectId host, uint32_t sourceSequence, uint64_t confirmedTick) {
    if (!acceptsConfirmedTick(confirmedTick) || !host || !m_damage) return 0;
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(host);
    const ObjectContainmentComponent* containment = entity
        ? ecs::try_get<ObjectContainmentComponent>(m_world.m_registry, *entity)
        : nullptr;
    if (!containment) return 0;
    container::Vector<ObjectId> occupants;
    occupants.reserve(containment->objects.size());
    for (const ObjectContainedObjectRecord& record : containment->objects) {
        if (record.object) occupants.push_back(record.object);
    }
    size_t killed = 0;
    for (const ObjectId occupant : occupants) {
        killed += m_damage->queueObjectDamage({
            .target = occupant,
            .source = INVALID_OBJECT_ID,
            .sourceSequence = sourceSequence,
            .amount = math::q32_32{},
            .damageType = game::DamageType::UNRESISTABLE,
            .deathType = game::DeathType::NORMAL,
            .forceKill = true,
            .confirmedTick = confirmedTick,
        }) ? 1u : 0u;
    }
    if (killed != 0) m_damage->resolveQueuedObjectDamage();
    return killed;
}

} // namespace engine
