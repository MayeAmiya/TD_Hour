#include "game/session/command/GameSessionConfirmedCommandPort.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/session/query/GameSessionCommandQueryPort.h"
#include "game/session/query/GameSessionObjectQueryPort.h"
#include "game/session/query/ObjectContainmentQuery.h"
#include "game/session/query/WorldCommandQueryPort.h"
#include "game/session/transaction/GameSessionContainmentTransactions.h"
#include "game/session/transaction/GameSessionPlayerOrderTransactions.h"

#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"

#include <algorithm>

namespace engine {
namespace {

[[nodiscard]] bool isLivePlayerContainmentActor(
    bool sessionActive, uint64_t sessionConfirmedTick,
    const PlayerRegistry& players, const ObjectLifecycle& objects,
    const ObjectOwnershipIndex& ownership,
    ObjectId actor, PlayerId player,
    uint64_t confirmedTick) noexcept {
    const PlayerState* commandPlayer = players.get(player);
    return sessionActive && commandPlayer && commandPlayer->isCommandPlayer() &&
        actor && sessionConfirmedTick == confirmedTick &&
        objects.entityFromId(actor).has_value() &&
        !objects.isPendingDestroy(actor) &&
        ownership.ownerOf(actor) == std::optional<PlayerId>{player};
}

[[nodiscard]] uint64_t nextExternalOrderRevision(
    const ObjectOrderQueueComponent* queue) noexcept {
    uint64_t revision = queue ? queue->externalRevision + 1u : 1u;
    if (revision == 0) revision = 1;
    return revision;
}

[[nodiscard]] std::optional<ObjectContainmentKind> exitNetworkKind(
    const ObjectContainmentRuntimeComponent* runtime) noexcept {
    if (!runtime || !runtime->plan) return std::nullopt;
    for (const ObjectContainmentRule& rule : runtime->plan->rules) {
        if (rule.kind == ObjectContainmentKind::Cave ||
            rule.kind == ObjectContainmentKind::Tunnel) {
            return rule.kind;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool hasObjectKind(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

} // namespace

bool GameSessionCommandQueryPort::canExitPassengerThrough(
    ObjectId container, ObjectId passenger) const {
    const GameSessionWorldState& world = *m_world;
    return session_query::canExitPassengerThrough(
        world.m_registry, world.m_objects, world.m_ownership,
        container, passenger);
}

container::Vector<ObjectId>
GameSessionCommandQueryPort::containmentPassengers(
    ObjectId container) const {
    container::Vector<ObjectId> result;
    const GameSessionWorldState& world = *m_world;
    const std::optional<ecs::entity> selectedEntity =
        world.m_objects.entityFromId(container);
    const ObjectContainmentComponent* local = selectedEntity
        ? ecs::try_get<ObjectContainmentComponent>(
              world.m_registry, *selectedEntity)
        : nullptr;
    const ObjectContainmentRuntimeComponent* runtime = selectedEntity
        ? ecs::try_get<ObjectContainmentRuntimeComponent>(
              world.m_registry, *selectedEntity)
        : nullptr;
    if (!selectedEntity || !local || !runtime || !runtime->plan)
        return result;
    const std::optional<PlayerId> owner =
        world.m_ownership.ownerOf(container);
    if (!exitNetworkKind(runtime)) {
        container::Vector<ObjectContainedObjectRecord> ordered =
            local->objects;
        std::sort(
            ordered.begin(), ordered.end(),
            [](const ObjectContainedObjectRecord& left,
               const ObjectContainedObjectRecord& right) noexcept {
                if (left.entryOrdinal != right.entryOrdinal)
                    return left.entryOrdinal < right.entryOrdinal;
                return left.object < right.object;
            });
        result.reserve(ordered.size());
        for (const ObjectContainedObjectRecord& record : ordered) {
            if (record.object && (!owner ||
                world.m_ownership.ownerOf(record.object) == owner)) {
                result.push_back(record.object);
            }
        }
        return result;
    }

    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectContainedByComponent>(
        world.m_registry);
    result.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (identity.id && canExitPassengerThrough(container, identity.id) &&
            (!owner ||
             world.m_ownership.ownerOf(identity.id) == owner)) {
            result.push_back(identity.id);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool GameSessionConfirmedCommandPort::enterContainer(
    ObjectId object, ObjectId container, PlayerId player,
    uint64_t confirmedTick, uint32_t sourceSequence) {
    if (objectForbidsPlayerCommands(object)) return true;
    if (!isLivePlayerContainmentActor(
            domainState().contentState().m_active,
            domainState().presentationState().m_confirmedTick,
            domainState().contentState().m_players,
            domainState().worldState().m_objects,
            domainState().worldState().m_ownership,
            object, player, confirmedTick) || !container ||
        object == container ||
        domainState().worldState().m_objects.isPendingDestroy(container)) {
        return false;
    }
    const std::optional<ecs::entity> containerEntity =
        domainState().worldState().m_objects.entityFromId(container);
    const ObjectContainmentRuntimeComponent* containment = containerEntity
        ? ecs::try_get<ObjectContainmentRuntimeComponent>(
              domainState().worldState().m_registry, *containerEntity)
        : nullptr;
    const bool garrisonContainer = containment && containment->plan &&
        std::any_of(
            containment->plan->rules.begin(), containment->plan->rules.end(),
            [](const ObjectContainmentRule& rule) noexcept {
                return rule.kind == ObjectContainmentKind::Garrison;
            });
    GameSessionContainmentTransactions transactions{
        domainState().worldState().m_registry,
        domainState().worldState().m_objects,
        domainState().worldState().m_objectSimulation,
        domainState().worldState().m_spatialIndex,
        domainState().worldState().m_objectTeams,
        domainState().contentState().m_players,
        domainState().contentState().m_contentSnapshot};
    // GarrisonContain owns its own capacity, ownership and pending-entry
    // policy. Routing a player click through the generic transport path let
    // simultaneous entries race a capture/ownership update and made normal
    // infantry garrison clicks fail to use the dedicated validation.
    return garrisonContainer
        ? transactions.requestObjectGarrison(
              object, container, sourceSequence, confirmedTick)
        : transactions.requestObjectEnter(
              object, container, sourceSequence, confirmedTick);
}

bool GameSessionConfirmedCommandPort::exitContainer(
    ObjectId container, ObjectId passenger, PlayerId player,
    uint64_t confirmedTick) {
    if (objectForbidsPlayerCommands(container) ||
        objectForbidsPlayerCommands(passenger)) {
        return true;
    }
    if (!isLivePlayerContainmentActor(
            domainState().contentState().m_active,
            domainState().presentationState().m_confirmedTick,
            domainState().contentState().m_players,
            domainState().worldState().m_objects,
            domainState().worldState().m_ownership,
            container, player, confirmedTick) ||
        !isLivePlayerContainmentActor(
            domainState().contentState().m_active,
            domainState().presentationState().m_confirmedTick,
            domainState().contentState().m_players,
            domainState().worldState().m_objects,
            domainState().worldState().m_ownership,
            passenger, player, confirmedTick)) {
        return false;
    }
    return GameSessionContainmentTransactions{
        domainState().worldState().m_registry,
        domainState().worldState().m_objects,
        domainState().worldState().m_objectSimulation,
        domainState().worldState().m_spatialIndex,
        domainState().worldState().m_objectTeams,
        domainState().contentState().m_players,
        domainState().contentState().m_contentSnapshot}
        .requestPlayerExit(
            container, passenger, confirmedTick,
            domainState().worldState().m_ownership,
            domainState().aiState().m_objectAI);
}

bool GameSessionConfirmedCommandPort::evacuate(
    ObjectId container, PlayerId player, uint64_t confirmedTick,
    uint32_t sourceSequence) {
    if (objectForbidsPlayerCommands(container)) return true;
    if (!isLivePlayerContainmentActor(
            domainState().contentState().m_active,
            domainState().presentationState().m_confirmedTick,
            domainState().contentState().m_players,
            domainState().worldState().m_objects,
            domainState().worldState().m_ownership,
            container, player, confirmedTick)) {
        return false;
    }
    const std::optional<ecs::entity> containerEntity =
        domainState().worldState().m_objects.entityFromId(container);
    const ObjectContainmentRuntimeComponent* runtime = containerEntity
        ? ecs::try_get<ObjectContainmentRuntimeComponent>(
              domainState().worldState().m_registry, *containerEntity)
        : nullptr;
    container::Vector<ObjectId> passengers =
        GameSessionCommandQueryPort{
            domainState().contentState(), domainState().worldState(),
            domainState().aiState()}
            .containmentPassengers(container);
    if (!containerEntity || passengers.empty() || !runtime || !runtime->plan ||
        isObjectDisabledBy(domainState().worldState().m_registry,
                           *containerEntity,
                           ObjectDisabledReason::Subdued,
                           confirmedTick)) {
        return false;
    }

    ObjectAirborneComponent* airborne =
        ecs::try_get<ObjectAirborneComponent>(
            domainState().worldState().m_registry, *containerEntity);
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(
            domainState().worldState().m_registry, *containerEntity);
    ObjectLocomotionComponent* locomotion =
        ecs::try_get<ObjectLocomotionComponent>(
            domainState().worldState().m_registry, *containerEntity);
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(
            domainState().worldState().m_registry, *containerEntity);
    if (!transform) return false;
    const LogicFixedVec3 containerPosition = readAuthoritativeObjectPosition(
        domainState().worldState().m_registry, *containerEntity, *transform);
    const game::terrain::TerrainPathfindLayerId landingLayer =
        domainState().contentState().m_terrain.highestPathfindLayerAtXYRaw(
            containerPosition.x.raw(), containerPosition.y.raw());
    const math::q32_32 landingZ = math::q32_32::from_raw(
        domainState().contentState().m_terrain.pathfindLayerHeightRawAt(
            landingLayer, containerPosition.x.raw(),
            containerPosition.y.raw())
            .value_or(domainState().contentState().m_terrain.groundHeightRaw(
                containerPosition.x.raw(), containerPosition.y.raw())));
    const bool elevatedAircraft =
        hasObjectKind(kinds, game::ObjectKindOf::Aircraft) &&
        containerPosition.z > landingZ + math::q32_32{int32_t{1}};
    if ((airborne && airborne->isAirborne) || elevatedAircraft) {
        if (!locomotion) return false;
        const bool previousUsePreciseZPosition =
            locomotion->usePreciseZPosition;
        locomotion->usePreciseZPosition = true;
        PlayerOrder landing;
        landing.player = player;
        landing.tick = static_cast<GameTick>(confirmedTick);
        landing.sequence = sourceSequence == 0 ? 1u : sourceSequence;
        landing.kind = ObjectOrderKind::Move;
        landing.actors.push_back(container);
        landing.targetPosition = {
            .x = containerPosition.x,
            .y = containerPosition.y,
            .z = landingZ,
            .valid = true,
        };
        const OrderExecutionResult accepted = executeOrder(landing);
        if (!accepted.accepted) {
            locomotion->usePreciseZPosition = previousUsePreciseZPosition;
            return false;
        }
        const ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(
                domainState().worldState().m_registry, *containerEntity);
        if (!queue || queue->externalRevision == 0) return false;
        return GameSessionPlayerOrderTransactions{
            domainState().contentState(),
            domainState().presentationState(),
            domainState().worldState(), domainState().aiState().m_objectAI,
            domainState().aiState().m_playerOrderCapabilitySnapshot}
            .stagePendingEvacuation(
                container, player, queue->externalRevision, confirmedTick,
                confirmedTick +
                std::max<uint64_t>(
                    1u, static_cast<uint64_t>(std::max(
                        1, domainState().contentState()
                            .m_startInfo.gameSpeedFPS))) * 30u,
                landing.sequence, landingZ,
                previousUsePreciseZPosition);
    }

    // OpenContain::orderAllPassengersToExit starts AI_EXIT on every rider;
    // the shared TransportContain door/ExitDelay then admits one rider at a
    // time. Do not use EjectAll here: that is the force/system primitive and
    // intentionally bypasses the door, which detaches the complete roster in
    // one tick and stacks every passenger at the same exit.
    size_t stagedCount = 0;
    for (const ObjectId passenger : passengers) {
        stagedCount += exitContainer(
            container, passenger, player, confirmedTick) ? 1u : 0u;
    }
    return stagedCount != 0;
}

bool GameSessionConfirmedCommandPort::executeRailedTransport(
    ObjectId transport, PlayerId player, uint64_t confirmedTick) {
    if (objectForbidsPlayerCommands(transport)) return true;
    if (!isLivePlayerContainmentActor(
            domainState().contentState().m_active,
            domainState().presentationState().m_confirmedTick,
            domainState().contentState().m_players,
            domainState().worldState().m_objects,
            domainState().worldState().m_ownership,
            transport, player, confirmedTick)) {
        return false;
    }
    return GameSessionPlayerOrderTransactions{
        domainState().contentState(), domainState().presentationState(),
        domainState().worldState(), domainState().aiState().m_objectAI,
        domainState().aiState().m_playerOrderCapabilitySnapshot}
        .executeRailedTransport(transport, confirmedTick);
}

} // namespace engine
