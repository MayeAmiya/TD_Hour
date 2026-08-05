#include "game/session/transaction/GameSessionContainmentTransactions.h"

#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectTeamRegistry.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/ai/runtime/ObjectAIRuntime.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/session/query/ObjectContainmentQuery.h"

#include <algorithm>
#include <limits>

namespace engine {
namespace {

[[nodiscard]] bool hasObjectKind(
    const ObjectKindOfComponent* kinds, game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

} // namespace

GameSessionContainmentTransactions::GameSessionContainmentTransactions(
    ecs::registry& registry, ObjectLifecycle& objects,
    ObjectSimulation& simulation, ObjectSpatialIndex& spatialIndex,
    ObjectTeamRegistry& objectTeams, PlayerRegistry& players,
    const GameContentSnapshot& content) noexcept
    : m_registry(registry),
      m_objects(objects),
      m_simulation(simulation),
      m_spatialIndex(spatialIndex),
      m_objectTeams(objectTeams),
      m_players(players),
      m_content(content) {}

bool GameSessionContainmentTransactions::request(
    ObjectContainmentRequest request) {
    if (request.kind == ObjectContainmentRequestKind::Detach &&
        !request.container && request.object) {
        const std::optional<ecs::entity> entity =
            m_objects.entityFromId(request.object);
        const ObjectContainedByComponent* contained = entity
            ? ecs::try_get<ObjectContainedByComponent>(m_registry, *entity)
            : nullptr;
        if (!contained || !contained->container) return false;
        request.container = contained->container;
    }
    return m_simulation.requestContainment(
        m_registry, m_objects, request, &m_players, &m_content);
}

bool GameSessionContainmentTransactions::setEvacuationDisposition(
    ObjectId container,
    ObjectContainmentEvacuationDisposition disposition) {
    if (!container) return false;
    const std::optional<ecs::entity> entity =
        m_objects.entityFromId(container);
    ObjectContainmentRuntimeComponent* runtime = entity
        ? ecs::try_get<ObjectContainmentRuntimeComponent>(m_registry, *entity)
        : nullptr;
    if (!runtime || !runtime->plan || !std::any_of(
            runtime->plan->rules.begin(), runtime->plan->rules.end(),
            [](const ObjectContainmentRule& rule) {
                return rule.kind == ObjectContainmentKind::Garrison;
            })) {
        return false;
    }
    if (runtime->evacuationDisposition == disposition) return true;
    runtime->evacuationDisposition = disposition;
    ++runtime->evacuationDispositionRevision;
    if (runtime->evacuationDispositionRevision == 0)
        ++runtime->evacuationDispositionRevision;
    return true;
}

bool GameSessionContainmentTransactions::requestObjectEnter(
    ObjectId object, ObjectId container, uint32_t sourceSequence,
    uint64_t confirmedTick, uint32_t reservedCapacity) {
    if (!object || !container || object == container) return false;
    const std::optional<ecs::entity> entity = m_objects.entityFromId(object);
    const std::optional<ecs::entity> target =
        m_objects.entityFromId(container);
    if (!entity || !target ||
        !ecs::try_get<ObjectLocomotionComponent>(m_registry, *entity) ||
        ecs::try_get<ObjectContainedByComponent>(m_registry, *entity) ||
        !ecs::try_get<ObjectContainmentRuntimeComponent>(m_registry, *target) ||
        !m_simulation.canContain(
            m_registry, m_objects,
            {.kind = ObjectContainmentRequestKind::Attach,
             .container = container,
             .object = object,
             .confirmedTick = confirmedTick},
            &m_players)) {
        return false;
    }
    if (reservedCapacity == 0) {
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_registry, *entity);
        reservedCapacity = type && type->archetype
            ? std::max<uint32_t>(
                  1u, type->archetype->templateData.transportSlotCount)
            : 1u;
    }
    const TransformComponent* targetTransform =
        ecs::try_get<TransformComponent>(m_registry, *target);
    if (!targetTransform) return false;

