#include "game/session/transaction/GameSessionProductionExitRoutes.h"

#include "game/object/ai/runtime/ObjectAIPathSequenceSnapshot.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/navigation/runtime/NavigationSystem.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace engine::production_exit {

namespace {

[[nodiscard]] ObjectProductionRoutePoint snapQueueNaturalRallyPoint(
    const ecs::registry& registry,
    ecs::entity entity,
    const ObjectProductionRoutePoint& authored,
    const navigation::NavigationSystem& navigationSystem) noexcept {
    if (!navigationSystem.isInitialized()) {
        return authored;
    }
    const ObjectTerrainLayerComponent* terrainLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);
    if (!terrainLayer) return authored;

    navigation::NavigationLayerId layer;
    if (!navigation::tryNavigationLayerFromTerrainPathfindLayer(
            terrainLayer->pathfindLayer, layer)) {
        return authored;
    }
    const navigation::NavigationGrid* grid =
        navigationSystem.layers().find(layer);
    if (!grid) return authored;

    const ObjectGeometryComponent* geometry =
        ecs::try_get<ObjectGeometryComponent>(registry, entity);
    const navigation::NavigationClearanceClass clearance =
        navigation::clearanceClassForRadiusRaw(
            geometry
                ? math::q32_32::max(
                      {}, geometry->boundingCircleRadiusFixed).raw()
                : 0,
            grid->transform().cellSizeRaw);
    const navigation::NavigationWorldPosition requested{
        authored.x.raw(), authored.y.raw(), authored.z.raw()};
    const navigation::NavigationCellId cell =
        grid->cellAt(requested, clearance);
    navigation::NavigationWorldPosition snapped;
    if (!grid->cellPosition(cell, clearance, snapped)) return authored;
    return {
        .x = math::q32_32::from_raw(snapped.xRaw),
        .y = math::q32_32::from_raw(snapped.yRaw),
        .z = math::q32_32::from_raw(snapped.zRaw),
    };
}

[[nodiscard]] bool sameRoutePoint(
    const ObjectProductionRoutePoint& left,
    const ObjectProductionRoutePoint& right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

} // namespace

void queueProductionExitRoute(
    ecs::registry& registry,
    ecs::entity entity,
    const ObjectProductionExitRoute& intent,
    uint64_t confirmedTick,
    const navigation::NavigationSystem& navigationSystem) {
    if (intent.points.empty() ||
        !ecs::try_get<ObjectLocomotionComponent>(registry, entity)) {
        return;
    }

    ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(registry, entity);
    if (!queue) {
        queue = &ecs::emplace<ObjectOrderQueueComponent>(registry, entity);
    }
    const bool routeStartsAtQueueHead = queue->orders.empty();
    uint32_t sourceSequence = intent.sourceSequence == 0
        ? 1u
        : intent.sourceSequence;
    ObjectSystemPathSequenceComponent route{
        .routeSubtype = ObjectMoveRouteSubtype::FollowExitProductionPath,
        .systemPurpose = ObjectOrderSystemPurpose::ProductionExit,
        .ignoredObstacle = intent.producer,
        .issuedTick = confirmedTick,
        .firstSourceSequence = sourceSequence,
    };
    route.points.reserve(std::min(
        intent.points.size(),
        ObjectOrderQueueComponent::MaximumQueuedOrders - queue->orders.size()));
    const bool queueExit = intent.kind ==
        game::ObjectProductionExitKind::Queue;
    const ObjectProductionRoutePoint queueNaturalRally = queueExit
        ? snapQueueNaturalRallyPoint(
              registry, entity, intent.points.front(), navigationSystem)
        : intent.points.front();
    for (const ObjectProductionRoutePoint& point : intent.points) {
        if (queue->orders.size() >=
            ObjectOrderQueueComponent::MaximumQueuedOrders) {
            break;
        }
        // QueueProductionExitUpdate snaps the natural point once and then
        // duplicates that snapped value for multi-produced infantry.  Match
        // by authored value so both copies receive the identical endpoint.
        const ObjectProductionRoutePoint routedPoint =
            queueExit && sameRoutePoint(point, intent.points.front())
                ? queueNaturalRally : point;
        queue->orders.push_back({
            .kind = ObjectOrderKind::Move,
            .source = ObjectOrderSource::System,
            .contextPlayer = intent.owner,
            .issuedTick = confirmedTick,
            .sourceSequence = sourceSequence,
            .targetX = routedPoint.x,
            .targetY = routedPoint.y,
            .targetZ = routedPoint.z,
            .hasTargetPosition = true,
            .moveRouteSubtype = routeStartsAtQueueHead && route.points.empty()
                ? ObjectMoveRouteSubtype::FollowExitProductionPath
                : ObjectMoveRouteSubtype::Direct,
            .systemPurpose = ObjectOrderSystemPurpose::ProductionExit,
        });
        route.points.push_back({
            routedPoint.x, routedPoint.y, routedPoint.z});
        if (sourceSequence != std::numeric_limits<uint32_t>::max()) {
            ++sourceSequence;
        }
    }
    if (route.points.empty()) return;

    route.queuedOrderCount = static_cast<uint32_t>(route.points.size());
    if (routeStartsAtQueueHead) {
        if (ObjectSystemPathSequenceComponent* existing =
                ecs::try_get<ObjectSystemPathSequenceComponent>(
                    registry, entity)) {
            *existing = std::move(route);
        } else {
            ecs::emplace<ObjectSystemPathSequenceComponent>(
                registry, entity, std::move(route));
        }
    }
    ++queue->revision;
}

void queueProducedUnitRoute(
    ecs::registry& registry,
    ecs::entity entity,
    const ObjectProductionSpawnIntent& intent,
    uint64_t confirmedTick,
    const navigation::NavigationSystem& navigationSystem) {
    queueProductionExitRoute(
        registry, entity,
        {.producer = intent.producer,
         .owner = intent.owner,
         .sourceSequence = intent.sourceSequence,
         .kind = intent.exitReservation.kind,
         .points = intent.exitRoute},
        confirmedTick, navigationSystem);
}

void queueSpawnSlaveRoute(
    ecs::registry& registry,
    ecs::entity entity,
    const ObjectSpawnSlaveRequest& request) {
    if (request.holdAfterSpawn || !request.hasExitTarget ||
        !ecs::try_get<ObjectLocomotionComponent>(registry, entity)) {
        return;
    }
    ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(registry, entity);
    if (!queue) {
        queue = &ecs::emplace<ObjectOrderQueueComponent>(registry, entity);
    }
    if (queue->orders.size() >=
        ObjectOrderQueueComponent::MaximumQueuedOrders) {
        return;
    }
    queue->orders.push_back({
        .kind = ObjectOrderKind::Move,
        .source = ObjectOrderSource::System,
        .contextPlayer = request.owner,
        .issuedTick = request.confirmedTick,
        .sourceSequence = request.emissionSequence == 0
            ? 1u
            : request.emissionSequence,
        .targetX = request.exitTarget.x,
        .targetY = request.exitTarget.y,
        .targetZ = request.exitTarget.z,
        .hasTargetPosition = true,
        .systemPurpose = ObjectOrderSystemPurpose::ProductionExit,
    });
    ++queue->revision;
}

} // namespace engine::production_exit
