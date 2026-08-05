#include "game/session/transaction/GameSessionPlayerOrderTransactions.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "core/math/fixed/fixed_raw_mean.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/status/ObjectCrateCollide.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/runtime/ObjectHackInternetOrderAdapter.h"
#include "game/session/command/GameSessionPlayerCommandPolicy.h"
#include "game/session/command/OrderExecutor.h"
#include "game/session/state/GameSessionDomainState.h"

namespace engine
{
namespace
{

[[nodiscard]] int64_t saturatingSubtractRaw(
    int64_t left, int64_t right) noexcept
{
    if (right > 0 && left < INT64_MIN + right) return INT64_MIN;
    if (right < 0 && left > INT64_MAX + right) return INT64_MAX;
    return left - right;
}

struct GroupCandidate final {
    ObjectId object = INVALID_OBJECT_ID;
    LogicFixedVec3 position{};
    math::q32_32 radius{};
    bool infantry = false;
    bool vehicle = false;
};

struct GroupPlannedOffset final {
    ObjectId object = INVALID_OBJECT_ID;
    math::q32_32 offsetX{};
    math::q32_32 offsetY{};
};

[[nodiscard]] std::optional<PlayerGroupPathPlan> makeGroupPathPlan(
    const ecs::registry& registry, const ObjectLifecycle& objects,
    const AISimulationRules& rules, const PlayerOrder& order)
{
    static_cast<void>(rules);
    if (order.kind != ObjectOrderKind::Move || order.queued ||
        !order.targetPosition.valid || order.actors.size() < 2)
        return std::nullopt;
    container::Vector<GroupCandidate> members;
    uint32_t infantryCount = 0;
    uint32_t vehicleCount = 0;
    for (const ObjectId object : order.actors) {
        const std::optional<ecs::entity> entity =
            objects.entityFromId(object);
        if (!entity || isObjectDisabledBy(
                registry, *entity, ObjectDisabledReason::Held,
                order.tick)) continue;
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(registry, *entity);
        const bool infantry = kinds && game::objectHasKind(
            kinds->mask, game::ObjectKindOf::Infantry);
        const bool vehicle = kinds && game::objectHasKind(
            kinds->mask, game::ObjectKindOf::Vehicle) &&
            !game::objectHasKind(kinds->mask, game::ObjectKindOf::Aircraft);
        const ObjectFixedTransformComponent* transform =
            ecs::try_get<ObjectFixedTransformComponent>(registry, *entity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, *entity);
        if ((!infantry && !vehicle) || !transform ||
            !transform->authoritative) continue;
        members.push_back({
            object,
            transform->position,
            geometry
                ? math::q32_32::max(
                      {}, geometry->boundingCircleRadiusFixed)
                : math::q32_32{},
            infantry,
            vehicle,
        });
        infantryCount += infantry;
        vehicleCount += vehicle;
    }
    if (members.size() < 2) return std::nullopt;

    const int64_t divisor = static_cast<int64_t>(members.size());
    math::FixedRawMeanAccumulator meanX{divisor};
    math::FixedRawMeanAccumulator meanY{divisor};
    for (const GroupCandidate& value : members) {
        meanX.add(value.position.x.raw());
        meanY.add(value.position.y.raw());
    }
    const math::q32_32 centerX = math::q32_32::from_raw(meanX.value());
    const math::q32_32 centerY = math::q32_32::from_raw(meanY.value());
    const math::q32_32 dx = order.targetPosition.x - centerX;
    const math::q32_32 dy = order.targetPosition.y - centerY;
    const math::q32_32 travelSquared = dx * dx + dy * dy;
    if (math::q32_32::sqrt(travelSquared) <=
        math::q32_32::from_fraction(1, 10000))
        return std::nullopt;

    // RefCode friend_computeGroundPath() does not start the shared route at
    // the arithmetic mean (which may be inside a building or on the far side
    // of a wall).  It selects the real unit nearest that mean.  Keep that
    // actor at ordinal zero; NavigationRequestQueue gives ordinal zero
    // precedence over followers, so no follower can observe an unbuilt
    // centerline merely because it owns a smaller ObjectId.
    const auto leader = std::min_element(
        members.begin(), members.end(),
        [centerX, centerY](const GroupCandidate& left,
                           const GroupCandidate& right) {
            const math::q32_32 leftX = left.position.x - centerX;
            const math::q32_32 leftY = left.position.y - centerY;
            const math::q32_32 rightX = right.position.x - centerX;
            const math::q32_32 rightY = right.position.y - centerY;
            const math::q32_32 leftDistance =
                leftX * leftX + leftY * leftY;
            const math::q32_32 rightDistance =
                rightX * rightX + rightY * rightY;
            return leftDistance != rightDistance
                ? leftDistance < rightDistance
                : left.object < right.object;
    });
    std::iter_swap(members.begin(), leader);
    const math::q32_32 routeX =
        order.targetPosition.x - members.front().position.x;
    const math::q32_32 routeY =
        order.targetPosition.y - members.front().position.y;
    const math::q32_32 routeLength = math::q32_32::sqrt(
        routeX * routeX + routeY * routeY);
    if (routeLength <= math::q32_32::from_fraction(1, 10000))
        return std::nullopt;
    uint64_t id = (static_cast<uint64_t>(order.tick) << 32u) ^
        (static_cast<uint64_t>(order.sequence) << 8u) ^ order.player.value;
    if (id == 0) id = 1;
    PlayerGroupPathPlan plan{
        .id = id,
        .startX = members.front().position.x,
        .startY = members.front().position.y,
        .startZ = members.front().position.z,
    };
    const math::q32_32 forwardX = routeX / routeLength;
    const math::q32_32 forwardY = routeY / routeLength;
    const math::q32_32 normalX = -forwardY;
    const math::q32_32 normalY = forwardX;
    // Assign slots in the same monotonic order as RefCode's AIGroup.  The
    // previous adapter sorted the followers by lateral position but then
    // assigned them -1,+1,0 in a repeating sequence.  That defeats the sort:
    // the left unit can receive the right slot and the group crosses itself
    // while travelling along a straight path.  Build balanced columns from
    // left to right instead, retaining ObjectId only as a deterministic tie
    // break for coincident units.
    std::sort(members.begin() + 1, members.end(),
              [centerX, centerY, normalX, normalY](
                  const GroupCandidate& left,
                  const GroupCandidate& right) {
                  const math::q32_32 leftLateral =
                      (left.position.x - centerX) * normalX +
                      (left.position.y - centerY) * normalY;
                  const math::q32_32 rightLateral =
                      (right.position.x - centerX) * normalX +
                      (right.position.y - centerY) * normalY;
                  return leftLateral != rightLateral
                      ? leftLateral < rightLateral
                      : left.object < right.object;
              });
    container::Vector<GroupPlannedOffset> plannedOffsets;
    plannedOffsets.reserve(members.size());
    plannedOffsets.push_back({members.front().object, {}, {}});

    const auto assignMonotonicSlots = [&](bool vehicle) {
        container::Vector<size_t> indices;
        for (size_t index = 1; index < members.size(); ++index) {
            if (members[index].vehicle == vehicle)
                indices.push_back(index);
        }
        if (indices.empty()) return;

        const int32_t columnCount = vehicle
            ? (vehicleCount < 5u ? 2 : 3)
            : (infantryCount < 16u ? 3 : 5);
        const int32_t columnCentre = (columnCount - 1) / 2;
        container::Vector<uint32_t> rows(
            static_cast<size_t>(columnCount), 0u);
        for (size_t rank = 0; rank < indices.size(); ++rank) {
            int32_t columnIndex = 0;
            if (indices.size() == 1) {
                // A two-member group must not put its only follower in the
                // centre column. That produced an exact duplicate destination
                // and made the pair travel as one line. Preserve its current
                // side of the route; coincident members use ObjectId as the
                // deterministic side tie-break.
                const GroupCandidate& member = members[indices.front()];
                const math::q32_32 lateral =
                    (member.position.x - centerX) * normalX +
                    (member.position.y - centerY) * normalY;
                const int32_t side = lateral < math::q32_32{}
                    ? -1
                    : lateral > math::q32_32{}
                    ? 1
                    : member.object < members.front().object ? -1 : 1;
                columnIndex = columnCount == 2
                    ? (side < 0 ? 0 : 1)
                    : side + columnCentre;
            } else {
                columnIndex = static_cast<int32_t>(
                    (rank * static_cast<size_t>(columnCount - 1) +
                     (indices.size() - 1u) / 2u) /
                    (indices.size() - 1u));
            }
            const int32_t column = columnCount == 2
                ? (columnIndex == 0 ? -1 : 1)
                : columnIndex - columnCentre;
            const uint32_t row = rows[static_cast<size_t>(columnIndex)]++;
            const GroupCandidate& member = members[indices[rank]];
            const math::q32_32 spacing = math::q32_32::max(
                vehicle
                    ? (vehicleCount < 5u ? math::q32_32{int32_t{15}}
                                         : math::q32_32{int32_t{32}})
                    : math::q32_32{int32_t{22}},
                member.radius * math::q32_32{int32_t{2}} +
                    math::q32_32{int32_t{2}});
            plannedOffsets.push_back({
                member.object,
                normalX * math::q32_32{column} * spacing -
                    forwardX * math::q32_32{static_cast<int32_t>(row)} *
                        spacing,
                normalY * math::q32_32{column} * spacing -
                    forwardY * math::q32_32{static_cast<int32_t>(row)} *
                        spacing,
            });
        }
    };
    assignMonotonicSlots(false);
    assignMonotonicSlots(true);

    plan.members.reserve(members.size());
    for (const GroupPlannedOffset& offset : plannedOffsets) {
        plan.members.push_back({
            .object = offset.object,
            .offsetX = offset.offsetX,
            .offsetY = offset.offsetY,
        });
    }
    return plan;
}

} // namespace

OrderExecutionResult GameSessionPlayerOrderTransactions::toggleFormation(
    const GameCommand& command)
{
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        command.tick != m_presentation.m_confirmedTick ||
        !command.player.isMapPlayer() || command.actors.empty())
    {
        return {
            .accepted = false,
            .rejection = OrderRejectionReason::MalformedOrder,
            .message =
                "formation command is outside the confirmed authority frame",
        };
    }