    ObjectScriptContainmentEnterComponent* intent =
        ecs::try_get<ObjectScriptContainmentEnterComponent>(
            m_registry, *entity);
    const uint32_t sequence = sourceSequence == 0 ? 1u : sourceSequence;
    if (!intent) {
        intent = &ecs::emplace<ObjectScriptContainmentEnterComponent>(
            m_registry, *entity,
            ObjectScriptContainmentEnterComponent{
                .target = container,
                .issuedTick = confirmedTick,
                .sourceSequence = sequence,
                .reservedCapacity = reservedCapacity,
                .approachAttempts = 0,
            });
    } else {
        intent->target = container;
        intent->issuedTick = confirmedTick;
        intent->sourceSequence = sequence;
        intent->reservedCapacity = reservedCapacity;
        intent->approachAttempts = 0;
        ++intent->revision;
        if (intent->revision == 0) ++intent->revision;
    }

    ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(m_registry, *entity);
    if (!queue) {
        queue = &ecs::emplace<ObjectOrderQueueComponent>(m_registry, *entity);
    }
    const LogicFixedVec3 targetPosition = readAuthoritativeObjectPosition(
        m_registry, *target, *targetTransform);
    queue->orders.clear();
    queue->orders.push_back({
        .kind = ObjectOrderKind::Move,
        .source = ObjectOrderSource::System,
        .issuedTick = confirmedTick,
        .sourceSequence = sequence,
        .targetObject = container,
        .targetX = targetPosition.x,
        .targetY = targetPosition.y,
        .targetZ = targetPosition.z,
        .hasTargetPosition = true,
        .systemPurpose = ObjectOrderSystemPurpose::ContainmentEnter,
        .systemPurposeInstance = container.value,
    });
    ++queue->revision;
    ++queue->externalRevision;
    if (queue->externalRevision == 0) ++queue->externalRevision;
    return true;
}

bool GameSessionContainmentTransactions::requestObjectGarrison(
    ObjectId object, std::optional<ObjectId> building,
    uint32_t sourceSequence, uint64_t confirmedTick) {
    if (!object) return false;
    const std::optional<ecs::entity> actor = m_objects.entityFromId(object);
    const ObjectKindOfComponent* actorKinds = actor
        ? ecs::try_get<ObjectKindOfComponent>(m_registry, *actor)
        : nullptr;
    if (!actor) return false;
    const bool hacker =
        hasObjectKind(actorKinds, game::ObjectKindOf::MoneyHacker);
    const ObjectMapStatusComponent* actorMap =
        ecs::try_get<ObjectMapStatusComponent>(m_registry, *actor);

    container::HashMap<ObjectId, uint64_t> pendingByContainer;
    const auto pending = ecs::view<
        const ObjectIdentityComponent,
        const ObjectScriptContainmentEnterComponent>(m_registry);
    pendingByContainer.reserve(pending.size_hint());
    for (const ecs::entity pendingEntity : pending) {
        const ObjectIdentityComponent& pendingIdentity =
            pending.template get<const ObjectIdentityComponent>(pendingEntity);
        const ObjectScriptContainmentEnterComponent& intent =
            pending.template get<const ObjectScriptContainmentEnterComponent>(
                pendingEntity);
        if (pendingIdentity.id != object && intent.target) {
            pendingByContainer[intent.target] +=
                std::max<uint32_t>(1u, intent.reservedCapacity);
        }
    }

    const auto validBuilding = [&](ObjectId candidate) {
        const std::optional<ecs::entity> entity =
            m_objects.entityFromId(candidate);
        if (!entity) return false;
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(m_registry, *entity);
        if (!hasObjectKind(kinds, game::ObjectKindOf::Structure) ||
            (!building && hacker != hasObjectKind(
                 kinds, game::ObjectKindOf::FsInternetCenter))) {
            return false;
        }
        if (!building) {
            const ObjectMapStatusComponent* candidateMap =
                ecs::try_get<ObjectMapStatusComponent>(m_registry, *entity);
            const bool actorOffMap = actorMap && actorMap->offMap;
            const bool candidateOffMap = candidateMap && candidateMap->offMap;
            if (actorOffMap != candidateOffMap) return false;
        }
        const ObjectContainmentRuntimeComponent* runtime =
            ecs::try_get<ObjectContainmentRuntimeComponent>(
                m_registry, *entity);
        if (!runtime || !runtime->plan || !std::any_of(
                runtime->plan->rules.begin(), runtime->plan->rules.end(),
                [](const ObjectContainmentRule& rule) {
                    return rule.kind == ObjectContainmentKind::Garrison;
                })) {
            return false;
        }
        uint64_t capacity = 0;
        size_t garrisonRuleIndex = std::numeric_limits<size_t>::max();
        for (size_t index = 0; index < runtime->plan->rules.size(); ++index) {
            const ObjectContainmentRule& rule = runtime->plan->rules[index];
            if (rule.kind != ObjectContainmentKind::Garrison) continue;
            capacity = rule.containMax;
            garrisonRuleIndex = index;
            break;
        }
        const OwnerComponent* actorOwner =
            ecs::try_get<OwnerComponent>(m_registry, *actor);
        const ObjectContainmentComponent* contents =
            ecs::try_get<ObjectContainmentComponent>(m_registry, *entity);
        uint64_t used = 0;
        if (contents) {
            for (const ObjectContainedObjectRecord& record :
                 contents->objects) {
                const std::optional<ecs::entity> occupant =
                    m_objects.entityFromId(record.object);
                const ObjectContainedByComponent* edge = occupant
                    ? ecs::try_get<ObjectContainedByComponent>(
                          m_registry, *occupant)
                    : nullptr;
                if (!edge ||
                    edge->containmentRuleIndex != garrisonRuleIndex) {
                    continue;
                }
                ++used;
                const OwnerComponent* occupantOwner = occupant
                    ? ecs::try_get<OwnerComponent>(m_registry, *occupant)
                    : nullptr;
                if (!actorOwner || !occupantOwner ||
                    actorOwner->player != occupantOwner->player) {
                    return false;
                }
            }
        }
        const auto pendingReservation = pendingByContainer.find(candidate);
        const uint64_t reserved =
            pendingReservation == pendingByContainer.end()
            ? 0u
            : pendingReservation->second;
        if (used + reserved >= capacity) return false;
        return m_simulation.canContain(
            m_registry, m_objects,
            {.kind = ObjectContainmentRequestKind::Attach,
             .container = candidate,
             .object = object,
             .confirmedTick = confirmedTick},
            &m_players);
    };

    ObjectId selected = building.value_or(INVALID_OBJECT_ID);
    if (selected) {
        if (!validBuilding(selected)) return false;
    } else {
        const TransformComponent* actorTransform =
            ecs::try_get<TransformComponent>(m_registry, *actor);
        if (!actorTransform) return false;
        const LogicFixedVec3 actorPosition = readAuthoritativeObjectPosition(
            m_registry, *actor, *actorTransform);
        math::q32_32 bestDistance = math::q32_32::from_raw(
            std::numeric_limits<int64_t>::max());
        for (const ObjectSpatialRecord& record : m_spatialIndex.records()) {
            if (!record.object || !validBuilding(record.object)) continue;
            const math::q32_32 dx = record.position.x - actorPosition.x;
            const math::q32_32 dy = record.position.y - actorPosition.y;
            const math::q32_32 dz = record.position.z - actorPosition.z;
            const math::q32_32 distance = dx * dx + dy * dy + dz * dz;
            if (!selected || distance < bestDistance ||
                (distance == bestDistance && record.object < selected)) {
                selected = record.object;
                bestDistance = distance;
            }
        }
        if (!selected) return false;
    }
    return requestObjectEnter(
        object, selected, sourceSequence, confirmedTick, 1u);
}

bool GameSessionContainmentTransactions::requestPlayerExit(
    ObjectId container, ObjectId passenger, uint64_t confirmedTick,
    const ObjectOwnershipIndex& ownership,
    ai::ObjectAIRuntime& objectAI) {
    const std::optional<ecs::entity> entity =
        m_objects.entityFromId(passenger);
    ObjectOrderQueueComponent* queue = entity
        ? ecs::try_get<ObjectOrderQueueComponent>(m_registry, *entity)
        : nullptr;
    const std::optional<ecs::entity> containerEntity =
        m_objects.entityFromId(container);
    const ObjectContainmentRuntimeComponent* runtime = containerEntity
        ? ecs::try_get<ObjectContainmentRuntimeComponent>(
              m_registry, *containerEntity)
        : nullptr;
    if (!entity ||
        !session_query::canExitPassengerThrough(
            m_registry, m_objects, ownership, container, passenger) ||
        !containerEntity || !runtime || !runtime->plan ||
        isObjectDisabledBy(
            m_registry, *containerEntity,
            ObjectDisabledReason::Subdued, confirmedTick) ||
        m_objects.isPendingDestroy(container)) {
        return false;
    }
    uint64_t externalRevision = queue ? queue->externalRevision + 1u : 1u;
    if (externalRevision == 0) externalRevision = 1;
    const ai::ObjectAIContainmentTransitionResult staged =
        objectAI.stageContainmentExitState(
            passenger, container, ai::AIStateId::Exit, confirmedTick,
            externalRevision,
            {.kind = ai::AIContainmentFeedbackKind::ExitEntryReady,
             .goal = container,
             .goalHasContain = true,
             .goalHasExitInterface = true});
    if (!staged.succeeded()) return false;
    if (!queue) {
        queue = &ecs::emplace<ObjectOrderQueueComponent>(m_registry, *entity);
    }
    queue->orders.clear();
    ++queue->revision;
    queue->externalRevision = externalRevision;
    return true;
}

size_t GameSessionContainmentTransactions::requestTeamEnter(
    ObjectTeamId team, ObjectId container, bool requireGarrison,
    uint32_t sourceSequence, uint64_t confirmedTick) {
    if (!m_objectTeams.find(team) || !container) return 0;
    const std::optional<ecs::entity> target =
        m_objects.entityFromId(container);
    const ObjectContainmentRuntimeComponent* runtime = target
        ? ecs::try_get<ObjectContainmentRuntimeComponent>(m_registry, *target)
        : nullptr;
    if (!runtime || !runtime->plan) return 0;

    const ObjectContainmentKind requiredKind = requireGarrison
        ? ObjectContainmentKind::Garrison
        : ObjectContainmentKind::Transport;
    size_t ruleIndex = std::numeric_limits<size_t>::max();
    uint64_t capacity = 0;
    for (size_t index = 0; index < runtime->plan->rules.size(); ++index) {
        if (runtime->plan->rules[index].kind != requiredKind) continue;
        ruleIndex = index;
        capacity = runtime->plan->rules[index].containMax;
        break;
    }
    if (ruleIndex == std::numeric_limits<size_t>::max() || capacity == 0)
        return 0;

    const auto slotCount = [&](ecs::entity entity) {
        if (requireGarrison) return uint32_t{1};
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_registry, entity);
        return type && type->archetype
            ? std::max<uint32_t>(
                  1u, type->archetype->templateData.transportSlotCount)
            : 1u;
    };

