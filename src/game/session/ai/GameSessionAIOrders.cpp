#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/transaction/GameSessionAIAttackOrderTransactions.h"
#include "game/session/transaction/GameSessionAIMoveOrderTransactions.h"
#include "game/object/definition/ObjectArchetype.h"

#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/ai/runtime/AIRecipeOwnerRoute.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/contracts/ObjectOrderClassification.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace engine {
namespace {

[[nodiscard]] bool hasObjectKind(const ObjectKindOfComponent* kinds,
                                 game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] navigation::NavigationMovementMask
locomotionNavigationSurfaceMask(
    const ObjectLocomotionComponent* locomotion) noexcept {
    static_assert(
        game::locomotorSurfaceBit(game::LocomotorSurface::Ground) ==
            navigation::NavigationMovement::Ground &&
        game::locomotorSurfaceBit(game::LocomotorSurface::Water) ==
            navigation::NavigationMovement::Water &&
        game::locomotorSurfaceBit(game::LocomotorSurface::Cliff) ==
            navigation::NavigationMovement::Cliff &&
        game::locomotorSurfaceBit(game::LocomotorSurface::Air) ==
            navigation::NavigationMovement::Air &&
        game::locomotorSurfaceBit(game::LocomotorSurface::Rubble) ==
            navigation::NavigationMovement::Rubble);
    if (!locomotion) return navigation::NavigationMovement::Ground;
    game::LocomotorSurfaceMask surfaces = 0;
    for (const game::FrozenLocomotorTemplate& profile : locomotion->profiles)
        surfaces |= profile.surfaces;
    if (surfaces == 0) surfaces = locomotion->surfaces;
    return surfaces != 0
        ? static_cast<navigation::NavigationMovementMask>(surfaces)
         : navigation::NavigationMovement::Ground;
}

[[nodiscard]] LogicFixedVec3 fixedOrderTarget(
    const ObjectOrderIntent& order) noexcept {
    return {order.targetX, order.targetY, order.targetZ};
}

[[nodiscard]] math::q32_32 nonNegative(math::q32_32 value) noexcept {
    return math::q32_32::max(math::q32_32{}, value);
}

[[nodiscard]] std::optional<ai::ObjectAIMoveRouteSubtype>
objectAIWaypointRouteSubtype(ObjectMoveRouteSubtype subtype) noexcept {
    switch (subtype) {
    case ObjectMoveRouteSubtype::WaypointPathIndividuals:
        return ai::ObjectAIMoveRouteSubtype::WaypointPathIndividuals;
    case ObjectMoveRouteSubtype::WaypointPathTeam:
        return ai::ObjectAIMoveRouteSubtype::WaypointPathTeam;
    case ObjectMoveRouteSubtype::WaypointPathIndividualsExact:
        return ai::ObjectAIMoveRouteSubtype::WaypointPathIndividualsExact;
    case ObjectMoveRouteSubtype::WaypointPathTeamExact:
        return ai::ObjectAIMoveRouteSubtype::WaypointPathTeamExact;
    case ObjectMoveRouteSubtype::WanderWaypointPath:
        return ai::ObjectAIMoveRouteSubtype::WanderWaypointPath;
    case ObjectMoveRouteSubtype::PanicWaypointPath:
        return ai::ObjectAIMoveRouteSubtype::PanicWaypointPath;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] ai::ObjectAIOrderIdentity objectAIOrderIdentity(
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
        .systemPurpose = static_cast<ai::ObjectAIOrderSystemPurpose>(
            order.systemPurpose),
        .systemPurposeInstance = order.systemPurposeInstance,
    };
}

// Keep the session admission gate on the same typed owner table used by the
// runtime movement consumer.  Capabilities come from the admission storage,
// not its derived iteration mirrors.  The ObjectAI protocol enums are
// explicit value mirrors of the ECS order protocol; an appended unmirrored
// value becomes Unsupported here instead of accidentally being admitted by a
// broad branch.
[[nodiscard]] ai::ObjectAIOrderOwner objectAIOrderOwner(
    const ObjectOrderIntent& order,
    ai::ObjectAIOrderCapability capabilities) noexcept {
    return ai::objectAIOrderOwner(
        static_cast<ai::ObjectAIOrderKind>(order.kind),
        static_cast<ai::ObjectAIOrderSource>(order.source),
        static_cast<ai::ObjectAIOrderSystemPurpose>(order.systemPurpose),
        capabilities,
        order.attackMove,
        static_cast<ai::ObjectAIMoveRouteSubtype>(order.moveRouteSubtype),
        static_cast<ai::ObjectAITacticalAttackSubtype>(
            order.tacticalAttackSubtype));
}

[[nodiscard]] bool discardInvalidHeadOrder(
    ai::ObjectAIRuntime& runtime, ObjectId subject,
    ObjectOrderQueueComponent& queue) {
    const ai::ObjectAIOrderAdmissionResult synchronized =
        runtime.synchronizeOrderExternalRevision(
            subject, queue.externalRevision);
    if (!synchronized.succeeded()) return false;
    queue.orders.erase(queue.orders.begin());
    ++queue.revision;
    static_cast<void>(runtime.clearSubjectTransients(subject));
    return true;
}

} // namespace