    container::Vector<ecs::entity> entities;
    entities.reserve(command.actors.size());
    ObjectId previous = INVALID_OBJECT_ID;
    for (const ObjectId actor : command.actors)
    {
        if (!actor || (previous && !(previous < actor)) ||
            m_world.m_ownership.ownerOf(actor) != command.player ||
            session_command_policy::objectForbidsPlayerCommands(
                m_world.m_registry, m_world.m_objects, actor))
        {
            return {
                .accepted = false,
                .rejection = OrderRejectionReason::OwnershipMismatch,
                .message =
                    "formation actors are not canonical controlled objects",
            };
        }
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(actor);
        if (!entity || !ecs::try_get<ObjectFixedTransformComponent>(
                m_world.m_registry, *entity))
        {
            return {
                .accepted = false,
                .rejection = OrderRejectionReason::MissingActor,
                .message =
                    "formation actor has no live authoritative transform",
            };
        }
        entities.push_back(*entity);
        previous = actor;
    }

    bool alreadyFormation = true;
    uint64_t existingId = 0;
    for (const ecs::entity entity : entities)
    {
        const auto* formation =
            ecs::try_get<ObjectPlayerFormationComponent>(
                m_world.m_registry, entity);
        if (!formation || formation->id == 0)
        {
            alreadyFormation = false;
            break;
        }
        if (existingId == 0) existingId = formation->id;
        if (formation->id != existingId)
        {
            alreadyFormation = false;
            break;
        }
    }
    if (alreadyFormation)
    {
        for (const ecs::entity entity : entities)
        {
            ecs::remove<ObjectPlayerFormationComponent>(
                m_world.m_registry, entity);
        }
        return {.accepted = true, .actorCount = entities.size()};
    }