    uint64_t unavailable = 0;
    if (const ObjectContainmentComponent* contents =
            ecs::try_get<ObjectContainmentComponent>(m_registry, *target)) {
        for (const ObjectContainedObjectRecord& record : contents->objects) {
            const std::optional<ecs::entity> occupant =
                m_objects.entityFromId(record.object);
            const ObjectContainedByComponent* edge = occupant
                ? ecs::try_get<ObjectContainedByComponent>(
                      m_registry, *occupant)
                : nullptr;
            if (occupant && edge && edge->containmentRuleIndex == ruleIndex)
                unavailable += slotCount(*occupant);
        }
    }
    const auto pendingView =
        ecs::view<const ObjectScriptContainmentEnterComponent>(m_registry);
    for (const ecs::entity pendingEntity : pendingView) {
        const ObjectScriptContainmentEnterComponent& pending =
            pendingView.template get<
                const ObjectScriptContainmentEnterComponent>(pendingEntity);
        if (pending.target == container) {
            unavailable += std::max<uint32_t>(
                1u, pending.reservedCapacity);
        }
    }
    uint64_t remaining = unavailable >= capacity
        ? 0u
        : capacity - unavailable;

    const container::Span<const ObjectId> members =
        m_objectTeams.legacyMembers(team);
    container::Vector<ObjectId> actors(members.begin(), members.end());
    std::sort(actors.begin(), actors.end());
    size_t accepted = 0;
    uint32_t sequence = sourceSequence == 0 ? 1u : sourceSequence;
    for (const ObjectId member : actors) {
        const std::optional<ecs::entity> entity =
            m_objects.entityFromId(member);
        if (!entity) continue;
        const uint32_t required = slotCount(*entity);
        const ObjectScriptContainmentEnterComponent* previous =
            ecs::try_get<ObjectScriptContainmentEnterComponent>(
                m_registry, *entity);
        const uint32_t previousReservation = previous &&
                previous->target == container
            ? std::max<uint32_t>(1u, previous->reservedCapacity)
            : 0u;
        remaining = std::min<uint64_t>(
            capacity, remaining + previousReservation);
        if (remaining < required) {
            remaining = remaining >= previousReservation
                ? remaining - previousReservation
                : 0u;
            if (sequence != std::numeric_limits<uint32_t>::max()) ++sequence;
            continue;
        }
        const bool result = requireGarrison
            ? requestObjectGarrison(
                  member, container, sequence, confirmedTick)
            : requestObjectEnter(
                  member, container, sequence, confirmedTick, required);
        if (result) {
            remaining -= required;
            ++accepted;
        } else {
            remaining = remaining >= previousReservation
                ? remaining - previousReservation
                : 0u;
        }
        if (sequence != std::numeric_limits<uint32_t>::max()) ++sequence;
    }
    return accepted;
}

} // namespace engine