void GameSessionAIMoveOrderTransactions::observeOrders() {
    // ScriptRuntime has already observed the previous frame's one-tick
    // completedWaypoint projection. ZH clears that value in the following
    // AIUpdate, so retire the ECS marker at the matching object-AI boundary.
    container::Vector<ecs::entity> expiredWaypointCompletions;
    const auto completionView =
        ecs::view<ObjectWaypointCompletionComponent>(
            m_world.m_registry);
    for (const ecs::entity entity : completionView) {
        const ObjectWaypointCompletionComponent& completion =
            completionView.template get<
                ObjectWaypointCompletionComponent>(entity);
        if (completion.completedAtTick <
            m_presentation.m_confirmedTick) {
            expiredWaypointCompletions.push_back(entity);
        }
    }
    for (const ecs::entity entity : expiredWaypointCompletions) {
        ecs::remove<ObjectWaypointCompletionComponent>(
            m_world.m_registry, entity);
    }
    // Lifecycle creates an unbound actor because it cannot retain an ECS or
    // archetype pointer. Resolve the final inherited recipe once, before any
    // order observer can claim ownership. Missing/ambiguous content becomes
    // a capability-off actor; an already-bound disagreement is structural.
    for (const ai::AIStateSoASubjectSlot& actor :
         m_ai.m_objectAI.orderedSubjects()) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(actor.subject);
        if (!entity) continue;
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(
                m_world.m_registry, *entity);
        const ai::AIRecipeId recipe =
            type && type->archetype && type->archetype->hasAiUpdate
            ? type->archetype->aiRecipe
            : ai::AIRecipeId::Invalid;
        const ai::ObjectAIRecipeBindingResult bound =
            recipe != ai::AIRecipeId::Invalid
            ? m_ai.m_objectAI.bindRecipe(actor.subject,
                                                             recipe)
            : m_ai.m_objectAI.markRecipeContentUnavailable(
                  actor.subject);
        if (!bound.succeeded()) {
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::Membership,
                .code = SimulationFaultCode::InvalidEvent,
                .confirmedTick = m_presentation.m_confirmedTick,
                .subject = actor.subject.value,
            }));
            return;
        }
        if (bound.changed &&
            bound.status == ai::ObjectAIRecipeBindingStatus::ContentUnavailable) {
            m_frame.noteDegradation(FrameDegradation::MissingObjectRecipe);
        }
    }

    container::Vector<ecs::entity> staleSystemPaths;
    const auto systemPaths =
        ecs::view<const ObjectSystemPathSequenceComponent>(m_world.m_registry);
    for (const ecs::entity entity : systemPaths) {
        const ObjectSystemPathSequenceComponent& route =
            systemPaths.template get<
                const ObjectSystemPathSequenceComponent>(entity);
        const ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, entity);
        const ObjectOrderIntent* head = queue && !queue->orders.empty()
            ? &queue->orders.front() : nullptr;
        const bool supportedRoute =
            (route.routeSubtype == ObjectMoveRouteSubtype::FollowPath &&
             route.source == ObjectOrderSource::Player &&
             route.systemPurpose == ObjectOrderSystemPurpose::Generic) ||
            (route.routeSubtype == ObjectMoveRouteSubtype::FollowPath &&
             route.source == ObjectOrderSource::System &&
             route.systemPurpose ==
                 ObjectOrderSystemPurpose::ContainmentExit) ||
            (route.routeSubtype ==
                 ObjectMoveRouteSubtype::FollowExitProductionPath &&
             route.source == ObjectOrderSource::System &&
             route.systemPurpose ==
                 ObjectOrderSystemPurpose::ProductionExit);
        if (!supportedRoute || !head ||
            head->kind != ObjectOrderKind::Move ||
            head->source != route.source ||
            head->systemPurpose != route.systemPurpose ||
            head->moveRouteSubtype != route.routeSubtype ||
            head->issuedTick != route.issuedTick ||
            head->sourceSequence != route.firstSourceSequence) {
            staleSystemPaths.push_back(entity);
        }
    }
    for (const ecs::entity entity : staleSystemPaths) {
        ecs::remove<ObjectSystemPathSequenceComponent>(
            m_world.m_registry, entity);
    }

    m_ai.m_objectAIPathSequences.clear();
    container::Vector<ai::AIFixedPosition> pathSequenceScratch;
    for (const ai::AIStateSoASubjectSlot& actor :
         m_ai.m_objectAI.orderedSubjects()) {
        const ObjectId subject = actor.subject;
        const std::optional<ai::ObjectAIOrderCapability> capabilities =
            m_ai.m_objectAI.orderCapabilities(subject);
        if (!capabilities ||
            *capabilities == ai::ObjectAIOrderCapability::None) {
            continue;
        }
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(subject);
        ObjectOrderQueueComponent* queue = entity
            ? ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity)
            : nullptr;
        if (!entity || !queue) continue;
        const ObjectOrderIntent* order = !queue->orders.empty()
            ? &queue->orders.front()
            : nullptr;
        const bool autoHealingMove = order &&
            order->kind == ObjectOrderKind::Move &&
            order->source == ObjectOrderSource::System &&
            order->systemPurpose ==
                ObjectOrderSystemPurpose::AutoFindHealing &&
            order->targetObject;
        if (autoHealingMove) {
            const std::optional<ecs::entity> healPadEntity =
                m_world.m_objects.entityFromId(order->targetObject);
            const TransformComponent* subjectTransform =
                ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
            const TransformComponent* healPadTransform = healPadEntity
                ? ecs::try_get<TransformComponent>(m_world.m_registry, *healPadEntity)
                : nullptr;
            const ObjectGeometryComponent* subjectGeometry =
                ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, *entity);
            const ObjectGeometryComponent* healPadGeometry = healPadEntity
                ? ecs::try_get<ObjectGeometryComponent>(m_world.m_registry,
                                                         *healPadEntity)
                : nullptr;
            const ObjectLocomotionComponent* locomotion =
                ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *entity);
            const ObjectHealthComponent* subjectHealth =
                ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
            const ObjectKindOfComponent* subjectKinds =
                ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *entity);
            const ObjectKindOfComponent* healPadKinds = healPadEntity
                ? ecs::try_get<ObjectKindOfComponent>(m_world.m_registry,
                                                       *healPadEntity)
                : nullptr;
            const ObjectContainmentRuntimeComponent* healPadContainment =
                healPadEntity
                    ? ecs::try_get<ObjectContainmentRuntimeComponent>(
                          m_world.m_registry, *healPadEntity)
                    : nullptr;
            const bool hasHealContain = healPadContainment &&
                healPadContainment->plan &&
                std::any_of(
                    healPadContainment->plan->rules.begin(),
                    healPadContainment->plan->rules.end(),
                    [](const ObjectContainmentRule& rule) noexcept {
                        return rule.kind == ObjectContainmentKind::Heal;
                    });
            if (subjectTransform && healPadTransform && locomotion &&
                subjectHealth &&
                subjectHealth->currentFixed < subjectHealth->maximumFixed &&
                hasObjectKind(subjectKinds, game::ObjectKindOf::Infantry) &&
                hasObjectKind(healPadKinds, game::ObjectKindOf::HealPad) &&
                hasHealContain) {
                const LogicFixedVec3 subjectPosition =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry, *entity,
                        *subjectTransform);
                const LogicFixedVec3 healPadPosition =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry, *healPadEntity,
                        *healPadTransform);
                const math::q32_32 dx =
                    subjectPosition.x - healPadPosition.x;
                const math::q32_32 dy =
                    subjectPosition.y - healPadPosition.y;
                const math::q32_32 arrival = math::q32_32::max(
                    math::q32_32{int32_t{1}}, locomotion->closeEnough) +
                    (subjectGeometry
                        ? nonNegative(subjectGeometry->boundingCircleRadiusFixed)
                        : math::q32_32{}) +
                    (healPadGeometry
                        ? nonNegative(healPadGeometry->boundingCircleRadiusFixed)
                        : math::q32_32{});
                if (dx * dx + dy * dy <= arrival * arrival &&
                    m_world.m_objectSimulation.requestContainment(
                        m_world.m_registry, m_world.m_objects,
                        {.kind = ObjectContainmentRequestKind::Attach,
                         .container = order->targetObject,
                         .object = subject,
                         .confirmedTick = m_presentation.m_confirmedTick}, &m_content.m_players,
                        &m_content.m_contentSnapshot)) {
                    queue->orders.erase(queue->orders.begin());
                    ++queue->revision;
                    continue;
                }
            }
        }
        ObjectSystemPathSequenceComponent* systemPath =
            ecs::try_get<ObjectSystemPathSequenceComponent>(
                m_world.m_registry, *entity);
        const bool playerPath = systemPath &&
            systemPath->routeSubtype == ObjectMoveRouteSubtype::FollowPath &&
            systemPath->source == ObjectOrderSource::Player &&
            systemPath->systemPurpose == ObjectOrderSystemPurpose::Generic;
        const bool supportedSystemPath = systemPath &&
            (playerPath ||
             ((systemPath->routeSubtype ==
                  ObjectMoveRouteSubtype::FollowPath &&
              systemPath->source == ObjectOrderSource::System &&
              systemPath->systemPurpose ==
                  ObjectOrderSystemPurpose::ContainmentExit) ||
             (systemPath->routeSubtype ==
                  ObjectMoveRouteSubtype::FollowExitProductionPath &&
              systemPath->source == ObjectOrderSource::System &&
              systemPath->systemPurpose ==
                  ObjectOrderSystemPurpose::ProductionExit)));
        bool ownsSystemPath = order && supportedSystemPath &&
            order->kind == ObjectOrderKind::Move &&
            order->source == systemPath->source &&
            order->systemPurpose == systemPath->systemPurpose &&
            order->moveRouteSubtype == systemPath->routeSubtype &&
            order->issuedTick == systemPath->issuedTick &&
            order->sourceSequence ==
                systemPath->firstSourceSequence &&
            systemPath->queuedOrderCount != 0 &&
            systemPath->queuedOrderCount ==
                systemPath->points.size() &&
            systemPath->queuedOrderCount <= queue->orders.size();
        if (ownsSystemPath) {
            uint32_t expectedSequence =
                systemPath->firstSourceSequence;
            for (uint32_t index = 0;
                 index < systemPath->queuedOrderCount; ++index) {
                const ObjectOrderIntent& segment = queue->orders[index];
                const LogicFixedVec3 target = fixedOrderTarget(segment);
                if (segment.kind != ObjectOrderKind::Move ||
                    segment.source != systemPath->source ||
                    segment.systemPurpose != systemPath->systemPurpose ||
                    segment.moveRouteSubtype !=
                        (index == 0 ? systemPath->routeSubtype
                                    : ObjectMoveRouteSubtype::Direct) ||
                    (!playerPath &&
                     (segment.issuedTick != systemPath->issuedTick ||
                      segment.sourceSequence != expectedSequence)) ||
                    !segment.hasTargetPosition ||
                    target.x != systemPath->points[index].x ||
                    target.y != systemPath->points[index].y ||
                    target.z != systemPath->points[index].z) {
                    ownsSystemPath = false;
                    break;
                }
                if (expectedSequence !=
                    std::numeric_limits<uint32_t>::max()) {
                    ++expectedSequence;
                }
            }
            if (ownsSystemPath && playerPath) {
                ownsSystemPath = queue->orders.front().issuedTick ==
                        systemPath->issuedTick &&
                    queue->orders.front().sourceSequence ==
                        systemPath->firstSourceSequence;
            }
        }
        std::optional<uint64_t> systemPathRevision;
        if (ownsSystemPath) {
            pathSequenceScratch.clear();
            pathSequenceScratch.reserve(systemPath->points.size());
            for (const LogicFixedVec3& point : systemPath->points) {
                pathSequenceScratch.push_back({
                    .xRaw = point.x.raw(),
                    .yRaw = point.y.raw(),
                    .zRaw = point.z.raw(),
                });
            }
            uint64_t revision = 0;
            ownsSystemPath = m_ai.m_objectAIPathSequences.append(
                ai::AIPathSequenceHandle{
                    static_cast<uint64_t>(subject.value)},
                pathSequenceScratch, &revision,
                systemPath->sequenceRevision);
            if (ownsSystemPath) systemPathRevision = revision;
        }
        if (systemPath && !ownsSystemPath) {
            if (order && order->moveRouteSubtype ==
                    systemPath->routeSubtype) {
                queue->orders.front().moveRouteSubtype =
                    ObjectMoveRouteSubtype::Direct;
                ++queue->revision;
                order = &queue->orders.front();
            }
            ecs::remove<ObjectSystemPathSequenceComponent>(
                m_world.m_registry, *entity);
            systemPath = nullptr;
        }
        const ai::ObjectAIOrderOwner owner = order
            ? objectAIOrderOwner(*order, *capabilities)
            : ai::ObjectAIOrderOwner::None;
        const bool ordinaryMove = order &&
            order->kind == ObjectOrderKind::Move &&
            owner == ai::ObjectAIOrderOwner::ObjectAIRuntime;
        if (!ordinaryMove) {
            const bool ordinaryAttack =
                isCombatDirectAttackOrder(order);
            if (ordinaryAttack &&
                owner == ai::ObjectAIOrderOwner::ObjectAIRuntime)
                continue;
            const bool tacticalAttack =
                isCombatTacticalAttackOrder(order);
            const bool tacticalOwnershipComplete = tacticalAttack &&
                owner == ai::ObjectAIOrderOwner::ObjectAIRuntime;
            if (tacticalOwnershipComplete)
                continue;
            static_cast<void>(m_ai.m_objectAI.synchronizeOrderExternalRevision(
                subject, queue->externalRevision));
            continue;
        }

        ai::ObjectAIOrderIdentity identity =
            objectAIOrderIdentity(subject, *queue, *order);
        if (ownsSystemPath && playerPath &&
            systemPath->activeQueueRevision != 0) {
            identity.queueRevision = systemPath->activeQueueRevision;
            identity.externalRevision =
                systemPath->activeExternalRevision;
        }

        const bool diagnosePlayerTailAppend = ownsSystemPath && playerPath &&
            systemPath->queuedOrderCount >= 3 && !queue->orders.empty() &&
            queue->orders.back().source == ObjectOrderSource::Player &&
            queue->orders.back().issuedTick ==
                m_presentation.m_confirmedTick;
        if (diagnosePlayerTailAppend) {
            const std::optional<ai::ObjectAIActorStateView> actorState =
                m_ai.m_objectAI.actorState(subject);
            const std::optional<uint32_t> activePoint =
                m_ai.m_objectAI.followPathCurrentPointIndex(
                    subject, identity,
                    ai::ObjectAIMoveRouteSubtype::FollowPath);
            TD_LOG_INFO(
                "[WaypointPath] tail append observed subject={} tick={} "
                "points={} queueOrders={} queueRevision={} externalRevision={} "
                "activeQueueRevision={} activeExternalRevision={} "
                "state={} point={}",
                subject.value, m_presentation.m_confirmedTick,
                systemPath->queuedOrderCount, queue->orders.size(),
                queue->revision, queue->externalRevision,
                systemPath->activeQueueRevision,
                systemPath->activeExternalRevision,
                actorState
                    ? static_cast<uint32_t>(actorState->state)
                    : std::numeric_limits<uint32_t>::max(),
                activePoint.value_or(std::numeric_limits<uint32_t>::max()));
        }

        if (ownsSystemPath && playerPath) {
            if (const std::optional<uint32_t> current =
                    m_ai.m_objectAI.followPathCurrentPointIndex(
                        subject, identity,
                        ai::ObjectAIMoveRouteSubtype::FollowPath)) {
                systemPath->currentPointIndex = std::min<uint32_t>(
                    *current, systemPath->queuedOrderCount);
            }
        }

        if (!ownsSystemPath &&
            !isObjectWaypointRouteSubtype(order->moveRouteSubtype)) {
            if (const std::optional<ai::ObjectAIOrderCompletion> outcome =
                    m_ai.m_objectAI.moveOrderOutcome(subject, identity)) {
                const bool containmentEnterSucceeded =
                    *outcome == ai::ObjectAIOrderCompletion::Success &&
                    order->source == ObjectOrderSource::System &&
                    order->systemPurpose ==
                        ObjectOrderSystemPurpose::ContainmentEnter;
                const ai::ObjectAIOrderAdmissionResult completed =
                    m_ai.m_objectAI.completeMoveOrder(
                        subject, ai::toAIAsyncOrderIdentity(identity),
                        *outcome);
                if (!completed.succeeded()) {
                    TD_LOG_ERROR(
                        "[GameSession] Object AI Move terminal fallback "
                        "rejected: subject={} tick={} status={}",
                        subject.value, m_presentation.m_confirmedTick,
                        static_cast<uint32_t>(completed.status));
                    continue;
                }
                // ContainmentEnter owns a two-stage transaction: ObjectAI
                // proves arrival, then the post-movement containment resolver
                // validates capacity and commits Attach.  Removing the Move
                // here made that resolver observe an orphaned intent and drop
                // it without ever attaching the passenger.  Failed movement
                // remains terminal, while successful arrival keeps the exact
                // correlated head order until the containment transaction
                // consumes it later in this confirmed tick.
                if (!containmentEnterSucceeded) {
                    const bool moveAsideCompleted =
                        order->source == ObjectOrderSource::System &&
                        order->systemPurpose ==
                            ObjectOrderSystemPurpose::MoveAside &&
                        order->moveRouteSubtype ==
                            ObjectMoveRouteSubtype::MoveAside;
                    queue->orders.erase(queue->orders.begin());
                    ++queue->revision;
                    if (moveAsideCompleted) {
                        // AIMoveOutOfTheWayState::onExit() in RefCode calls
                        // clearMoveOutOfWay().  Our remembered blocker lives
                        // in ECS, so clear it at the same terminal boundary.
                        ecs::remove<
                            ObjectAIMovementObstructionStateComponent>(
                                m_world.m_registry, *entity);
                        ecs::remove<ObjectTemporaryCollisionIgnoreComponent>(
                            m_world.m_registry, *entity);
                    }
                }
                static_cast<void>(
                    m_ai.m_objectAI.clearSubjectTransients(subject));
                continue;
            }
        }

        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *entity);
        if (!locomotion) continue;
        if (ownsSystemPath) {
            const ai::ObjectAIMoveRouteSubtype aiRouteSubtype =
                systemPath->routeSubtype == ObjectMoveRouteSubtype::FollowPath
                    ? ai::ObjectAIMoveRouteSubtype::FollowPath
                    : ai::ObjectAIMoveRouteSubtype::
                          FollowExitProductionPath;
            if (const std::optional<ai::ObjectAIOrderCompletion> outcome =
                    m_ai.m_objectAI.followPathOrderOutcome(
                        subject, identity, aiRouteSubtype)) {
                if (diagnosePlayerTailAppend) {
                    TD_LOG_ERROR(
                        "[WaypointPath] appended route was already terminal: "
                        "subject={} tick={} outcome={} points={} point={}",
                        subject.value, m_presentation.m_confirmedTick,
                        static_cast<uint32_t>(*outcome),
                        systemPath->queuedOrderCount,
                        systemPath->currentPointIndex);
                }
                const ai::ObjectAIOrderAdmissionResult completed =
                    m_ai.m_objectAI.completeFollowPathOrder(
                        subject, identity, *outcome, aiRouteSubtype);
                if (!completed.succeeded()) {
                    TD_LOG_ERROR(
                        "[GameSession] Object AI system-path completion "
                        "rejected: subject={} tick={} status={}",
                        subject.value, m_presentation.m_confirmedTick,
                        static_cast<uint32_t>(completed.status));
                    continue;
                }
                queue->orders.erase(
                    queue->orders.begin(),
                    queue->orders.begin() +
                        systemPath->queuedOrderCount);
                ecs::remove<ObjectSystemPathSequenceComponent>(
                    m_world.m_registry, *entity);
                ++queue->revision;
                static_cast<void>(
                    m_ai.m_objectAI.clearSubjectTransients(subject));
                continue;
            }

            ai::AIStateParameters parameters;
            parameters.sourceOrderRevision = identity.queueRevision;
            parameters.pathSurfaceMask =
                locomotionNavigationSurfaceMask(locomotion);
            parameters.arrivalRadiusRaw =
                nonNegative(locomotion->closeEnough).raw();
            parameters.adjustDestinations = true;
            parameters.pathSequence = ai::AIPathSequenceHandle{
                static_cast<uint64_t>(subject.value)};
            parameters.pathSequenceRevision =
                *systemPathRevision;
            parameters.ignoredObstacle =
                systemPath->ignoredObstacle;
            const ai::ObjectAIOrderAdmissionResult admitted =
                m_ai.m_objectAI.observeFollowPathOrder(
                    subject, identity, parameters, aiRouteSubtype);
            if (diagnosePlayerTailAppend) {
                const std::optional<ai::ObjectAIActorStateView> actorState =
                    m_ai.m_objectAI.actorState(subject);
                TD_LOG_INFO(
                    "[WaypointPath] tail append admission subject={} tick={} "
                    "status={} action={} state={}",
                    subject.value, m_presentation.m_confirmedTick,
                    static_cast<uint32_t>(admitted.status),
                    static_cast<uint32_t>(admitted.action),
                    actorState
                        ? static_cast<uint32_t>(actorState->state)
                        : std::numeric_limits<uint32_t>::max());
            }
            if (!admitted.succeeded()) {
                TD_LOG_ERROR(
                    "[GameSession] Object AI system-path admission "
                    "rejected: subject={} tick={} status={}",
                    subject.value, m_presentation.m_confirmedTick,
                    static_cast<uint32_t>(admitted.status));
            }
            continue;
        }
        if (isObjectWaypointRouteSubtype(order->moveRouteSubtype)) {
            const std::optional<ai::ObjectAIMoveRouteSubtype> routeSubtype =
                objectAIWaypointRouteSubtype(order->moveRouteSubtype);
            if (!routeSubtype) continue;
            const ai::ObjectAIMoveRouteSubtype aiRouteSubtype = *routeSubtype;
            if (const std::optional<ai::ObjectAIOrderCompletion> outcome =
                    m_ai.m_objectAI.waypointOrderOutcome(
                        subject, identity, aiRouteSubtype)) {
                const std::optional<ai::AIWaypointHandle> completedWaypoint =
                    *outcome == ai::ObjectAIOrderCompletion::Success
                    ? m_ai.m_objectAI.waypointOrderCompletedWaypoint(
                          subject, identity, aiRouteSubtype)
                    : std::nullopt;
                const ai::ObjectAIOrderAdmissionResult completed =
                    m_ai.m_objectAI.completeWaypointOrder(
                        subject, identity, *outcome, aiRouteSubtype);
                if (!completed.succeeded()) {
                    TD_LOG_ERROR(
                        "[GameSession] Object AI waypoint completion rejected: "
                        "subject={} tick={} status={}",
                        subject.value, m_presentation.m_confirmedTick,
                        static_cast<uint32_t>(completed.status));
                    continue;
                }
                if (completedWaypoint && completedWaypoint->value != 0 &&
                    completedWaypoint->value - 1 <=
                        std::numeric_limits<uint32_t>::max()) {
                    const ObjectWaypointCompletionComponent completion{
                        .terminalWaypointId = static_cast<uint32_t>(
                            completedWaypoint->value - 1),
                        .waypointGraphRevision =
                            order->waypointGraphRevision,
                        .completedAtTick = m_presentation.m_confirmedTick,
                    };
                    if (ObjectWaypointCompletionComponent* existing =
                            ecs::try_get<ObjectWaypointCompletionComponent>(
                                m_world.m_registry, *entity)) {
                        *existing = completion;
                    } else {
                        ecs::emplace<ObjectWaypointCompletionComponent>(
                            m_world.m_registry, *entity, completion);
                    }
                }
                queue->orders.erase(queue->orders.begin());
                ++queue->revision;
                static_cast<void>(
                    m_ai.m_objectAI.clearSubjectTransients(subject));
                continue;
            }

            ai::AIStateParameters parameters;
            parameters.sourceOrderRevision = queue->revision;
            parameters.pathSurfaceMask =
                locomotionNavigationSurfaceMask(locomotion);
            parameters.arrivalRadiusRaw =
                nonNegative(locomotion->closeEnough).raw();
            parameters.adjustDestinations = true;
            parameters.waypoint = ai::AIWaypointHandle{
                static_cast<uint64_t>(order->waypointStartId) + 1};
            parameters.waypointGraphRevision =
                order->waypointGraphRevision;
            parameters.waypointTeam = ai::AITeamHandle{
                static_cast<uint64_t>(order->waypointTeam.value)};
            parameters.waypointGroupOffset = {
                .xRaw = order->waypointGroupOffsetX.raw(),
                .yRaw = order->waypointGroupOffsetY.raw(),
                .zRaw = 0,
            };
            parameters.waypointGroupSpeedRaw =
                order->waypointGroupSpeed.raw();
            const ai::ObjectAIOrderAdmissionResult admitted =
                m_ai.m_objectAI.observeWaypointOrder(
                    subject, identity, parameters, aiRouteSubtype,
                    order->attackMove);
            if (!admitted.succeeded()) {
                TD_LOG_ERROR(
                    "[GameSession] Object AI waypoint admission rejected: "
                    "subject={} tick={} status={}",
                    subject.value, m_presentation.m_confirmedTick,
                    static_cast<uint32_t>(admitted.status));
            }
            continue;
        }
        const bool supplyDockMove =
            order->source == ObjectOrderSource::System &&
            order->systemPurpose == ObjectOrderSystemPurpose::SupplyTruck &&
            order->targetObject && order->hasTargetPosition;
        const bool builderApproachMove =
            order->source == ObjectOrderSource::System &&
            order->systemPurpose == ObjectOrderSystemPurpose::Builder &&
            order->targetObject && order->hasTargetPosition;
        const bool specialAbilityApproachMove =
            order->source == ObjectOrderSource::System &&
            order->systemPurpose ==
                ObjectOrderSystemPurpose::SpecialAbility &&
            order->targetObject && order->hasTargetPosition;
        const bool constructionEvacuationMove =
            order->source == ObjectOrderSource::System &&
            order->systemPurpose ==
                ObjectOrderSystemPurpose::ConstructionEvacuation &&
            order->targetObject && order->hasTargetPosition;
        const bool fixedTargetObstacleMove =
            supplyDockMove || builderApproachMove ||
            specialAbilityApproachMove || constructionEvacuationMove;
        LogicFixedVec3 goal = fixedOrderTarget(*order);
        if (order->targetObject) {
            const std::optional<ecs::entity> target =
                m_world.m_objects.entityFromId(order->targetObject);
            const TransformComponent* targetTransform = target
                ? ecs::try_get<TransformComponent>(m_world.m_registry, *target)
                : nullptr;
            if (!targetTransform) {
                if (!discardInvalidHeadOrder(
                        m_ai.m_objectAI, subject, *queue)) {
                    TD_LOG_ERROR(
                        "[GameSession] Object AI invalid Move target cleanup "
                        "rejected: subject={} tick={}",
                        subject.value, m_presentation.m_confirmedTick);
                }
                continue;
            }
            if (!fixedTargetObstacleMove) {
                goal = readAuthoritativeObjectPosition(
                    m_world.m_registry, *target,
                    *targetTransform);
            }
        } else if (!order->hasTargetPosition) {
            if (!discardInvalidHeadOrder(
                    m_ai.m_objectAI, subject, *queue)) {
                TD_LOG_ERROR(
                    "[GameSession] Object AI malformed Move cleanup rejected: "
                    "subject={} tick={}",
                    subject.value, m_presentation.m_confirmedTick);
            }
            continue;
        }

        ai::AIStateParameters parameters;
        parameters.goalPosition = {
            .xRaw = goal.x.raw(),
            .yRaw = goal.y.raw(),
            .zRaw = goal.z.raw(),
        };
        parameters.sourceOrderRevision = queue->revision;
        parameters.pathSurfaceMask =
            locomotionNavigationSurfaceMask(locomotion);
        math::q32_32 arrivalRadius = nonNegative(locomotion->closeEnough);
        const bool containmentEnter =
            order->source == ObjectOrderSource::System &&
            order->systemPurpose ==
                ObjectOrderSystemPurpose::ContainmentEnter &&
            static_cast<bool>(order->targetObject);
        const bool autoHealingEnter =
            order->source == ObjectOrderSource::System &&
            order->systemPurpose ==
                ObjectOrderSystemPurpose::AutoFindHealing &&
            static_cast<bool>(order->targetObject);
        const bool pilotContactMove =
            order->source == ObjectOrderSource::System &&
            order->systemPurpose ==
                ObjectOrderSystemPurpose::PilotFindVehicle &&
            static_cast<bool>(order->targetObject);
        const bool assaultRecallMove =
            order->source == ObjectOrderSource::System &&
            order->systemPurpose ==
                ObjectOrderSystemPurpose::AssaultTransport &&
            static_cast<bool>(order->targetObject);
        const bool contactTargetMove = containmentEnter || autoHealingEnter ||
            pilotContactMove || assaultRecallMove;
        if (contactTargetMove) {
            const ObjectGeometryComponent* actorGeometry =
                ecs::try_get<ObjectGeometryComponent>(
                    m_world.m_registry, *entity);
            const std::optional<ecs::entity> target =
                m_world.m_objects.entityFromId(
                    order->targetObject);
            const ObjectGeometryComponent* targetGeometry = target
                ? ecs::try_get<ObjectGeometryComponent>(
                      m_world.m_registry, *target)
                : nullptr;
            arrivalRadius += actorGeometry
                ? nonNegative(actorGeometry->boundingCircleRadiusFixed)
                : math::q32_32{};
            arrivalRadius += targetGeometry
                ? nonNegative(targetGeometry->boundingCircleRadiusFixed)
                : math::q32_32{};
            parameters.goalObject = order->targetObject;
            parameters.ignoredObstacle = order->targetObject;
        }
        if (fixedTargetObstacleMove) {
            // These orders carry two independent facts: targetObject is the
            // structure whose collision is ignored, while targetPosition is
            // the authored protocol/ability approach point. Treating either
            // as a generic follow-object order collapses the goal to the
            // structure origin and prevents the state machine from advancing.
            parameters.ignoredObstacle = order->targetObject;
        }
        const bool moveAside =
            order->source == ObjectOrderSource::System &&
            order->systemPurpose == ObjectOrderSystemPurpose::MoveAside &&
            order->moveRouteSubtype == ObjectMoveRouteSubtype::MoveAside;
        if (moveAside) {
            parameters.ignoredObstacle =
                ObjectId{order->systemPurposeInstance};
        }
        parameters.arrivalRadiusRaw = arrivalRadius.raw();
        parameters.hasGoalPosition = true;
        // Builder orders carry a frozen work-site point rather than a moving
        // object goal. Unlike exact Dock/ability protocol points, the legacy
        // Dozer path asks Pathfinder::findPositionAround() for a reachable
        // point on the same work annulus. Let navigation adjust this one
        // fixed target while still ignoring the structure itself.
        parameters.adjustDestinations = builderApproachMove ||
            constructionEvacuationMove ||
            moveAside ||
            (!containmentEnter && !fixedTargetObstacleMove);
        parameters.groupPathId = order->groupPathId;
        parameters.groupPathMemberOrdinal = order->groupPathMemberOrdinal;
        parameters.groupPathMemberCount = order->groupPathMemberCount;
        parameters.groupPathStart = {
            .xRaw = order->groupPathStartX.raw(),
            .yRaw = order->groupPathStartY.raw(),
            .zRaw = order->groupPathStartZ.raw(),
        };
        parameters.groupPathOffset = {
            .xRaw = order->groupPathOffsetX.raw(),
            .yRaw = order->groupPathOffsetY.raw(),
        };

        const ai::ObjectAIOrderAdmissionResult admitted = order->attackMove
            ? m_ai.m_objectAI.observeAttackMoveOrder(
                  subject, identity, parameters, m_presentation.m_confirmedTick)
            : m_ai.m_objectAI.observeMoveOrder(
                  subject, identity, parameters,
                  order->moveRouteSubtype == ObjectMoveRouteSubtype::Tighten
                      ? ai::ObjectAIMoveRouteSubtype::Tighten
                  : order->moveRouteSubtype ==
                        ObjectMoveRouteSubtype::MoveAside
                      ? ai::ObjectAIMoveRouteSubtype::MoveAside
                  : order->moveRouteSubtype ==
                        ObjectMoveRouteSubtype::WanderInPlace
                      ? ai::ObjectAIMoveRouteSubtype::WanderInPlace
                      : ai::ObjectAIMoveRouteSubtype::Direct,
                  m_presentation.m_confirmedTick);
        if (!admitted.succeeded()) {
            TD_LOG_ERROR(
                "[GameSession] Object AI {} admission rejected: "
                "subject={} tick={} status={}",
                order->attackMove ? "AttackMove" : "Move", subject.value,
                m_presentation.m_confirmedTick,
                static_cast<uint32_t>(admitted.status));
        }
    }
}