    const int64_t divisor = static_cast<int64_t>(entities.size());
    math::FixedRawMeanAccumulator centerX{divisor};
    math::FixedRawMeanAccumulator centerY{divisor};
    for (const ecs::entity entity : entities)
    {
        const auto* transform = ecs::try_get<ObjectFixedTransformComponent>(
            m_world.m_registry, entity);
        centerX.add(transform->position.x.raw());
        centerY.add(transform->position.y.raw());
    }
    uint64_t formationId =
        (static_cast<uint64_t>(command.tick) << 32u) |
        static_cast<uint64_t>(command.sequence);
    if (formationId == 0) formationId = 1;
    for (const ecs::entity entity : entities)
    {
        const auto* transform = ecs::try_get<ObjectFixedTransformComponent>(
            m_world.m_registry, entity);
        const ObjectPlayerFormationComponent value{
            .id = formationId,
            .offsetX = math::q32_32::from_raw(saturatingSubtractRaw(
                transform->position.x.raw(), centerX.value())),
            .offsetY = math::q32_32::from_raw(saturatingSubtractRaw(
                transform->position.y.raw(), centerY.value())),
        };
        if (auto* current = ecs::try_get<ObjectPlayerFormationComponent>(
                m_world.m_registry, entity))
        {
            *current = value;
        }
        else
        {
            ecs::emplace<ObjectPlayerFormationComponent>(
                m_world.m_registry, entity, value);
        }
    }
    return {.accepted = true, .actorCount = entities.size()};
}

