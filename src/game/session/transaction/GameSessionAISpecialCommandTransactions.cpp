#include "game/session/transaction/GameSessionAISpecialCommandTransactions.h"
#include "game/session/frame/GameSessionFxAnchorSnapshot.h"
#include "game/session/state/GameSessionDomainState.h"

#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/structure/ObjectChinookCombatDropResolver.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/contracts/ObjectToppleMath.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/ai/runtime/ObjectAITransientStore.h"
#include "game/object/ai/states/special/AIDockStateSoAKernels.h"
#include "game/object/ai/states/special/AIContainmentStateSoAKernels.h"
#include "game/object/ai/states/special/AIInsertionStateSoAKernels.h"
#include "game/session/query/ObjectContainmentQuery.h"
#include "math/fixed/q32_32_trig.h"

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

} // namespace

void GameSessionAISpecialCommandTransactions::resolve() {
    ai::ObjectAITransientStore& transients = m_ai.m_objectAI.transients();
    const auto raiseFeedbackFault = [this](
        ai::ObjectAITransientStatus status, ObjectId subject) {
        static_cast<void>(m_publication.raiseSimulationFault({
            .domain = SimulationFaultDomain::Feedback,
            .code = status == ai::ObjectAITransientStatus::CapacityExceeded
                ? SimulationFaultCode::CapacityExceeded
                : SimulationFaultCode::InvalidEvent,
            .confirmedTick = m_presentation.m_confirmedTick,
            .subject = subject.value,
        }));
    };
    // An Enter state may finish, fail, or be superseded before the normal
    // post-movement containment resolver runs.  Remove only the approach
    // order owned by that intent; clearing the entire queue would erase an
    // unrelated player or script command that was issued after it.
    const auto clearContainmentEnterMove = [this](ecs::entity subject,
                                                  ObjectId target,
                                                  uint32_t sourceSequence) {
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry,
                                                     subject);
        if (!queue || !target || sourceSequence == 0) return false;
        const auto retained = std::remove_if(
            queue->orders.begin(), queue->orders.end(),
            [target, sourceSequence](const ObjectOrderIntent& order) {
                return order.source == ObjectOrderSource::System &&
                    order.systemPurpose ==
                        ObjectOrderSystemPurpose::ContainmentEnter &&
                    order.systemPurposeInstance == target.value &&
                    order.sourceSequence == sourceSequence;
            });
        if (retained == queue->orders.end()) return false;
        queue->orders.erase(retained, queue->orders.end());
        ++queue->revision;
        return true;
    };

    // RepairDock owns reservation/action state; GameSession owns the
    // movement bridge. Requests stay in the transient store while a Move is
    // active so the same correlation can be polled without a pointer or an
    // out-of-band callback.
    container::Vector<ai::AIDockRequest> consumedDockRequests;
    const auto clearDockMove = [this](ObjectId subject) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(subject);
        ObjectOrderQueueComponent* queue = entity
            ? ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity)
            : nullptr;
        if (!queue) return false;
        const auto retained = std::remove_if(
            queue->orders.begin(), queue->orders.end(),
            [](const ObjectOrderIntent& order) {
                return order.source == ObjectOrderSource::System &&
                    order.systemPurpose ==
                        ObjectOrderSystemPurpose::RepairDock;
            });
        if (retained != queue->orders.end()) {
            queue->orders.erase(retained, queue->orders.end());
            ++queue->revision;
            return true;
        }
        return false;
    };
    const auto stageDock = [&transients](
        const ai::AIDockRequest& request,
        ai::AIDockFeedbackStatus status,
        ai::AIFixedPosition position = {},
        int32_t approachPosition = -1,
        uint32_t actionDelayTicks = 0,
        ObjectId drone = INVALID_OBJECT_ID,
        bool allowPassthrough = false) {
        return transients.stage(ai::AIDockFeedback{
            .correlation = request.correlation,
            .request = request.kind,
            .status = status,
            .position = position,
            .approachPosition = approachPosition,
            .actionDelayTicks = actionDelayTicks,
            .drone = drone,
            .allowPassthrough = allowPassthrough,
        });
    };
    for (const ai::AIDockRequest& request : transients.dockRequests()) {
        const ObjectId subject = request.correlation.token.subject;
        const ObjectId dock = request.correlation.token.dock;
        bool consume = false;
        bool sideEffectCommitted = false;
        ai::ObjectAITransientStatus staged =
            ai::ObjectAITransientStatus::Success;

        if (request.kind == ai::AIDockRequestKind::BeginMove) {
            const std::optional<ecs::entity> entity =
                m_world.m_objects.entityFromId(subject);
            const TransformComponent* transform = entity
                ? ecs::try_get<TransformComponent>(m_world.m_registry, *entity)
                : nullptr;
            ObjectOrderQueueComponent* queue = entity
                ? ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity)
                : nullptr;
            const ObjectLocomotionComponent* locomotion = entity
                ? ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *entity)
                : nullptr;
            if (!entity || !transform || !queue || !locomotion) {
                staged = stageDock(
                    request, ai::AIDockFeedbackStatus::MovementFailed);
                consume = staged == ai::ObjectAITransientStatus::Success;
            } else {
                const math::q32_32 goalX =
                    math::q32_32::from_raw(request.position.xRaw);
                const math::q32_32 goalY =
                    math::q32_32::from_raw(request.position.yRaw);
                const LogicFixedVec3 subjectPosition =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry, *entity,
                        *transform);
                const math::q32_32 dx =
                    subjectPosition.x - goalX;
                const math::q32_32 dy =
                    subjectPosition.y - goalY;
                const math::q32_32 tolerance = math::q32_32::max(
                    math::q32_32{int32_t{1}}, locomotion->closeEnough);
                if (dx * dx + dy * dy <= tolerance * tolerance) {
                    sideEffectCommitted = clearDockMove(subject);
                    staged = stageDock(
                        request,
                        ai::AIDockFeedbackStatus::MovementSucceeded);
                    consume = staged ==
                        ai::ObjectAITransientStatus::Success;
                } else {
                    auto existing = std::find_if(
                        queue->orders.begin(), queue->orders.end(),
                        [](const ObjectOrderIntent& order) {
                            return order.source == ObjectOrderSource::System &&
                                order.systemPurpose ==
                                    ObjectOrderSystemPurpose::RepairDock;
                        });
                    if (existing == queue->orders.end()) {
                        if (queue->orders.size() >=
                            ObjectOrderQueueComponent::MaximumQueuedOrders) {
                            staged = stageDock(
                                request,
                                ai::AIDockFeedbackStatus::MovementFailed);
                            consume = staged ==
                                ai::ObjectAITransientStatus::Success;
                        } else {
                            queue->orders.insert(
                                queue->orders.begin(), ObjectOrderIntent{
                                    .kind = ObjectOrderKind::Move,
                                    .source = ObjectOrderSource::System,
                                    .issuedTick = m_presentation.m_confirmedTick,
                                    .sourceSequence = request.correlation
                                        .exchangeSequence,
                                    .targetX = goalX,
                                     .targetY = goalY,
                                     .targetZ = math::q32_32::from_raw(
                                         request.position.zRaw),
                                     .hasTargetPosition = true,
                                     .systemPurpose =
                                         ObjectOrderSystemPurpose::RepairDock,
                                 });
                            ++queue->revision;
                            sideEffectCommitted = true;
                        }
                    } else if (!existing->hasTargetPosition ||
                               existing->targetX != goalX ||
                               existing->targetY != goalY ||
                               existing->targetZ.raw() !=
                                   request.position.zRaw) {
                        existing->targetX = goalX;
                        existing->targetY = goalY;
                        existing->targetZ = math::q32_32::from_raw(
                            request.position.zRaw);
                        existing->hasTargetPosition = true;
                        existing->sourceSequence =
                            request.correlation.exchangeSequence;
                        ++queue->revision;
                        sideEffectCommitted = true;
                    }
                    if (!consume) {
                        staged = stageDock(
                            request,
                            ai::AIDockFeedbackStatus::MovementMoving);
                        // Retain this request until arrival/failure.
                    }
                }
            }
        } else if (request.kind == ai::AIDockRequestKind::EndMove ||
                   request.kind ==
                       ai::AIDockRequestKind::RestorePathing) {
            static_cast<void>(clearDockMove(subject));
            consume = true;
        } else if (request.kind ==
                   ai::AIDockRequestKind::QueryRallyPosition) {
            const std::optional<ecs::entity> dockEntity =
                m_world.m_objects.entityFromId(dock);
            const ObjectProductionExitComponent* exit = dockEntity
                ? ecs::try_get<ObjectProductionExitComponent>(
                      m_world.m_registry, *dockEntity)
                : nullptr;
            if (exit && exit->rallyPoint.exists) {
                staged = stageDock(
                    request, ai::AIDockFeedbackStatus::Accepted,
                    {.xRaw = exit->rallyPoint.x.raw(),
                     .yRaw = exit->rallyPoint.y.raw(),
                     .zRaw = exit->rallyPoint.z.raw()});
            } else {
                staged = stageDock(
                    request, ai::AIDockFeedbackStatus::NoRally);
            }
            consume = staged == ai::ObjectAITransientStatus::Success;
        } else if (request.kind == ai::AIDockRequestKind::CancelDock) {
            static_cast<void>(m_world.m_objectSimulation.processRepairDockCommand(
                m_world.m_registry, m_world.m_objects, ObjectRepairDockCommand{
                    .kind = ObjectRepairDockCommandKind::Cancel,
                    .dock = dock,
                    .docker = subject,
                    .confirmedTick = m_presentation.m_confirmedTick,
            }));
            static_cast<void>(clearDockMove(subject));
            consume = true;
        } else if (request.correlation.token.purpose !=
                   ai::AIDockPurpose::Repair) {
            staged = stageDock(
                request, ai::AIDockFeedbackStatus::Unsupported);
            consume = staged == ai::ObjectAITransientStatus::Success;
        } else {
            std::optional<ObjectRepairDockCommandKind> kind;
            bool awaitsFeedback = true;
            switch (request.kind) {
            case ai::AIDockRequestKind::ReserveApproach:
                kind = ObjectRepairDockCommandKind::ReserveApproach; break;
            case ai::AIDockRequestKind::PollClearance:
                kind = ObjectRepairDockCommandKind::PollClearance; break;
            case ai::AIDockRequestKind::AdvanceApproach:
                kind = ObjectRepairDockCommandKind::AdvanceApproach; break;
            case ai::AIDockRequestKind::QueryEntryPosition:
                kind = ObjectRepairDockCommandKind::QueryEntryPosition; break;
            case ai::AIDockRequestKind::QueryDockPosition:
                kind = ObjectRepairDockCommandKind::QueryDockPosition; break;
            case ai::AIDockRequestKind::QueryExitPosition:
                kind = ObjectRepairDockCommandKind::QueryExitPosition; break;
            case ai::AIDockRequestKind::NotifyApproachReached:
                kind = ObjectRepairDockCommandKind::NotifyApproachReached;
                awaitsFeedback = false; break;
            case ai::AIDockRequestKind::NotifyEnterReached:
                kind = ObjectRepairDockCommandKind::NotifyEnterReached;
                awaitsFeedback = false; break;
            case ai::AIDockRequestKind::NotifyDockReached:
                kind = ObjectRepairDockCommandKind::NotifyDockReached;
                awaitsFeedback = false; break;
            case ai::AIDockRequestKind::NotifyExitReached:
                kind = ObjectRepairDockCommandKind::NotifyExitReached;
                awaitsFeedback = false; break;
            case ai::AIDockRequestKind::ProcessAction:
                kind = ObjectRepairDockCommandKind::ProcessAction; break;
            default: break;
            }
            if (!kind) {
                staged = stageDock(
                    request, ai::AIDockFeedbackStatus::Unsupported);
                consume = staged == ai::ObjectAITransientStatus::Success;
            } else {
                const ObjectRepairDockCommandResult result =
                    m_world.m_objectSimulation.processRepairDockCommand(
                        m_world.m_registry, m_world.m_objects, ObjectRepairDockCommand{
                            .kind = *kind,
                            .dock = dock,
                            .docker = subject,
                            .approachPosition = request.approachPosition,
                            .confirmedTick = m_presentation.m_confirmedTick,
                        });
                sideEffectCommitted =
                    *kind == ObjectRepairDockCommandKind::ReserveApproach ||
                    *kind == ObjectRepairDockCommandKind::AdvanceApproach ||
                    *kind == ObjectRepairDockCommandKind::ProcessAction;
                const auto status = [&result] {
                    switch (result.status) {
                    case ObjectRepairDockCommandStatus::Accepted:
                        return ai::AIDockFeedbackStatus::Accepted;
                    case ObjectRepairDockCommandStatus::Denied:
                        return ai::AIDockFeedbackStatus::Denied;
                    case ObjectRepairDockCommandStatus::DockMissing:
                        return ai::AIDockFeedbackStatus::DockMissing;
                    case ObjectRepairDockCommandStatus::DockClosed:
                        return ai::AIDockFeedbackStatus::DockClosed;
                    case ObjectRepairDockCommandStatus::ClearanceWaiting:
                        return ai::AIDockFeedbackStatus::ClearanceWaiting;
                    case ObjectRepairDockCommandStatus::ClearToAdvance:
                        return ai::AIDockFeedbackStatus::ClearToAdvance;
                    case ObjectRepairDockCommandStatus::ClearToEnter:
                        return ai::AIDockFeedbackStatus::ClearToEnter;
                    case ObjectRepairDockCommandStatus::ActionContinue:
                        return ai::AIDockFeedbackStatus::ActionContinue;
                    case ObjectRepairDockCommandStatus::ActionComplete:
                        return ai::AIDockFeedbackStatus::ActionComplete;
                    }
                    return ai::AIDockFeedbackStatus::Unsupported;
                }();
                if (awaitsFeedback) {
                    staged = stageDock(
                        request, status,
                        {.xRaw = result.position.x.raw(),
                         .yRaw = result.position.y.raw(),
                         .zRaw = result.position.z.raw()},
                        result.approachPosition, 1u, result.drone,
                        result.allowsPassthrough);
                    consume = staged ==
                        ai::ObjectAITransientStatus::Success;
                } else {
                    consume = true;
                }
            }
        }

        if (staged != ai::ObjectAITransientStatus::Success) {
            TD_LOG_ERROR(
                "[GameSession] Object AI Dock feedback rejected: "
                "subject={} tick={} status={}",
                subject.value, m_presentation.m_confirmedTick,
                static_cast<uint32_t>(staged));
            if (sideEffectCommitted) {
                raiseFeedbackFault(staged, subject);
                return;
            }
        }
        if (consume) consumedDockRequests.push_back(request);
    }
    for (const ai::AIDockRequest& request : consumedDockRequests) {
        static_cast<void>(transients.removeDockRequest(
            request.correlation, request.kind));
    }

    // Containment commands are sorted by correlation.  Publish at most one
    // owner result for one state step: duplicate feedback for the same
    // correlation is deliberately treated as ambiguous by the SoA kernel.
    bool containmentConsumed = true;
    const auto containmentCommands = transients.containmentCommands();
    size_t containmentCursor = 0;
    while (containmentCursor < containmentCommands.size()) {
        const ai::AIContainmentCorrelation correlation =
            containmentCommands[containmentCursor].correlation;
        size_t containmentEnd = containmentCursor + 1;
        while (containmentEnd < containmentCommands.size() &&
               containmentCommands[containmentEnd].correlation ==
                   correlation) {
            ++containmentEnd;
        }

        ai::AIContainmentFeedback feedback{
            .correlation = correlation,
            .kind = ai::AIContainmentFeedbackKind::None,
        };
        for (size_t index = containmentCursor;
             index < containmentEnd; ++index) {
            const ai::AIContainmentCommand& command =
                containmentCommands[index];
            feedback.goal = command.goal;
            const std::optional<ecs::entity> subjectEntity =
                m_world.m_objects.entityFromId(correlation.subject);
            if (!subjectEntity) {
                switch (command.kind) {
                case ai::AIContainmentCommandKind::SetWantsToExit:
                case ai::AIContainmentCommandKind::ExitViaDoor:
                    feedback.kind =
                        ai::AIContainmentFeedbackKind::ExitNoInterface;
                    break;
                default:
                    feedback.kind = ai::AIContainmentFeedbackKind::GoalMissing;
                    break;
                }
                continue;
            }
            switch (command.kind) {
            case ai::AIContainmentCommandKind::SetWantsToEnter:
                // BeginEnterMovement below installs the existing
                // containment-enter intent.  This edge is retained as a
                // correlated acknowledgement so the parent state does not
                // treat the hand-off as unsupported before that command is
                // consumed in the same batch.
                feedback.kind = ai::AIContainmentFeedbackKind::EnterMoving;
                break;

            case ai::AIContainmentCommandKind::BeginEnterMovement: {
                const std::optional<ecs::entity> target =
                    m_world.m_objects.entityFromId(command.goal);
                const TransformComponent* targetTransform = target
                    ? ecs::try_get<TransformComponent>(m_world.m_registry,
                                                        *target)
                    : nullptr;
                if (!target || !targetTransform) {
                    feedback.kind = ai::AIContainmentFeedbackKind::GoalMissing;
                    break;
                }
                ObjectScriptContainmentEnterComponent* intent =
                    ecs::try_get<ObjectScriptContainmentEnterComponent>(
                        m_world.m_registry, *subjectEntity);
                if (!intent) {
                    intent = &ecs::emplace<ObjectScriptContainmentEnterComponent>(
                        m_world.m_registry, *subjectEntity,
                        ObjectScriptContainmentEnterComponent{
                            .target = command.goal,
                            .issuedTick = m_presentation.m_confirmedTick,
                            .sourceSequence =
                                command.correlation.stateRequest.sequence,
                            .reservedCapacity = 1u,
                        });
                } else {
                    intent->target = command.goal;
                    intent->issuedTick = m_presentation.m_confirmedTick;
                    intent->sourceSequence =
                        command.correlation.stateRequest.sequence;
                    intent->reservedCapacity = 1u;
                    intent->approachAttempts = 0;
                    ++intent->revision;
                    if (intent->revision == 0) ++intent->revision;
                }
                ObjectOrderQueueComponent* queue =
                    ecs::try_get<ObjectOrderQueueComponent>(
                        m_world.m_registry, *subjectEntity);
                if (!queue) {
                    queue = &ecs::emplace<ObjectOrderQueueComponent>(
                        m_world.m_registry, *subjectEntity);
                }
                const LogicFixedVec3 targetPosition =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry, *target, *targetTransform);
                // Containment owns only its own approach order.  Clearing
                // the whole queue here discards unrelated player/script
                // intent and can strand a unit after an AI enter attempt.
                const auto retained = std::remove_if(
                    queue->orders.begin(), queue->orders.end(),
                    [](const ObjectOrderIntent& order) {
                        return order.source == ObjectOrderSource::System &&
                            order.systemPurpose ==
                                ObjectOrderSystemPurpose::ContainmentEnter;
                    });
                queue->orders.erase(retained, queue->orders.end());
                queue->orders.insert(queue->orders.begin(), {
                    .kind = ObjectOrderKind::Move,
                    .source = ObjectOrderSource::System,
                    .issuedTick = m_presentation.m_confirmedTick,
                    .sourceSequence = command.correlation.stateRequest.sequence,
                    .targetObject = command.goal,
                    .targetX = targetPosition.x,
                    .targetY = targetPosition.y,
                    .targetZ = targetPosition.z,
                    .hasTargetPosition = true,
                    .systemPurpose = ObjectOrderSystemPurpose::ContainmentEnter,
                    .systemPurposeInstance = command.goal.value,
                });
                ++queue->revision;
                ++queue->externalRevision;
                if (queue->externalRevision == 0) ++queue->externalRevision;
                feedback.kind = ai::AIContainmentFeedbackKind::EnterMoving;
                break;
            }

            case ai::AIContainmentCommandKind::RefreshEnterMovementGoal: {
                const ObjectContainedByComponent* contained =
                    ecs::try_get<ObjectContainedByComponent>(
                        m_world.m_registry, *subjectEntity);
                feedback.kind = contained &&
                    contained->container == command.goal
                    ? ai::AIContainmentFeedbackKind::EnterHeld
                    : ai::AIContainmentFeedbackKind::EnterMoving;
                break;
            }

            case ai::AIContainmentCommandKind::EndEnterMovement:
                if (const ObjectScriptContainmentEnterComponent* intent =
                        ecs::try_get<ObjectScriptContainmentEnterComponent>(
                            m_world.m_registry, *subjectEntity)) {
                    static_cast<void>(clearContainmentEnterMove(
                        *subjectEntity, intent->target, intent->sourceSequence));
                }
                ecs::remove<ObjectScriptContainmentEnterComponent>(
                    m_world.m_registry, *subjectEntity);
                feedback.kind = ai::AIContainmentFeedbackKind::EnterMovementFailed;
                break;

            case ai::AIContainmentCommandKind::AttackGoal:
                // The containment state uses this only after an Enter denial.
                // Preserve the terminal denial instead of clearing a valid
                // command as unsupported; combat order admission remains its
                // own owner.
                feedback.kind = ai::AIContainmentFeedbackKind::EnterDenied;
                break;

            case ai::AIContainmentCommandKind::SetWantsToExit: {
                const std::optional<ecs::entity> containerEntity =
                    m_world.m_objects.entityFromId(command.goal);
                const ObjectContainedByComponent* edge = subjectEntity
                    ? ecs::try_get<ObjectContainedByComponent>(
                          m_world.m_registry, *subjectEntity)
                    : nullptr;
                const ObjectContainmentRuntimeComponent* runtime =
                    containerEntity
                    ? ecs::try_get<ObjectContainmentRuntimeComponent>(
                          m_world.m_registry, *containerEntity)
                    : nullptr;
                const ObjectHealthComponent* containerHealth =
                    containerEntity
                    ? ecs::try_get<ObjectHealthComponent>(
                          m_world.m_registry, *containerEntity)
                    : nullptr;
                if (!subjectEntity || !containerEntity || !edge ||
                    edge->container != command.goal || !runtime ||
                    !runtime->plan || edge->containmentRuleIndex >=
                        runtime->plan->rules.size()) {
                    feedback.kind =
                        ai::AIContainmentFeedbackKind::ExitNoInterface;
                } else if (m_world.m_objects.isPendingDestroy(command.goal) ||
                           (containerHealth &&
                            containerHealth->effectivelyDead)) {
                    feedback.kind =
                        ai::AIContainmentFeedbackKind::ExitContainerDead;
                } else {
                    const ObjectContainmentRule& rule = runtime->plan->rules[
                        edge->containmentRuleIndex];
                    const ObjectAirborneComponent* airborne =
                        ecs::try_get<ObjectAirborneComponent>(
                            m_world.m_registry, *containerEntity);
                    if (rule.kind != ObjectContainmentKind::RiderChange &&
                        m_presentation.m_confirmedTick <
                            runtime->exitNotBusyTick) {
                        feedback.kind =
                            ai::AIContainmentFeedbackKind::ExitBusy;
                    } else if (rule.delayExitInAir && airborne &&
                               airborne->isAirborne) {
                        feedback.kind =
                            ai::AIContainmentFeedbackKind::ExitWait;
                    } else {
                        feedback.kind =
                            ai::AIContainmentFeedbackKind::ExitReady;
                        feedback.reservedDoor =
                            ai::AIContainmentDoor::Door1;
                    }
                }
                break;
            }
            case ai::AIContainmentCommandKind::SetWantsNeither:
                if (const ObjectScriptContainmentEnterComponent* intent =
                        ecs::try_get<ObjectScriptContainmentEnterComponent>(
                            m_world.m_registry, *subjectEntity)) {
                    static_cast<void>(clearContainmentEnterMove(
                        *subjectEntity, intent->target, intent->sourceSequence));
                }
                ecs::remove<ObjectScriptContainmentEnterComponent>(
                    m_world.m_registry, *subjectEntity);
                feedback.kind = ai::AIContainmentFeedbackKind::EnterMovementFailed;
                break;
            case ai::AIContainmentCommandKind::ForceIntoContain: {
                const bool accepted = m_world.m_objectSimulation.requestContainment(
                    m_world.m_registry, m_world.m_objects,
                    {.kind = ObjectContainmentRequestKind::Attach,
                     .container = command.goal,
                     .object = correlation.subject,
                     .confirmedTick = m_presentation.m_confirmedTick},
                    &m_content.m_players,
                    &m_content.m_contentSnapshot);
                feedback.kind = accepted
                    ? ai::AIContainmentFeedbackKind::EnterHeld
                    : ai::AIContainmentFeedbackKind::EnterDenied;
                break;
            }
            case ai::AIContainmentCommandKind::ExitViaDoor: {
                if (command.door == ai::AIContainmentDoor::None) {
                    feedback.kind = ai::AIContainmentFeedbackKind::ExitNoDoor;
                    break;
                }
                const bool accepted =
                    m_world.m_objectSimulation.requestContainment(
                        m_world.m_registry, m_world.m_objects,
                        {.kind = ObjectContainmentRequestKind::Detach,
                         .container = command.goal,
                         .object = correlation.subject,
                         .confirmedTick = m_presentation.m_confirmedTick},
                        &m_content.m_players,
                        &m_content.m_contentSnapshot);
                feedback.kind = accepted
                    ? ai::AIContainmentFeedbackKind::ExitSucceeded
                    : ai::AIContainmentFeedbackKind::ExitWait;
                break;
            }
            }
        }
        const ai::ObjectAITransientStatus staged = transients.stage(feedback);
        if (staged != ai::ObjectAITransientStatus::Success) {
            containmentConsumed = false;
            raiseFeedbackFault(staged, correlation.subject);
            break;
        }
        containmentCursor = containmentEnd;
    }
    if (containmentConsumed) transients.discardContainmentCommands();

    // Rappel motion is deliberately executed here, after the SoA kernel has
    // observed the confirmed pose.  Do not publish a second motion feedback
    // for this command batch: GameSessionAIInsertionTransactions owns the
    // single next-tick pose feedback and the kernel rejects ambiguous pairs.
    for (const ai::AIInsertionMotionCommand& command :
         transients.insertionMotionCommands()) {
        if (command.confirmedTick != m_presentation.m_confirmedTick) continue;
        const std::optional<ecs::entity> subjectEntity =
            m_world.m_objects.entityFromId(command.correlation.subject);
        if (!subjectEntity) continue;
        ObjectPhysicsComponent* physics = ecs::try_get<ObjectPhysicsComponent>(
            m_world.m_registry, *subjectEntity);
        ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry,
                                                     *subjectEntity);
        switch (command.kind) {
        case ai::AIInsertionMotionCommandKind::SetRappelling: {
            ObjectRappellingComponent* rappelling =
                ecs::try_get<ObjectRappellingComponent>(m_world.m_registry,
                                                         *subjectEntity);
            if (!rappelling) {
                static_cast<void>(ecs::emplace<ObjectRappellingComponent>(
                    m_world.m_registry, *subjectEntity,
                    ObjectRappellingComponent{
                        .startedTick = m_presentation.m_confirmedTick}));
            } else {
                rappelling->startedTick = m_presentation.m_confirmedTick;
            }
            break;
        }
        case ai::AIInsertionMotionCommandKind::ResetDynamicPhysics: {
            if (!physics) break;
            if (const TransformComponent* transform =
                    ecs::try_get<TransformComponent>(m_world.m_registry,
                                                     *subjectEntity)) {
                physics->position = readAuthoritativeObjectPosition(
                    m_world.m_registry, *subjectEntity, *transform);
            }
            physics->velocityUnitsPerSecond = {};
            physics->pendingForce = {};
            physics->previousAcceleration = {};
            physics->yawRate = {};
            physics->pitchRate = {};
            physics->rollRate = {};
            break;
        }
        case ai::AIInsertionMotionCommandKind::SetLayer: {
            ObjectTerrainLayerComponent* layer =
                ecs::try_get<ObjectTerrainLayerComponent>(m_world.m_registry,
                                                          *subjectEntity);
            if (!layer) {
                static_cast<void>(ecs::emplace<ObjectTerrainLayerComponent>(
                    m_world.m_registry, *subjectEntity,
                    ObjectTerrainLayerComponent{
                        .pathfindLayer = command.layer,
                        .lastChangedTick = m_presentation.m_confirmedTick}));
            } else {
                static_cast<void>(layer->assign(
                    command.layer, m_presentation.m_confirmedTick));
            }
            break;
        }
        case ai::AIInsertionMotionCommandKind::ConstrainRappelVelocity:
            if (physics) {
                physics->velocityUnitsPerSecond.x = {};
                physics->velocityUnitsPerSecond.y = {};
                physics->velocityUnitsPerSecond.z =
                    math::q32_32::from_raw(command.verticalSpeedRaw);
            }
            if (locomotion) {
                locomotion->forwardSpeed = {};
                locomotion->verticalSpeed =
                    math::q32_32::from_raw(command.verticalSpeedRaw);
            }
            break;
        case ai::AIInsertionMotionCommandKind::SnapAltitude:
        case ai::AIInsertionMotionCommandKind::PlaceAtFallback: {
            const LogicFixedVec3 position{
                math::q32_32::from_raw(command.position.xRaw),
                math::q32_32::from_raw(command.position.yRaw),
                math::q32_32::from_raw(command.position.zRaw)};
            writeAuthoritativeObjectPosition(m_world.m_registry,
                                             *subjectEntity, position);
            if (command.kind ==
                ai::AIInsertionMotionCommandKind::PlaceAtFallback) {
                writeAuthoritativeObjectYaw(
                    m_world.m_registry, *subjectEntity,
                    math::q32_32::from_raw(command.orientationRaw));
            }
            if (physics) {
                physics->position = position;
                physics->velocityUnitsPerSecond = {};
                physics->pendingForce = {};
            }
            if (locomotion) locomotion->verticalSpeed = {};
            break;
        }
        case ai::AIInsertionMotionCommandKind::FollowFallbackPath: {
            ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry,
                                                         *subjectEntity);
            if (!queue) {
                queue = &ecs::emplace<ObjectOrderQueueComponent>(
                    m_world.m_registry, *subjectEntity);
            }
            if (queue->orders.size() <
                ObjectOrderQueueComponent::MaximumQueuedOrders) {
                queue->orders.push_back({
                    .kind = ObjectOrderKind::Move,
                    .source = ObjectOrderSource::System,
                    .issuedTick = m_presentation.m_confirmedTick,
                    .sourceSequence = command.correlation.stateRequest.sequence,
                    .targetX = math::q32_32::from_raw(command.position.xRaw),
                    .targetY = math::q32_32::from_raw(command.position.yRaw),
                    .targetZ = math::q32_32::from_raw(command.position.zRaw),
                    .hasTargetPosition = true,
                    .systemPurpose = ObjectOrderSystemPurpose::Generic,
                    .systemPurposeInstance =
                        command.correlation.stateRequest.sequence,
                });
                ++queue->revision;
            }
            break;
        }
        case ai::AIInsertionMotionCommandKind::ClearRappelling:
            ecs::remove<ObjectRappellingComponent>(m_world.m_registry,
                                                    *subjectEntity);
            break;
        case ai::AIInsertionMotionCommandKind::RestoreFastDesiredSpeed:
            if (locomotion) locomotion->verticalSpeed = {};
            break;
        case ai::AIInsertionMotionCommandKind::ConfigureCombatDropApproach:
        case ai::AIInsertionMotionCommandKind::RestoreCombatDropApproach:
            if (locomotion) {
                locomotion->preferredHeightFixed =
                    math::q32_32::from_raw(command.preferredHeightRaw);
                locomotion->ultraAccurate = command.ultraAccurate;
            }
            break;
        }
    }
    transients.discardInsertionMotionCommands();

    bool insertionContainmentConsumed = true;
    for (const ai::AIInsertionContainmentCommand& command :
         transients.insertionContainmentCommands()) {
        ai::AIInsertionContainmentFeedback feedback{
            .correlation = command.correlation,
            .building = command.building,
        };
        const std::optional<ecs::entity> subjectEntity =
            command.confirmedTick == m_presentation.m_confirmedTick
            ? m_world.m_objects.entityFromId(command.correlation.subject)
            : std::nullopt;
        const std::optional<ecs::entity> buildingEntity = subjectEntity
            ? m_world.m_objects.entityFromId(command.building)
            : std::nullopt;
        const ObjectHealthComponent* buildingHealth = buildingEntity
            ? ecs::try_get<ObjectHealthComponent>(m_world.m_registry,
                                                   *buildingEntity)
            : nullptr;
        if (!subjectEntity || !buildingEntity ||
            (buildingHealth && buildingHealth->effectivelyDead)) {
            feedback.kind = ai::AIInsertionContainmentFeedbackKind::BuildingMissing;
        } else if (command.kind ==
                   ai::AIInsertionContainmentCommandKind::ResolveBuildingLanding) {
            const OwnerComponent* subjectOwner =
                ecs::try_get<OwnerComponent>(m_world.m_registry,
                                              *subjectEntity);
            const ObjectContainmentComponent* contents =
                ecs::try_get<ObjectContainmentComponent>(m_world.m_registry,
                                                         *buildingEntity);
            container::Vector<ObjectId> occupants;
            if (contents) {
                for (const ObjectContainedObjectRecord& record : contents->objects)
                    occupants.push_back(record.object);
                std::sort(occupants.begin(), occupants.end());
            }
            const uint8_t limit = command.maximumEnemiesToKill;
            for (const ObjectId occupant : occupants) {
                if (feedback.enemiesKilled >= limit) break;
                const std::optional<ecs::entity> occupantEntity =
                    m_world.m_objects.entityFromId(occupant);
                const OwnerComponent* occupantOwner = occupantEntity
                    ? ecs::try_get<OwnerComponent>(m_world.m_registry,
                                                    *occupantEntity)
                    : nullptr;
                if (!subjectOwner || !occupantOwner ||
                    m_content.m_players.relationship(subjectOwner->player,
                        occupantOwner->player) != PlayerRelationship::Enemies) {
                    continue;
                }
                static_cast<void>(m_world.m_objectSimulation.requestContainment(
                    m_world.m_registry, m_world.m_objects,
                    {.kind = ObjectContainmentRequestKind::Detach,
                     .container = command.building,
                     .object = occupant,
                     .confirmedTick = m_presentation.m_confirmedTick,
                     .force = true}, &m_content.m_players,
                    &m_content.m_contentSnapshot));
                if (m_lifecycle.requestDestroyObject(
                        occupant, ObjectDestroyReason::System,
                        m_presentation.m_confirmedTick)) {
                    ++feedback.enemiesKilled;
                }
            }
            if (feedback.enemiesKilled < limit) {
                feedback.canContain =
                    m_world.m_objectSimulation.requestContainment(
                        m_world.m_registry, m_world.m_objects,
                        {.kind = ObjectContainmentRequestKind::Attach,
                         .container = command.building,
                         .object = command.correlation.subject,
                         .confirmedTick = m_presentation.m_confirmedTick},
                        &m_content.m_players, &m_content.m_contentSnapshot);
            }
            if (!feedback.canContain) {
                const TransformComponent* buildingTransform =
                    ecs::try_get<TransformComponent>(m_world.m_registry,
                                                     *buildingEntity);
                if (buildingTransform) {
                    const LogicFixedVec3 position =
                        readAuthoritativeObjectPosition(
                            m_world.m_registry, *buildingEntity,
                            *buildingTransform);
                    const ObjectGeometryComponent* geometry =
                        ecs::try_get<ObjectGeometryComponent>(
                            m_world.m_registry, *buildingEntity);
                    const math::q32_32 offset = geometry
                        ? math::q32_32::max(math::q32_32{},
                                             geometry->boundingCircleRadiusFixed)
                        : math::q32_32{int32_t{1}};
                    const math::q32_32 yaw = readAuthoritativeObjectYaw(
                        m_world.m_registry, *buildingEntity,
                        *buildingTransform);
                    const math::q32_32_sincos direction =
                        math::fixed_sincos(
                            yaw + math::q32_32::from_fraction(3, 2) *
                                game::kTopplePi);
                    const LogicFixedVec3 fallback{
                        position.x + offset * direction.cosine,
                        position.y + offset * direction.sine,
                        math::q32_32::from_raw(m_content.m_terrain.groundHeightRaw(
                            (position.x + offset * direction.cosine).raw(),
                            (position.y + offset * direction.sine).raw()))};
                    feedback.fallbackPosition = {
                        .xRaw = fallback.x.raw(), .yRaw = fallback.y.raw(),
                        .zRaw = fallback.z.raw()};
                    feedback.fallbackOrientationRaw = yaw.raw();
                }
            }
            feedback.kind =
                ai::AIInsertionContainmentFeedbackKind::BuildingLandingResolved;
        } else if (command.kind ==
                   ai::AIInsertionContainmentCommandKind::AddToContainer) {
            const ObjectContainedByComponent* edge =
                ecs::try_get<ObjectContainedByComponent>(m_world.m_registry,
                                                         *subjectEntity);
            feedback.canContain = edge && edge->container == command.building;
            if (!feedback.canContain) {
                feedback.canContain =
                    m_world.m_objectSimulation.requestContainment(
                        m_world.m_registry, m_world.m_objects,
                        {.kind = ObjectContainmentRequestKind::Attach,
                         .container = command.building,
                         .object = command.correlation.subject,
                         .confirmedTick = m_presentation.m_confirmedTick},
                        &m_content.m_players, &m_content.m_contentSnapshot);
            }
            feedback.kind =
                ai::AIInsertionContainmentFeedbackKind::BuildingLandingResolved;
        } else {
            // ResolveBuildingLanding may have attached eagerly to retain the
            // exact capacity outcome for the subsequent SoA state.  An early
            // exit must undo that provisional edge.
            if (const ObjectContainedByComponent* edge =
                    ecs::try_get<ObjectContainedByComponent>(
                        m_world.m_registry, *subjectEntity);
                edge && edge->container == command.building) {
                static_cast<void>(m_world.m_objectSimulation.requestContainment(
                    m_world.m_registry, m_world.m_objects,
                    {.kind = ObjectContainmentRequestKind::Detach,
                     .container = command.building,
                     .object = command.correlation.subject,
                     .confirmedTick = m_presentation.m_confirmedTick,
                     .force = true}, &m_content.m_players,
                    &m_content.m_contentSnapshot));
            }
            feedback.kind =
                ai::AIInsertionContainmentFeedbackKind::BuildingLandingResolved;
        }
        const ai::ObjectAITransientStatus staged = transients.stage(feedback);
        if (staged != ai::ObjectAITransientStatus::Success) {
            insertionContainmentConsumed = false;
            raiseFeedbackFault(staged, command.correlation.subject);
            break;
        }
    }
    if (insertionContainmentConsumed)
        transients.discardInsertionContainmentCommands();

    bool insertionOperationConsumed = true;
    for (const ai::AIInsertionOperationCommand& command :
         transients.insertionOperationCommands()) {
        const ObjectId subject = command.correlation.subject;
        const std::optional<ecs::entity> subjectEntity =
            command.confirmedTick == m_presentation.m_confirmedTick
            ? m_world.m_objects.entityFromId(subject) : std::nullopt;
        ObjectAirfieldComponent* airfield = subjectEntity
            ? ecs::try_get<ObjectAirfieldComponent>(m_world.m_registry,
                                                     *subjectEntity)
            : nullptr;
        const auto clearHeldWhenIdle = [&]() {
            if (!subjectEntity || !airfield) return;
            const bool active = std::any_of(
                airfield->chinookAi.begin(), airfield->chinookAi.end(),
                [](const ObjectChinookAiRuntime& runtime) {
                    return runtime.combatDropActive;
                });
            if (!active) static_cast<void>(ObjectDisabledSystem::clear(
                m_world.m_registry, *subjectEntity,
                ObjectDisabledReason::Held, m_presentation.m_confirmedTick));
        };
        const auto matchingModule = [&]() -> std::optional<size_t> {
            if (!airfield || !airfield->plan || !command.operation) return {};
            const ai::AIInsertionOperationIdentity identity =
                ai::decodeAIInsertionOperationHandle(command.operation);
            if (!identity || identity.ownerIndex >= airfield->chinookAi.size() ||
                identity.ownerIndex >= airfield->plan->chinookAi.size()) return {};
            const ObjectChinookAiRuntime& runtime =
                airfield->chinookAi[identity.ownerIndex];
            return runtime.combatDropActive &&
                runtime.ropeGeneration == identity.generation
                ? std::optional<size_t>{identity.ownerIndex} : std::nullopt;
        };
        ai::AIInsertionOperationFeedback feedback{
            .correlation = command.correlation,
            .operation = command.operation,
            .kind = ai::AIInsertionOperationFeedbackKind::Failed,
            .eventSequence = command.eventSequence,
        };
        bool publishFeedback = true;
        switch (command.kind) {
        case ai::AIInsertionOperationCommandKind::Begin: {
            if (!subjectEntity || !airfield || !airfield->plan) break;
            const ThingTemplateComponent* type =
                ecs::try_get<ThingTemplateComponent>(m_world.m_registry,
                                                     *subjectEntity);
            const RenderModelComponent* visual =
                ecs::try_get<RenderModelComponent>(m_world.m_registry,
                                                   *subjectEntity);
            const TransformComponent* transform =
                ecs::try_get<TransformComponent>(m_world.m_registry,
                                                 *subjectEntity);
            const game::W3dPristineBoneCatalog* catalog =
                m_content.m_contentSnapshot.pristineBoneCatalog();
            if (!type || !type->archetype || !visual || !transform || !catalog)
                break;
            std::optional<size_t> index;
            const size_t count = std::min(airfield->chinookAi.size(),
                                          airfield->plan->chinookAi.size());
            for (size_t candidate = 0; candidate < count; ++candidate) {
                if (!airfield->chinookAi[candidate].combatDropActive &&
                    airfield->plan->chinookAi[candidate].numRopes != 0) {
                    index = candidate;
                    break;
                }
            }
            if (!index) break;
            const std::optional<ObjectChinookCombatDropBeginRequest> begin =
                resolveObjectChinookCombatDropBegin({
                    .object = subject,
                    .moduleIndex = *index,
                    .archetypeName = type->archetype->name,
                    .visualRuleIndex = game::selectModelConditionVisualRuleIndex(
                        type->archetype->templateData,
                        visual->modelConditionFlags),
                    .numRopes = airfield->plan->chinookAi[*index].numRopes,
                    .objectPosition = readAuthoritativeObjectPosition(
                        m_world.m_registry, *subjectEntity, *transform),
                    .objectYawRadians = readAuthoritativeObjectYaw(
                        m_world.m_registry, *subjectEntity, *transform),
                    .confirmedTick = m_presentation.m_confirmedTick},
                    *catalog, m_content.m_terrain);
            if (!begin || !m_world.m_objectSimulation.beginChinookCombatDrop(
                    m_world.m_registry, m_world.m_objects,
                    m_content.m_simulationRandom, *begin)) break;
            static_cast<void>(ObjectDisabledSystem::setUntil(
                m_world.m_registry, *subjectEntity,
                ObjectDisabledReason::Held, OBJECT_DISABLED_FOREVER_TICK,
                m_presentation.m_confirmedTick));
            feedback.operation = ai::makeAIInsertionOperationHandle(
                static_cast<uint32_t>(*index),
                airfield->chinookAi[*index].ropeGeneration);
            feedback.kind = ai::AIInsertionOperationFeedbackKind::Begun;
            break;
        }
        case ai::AIInsertionOperationCommandKind::Poll: {
            const std::optional<size_t> index = matchingModule();
            if (!index || !subjectEntity) break;
            ObjectChinookAiRuntime& runtime = airfield->chinookAi[*index];
            const ObjectContainmentComponent* contents =
                ecs::try_get<ObjectContainmentComponent>(m_world.m_registry,
                                                         *subjectEntity);
            container::Vector<ObjectId> candidates;
            if (contents) {
                for (const ObjectContainedObjectRecord& record : contents->objects)
                    candidates.push_back(record.object);
                std::sort(candidates.begin(), candidates.end());
            }
            if (runtime.pendingRappeller) {
                const std::optional<ecs::entity> pending =
                    m_world.m_objects.entityFromId(runtime.pendingRappeller);
                const ObjectContainedByComponent* edge = pending
                    ? ecs::try_get<ObjectContainedByComponent>(
                          m_world.m_registry, *pending)
                    : nullptr;
                if (!edge || edge->container != subject) {
                    runtime.pendingRappeller = INVALID_OBJECT_ID;
                    runtime.pendingRopeIndex = 0;
                    runtime.pendingEventSequence = 0;
                }
            }
            ObjectId child = INVALID_OBJECT_ID;
            for (const ObjectId candidate : candidates) {
                const std::optional<ecs::entity> entity =
                    m_world.m_objects.entityFromId(candidate);
                const ObjectContainedByComponent* edge = entity
                    ? ecs::try_get<ObjectContainedByComponent>(
                          m_world.m_registry, *entity)
                    : nullptr;
                const ObjectHealthComponent* health = entity
                    ? ecs::try_get<ObjectHealthComponent>(m_world.m_registry,
                                                           *entity)
                    : nullptr;
                const ObjectKindOfComponent* kinds = entity
                    ? ecs::try_get<ObjectKindOfComponent>(m_world.m_registry,
                                                          *entity)
                    : nullptr;
                if (entity && edge && edge->container == subject &&
                    !(health && health->effectivelyDead) &&
                    hasObjectKind(kinds, game::ObjectKindOf::CanRappel)) {
                    child = candidate;
                    break;
                }
            }
            bool activeRappeller = false;
            for (const ObjectChinookAiRuntime::Rope& rope : runtime.ropes) {
                activeRappeller = activeRappeller || !rope.rappellers.empty();
            }
            const std::optional<ObjectChinookRopeReadyResult> ready =
                runtime.pendingRappeller ? std::nullopt :
                m_world.m_objectSimulation.nextReadyChinookRope(
                    m_world.m_registry, m_world.m_objects, subject, *index,
                    m_presentation.m_confirmedTick);
            if (ready && child) {
                runtime.pendingRappeller = child;
                runtime.pendingRopeIndex = static_cast<uint32_t>(ready->ropeIndex);
                runtime.pendingEventSequence = command.eventSequence;
                feedback.kind = ai::AIInsertionOperationFeedbackKind::ChildRappelReady;
                feedback.child = child;
                feedback.childRappelSpeedRaw = (
                    ready->rappelSpeedPerFrame * math::q32_32{static_cast<int32_t>(
                        std::max<uint32_t>(1u,
                            m_content.m_objectSimulationRules.logicFramesPerSecond))}).raw();
            } else if (!activeRappeller && !child && !runtime.pendingRappeller) {
                static_cast<void>(m_world.m_objectSimulation.endChinookCombatDrop(
                    m_world.m_registry, m_world.m_objects, subject, *index,
                    m_presentation.m_confirmedTick));
                clearHeldWhenIdle();
                feedback.kind = ai::AIInsertionOperationFeedbackKind::Completed;
            } else {
                feedback.kind = ai::AIInsertionOperationFeedbackKind::Progress;
            }
            break;
        }
        case ai::AIInsertionOperationCommandKind::OrderChildRappel: {
            // Paired with Poll(sequence + 1); only Poll emits the feedback.
            publishFeedback = false;
            const std::optional<size_t> index = matchingModule();
            if (!index || !subjectEntity || !command.child) break;
            ObjectChinookAiRuntime& runtime = airfield->chinookAi[*index];
            if (runtime.pendingRappeller != command.child ||
                runtime.pendingEventSequence != command.eventSequence ||
                runtime.pendingRopeIndex >= runtime.ropes.size()) break;
            const std::optional<ecs::entity> childEntity =
                m_world.m_objects.entityFromId(command.child);
            const ObjectContainedByComponent* edge = childEntity
                ? ecs::try_get<ObjectContainedByComponent>(m_world.m_registry,
                                                           *childEntity)
                : nullptr;
            if (!childEntity || !edge || edge->container != subject) break;
            const ObjectChinookAiRuntime::Rope& rope =
                runtime.ropes[runtime.pendingRopeIndex];
            ai::AIStateParameters parameters;
            parameters.goalObject = command.goal;
            parameters.goalPosition = command.goalPosition;
            parameters.hasGoalPosition = true;
            const uint64_t activationTick =
                m_presentation.m_confirmedTick == std::numeric_limits<uint64_t>::max()
                ? m_presentation.m_confirmedTick
                : m_presentation.m_confirmedTick + 1u;
            const ai::ObjectAIInsertionTransitionResult transition =
                m_ai.m_objectAI.stageInsertionState(
                    command.child, ai::AIStateId::RappelInto, parameters,
                    activationTick);
            if (!transition.succeeded() ||
                !m_world.m_objectSimulation.requestContainment(
                    m_world.m_registry, m_world.m_objects,
                    {.kind = ObjectContainmentRequestKind::Detach,
                     .container = subject,
                     .object = command.child,
                     .confirmedTick = m_presentation.m_confirmedTick,
                     .force = true}, &m_content.m_players,
                    &m_content.m_contentSnapshot)) break;
            writeAuthoritativeObjectPosition(m_world.m_registry, *childEntity,
                                             rope.endpoint.dropStart);
            const LogicFixedQuaternion& orientation = rope.endpoint.dropOrientation;
            writeAuthoritativeObjectYaw(m_world.m_registry, *childEntity,
                math::fixed_atan2(
                    math::q32_32{int32_t{2}} *
                        (orientation.w * orientation.z + orientation.x * orientation.y),
                    math::q32_32{int32_t{1}} - math::q32_32{int32_t{2}} *
                        (orientation.y * orientation.y + orientation.z * orientation.z)));
            if (!m_world.m_objectSimulation.notifyChinookRappellerStarted(
                    m_world.m_registry, m_world.m_objects,
                    m_content.m_simulationRandom, subject, *index,
                    runtime.pendingRopeIndex, m_presentation.m_confirmedTick,
                    command.child)) break;
            runtime.pendingRappeller = INVALID_OBJECT_ID;
            runtime.pendingRopeIndex = 0;
            runtime.pendingEventSequence = 0;
            break;
        }
        case ai::AIInsertionOperationCommandKind::Cancel: {
            if (const std::optional<size_t> index = matchingModule()) {
                static_cast<void>(m_world.m_objectSimulation.endChinookCombatDrop(
                    m_world.m_registry, m_world.m_objects, subject, *index,
                    m_presentation.m_confirmedTick));
            }
            clearHeldWhenIdle();
            feedback.kind = ai::AIInsertionOperationFeedbackKind::Cancelled;
            break;
        }
        }
        if (publishFeedback) {
            const ai::ObjectAITransientStatus staged = transients.stage(feedback);
            if (staged != ai::ObjectAITransientStatus::Success) {
                insertionOperationConsumed = false;
                raiseFeedbackFault(staged, command.correlation.subject);
                break;
            }
        }
    }
    if (insertionOperationConsumed)
        transients.discardInsertionOperationCommands();

    // Effect commands are one-way by protocol. CombatDropKillFX is resolved
    // from the rappeller's immutable per-unit semantic table and anchored to
    // the building, matching AIRappelState::doFXObj(building).
    for (const ai::AIInsertionEffectCommand& command :
         transients.insertionEffectCommands()) {
        switch (command.kind) {
        case ai::AIInsertionEffectCommandKind::KillSubject:
            if (command.target != command.correlation.subject ||
                !m_lifecycle.requestDestroyObject(command.target,
                                      ObjectDestroyReason::System,
                                      m_presentation.m_confirmedTick)) {
                TD_LOG_WARN(
                    "[GameSession] Unsupported Object AI KillSubject effect: "
                    "subject={} target={} tick={}",
                    command.correlation.subject.value,
                    command.target.value, m_presentation.m_confirmedTick);
            }
            break;
        case ai::AIInsertionEffectCommandKind::PlayCombatDropKillEffect:
            if (const std::optional<ecs::entity> subjectEntity =
                    m_world.m_objects.entityFromIdIncludingPending(
                        command.correlation.subject)) {
                const ThingTemplateComponent* type =
                    ecs::try_get<ThingTemplateComponent>(
                        m_world.m_registry, *subjectEntity);
                if (type && type->archetype) {
                    const auto found = type->archetype->templateData
                        .unitSpecificFx.find("CombatDropKillFX");
                    if (found != type->archetype->templateData
                            .unitSpecificFx.end() &&
                        !found->second.empty()) {
                        const std::optional<game::FxInvocationAnchor> anchor =
                            session_fx::snapshotAnchor(
                                m_world.m_registry, m_world.m_objects, command.target);
                        if (anchor) {
                            static_cast<void>(m_publication.emitFxInvocationEvent({
                                .fxListName = found->second,
                                .anchorKind = game::
                                    FxInvocationAnchorKind::ObjectAttachment,
                                .primary = *anchor,
                            }));
                        }
                    }
                }
            }
            break;
        }
    }
    transients.discardInsertionEffectCommands();
}

} // namespace engine