void GameSessionAIMoveOrderTransactions::commitWaypointCompletions() {
    ai::ObjectAITransientStore& transients =
        m_ai.m_objectAI.transients();
    const container::Span<const ai::AIWaypointCompletionEvent> staged =
        transients.waypointCompletions();
    container::Vector<ai::AIWaypointCompletionEvent> completions{
        staged.begin(), staged.end()};
    // Consume the bounded output batch before touching admission or ECS so a
    // later admission cleanup cannot invalidate the iterated storage.
    transients.discardWaypointCompletions();

    for (const ai::AIWaypointCompletionEvent& event : completions) {
        if (event.confirmedTick !=
            m_presentation.m_confirmedTick) {
            continue;
        }
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(event.subject);
        ObjectOrderQueueComponent* queue = entity
            ? ecs::try_get<ObjectOrderQueueComponent>(
                  m_world.m_registry, *entity)
            : nullptr;
        if (!queue || queue->orders.empty()) continue;
        const ObjectOrderIntent& order = queue->orders.front();
        const std::optional<ai::ObjectAIMoveRouteSubtype> routeSubtype =
            objectAIWaypointRouteSubtype(order.moveRouteSubtype);
        if (order.kind != ObjectOrderKind::Move || !routeSubtype) continue;

        const ai::ObjectAIOrderIdentity identity =
            objectAIOrderIdentity(event.subject, *queue, order);
        const std::optional<ai::AIWaypointHandle> terminal =
            m_ai.m_objectAI.
                waypointOrderCompletedWaypoint(
                    event.subject, identity, *routeSubtype);
        if (!terminal || *terminal != event.terminal) continue;

        const ai::ObjectAIOrderAdmissionResult completed =
            m_ai.m_objectAI.completeWaypointOrder(
                event.subject, identity,
                ai::ObjectAIOrderCompletion::Success, *routeSubtype);
        if (!completed.succeeded()) {
            TD_LOG_ERROR(
                "[GameSession] Same-tick Object AI waypoint completion "
                "rejected: subject={} tick={} status={}",
                event.subject.value, event.confirmedTick,
                static_cast<uint32_t>(completed.status));
            continue;
        }
        if (event.terminal.value - 1 <=
            std::numeric_limits<uint32_t>::max()) {
            const ObjectWaypointCompletionComponent completion{
                .terminalWaypointId = static_cast<uint32_t>(
                    event.terminal.value - 1),
                .waypointGraphRevision = order.waypointGraphRevision,
                .completedAtTick = event.confirmedTick,
            };
            if (ObjectWaypointCompletionComponent* existing =
                    ecs::try_get<ObjectWaypointCompletionComponent>(
                        m_world.m_registry, *entity)) {
                *existing = completion;
            } else {
                ecs::emplace<ObjectWaypointCompletionComponent>(
                    m_world.m_registry, *entity,
                    completion);
            }
        }
        queue->orders.erase(queue->orders.begin());
        ++queue->revision;
    }
}