OrderExecutionResult
GameSessionPlayerOrderTransactions::executeIntentionalContact(
    const GameCommand& command,
    container::Span<const ObjectId> admittedActors,
    ObjectIntentionalContactKind contactKind)
{
    const auto reject = [](OrderRejectionReason reason,
                           container::String message)
    {
        return OrderExecutionResult{
            .accepted = false,
            .rejection = reason,
            .message = std::move(message),
        };
    };
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        command.tick != m_presentation.m_confirmedTick ||
        !command.targetObject || command.targetPosition.valid ||
        admittedActors.empty() ||
        command.targetObject == admittedActors.front())
    {
        return reject(
            OrderRejectionReason::InvalidTarget,
            "intentional-contact CommandButton requires one object target");
    }

    const std::optional<ecs::entity> target =
        m_world.m_objects.entityFromId(command.targetObject);
    const ObjectFixedTransformComponent* targetTransform = target
        ? ecs::try_get<ObjectFixedTransformComponent>(
              m_world.m_registry, *target)
        : nullptr;
    if (!target || !targetTransform || !targetTransform->authoritative ||
        m_world.m_objects.isPendingDestroy(command.targetObject))
    {
        return reject(
            OrderRejectionReason::InvalidTarget,
            "intentional-contact target is unavailable");
    }

    if (const auto visibility = m_world.m_mapVisibility.snapshot();
        visibility && visibility->renderingActive)
    {
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(
                m_world.m_registry, *target);
        const math::q32_32 radius = geometry
            ? math::q32_32::max(
                  math::q32_32{}, geometry->boundingCircleRadiusFixed)
            : math::q32_32{};
        bool visible = visibility->footprintHasClearCellRaw(
            command.player, targetTransform->position.x.raw(),
            targetTransform->position.y.raw(), radius.raw());
        for (const PlayerId ally : m_content.m_players.activePlayerIds())
        {
            if (visible) break;
            if (m_content.m_players.relationship(command.player, ally) !=
                PlayerRelationship::Allies)
            {
                continue;
            }
            visible = visibility->footprintHasClearCellRaw(
                ally, targetTransform->position.x.raw(),
                targetTransform->position.y.raw(), radius.raw());
        }
        if (!visible)
        {
            return reject(
                OrderRejectionReason::InvalidTarget,
                "intentional-contact target is shrouded");
        }
    }

    size_t admitted = 0;
    for (const ObjectId actor : admittedActors)
    {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(actor);
        if (!entity || !ecs::try_get<ObjectLocomotionComponent>(
                m_world.m_registry, *entity) ||
            !canObjectPerformIntentionalCrateContact(
                m_world.m_registry, m_world.m_objects, m_content.m_terrain,
                m_content.m_players, actor, command.targetObject,
                contactKind))
        {
            continue;
        }
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(
                m_world.m_registry, *entity);
        if (!queue)
        {
            queue = &ecs::emplace<ObjectOrderQueueComponent>(
                m_world.m_registry, *entity);
        }
        if (command.queued && queue->orders.size() >=
                ObjectOrderQueueComponent::MaximumQueuedOrders)
        {
            continue;
        }
        ObjectOrderIntent order{
            .kind = ObjectOrderKind::Move,
            .source = ObjectOrderSource::Player,
            .contextPlayer = command.player,
            .issuedTick = command.tick,
            .sourceSequence = command.sequence,
            .targetObject = command.targetObject,
            .targetX = targetTransform->position.x,
            .targetY = targetTransform->position.y,
            .targetZ = targetTransform->position.z,
            .hasTargetPosition = true,
            .contentName = command.commandName,
            .systemPurpose = ObjectOrderSystemPurpose::IntentionalContact,
            .systemPurposeInstance =
                static_cast<uint32_t>(contactKind) + 1u,
        };
        if (!command.queued) queue->orders.clear();
        queue->orders.push_back(std::move(order));
        ++queue->revision;
        ++queue->externalRevision;
        if (queue->externalRevision == 0) ++queue->externalRevision;
        if (!command.queued) {
            queue->replacementExternalRevision = queue->externalRevision;
            queue->replacementExternalSource = ObjectOrderSource::Player;
            queue->replacementExternalKind = ObjectOrderKind::Move;
        }
        ++admitted;
    }
    return {.accepted = true, .actorCount = admitted};
}

