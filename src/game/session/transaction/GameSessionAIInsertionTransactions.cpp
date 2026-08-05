#include "game/session/transaction/GameSessionAIInsertionTransactions.h"

#include "game/session/state/GameSessionDomainState.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "debug/debug.h"

#include <algorithm>
#include <utility>

namespace engine {
namespace {

[[nodiscard]] bool insertionHasObjectKind(
    const ObjectKindOfComponent* kinds, game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] math::q32_32 insertionNonNegative(
    math::q32_32 value) noexcept {
    return math::q32_32::max(math::q32_32{}, value);
}

[[nodiscard]] LogicFixedVec3 insertionOrderTarget(
    const ObjectOrderIntent& order) noexcept {
    return {order.targetX, order.targetY, order.targetZ};
}

[[nodiscard]] navigation::NavigationMovementMask
insertionLocomotionSurfaceMask(
    const ObjectLocomotionComponent& locomotion) noexcept {
    game::LocomotorSurfaceMask surfaces = 0;
    for (const game::FrozenLocomotorTemplate& profile : locomotion.profiles)
        surfaces |= profile.surfaces;
    if (surfaces == 0) surfaces = locomotion.surfaces;
    return surfaces != 0
        ? static_cast<navigation::NavigationMovementMask>(surfaces)
        : navigation::NavigationMovement::Air;
}

[[nodiscard]] ai::ObjectAIOrderIdentity insertionOrderIdentity(
    ObjectId subject, const ObjectOrderQueueComponent& queue,
    const ObjectOrderIntent& order) noexcept {
    return {
        .subject = subject,
        .queueRevision = queue.revision,
        .externalRevision = queue.externalRevision,
        .issuedTick = order.issuedTick,
        .sourceSequence = order.sourceSequence,
        .sourceScriptId = order.sourceScriptId,
        .source = static_cast<ai::ObjectAIOrderSource>(order.source),
        .systemPurpose =
            static_cast<ai::ObjectAIOrderSystemPurpose>(order.systemPurpose),
        .systemPurposeInstance = order.systemPurposeInstance,
    };
}

} // namespace

void GameSessionAIInsertionTransactions::emitOutcome(
    CommandBackendOutcome outcome) {
    if (!m_content.m_active || !outcome.player || !outcome.source ||
        outcome.sourceSequence == 0) {
        return;
    }
    m_presentation.m_commandBackendOutcomes.push_back(std::move(outcome));
}

void GameSessionAIInsertionTransactions::stageMotionFeedback() {
    ai::ObjectAITransientStore& transients = m_ai.m_objectAI.transients();
    for (const ai::AIStateSoASubjectSlot& actor :
         m_ai.m_objectAI.orderedSubjects()) {
        const std::optional<ai::ObjectAIInsertionStateView> insertion =
            m_ai.m_objectAI.insertionState(actor.subject);
        if (!insertion || insertion->state != ai::AIStateId::RappelInto)
            continue;
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(actor.subject);
        const TransformComponent* transform = entity
            ? ecs::try_get<TransformComponent>(m_world.m_registry, *entity) : nullptr;
        if (!entity || !transform) continue;

        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, *entity, *transform);
        const game::terrain::TerrainPathfindLayerId destinationLayer =
            m_content.m_terrain.highestPathfindLayerAtRaw(
                position.x.raw(), position.y.raw(), position.z.raw());
        const math::q32_32 groundHeight = math::q32_32::from_raw(
            m_content.m_terrain.groundHeightRaw(
                position.x.raw(), position.y.raw()));
        const math::q32_32 layerHeight = math::q32_32::from_raw(
            m_content.m_terrain.pathfindLayerHeightRawAt(
                destinationLayer, position.x.raw(), position.y.raw())
                .value_or(groundHeight.raw()));

        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *entity);
        const bool canRappel =
            insertionHasObjectKind(kinds, game::ObjectKindOf::CanRappel);
        const ObjectId goal = insertion->parameters.goalObject;
        const std::optional<ecs::entity> goalEntity =
            m_world.m_objects.entityFromId(goal);
        const ObjectKindOfComponent* goalKinds = goalEntity
            ? ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *goalEntity)
            : nullptr;
        const ObjectHealthComponent* goalHealth = goalEntity
            ? ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *goalEntity)
            : nullptr;
        const bool goalIsStructure = goalEntity &&
            insertionHasObjectKind(goalKinds, game::ObjectKindOf::Structure);
        const bool goalAlive = goalEntity &&
            !(goalHealth && goalHealth->effectivelyDead);
        math::q32_32 buildingTop = layerHeight;
        if (goalEntity && goalIsStructure) {
            const TransformComponent* goalTransform =
                ecs::try_get<TransformComponent>(m_world.m_registry, *goalEntity);
            const ObjectGeometryComponent* geometry =
                ecs::try_get<ObjectGeometryComponent>(m_world.m_registry,
                                                       *goalEntity);
            if (goalTransform) {
                buildingTop = readAuthoritativeObjectPosition(
                    m_world.m_registry, *goalEntity,
                    *goalTransform).z +
                    (geometry
                        ? insertionNonNegative(geometry->heightFixed)
                        : math::q32_32{});
            }
        }

        const ai::AIInsertionMotionFeedbackKind kind =
            insertion->payload.rappelPhase ==
                    ai::AIRappelInsertionPhase::Inactive
                ? ai::AIInsertionMotionFeedbackKind::RappelEntryReady
                : ai::AIInsertionMotionFeedbackKind::RappelFrame;
        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *entity);
        const math::q32_32 desiredSpeed = locomotion
            ? insertionNonNegative(locomotion->maximumSpeed)
            : math::q32_32{};
        const math::q32_32 maximumRappelSpeed = math::q32_32::abs(
            m_content.m_objectSimulationRules
                .gravityUnitsPerSecondSq) *
            math::q32_32::from_fraction(5, 2);
        const ai::ObjectAITransientStatus staged = transients.stage(
            ai::AIInsertionMotionFeedback{
                .correlation = {
                    .subject = actor.subject,
                    .stateRequest = insertion->payload.request,
                    .state = ai::AIStateId::RappelInto,
                },
                .kind = kind,
                .goal = goal,
                .subjectPosition = {
                    .xRaw = position.x.raw(),
                    .yRaw = position.y.raw(),
                    .zRaw = position.z.raw(),
                },
                .layerHeightRaw = layerHeight.raw(),
                .groundHeightRaw = groundHeight.raw(),
                .buildingTopRaw = buildingTop.raw(),
                .desiredSpeedRaw = desiredSpeed.raw(),
                .maximumRappelSpeedRaw = maximumRappelSpeed.raw(),
                .destinationLayer = destinationLayer,
                .canRappel = canRappel,
                .goalIsStructure = goalIsStructure,
                .goalAlive = goalAlive,
            });
        if (staged != ai::ObjectAITransientStatus::Success) {
            TD_LOG_ERROR(
                "[GameSession] Rappel feedback rejected: subject={} "
                "tick={} status={}",
                actor.subject.value, m_presentation.m_confirmedTick,
                static_cast<uint32_t>(staged));
        }
    }
}