GameSessionAIAttackOrderTransactions::
GameSessionAIAttackOrderTransactions(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation) noexcept
    : m_content(content),
      m_world(world),
      m_ai(ai),
      m_presentation(presentation),
      m_policy(content, world, presentation) {}

void GameSessionAIAttackOrderTransactions::produceGuardRetaliationOrders(
    container::Span<const ObjectHealthEvent> damageEvents) {
    const math::q32_32 maximumRetaliationDistance =
        m_content.m_objectSimulationRules.ai.maximumRetaliationDistance;
    const math::q32_32 retaliationFriendsRadius =
        m_content.m_objectSimulationRules.ai.retaliationFriendsRadius;

    // Body freezes the retaliation facts before lifecycle removal. The
    // resulting orders are admitted below before the current outer Damage
    // transaction returns; they do not wait for a later frame publication.
    bool queuedRetaliation = false;
    for (const ObjectHealthEvent& damage : damageEvents) {
        const PlayerState* controllingPlayer =
            m_content.m_players.get(damage.victimPlayer);
        if (damage.kind != ObjectHealthEventKind::Damaged ||
            damage.damageType == game::DamageType::HEALING ||
            !damage.object || !damage.source ||
            !damage.sourceObjectPresent || !damage.sourceIsEnemy ||
            !controllingPlayer ||
            controllingPlayer->controller != PlayerControllerKind::Human ||
            !controllingPlayer->logicalRetaliationEnabled ||
            damage.sourceAirborne || damage.victimDrone) {
            continue;
        }
        if (maximumRetaliationDistance < math::q32_32{}) continue;
        const math::q32_32 victimX = damage.victimPositionFixed.x;
        const math::q32_32 victimY = damage.victimPositionFixed.y;
        const math::q32_32 aggressorDx =
            damage.sourcePositionFixed.x - victimX;
        const math::q32_32 aggressorDy =
            damage.sourcePositionFixed.y - victimY;
        const math::q32_32 retaliationAdmissionRadius =
            maximumRetaliationDistance +
            nonNegative(damage.victimBoundingSphereRadiusFixed) +
            nonNegative(damage.sourceBoundingSphereRadiusFixed);
        if (aggressorDx * aggressorDx + aggressorDy * aggressorDy >
            retaliationAdmissionRadius * retaliationAdmissionRadius) {
            continue;
        }

        const math::q32_32 friendRadius = retaliationFriendsRadius +
            nonNegative(damage.victimBoundingCircleRadiusFixed);
        const math::q32_32 friendRadiusSquared =
            friendRadius * friendRadius;
        const std::optional<ecs::entity> victimEntity =
            m_world.m_objects.entityFromId(damage.object);
        for (const ObjectSpatialRecord& friendRecord :
             m_world.m_spatialIndex.records()) {
            const std::optional<ecs::entity> friendEntity =
                m_world.m_objects.entityFromId(friendRecord.object);
            if (!friendEntity) continue;
            const TransformComponent* friendTransform =
                ecs::try_get<TransformComponent>(m_world.m_registry, *friendEntity);
            const OwnerComponent* friendOwner =
                ecs::try_get<OwnerComponent>(m_world.m_registry, *friendEntity);
            const ObjectHealthComponent* friendHealth =
                ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *friendEntity);
            const ObjectLocomotionComponent* friendLocomotion =
                ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *friendEntity);
            const ObjectWeaponComponent* friendWeapons =
                ecs::try_get<ObjectWeaponComponent>(m_world.m_registry, *friendEntity);
            ObjectOrderQueueComponent* friendQueue =
                ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *friendEntity);
            const ObjectKindOfComponent* friendKinds =
                ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *friendEntity);
            const ObjectStatusComponent* friendStatus =
                ecs::try_get<ObjectStatusComponent>(m_world.m_registry, *friendEntity);
            const std::optional<ai::ObjectAIActorStateView> actor =
                m_ai.m_objectAI.actorState(friendRecord.object);
            const std::optional<ai::ObjectAIOrderCapability> capabilities =
                m_ai.m_objectAI.orderCapabilities(friendRecord.object);
            const bool attackOwned = capabilities &&
                ai::hasObjectAIOrderCapability(
                    *capabilities, ai::ObjectAIOrderCapability::Attack);
            const bool moveStopOwned = capabilities &&
                ai::hasObjectAIOrderCapability(
                    *capabilities, ai::ObjectAIOrderCapability::MoveStop);
            const bool allied = friendOwner && victimEntity &&
                relationshipBetweenObjects(
                    m_world.m_registry, m_content.m_players, *friendEntity,
                    *victimEntity) == PlayerRelationship::Allies;
            const bool hiddenStealth = friendStatus &&
                friendStatus->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::Stealthed)) &&
                !friendStatus->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::Detected));
            const bool usingAbility = friendStatus &&
                friendStatus->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::IsUsingAbility));
            if (!friendTransform || !allied || !friendHealth ||
                friendHealth->effectivelyDead || !friendLocomotion ||
                !friendWeapons || !friendQueue || !friendQueue->orders.empty() ||
                !actor || !actor->idle || !attackOwned || !moveStopOwned ||
                hiddenStealth || usingAbility ||
                hasObjectKind(friendKinds,
                              game::ObjectKindOf::CannotRetaliate) ||
                hasObjectKind(friendKinds, game::ObjectKindOf::Immobile) ||
                hasObjectKind(friendKinds, game::ObjectKindOf::Drone)) {
                continue;
            }
            const LogicFixedVec3 friendPosition =
                readAuthoritativeObjectPosition(
                    m_world.m_registry, *friendEntity, *friendTransform);
            const math::q32_32 friendDx = friendPosition.x - victimX;
            const math::q32_32 friendDy = friendPosition.y - victimY;
            if (friendDx * friendDx + friendDy * friendDy >
                friendRadiusSquared) {
                continue;
            }
            friendQueue->orders.push_back(ObjectOrderIntent{
                .kind = ObjectOrderKind::TacticalAttack,
                .tacticalAttackSubtype =
                    ObjectTacticalAttackSubtype::GuardRetaliate,
                .source = ObjectOrderSource::System,
                .issuedTick = damage.confirmedTick,
                .sourceSequence = damage.object.value,
                .targetObject = damage.source,
                .targetX = friendPosition.x,
                .targetY = friendPosition.y,
                .targetZ = friendPosition.z,
                .hasTargetPosition = true,
                .systemPurpose = ObjectOrderSystemPurpose::Retaliation,
                .systemPurposeInstance = damage.object.value,
            });
            ++friendQueue->revision;
            queuedRetaliation = true;
        }
    }
    if (queuedRetaliation) observeAttackOrders();
}