OrderExecutionResult GameSessionPlayerOrderTransactions::execute(const PlayerOrder& order)
{
    if (!m_content.m_active)
    {
        return {
            .accepted = false,
            .rejection = OrderRejectionReason::InvalidPlayer,
            .message = "cannot execute a player order outside an active session",
        };
    }
    if (!m_content.m_players.get(order.player))
    {
        return {
            .accepted = false,
            .rejection = OrderRejectionReason::InvalidPlayer,
            .message = "player order has no active player authority",
        };
    }

    if (!m_presentation.m_hasConfirmedFrame || order.tick != m_presentation.m_confirmedTick)
    {
        return {
            .accepted = false,
            .rejection = OrderRejectionReason::MalformedOrder,
            .message = "player order does not belong to the active confirmed tick",
        };
    }
    const game::CommandButtonTemplate* button = order.kind == ObjectOrderKind::CommandButton
                                                    ? m_content.m_contentSnapshot.findCommandButton(order.contentName)
                                                    : nullptr;
    const bool hackInternet = isHackInternetCommandButton(order.kind, order.contentName, button);
    PlayerOrder admitted = order;
    admitted.actors.erase(std::remove_if(admitted.actors.begin(),
                                         admitted.actors.end(),
                                         [this, &order](ObjectId actor)
                                         {
                                             if (session_command_policy::objectForbidsPlayerCommands(
                                                     m_world.m_registry, m_world.m_objects, actor)) {
                                                 return true;
                                             }
                                             const bool ordinaryAICommand =
                                                 order.kind == ObjectOrderKind::Move ||
                                                 order.kind == ObjectOrderKind::Stop ||
                                                 order.kind == ObjectOrderKind::Attack ||
                                                 order.kind == ObjectOrderKind::TacticalAttack;
                                             if (!ordinaryAICommand) return false;
                                             const std::optional<ecs::entity> entity =
                                                 m_world.m_objects.entityFromId(actor);
                                             const ThingTemplateComponent* type = entity
                                                 ? ecs::try_get<ThingTemplateComponent>(
                                                       m_world.m_registry, *entity)
                                                 : nullptr;
                                             // RailedTransportAIUpdate::aiDoCommand ignores every
                                             // ordinary player command, while AI/script commands
                                             // delegate to AIUpdateInterface. Execute-transport and
                                             // evacuation use their dedicated confirmed ports.
                                             return type && type->archetype &&
                                                 type->archetype->aiRecipe ==
                                                     ai::AIRecipeId::RailedTransportAIUpdate;
                                         }),
                          admitted.actors.end());
    if (admitted.actors.empty())
    {
        return {.accepted = true, .actorCount = 0};
    }
    admitted.groupPath = makeGroupPathPlan(
        m_world.m_registry, m_world.m_objects,
        m_content.m_objectSimulationRules.ai, admitted);
    // OrderExecutor receives only this immutable capability projection.  It
    // must not reconstruct command ownership from ThingTemplate defaults,
    // because a live actor may still be unbound or have been retired.
    m_objectAI.captureOrderCapabilitySnapshot(m_orderCapabilityScratch);
    return OrderExecutor::executePlayer(m_world.m_registry,
                                        m_content.m_players,
                                        m_world.m_objects,
                                        admitted,
                                        hackInternet,
                                        admitted.combatDrop,
                                        m_content.m_objectSimulationRules.groupMoveClickToGatherFactor.raw(),
                                        m_orderCapabilityScratch);
}