void GameSessionAIInsertionTransactions::observeOrders() {
    for (const ai::AIStateSoASubjectSlot& actor :
         m_ai.m_objectAI.orderedSubjects()) {
        const ObjectId subject = actor.subject;
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(subject);
        if (!entity) continue;
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity);
        if (!queue) continue;

        ObjectCombatDropOrderRuntimeComponent* runtime =
            ecs::try_get<ObjectCombatDropOrderRuntimeComponent>(
                m_world.m_registry, *entity);
        const ObjectOrderIntent* order = queue->orders.empty()
            ? nullptr : &queue->orders.front();
        const bool combatDrop = order &&
            order->kind == ObjectOrderKind::CommandButton &&
            order->combatDrop && order->hasTargetPosition;

        if (!combatDrop) {
            if (runtime) {
                static_cast<void>(m_ai.m_objectAI.completeMoveOrder(
                    subject, runtime->orderIdentity,
                    ai::ObjectAIOrderCompletion::Cancelled));
                static_cast<void>(m_ai.m_objectAI.cancelInsertionState(
                    subject, m_presentation.m_confirmedTick));
                ecs::remove<ObjectCombatDropOrderRuntimeComponent>(
                    m_world.m_registry, *entity);
            }
            continue;
        }

        if (runtime) {
            const std::optional<ai::ObjectAIActorStateView> state =
                m_ai.m_objectAI.actorState(subject);
            const ai::AIAsyncOrderIdentity currentIdentity =
                ai::toAIAsyncOrderIdentity(
                    insertionOrderIdentity(subject, *queue, *order));
            const bool sameOrder =
                runtime->orderIdentity == currentIdentity;
            if (!sameOrder) {
                static_cast<void>(m_ai.m_objectAI.completeMoveOrder(
                    subject, runtime->orderIdentity,
                    ai::ObjectAIOrderCompletion::Cancelled));
                static_cast<void>(m_ai.m_objectAI.cancelInsertionState(
                    subject, m_presentation.m_confirmedTick));
                ecs::remove<ObjectCombatDropOrderRuntimeComponent>(
                    m_world.m_registry, *entity);
                continue;
            }
            if (state && state->idle) {
                const std::optional<ai::ObjectAIOrderCompletion> outcome =
                    m_ai.m_objectAI.combatDropOrderOutcome(
                        subject, runtime->orderIdentity);
                if (!outcome ||
                    !m_ai.m_objectAI.completeMoveOrder(
                        subject, runtime->orderIdentity, *outcome).succeeded())
                    continue;
                queue->orders.erase(queue->orders.begin());
                ++queue->revision;
                ecs::remove<ObjectCombatDropOrderRuntimeComponent>(
                    m_world.m_registry, *entity);
            }
            continue;
        }

        const ObjectAirfieldComponent* airfield =
            ecs::try_get<ObjectAirfieldComponent>(m_world.m_registry, *entity);
        if (!airfield || !airfield->plan || airfield->chinookAi.empty() ||
            airfield->plan->chinookAi.empty()) {
            emitOutcome({
                .player = order->contextPlayer,
                .source = subject,
                .sourceSequence = order->sourceSequence,
                .kind = CommandBackendKind::CombatDrop,
                .accepted = false,
                .confirmedTick = static_cast<GameTick>(
                    m_presentation.m_confirmedTick),
            });
            queue->orders.erase(queue->orders.begin());
            ++queue->revision;
            continue;
        }
        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry,
                                                     *entity);
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
        if (!locomotion || !transform) {
            emitOutcome({
                .player = order->contextPlayer,
                .source = subject,
                .sourceSequence = order->sourceSequence,
                .kind = CommandBackendKind::CombatDrop,
                .accepted = false,
                .confirmedTick = static_cast<GameTick>(
                    m_presentation.m_confirmedTick),
            });
            queue->orders.erase(queue->orders.begin());
            ++queue->revision;
            continue;
        }
        const ObjectId goalObject = order->targetObject;
        const std::optional<ecs::entity> goalEntity =
            m_world.m_objects.entityFromId(goalObject);
        const ObjectKindOfComponent* goalKinds = goalEntity
            ? ecs::try_get<ObjectKindOfComponent>(m_world.m_registry,
                                                   *goalEntity)
            : nullptr;
        const ObjectHealthComponent* goalHealth = goalEntity
            ? ecs::try_get<ObjectHealthComponent>(m_world.m_registry,
                                                   *goalEntity)
            : nullptr;
        const bool goalIsStructure = goalEntity &&
            insertionHasObjectKind(goalKinds, game::ObjectKindOf::Structure);
        const bool goalAlive = !goalObject ||
            (goalEntity && !(goalHealth && goalHealth->effectivelyDead));
        LogicFixedVec3 goal = insertionOrderTarget(*order);
        if (goalIsStructure) {
            const TransformComponent* goalTransform =
                ecs::try_get<TransformComponent>(m_world.m_registry,
                                                  *goalEntity);
            if (goalTransform) {
                const LogicFixedVec3 position = readAuthoritativeObjectPosition(
                    m_world.m_registry, *goalEntity, *goalTransform);
                goal.x = position.x;
                goal.y = position.y;
            }
        }
        const math::q32_32 previousPreferred =
            insertionNonNegative(locomotion->preferredHeightFixed);
        math::q32_32 approachPreferred = previousPreferred;
        if (goalIsStructure) {
            const ObjectGeometryComponent* geometry =
                ecs::try_get<ObjectGeometryComponent>(m_world.m_registry,
                                                       *goalEntity);
            const game::ObjectChinookAiRule& rule =
                airfield->plan->chinookAi.front();
            approachPreferred = math::q32_32::max(
                previousPreferred,
                (geometry ? insertionNonNegative(geometry->heightFixed)
                          : math::q32_32{}) +
                    insertionNonNegative(rule.minDropHeightFixed));
        }
        goal.z = math::q32_32::from_raw(
            m_content.m_terrain.groundHeightRaw(
                goal.x.raw(), goal.y.raw())) + approachPreferred;
        ai::AIStateParameters parameters;
        parameters.goalObject = goalObject;
        parameters.goalPosition = {
            .xRaw = goal.x.raw(),
            .yRaw = goal.y.raw(),
            .zRaw = goal.z.raw(),
        };
        parameters.hasGoalPosition = true;
        parameters.sourceOrderRevision = queue->revision;
        parameters.ignoredObstacle =
            goalIsStructure ? goalObject : INVALID_OBJECT_ID;
        parameters.pathSurfaceMask =
            insertionLocomotionSurfaceMask(*locomotion);
        parameters.arrivalRadiusRaw = math::q32_32::max(
            insertionNonNegative(locomotion->closeEnough),
            math::q32_32{int32_t{3}}).raw();
        parameters.adjustDestinations = false;
        const ai::ObjectAIOrderIdentity identity =
            insertionOrderIdentity(subject, *queue, *order);
        const LogicFixedVec3 subjectPosition =
            readAuthoritativeObjectPosition(
                m_world.m_registry, *entity, *transform);
        const ai::ObjectAIOrderAdmissionResult staged =
            m_ai.m_objectAI.observeCombatDropOrder(
                subject, identity, parameters,
                m_presentation.m_confirmedTick,
                ai::AIInsertionMotionFeedback{
                    .kind = ai::AIInsertionMotionFeedbackKind::
                        CombatDropApproachReady,
                    .goal = goalObject,
                    .subjectPosition = {
                        .xRaw = subjectPosition.x.raw(),
                        .yRaw = subjectPosition.y.raw(),
                        .zRaw = subjectPosition.z.raw(),
                    },
                    .previousPreferredHeightRaw = previousPreferred.raw(),
                    .approachPreferredHeightRaw = approachPreferred.raw(),
                    .goalIsStructure = goalIsStructure,
                    .goalAlive = goalAlive,
                });
        if (!staged.succeeded()) {
            TD_LOG_ERROR(
                "[GameSession] CombatDrop state admission rejected: "
                "subject={} tick={} status={}",
                subject.value, m_presentation.m_confirmedTick,
                static_cast<uint32_t>(staged.status));
            emitOutcome({
                .player = order->contextPlayer,
                .source = subject,
                .sourceSequence = order->sourceSequence,
                .kind = CommandBackendKind::CombatDrop,
                .accepted = false,
                .confirmedTick = static_cast<GameTick>(
                    m_presentation.m_confirmedTick),
            });
            queue->orders.erase(queue->orders.begin());
            ++queue->revision;
            continue;
        }
        emitOutcome({
            .player = order->contextPlayer,
            .source = subject,
            .sourceSequence = order->sourceSequence,
            .kind = CommandBackendKind::CombatDrop,
            .accepted = true,
            .confirmedTick = static_cast<GameTick>(
                m_presentation.m_confirmedTick),
        });
        ecs::emplace<ObjectCombatDropOrderRuntimeComponent>(
            m_world.m_registry, *entity,
            ObjectCombatDropOrderRuntimeComponent{
                .orderIdentity = ai::toAIAsyncOrderIdentity(identity),
                .queueRevision = queue->revision,
                .externalRevision = queue->externalRevision,
                .issuedTick = order->issuedTick,
                .sourceSequence = order->sourceSequence,
            });
    }
}



} // namespace engine