void GameSessionAIAttackOrderTransactions::observeAttackOrders() {
    for (const ai::AIStateSoASubjectSlot& actor :
         m_ai.m_objectAI.orderedSubjects()) {
        const ObjectId subject = actor.subject;
        const std::optional<ai::ObjectAIOrderCapability> capabilities =
            m_ai.m_objectAI.orderCapabilities(subject);
        if (!capabilities || !ai::hasObjectAIOrderCapability(
                *capabilities, ai::ObjectAIOrderCapability::Attack)) {
            continue;
        }
        const bool moveStopOwned = ai::hasObjectAIOrderCapability(
            *capabilities, ai::ObjectAIOrderCapability::MoveStop);
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(subject);
        ObjectOrderQueueComponent* queue = entity
            ? ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity)
            : nullptr;
        if (!entity || !queue) continue;
        const ObjectOrderIntent* order = !queue->orders.empty()
            ? &queue->orders.front()
            : nullptr;
        const bool ordinaryAttack = isCombatDirectAttackOrder(order);
        const auto clearMatchingCommonTarget =
            [this, subject](ObjectId target) {
                const std::optional<ObjectTeamId> team =
                    m_world.m_objectTeams.teamOf(subject);
                if (team && target) {
                    static_cast<void>(
                        m_world.m_objectTeams.clearCommonTargetIf(
                            *team, target));
                }
            };
        const ObjectContainedByComponent* contained =
            ecs::try_get<ObjectContainedByComponent>(m_world.m_registry, *entity);
        const bool legacyContainedProjection =
            contained && contained->enclosing;
        if (!ordinaryAttack || legacyContainedProjection) {
            const bool ordinaryMove = order &&
                order->kind == ObjectOrderKind::Move &&
                order->source != ObjectOrderSource::System &&
                order->systemPurpose == ObjectOrderSystemPurpose::Generic;
            if (ordinaryMove && moveStopOwned)
                continue;
            const bool tacticalAttack = order &&
                order->kind == ObjectOrderKind::TacticalAttack &&
                (order->tacticalAttackSubtype ==
                     ObjectTacticalAttackSubtype::Hunt ||
                 order->tacticalAttackSubtype ==
                     ObjectTacticalAttackSubtype::Guard ||
                 order->tacticalAttackSubtype ==
                     ObjectTacticalAttackSubtype::AttackSquad ||
                 order->tacticalAttackSubtype ==
                     ObjectTacticalAttackSubtype::AttackArea ||
                 order->tacticalAttackSubtype ==
                     ObjectTacticalAttackSubtype::GuardTunnelNetwork ||
                 order->tacticalAttackSubtype ==
                     ObjectTacticalAttackSubtype::GuardRetaliate) &&
                ((order->source == ObjectOrderSource::Script &&
                  order->systemPurpose ==
                      ObjectOrderSystemPurpose::Generic) ||
                 (order->source == ObjectOrderSource::System &&
                  order->systemPurpose ==
                      ObjectOrderSystemPurpose::Retaliation));
            if (tacticalAttack &&
                ((order->tacticalAttackSubtype !=
                      ObjectTacticalAttackSubtype::Guard &&
                  order->tacticalAttackSubtype !=
                      ObjectTacticalAttackSubtype::GuardTunnelNetwork) ||
                  moveStopOwned))
                continue;
            static_cast<void>(m_ai.m_objectAI.synchronizeOrderExternalRevision(
                subject, queue->externalRevision));
            continue;
        }

        if (order->maximumShots &&
            order->shotsFired >= *order->maximumShots) {
            const ai::ObjectAIOrderAdmissionResult synchronized =
                m_ai.m_objectAI.synchronizeOrderExternalRevision(
                    subject, queue->externalRevision);
            if (!synchronized.succeeded()) {
                TD_LOG_ERROR(
                    "[GameSession] Object AI capped Attack cleanup rejected: "
                    "subject={} tick={} status={}",
                    subject.value, m_presentation.m_confirmedTick,
                    static_cast<uint32_t>(synchronized.status));
                continue;
            }
            clearMatchingCommonTarget(order->targetObject);
            queue->orders.erase(queue->orders.begin());
            ++queue->revision;
            continue;
        }

        const ai::ObjectAIOrderIdentity identity{
            .subject = subject,
            .queueRevision = queue->revision,
            .externalRevision = queue->externalRevision,
            .issuedTick = order->issuedTick,
            .sourceSequence = order->sourceSequence,
            .sourceScriptId = order->sourceScriptId,
            .source = static_cast<ai::ObjectAIOrderSource>(order->source),
            .systemPurpose = static_cast<ai::ObjectAIOrderSystemPurpose>(
                order->systemPurpose),
            .systemPurposeInstance = order->systemPurposeInstance,
        };
        if (const std::optional<ai::ObjectAIOrderCompletion> outcome =
                m_ai.m_objectAI.attackOrderOutcome(subject, identity)) {
            const ai::ObjectAIOrderAdmissionResult completed =
                m_ai.m_objectAI.completeAttackOrder(
                    subject, ai::toAIAsyncOrderIdentity(identity), *outcome);
            if (!completed.succeeded()) {
                TD_LOG_ERROR(
                    "[GameSession] Object AI Attack terminal fallback "
                    "rejected: subject={} tick={} status={}",
                    subject.value, m_presentation.m_confirmedTick,
                    static_cast<uint32_t>(completed.status));
                continue;
            }
            clearMatchingCommonTarget(order->targetObject);
            queue->orders.erase(queue->orders.begin());
            ++queue->revision;
            static_cast<void>(
                m_ai.m_objectAI.clearSubjectTransients(subject));
            continue;
        }

        ai::AIStateId attackState = ai::AIStateId::AttackPosition;
        LogicFixedVec3 goal = fixedOrderTarget(*order);
        if (order->targetObject) {
            const std::optional<ecs::entity> target =
                m_world.m_objects.entityFromId(order->targetObject);
            const TransformComponent* targetTransform = target
                ? ecs::try_get<TransformComponent>(m_world.m_registry, *target)
                : nullptr;
            if (!targetTransform) {
                clearMatchingCommonTarget(order->targetObject);
                if (!discardInvalidHeadOrder(
                        m_ai.m_objectAI, subject, *queue)) {
                    TD_LOG_ERROR(
                        "[GameSession] Object AI invalid Attack target cleanup "
                        "rejected: subject={} tick={}",
                        subject.value, m_presentation.m_confirmedTick);
                }
                continue;
            }
            goal = readAuthoritativeObjectPosition(
                m_world.m_registry, *target,
                *targetTransform);
            attackState = order->forceAttack
                ? ai::AIStateId::ForceAttackObject
                : ai::AIStateId::AttackObject;
        } else if (!order->hasTargetPosition) {
            if (!discardInvalidHeadOrder(
                    m_ai.m_objectAI, subject, *queue)) {
                TD_LOG_ERROR(
                    "[GameSession] Object AI malformed Attack cleanup rejected: "
                    "subject={} tick={}",
                    subject.value, m_presentation.m_confirmedTick);
            }
            continue;
        }

        ai::AIStateParameters parameters;
        parameters.goalObject = order->targetObject;
        parameters.goalPosition = {
            .xRaw = goal.x.raw(),
            .yRaw = goal.y.raw(),
            .zRaw = goal.z.raw(),
        };
        parameters.sourceOrderRevision = queue->revision;
        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *entity);
        parameters.pathSurfaceMask =
            locomotionNavigationSurfaceMask(locomotion);
        if (locomotion) {
            parameters.arrivalRadiusRaw =
                nonNegative(locomotion->closeEnough).raw();
        }
        parameters.hasGoalPosition = true;
        parameters.adjustDestinations = true;

        const ai::ObjectAIOrderAdmissionResult admitted =
            m_ai.m_objectAI.observeAttackOrder(
                subject, identity, attackState, parameters);
        if (!admitted.succeeded()) {
            TD_LOG_ERROR(
                "[GameSession] Object AI Attack admission rejected: "
                "subject={} tick={} status={}",
                subject.value, m_presentation.m_confirmedTick,
                static_cast<uint32_t>(admitted.status));
        } else if (order->targetObject &&
                   m_presentation.m_scenarioDefinition) {
            const std::optional<ObjectTeamId> team =
                m_world.m_objectTeams.teamOf(subject);
            const ObjectTeamRecord* record = team
                ? m_world.m_objectTeams.find(*team) : nullptr;
            const scenario::ScriptTeamDefinition* definition = record &&
                    record->scenarioDefinition
                ? m_presentation.m_scenarioDefinition->findScriptTeam(
                      record->scenarioDefinition)
                : nullptr;
            if (definition && definition->plan.attackCommonTarget) {
                static_cast<void>(m_world.m_objectTeams.setCommonTarget(
                    *team, order->targetObject));
            }
        }
    }
}