bool GameSessionPlayerOrderTransactions::executeRailedTransport(
    ObjectId transport, uint64_t confirmedTick) {
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(transport);
    if (!entity ||
        !m_world.m_objectSimulation.requestRailedTransportExecute(
            m_world.m_registry, m_world.m_objects,
            transport, confirmedTick)) {
        return false;
    }
    ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity);
    uint64_t externalRevision = queue ? queue->externalRevision + 1u : 1u;
    if (externalRevision == 0) externalRevision = 1;
    if (!queue) {
        queue = &ecs::emplace<ObjectOrderQueueComponent>(
            m_world.m_registry, *entity);
    }
    queue->orders.clear();
    ++queue->revision;
    queue->externalRevision = externalRevision;
    return true;
}

bool GameSessionPlayerOrderTransactions::stagePendingEvacuation(
    ObjectId container, PlayerId player,
    uint64_t externalOrderRevision, uint64_t issuedTick,
    uint64_t deadlineTick, uint32_t sourceSequence,
    math::q32_32 landingZ, bool previousUsePreciseZPosition) {
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(container);
    if (!entity || externalOrderRevision == 0) return false;
    const ObjectPendingPlayerEvacuationComponent value{
        .player = player,
        .externalOrderRevision = externalOrderRevision,
        .issuedTick = issuedTick,
        .deadlineTick = deadlineTick,
        .sourceSequence = sourceSequence,
        .landingZ = landingZ,
        .previousUsePreciseZPosition = previousUsePreciseZPosition,
    };
    if (ObjectPendingPlayerEvacuationComponent* pending =
            ecs::try_get<ObjectPendingPlayerEvacuationComponent>(
                m_world.m_registry, *entity)) {
        *pending = value;
    } else {
        ecs::emplace<ObjectPendingPlayerEvacuationComponent>(
            m_world.m_registry, *entity, value);
    }
    return true;
}

} // namespace engine