void GameSessionAIAttackOrderTransactions::commitAttackCompletions() {
    for (const ai::AIAttackOrderCompletion& completion :
         m_ai.m_objectAI.transients().attackCompletions()) {
        const ai::AIAttackCorrelation& correlation = completion.correlation;
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(correlation.subject);
        ObjectOrderQueueComponent* queue = entity
            ? ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry,
                                                       *entity)
            : nullptr;
        if (!queue || queue->orders.empty()) continue;
        const ObjectOrderIntent& order = queue->orders.front();
        const ai::AIAsyncOrderIdentity& expected = correlation.orderIdentity;
        const bool matches = order.kind == ObjectOrderKind::Attack &&
            expected.subject == correlation.subject &&
            expected.queueRevision == queue->revision &&
            expected.externalRevision == queue->externalRevision &&
            expected.issuedTick == order.issuedTick &&
            expected.sourceSequence == order.sourceSequence &&
            expected.sourceScriptId == order.sourceScriptId &&
            expected.systemPurposeInstance == order.systemPurposeInstance &&
            expected.source == static_cast<uint8_t>(order.source) &&
            expected.systemPurpose ==
                static_cast<uint8_t>(order.systemPurpose);
        if (!matches) continue;

        const ai::ObjectAIOrderAdmissionResult completed =
            m_ai.m_objectAI.completeAttackOrder(
                correlation.subject, expected,
                completion.outcome == ai::AIStateOutcome::Success
                    ? ai::ObjectAIOrderCompletion::Success
                    : ai::ObjectAIOrderCompletion::Failed);
        if (!completed.succeeded()) {
            TD_LOG_ERROR(
                "[GameSession] Object AI Attack completion rejected: "
                "subject={} tick={} status={}",
                correlation.subject.value, m_presentation.m_confirmedTick,
                static_cast<uint32_t>(completed.status));
            continue;
        }
        queue->orders.erase(queue->orders.begin());
        ++queue->revision;
    }
    m_ai.m_objectAI.transients().discardAttackCompletions();
}

void GameSessionAIAttackOrderTransactions::observeTacticalAttackOrders() {
    for (const ai::AIStateSoASubjectSlot& actor :
         m_ai.m_objectAI.orderedSubjects()) {
        const ObjectId subject = actor.subject;
        const std::optional<ai::ObjectAIOrderCapability> capabilities =
            m_ai.m_objectAI.orderCapabilities(subject);
        if (!capabilities || !ai::hasObjectAIOrderCapability(
                *capabilities, ai::ObjectAIOrderCapability::Attack)) {
            continue;
        }
        const bool moveStopOwned = ai::hasObjectAIOrderCapability(
            *capabilities, ai::ObjectAIOrderCapability::MoveStop);
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(subject);
        ObjectOrderQueueComponent* queue = entity
            ? ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity)
            : nullptr;
        if (!entity || !queue) continue;
        const ObjectOrderIntent* order = !queue->orders.empty()
            ? &queue->orders.front()
            : nullptr;
        const ObjectContainedByComponent* contained =
            ecs::try_get<ObjectContainedByComponent>(m_world.m_registry, *entity);
        const bool combatTacticalOrder =
            isCombatTacticalAttackOrder(order);
        const bool tacticalHunt = combatTacticalOrder &&
            order->tacticalAttackSubtype ==
                ObjectTacticalAttackSubtype::Hunt &&
            (!contained || !contained->enclosing);
        const bool tacticalGuard = combatTacticalOrder &&
            order->tacticalAttackSubtype ==
                ObjectTacticalAttackSubtype::Guard &&
            !order->allArmyHunt && !order->useTeamCommonTarget &&
            !order->tacticalTargetTeam &&
            ((order->tacticalTargetAreaId ==
                  std::numeric_limits<uint32_t>::max() &&
              order->tacticalTargetRevision == 0) ||
             (order->tacticalTargetAreaId !=
                  std::numeric_limits<uint32_t>::max() &&
              order->tacticalTargetRevision != 0 &&
              order->hasTargetPosition && !order->targetObject)) &&
             (!contained || !contained->enclosing) && moveStopOwned;
        const bool tacticalTunnelGuard = combatTacticalOrder &&
            order->tacticalAttackSubtype ==
                ObjectTacticalAttackSubtype::GuardTunnelNetwork &&
            !order->targetObject && !order->hasTargetPosition &&
            !order->allArmyHunt && !order->useTeamCommonTarget &&
            !order->tacticalTargetTeam &&
             order->tacticalTargetAreaId ==
                 std::numeric_limits<uint32_t>::max() &&
             order->tacticalTargetRevision == 0 &&
             (!contained || !contained->enclosing) && moveStopOwned;
        const bool tacticalSquad = combatTacticalOrder &&
            order->tacticalAttackSubtype ==
                ObjectTacticalAttackSubtype::AttackSquad &&
            !order->targetObject && !order->hasTargetPosition &&
            !order->allArmyHunt && !order->useTeamCommonTarget &&
            order->tacticalTargetTeam &&
            order->tacticalTargetAreaId ==
                std::numeric_limits<uint32_t>::max() &&
            order->tacticalTargetRevision != 0 &&
            (!contained || !contained->enclosing);
        const bool tacticalArea = combatTacticalOrder &&
            order->tacticalAttackSubtype ==
                ObjectTacticalAttackSubtype::AttackArea &&
            !order->targetObject && !order->hasTargetPosition &&
            !order->allArmyHunt && !order->useTeamCommonTarget &&
            !order->tacticalTargetTeam &&
            order->tacticalTargetAreaId !=
                std::numeric_limits<uint32_t>::max() &&
            order->tacticalTargetRevision != 0 &&
            (!contained || !contained->enclosing);
        const bool tacticalRetaliate = combatTacticalOrder &&
            order->tacticalAttackSubtype ==
                ObjectTacticalAttackSubtype::GuardRetaliate &&
            order->targetObject && order->hasTargetPosition &&
            !order->allArmyHunt && !order->useTeamCommonTarget &&
            !order->tacticalTargetTeam &&
             order->tacticalTargetAreaId ==
                 std::numeric_limits<uint32_t>::max() &&
             order->tacticalTargetRevision == 0 &&
             (!contained || !contained->enclosing) && moveStopOwned;
        if (!tacticalHunt && !tacticalGuard && !tacticalTunnelGuard && !tacticalSquad &&
            !tacticalArea && !tacticalRetaliate) {
            const bool ordinaryAttack =
                isCombatDirectAttackOrder(order);
            if (ordinaryAttack) continue;
            const bool ordinaryMove = order &&
                order->kind == ObjectOrderKind::Move &&
                order->source != ObjectOrderSource::System &&
                order->systemPurpose ==
                    ObjectOrderSystemPurpose::Generic;
            if (ordinaryMove && moveStopOwned)
                continue;
            static_cast<void>(m_ai.m_objectAI.synchronizeOrderExternalRevision(
                subject, queue->externalRevision));
            continue;
        }

        const ai::ObjectAIOrderIdentity identity{
            .subject = subject,
            .queueRevision = queue->revision,
            .externalRevision = queue->externalRevision,
            .issuedTick = order->issuedTick,
            .sourceSequence = order->sourceSequence,
            .sourceScriptId = order->sourceScriptId,
            .source = static_cast<ai::ObjectAIOrderSource>(order->source),
            .systemPurpose = static_cast<ai::ObjectAIOrderSystemPurpose>(
                order->systemPurpose),
            .systemPurposeInstance = order->systemPurposeInstance,
        };
        const ai::ObjectAITacticalAttackSubtype aiSubtype =
            tacticalRetaliate
                ? ai::ObjectAITacticalAttackSubtype::GuardRetaliate
                : tacticalTunnelGuard
                ? ai::ObjectAITacticalAttackSubtype::GuardTunnelNetwork
                : tacticalHunt
                ? ai::ObjectAITacticalAttackSubtype::Hunt
                : tacticalSquad
                    ? ai::ObjectAITacticalAttackSubtype::AttackSquad
                    : tacticalArea
                        ? ai::ObjectAITacticalAttackSubtype::AttackArea
                        : ai::ObjectAITacticalAttackSubtype::Guard;
        const std::optional<ai::ObjectAIOrderCompletion> tacticalOutcome =
            !tacticalGuard && !tacticalTunnelGuard
            ? m_ai.m_objectAI.tacticalAttackOrderOutcome(
                  subject, identity, aiSubtype)
            : std::nullopt;
        if (tacticalOutcome) {
            const ai::ObjectAIOrderAdmissionResult completed =
                m_ai.m_objectAI.completeTacticalAttackOrder(
                    subject, identity, aiSubtype,
                    *tacticalOutcome);
            if (!completed.succeeded()) {
                TD_LOG_ERROR(
                    "[GameSession] Object AI tactical completion rejected: "
                    "subject={} tick={} status={}",
                    subject.value, m_presentation.m_confirmedTick,
                    static_cast<uint32_t>(completed.status));
                continue;
            }
            queue->orders.erase(queue->orders.begin());
            ++queue->revision;
            static_cast<void>(
                m_ai.m_objectAI.clearSubjectTransients(subject));
            continue;
        }
        ai::ObjectAIOrderAdmissionResult admitted;
        if (tacticalRetaliate) {
            const TransformComponent* transform =
                ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
            const ObjectLocomotionComponent* locomotion =
                ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *entity);
            const std::optional<ecs::entity> aggressor =
                m_world.m_objects.entityFromId(order->targetObject);
            const ObjectHealthComponent* aggressorHealth = aggressor
                ? ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *aggressor)
                : nullptr;
            if (!transform || !locomotion || !aggressor ||
                !aggressorHealth || aggressorHealth->effectivelyDead) {
                const std::optional<ai::ObjectAIActorStateView> state =
                    m_ai.m_objectAI.actorState(subject);
                if (!state || state->state !=
                        ai::AIStateId::GuardRetaliate) {
                    // Missing before admission: discard the never-owned
                    // system request. Once active, retain the queue identity
                    // and let Combat's target-invalid feedback unwind the
                    // Guard child transaction before terminal settlement.
                    queue->orders.erase(queue->orders.begin());
                    ++queue->revision;
                }
                continue;
            }
            const LogicFixedVec3 retaliationAnchor =
                fixedOrderTarget(*order);
            admitted = m_ai.m_objectAI.observeGuardRetaliateOrder(
                subject, identity, order->targetObject,
                {.xRaw = retaliationAnchor.x.raw(),
                 .yRaw = retaliationAnchor.y.raw(),
                 .zRaw = retaliationAnchor.z.raw()},
                effectiveObjectVisionRangeFixed(m_world.m_registry, *entity).raw(),
                effectiveObjectVisionRangeFixed(m_world.m_registry, *entity).raw(),
                locomotionNavigationSurfaceMask(locomotion),
                nonNegative(locomotion->closeEnough).raw());
        } else if (tacticalHunt) {
            admitted = m_ai.m_objectAI.observeHuntOrder(
                subject, identity, order->allArmyHunt,
                order->useTeamCommonTarget);
        } else if (tacticalSquad) {
            admitted = m_ai.m_objectAI.observeAttackSquadOrder(
                subject, identity,
                ai::AITargetCollectionHandle{
                    order->tacticalTargetTeam.value},
                order->tacticalTargetRevision,
                m_policy.squadTargetSelection(
                    subject,
                    identity.source == ai::ObjectAIOrderSource::Player));
        } else if (tacticalArea) {
            admitted = m_ai.m_objectAI.observeAttackAreaOrder(
                subject, identity,
                ai::AIAttackAreaHandle{
                    static_cast<uint64_t>(
                        order->tacticalTargetAreaId) + 1},
                order->tacticalTargetRevision);
        } else if (tacticalTunnelGuard) {
            const TransformComponent* transform =
                ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
            const ObjectLocomotionComponent* locomotion =
                ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *entity);
            if (!transform || !locomotion) continue;
            const ai::ObjectAIOrderAdmissionResult admittedTunnel =
                m_ai.m_objectAI.observeGuardTunnelNetworkOrder(
                    subject, identity,
                    [&]() {
                        const LogicFixedVec3 position =
                            readAuthoritativeObjectPosition(
                                m_world.m_registry,
                                *entity, *transform);
                        return ai::AIFixedPosition{
                            .xRaw = position.x.raw(),
                            .yRaw = position.y.raw(),
                            .zRaw = position.z.raw(),
                        };
                    }(),
                    effectiveObjectVisionRangeFixed(m_world.m_registry, *entity).raw(),
                    effectiveObjectVisionRangeFixed(m_world.m_registry, *entity).raw(),
                    locomotionNavigationSurfaceMask(locomotion),
                    nonNegative(locomotion->closeEnough).raw());
            if (!admittedTunnel.succeeded()) {
                TD_LOG_ERROR(
                    "[GameSession] Object AI tunnel guard admission rejected: "
                    "subject={} tick={} status={}",
                    subject.value, m_presentation.m_confirmedTick,
                    static_cast<uint32_t>(admittedTunnel.status));
            }
            continue;
        } else {
            const ThingTemplateComponent* type =
                ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
            const TransformComponent* transform =
                ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
            const ObjectLocomotionComponent* locomotion =
                ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *entity);
            if (!transform || !locomotion) {
                TD_LOG_ERROR(
                    "[GameSession] Object AI Guard admission lacks "
                    "transform/locomotion: subject={} tick={}",
                    subject.value, m_presentation.m_confirmedTick);
                continue;
            }
            const std::optional<ecs::entity> guardTarget = order->targetObject
                ? m_world.m_objects.entityFromId(order->targetObject)
                : std::nullopt;
            const TransformComponent* guardTargetTransform = guardTarget
                ? ecs::try_get<TransformComponent>(
                      m_world.m_registry, *guardTarget)
                : nullptr;
            const bool guardAreaOrder =
                order->tacticalTargetAreaId !=
                std::numeric_limits<uint32_t>::max();
            const game::terrain::PolygonTriggerRecord* guardArea =
                guardAreaOrder
                    ? m_content.m_terrain.triggerById(
                          order->tacticalTargetAreaId)
                    : nullptr;
            const bool guardAreaValid = guardArea &&
                game::terrain::TerrainLogic::triggerRevision(*guardArea) ==
                    order->tacticalTargetRevision;
            const auto guardAreaBounds = guardAreaValid
                ? game::terrain::TerrainLogic::legacyTriggerBounds(*guardArea)
                : std::nullopt;
            const auto settleMissingGuardTarget = [&]() {
                const ai::ObjectAIOrderAdmissionResult completed =
                    m_ai.m_objectAI.completeTacticalAttackOrder(
                        subject, identity,
                        ai::ObjectAITacticalAttackSubtype::Guard,
                        ai::ObjectAIOrderCompletion::Success);
                if (!completed.succeeded() &&
                    completed.status !=
                        ai::ObjectAIOrderAdmissionStatus::StaleIdentity) {
                    return false;
                }
                // Even a successfully completed admission may still have a
                // live Guard state pending its next shadow transition. A new
                // external revision both handles the pre-admission case and
                // returns that correlated state to Idle before queue removal.
                ++queue->externalRevision;
                if (queue->externalRevision == 0)
                    ++queue->externalRevision;
                const ai::ObjectAIOrderAdmissionResult synchronized =
                    m_ai.m_objectAI.synchronizeOrderExternalRevision(
                        subject, queue->externalRevision);
                if (!synchronized.succeeded()) return false;
                queue->orders.erase(queue->orders.begin());
                ++queue->revision;
                static_cast<void>(
                    m_ai.m_objectAI.clearSubjectTransients(subject));
                return true;
            };
            if (order->targetObject && !guardTargetTransform) {
                // GuardObject tracks a live object, not its last sampled
                // coordinate. RefCode drops back to Idle when that target is
                // gone; complete the correlated owner before removing the
                // queue head so no stale Guard child survives.
                static_cast<void>(settleMissingGuardTarget());
                continue;
            }
            if (guardAreaOrder &&
                (!guardAreaBounds ||
                 guardAreaBounds->radius < math::q32_32{})) {
                // PolygonTrigger handles are immutable for one map load, but
                // a malformed/stale handle must terminate like RefCode's
                // missing area instead of becoming an unfiltered Guard.
                static_cast<void>(settleMissingGuardTarget());
                continue;
            }
            const OwnerComponent* owner =
                ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
            const ThingTemplateComponent* thing =
                ecs::try_get<ThingTemplateComponent>(
                    m_world.m_registry, *entity);
            const PlayerState* player = owner
                ? m_content.m_players.get(owner->player) : nullptr;
            const bool humanControlled = player &&
                player->controller == PlayerControllerKind::Human;
            const math::q32_32 baseVisionRange =
                effectiveObjectVisionRangeFixed(m_world.m_registry, *entity);
            math::q32_32 innerVisionRange = baseVisionRange *
                m_content.m_objectSimulationRules.ai.guardInnerModifier(
                    humanControlled);
            math::q32_32 outerVisionRange = baseVisionRange *
                m_content.m_objectSimulationRules.ai.guardOuterModifier(
                    humanControlled);
            if (player && player->controller == PlayerControllerKind::Ai) {
                const ObjectAIBehaviorPolicyComponent* behavior =
                    ecs::try_get<ObjectAIBehaviorPolicyComponent>(
                        m_world.m_registry, *entity);
                if (behavior) {
                    math::q32_32 moodModifier{int32_t{1}};
                    if (behavior->attitude == ObjectAIAttitude::Sleep)
                        moodModifier = {};
                    else if (behavior->attitude == ObjectAIAttitude::Alert)
                        moodModifier =
                            m_content.m_objectSimulationRules.ai.alertRangeModifier;
                    else if (behavior->attitude ==
                             ObjectAIAttitude::Aggressive)
                        moodModifier = m_content.m_objectSimulationRules.ai
                            .aggressiveRangeModifier;
                    innerVisionRange *= moodModifier;
                    outerVisionRange *= moodModifier;
                }
            }
            const int64_t visionRangeRaw = outerVisionRange.raw();
            const int64_t guardRangeRaw = guardAreaBounds
                ? guardAreaBounds->radius.raw()
                : innerVisionRange.raw();
            const LogicFixedVec3 fixedPosition = guardAreaBounds
                ? LogicFixedVec3{
                      guardAreaBounds->centerX,
                      guardAreaBounds->centerY,
                      math::q32_32::from_raw(
                          m_content.m_terrain.
                              groundHeightRaw(
                                  guardAreaBounds->centerX.raw(),
                                  guardAreaBounds->centerY.raw()))}
                : guardTargetTransform
                ? readAuthoritativeObjectPosition(
                      m_world.m_registry, *guardTarget, *guardTargetTransform)
                : order->hasTargetPosition
                    ? fixedOrderTarget(*order)
                    : readAuthoritativeObjectPosition(
                          m_world.m_registry, *entity, *transform);
            admitted = m_ai.m_objectAI.observeGuardOrder(
                subject, identity,
                {.xRaw = fixedPosition.x.raw(),
                 .yRaw = fixedPosition.y.raw(),
                 .zRaw = fixedPosition.z.raw()},
                guardRangeRaw, visionRangeRaw,
                locomotionNavigationSurfaceMask(locomotion),
                nonNegative(locomotion->closeEnough).raw(),
                thing && thing->archetype &&
                    thing->archetype->templateData.enterGuard,
                static_cast<bool>(order->targetObject),
                order->guardWithoutPursuit,
                order->guardFlyingOnly,
                guardAreaOrder
                    ? ai::AIAttackAreaHandle{
                          static_cast<uint64_t>(
                              order->tacticalTargetAreaId) + 1}
                    : ai::AIAttackAreaHandle{},
                guardAreaOrder ? order->tacticalTargetRevision : 0);
        }
        if (!admitted.succeeded()) {
            TD_LOG_ERROR(
                "[GameSession] Object AI tactical admission rejected: "
                "subject={} tick={} status={}",
                subject.value, m_presentation.m_confirmedTick,
                static_cast<uint32_t>(admitted.status));
        }
    }
}

} // namespace engine
