#include "game/session/transaction/GameSessionPlayerRepairTransactions.h"

#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/simulation/economy/ObjectRepairRules.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/session/command/GameSessionPlayerCommandPolicy.h"
#include "game/session/state/GameSessionDomainState.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace engine {

OrderExecutionResult GameSessionPlayerRepairTransactions::execute(
    PlayerId player, container::Span<const ObjectId> actors,
    ObjectId structure, uint32_t sourceSequence,
    uint64_t confirmedTick) {
    const auto reject = [](OrderRejectionReason reason,
                           container::String message) {
        return OrderExecutionResult{
            .accepted = false,
            .rejection = reason,
            .message = std::move(message),
        };
    };
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick) {
        return reject(
            OrderRejectionReason::MalformedOrder,
            "repair command does not belong to the active confirmed tick");
    }
    const PlayerState* issuingPlayer = m_content.m_players.get(player);
    if (!issuingPlayer || !issuingPlayer->isCommandPlayer()) {
        return reject(
            OrderRejectionReason::InvalidPlayer,
            "repair issuer is not a live command player");
    }
    if (actors.empty() || actors.size() > 512u || !structure) {
        return reject(
            OrderRejectionReason::MalformedOrder,
            "repair requires a non-empty bounded actor group and target");
    }
    container::Vector<ObjectId> canonical(actors.begin(), actors.end());
    std::sort(canonical.begin(), canonical.end());
    if (!canonical.front() ||
        std::adjacent_find(canonical.begin(), canonical.end()) !=
            canonical.end()) {
        return reject(
            OrderRejectionReason::MalformedOrder,
            "repair actors must be canonical unique ObjectIds");
    }
    canonical.erase(
        std::remove_if(
            canonical.begin(), canonical.end(),
            [this](ObjectId actor) {
                return session_command_policy::objectForbidsPlayerCommands(
                    m_world.m_registry, m_world.m_objects, actor);
            }),
        canonical.end());
    if (canonical.empty()) return {.accepted = true, .actorCount = 0};
    if (m_world.m_objects.isPendingDestroy(structure)) {
        return reject(
            OrderRejectionReason::InvalidTarget,
            "repair target is pending destruction");
    }
    const std::optional<ecs::entity> targetEntity =
        m_world.m_objects.entityFromId(structure);
    if (!targetEntity) {
        return reject(
            OrderRejectionReason::InvalidTarget,
            "repair target is unavailable");
    }

    if (const auto visibility = m_world.m_mapVisibility.snapshot();
        issuingPlayer->controller == PlayerControllerKind::Human &&
        visibility && visibility->renderingActive) {
        const TransformComponent* transform = ecs::try_get<TransformComponent>(
            m_world.m_registry, *targetEntity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(
                m_world.m_registry, *targetEntity);
        const LogicFixedVec3 position = transform
            ? readAuthoritativeObjectPosition(
                  m_world.m_registry, *targetEntity, *transform)
            : LogicFixedVec3{};
        const math::q32_32 radius = geometry
            ? math::q32_32::max(
                  math::q32_32{}, geometry->boundingCircleRadiusFixed)
            : math::q32_32{};
        bool visible = transform && visibility->footprintHasClearCellRaw(
            player, position.x.raw(), position.y.raw(), radius.raw());
        for (const PlayerId ally : m_content.m_players.activePlayerIds()) {
            if (visible) break;
            if (ally == player || m_content.m_players.relationship(
                    player, ally) != PlayerRelationship::Allies) {
                continue;
            }
            if (transform && visibility->footprintHasClearCellRaw(
                    ally, position.x.raw(), position.y.raw(), radius.raw())) {
                visible = true;
                break;
            }
        }
        if (!visible) {
            return reject(
                OrderRejectionReason::InvalidTarget,
                "repair target is shrouded for the issuing player");
        }
    }

    const OwnerComponent* targetOwner = ecs::try_get<OwnerComponent>(
        m_world.m_registry, *targetEntity);
    const ObjectStatusComponent* targetStatus =
        ecs::try_get<ObjectStatusComponent>(m_world.m_registry,
                                             *targetEntity);
    const bool underConstruction = targetStatus && targetStatus->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction));
    const bool repairDock = object_repair_rules::isRepairDockTarget(
        m_world.m_registry, *targetEntity) && targetOwner &&
        targetOwner->player && m_content.m_players.relationship(
            player, targetOwner->player) == PlayerRelationship::Allies;
    const bool aircraftAirfield =
        object_repair_rules::isAircraftRepairAirfieldTarget(
            m_world.m_registry, *targetEntity) && targetOwner &&
        targetOwner->player && m_content.m_players.relationship(
            player, targetOwner->player) == PlayerRelationship::Allies;
    container::Vector<ObjectId> constructionActors;
    container::Vector<ObjectId> builderActors;
    container::Vector<ObjectId> dockActors;
    container::Vector<ObjectId> aircraftActors;
    constructionActors.reserve(canonical.size());
    builderActors.reserve(canonical.size());
    dockActors.reserve(canonical.size());
    aircraftActors.reserve(canonical.size());
    for (const ObjectId actor : canonical) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(actor);
        if (!entity) {
            return reject(
                m_world.m_objects.isPendingDestroy(actor)
                    ? OrderRejectionReason::PendingDestroy
                    : OrderRejectionReason::MissingActor,
                "repair references an unavailable actor");
        }
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(
            m_world.m_registry, *entity);
        if (!owner || owner->player != player) {
            return reject(
                OrderRejectionReason::OwnershipMismatch,
                "repair contains an actor not owned by its issuer");
        }
        if (underConstruction &&
            m_world.m_objectSimulation.canObjectResumeConstruction(
                m_world.m_registry, m_world.m_objects,
                m_content.m_players, actor, structure)) {
            constructionActors.push_back(actor);
        } else if (!underConstruction &&
                   m_world.m_objectSimulation.canObjectRepair(
                m_world.m_registry, m_world.m_objects, m_content.m_players,
                actor, structure)) {
            builderActors.push_back(actor);
        } else if (repairDock &&
                   object_repair_rules::isDamagedRepairDockActor(
                       m_world.m_registry, *entity) &&
                    m_ai.m_objectAI.hasOrderCapability(
                        actor, ai::ObjectAIOrderCapability::MoveStop)) {
            dockActors.push_back(actor);
        } else if (aircraftAirfield &&
                   object_repair_rules::isDamagedAircraftRepairActor(
                       m_world.m_registry, *entity)) {
            aircraftActors.push_back(actor);
        }
    }
    if (constructionActors.empty() && builderActors.empty() &&
        dockActors.empty() &&
        aircraftActors.empty()) {
        return reject(
            OrderRejectionReason::InvalidTarget,
            "no selected actor can resume, repair, or use the target repair dock");
    }

    size_t assigned = 0;
    // Construction sites have one active builder, matching
    // ActionManager::canResumeConstructionOf. Stable actor order makes the
    // first eligible worker the deterministic claimant; later actors observe
    // the live claim and remain idle.
    for (const ObjectId actor : constructionActors) {
        if (m_world.m_objectSimulation.resumeObjectConstruction(
                m_world.m_registry, m_world.m_objects, m_content.m_players,
                actor, structure, confirmedTick, sourceSequence, true)) {
            ++assigned;
            break;
        }
    }
    for (const ObjectId actor : builderActors) {
        if (m_world.m_objectSimulation.requestObjectRepair(
                m_world.m_registry, m_world.m_objects, m_content.m_players,
                actor, structure, confirmedTick, sourceSequence, true,
                issuingPlayer->controller == PlayerControllerKind::Human)) {
            ++assigned;
        }
    }
    for (const ObjectId actor : dockActors) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(actor);
        if (!entity) continue;
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(
                m_world.m_registry, *entity);
        uint64_t externalRevision = queue
            ? queue->externalRevision + 1u : 1u;
        if (externalRevision == 0) ++externalRevision;
        if (!m_ai.m_objectAI.activateRepairDock(
                actor, structure, confirmedTick, externalRevision)) {
            continue;
        }
        if (!queue) {
            queue = &ecs::emplace<ObjectOrderQueueComponent>(
                m_world.m_registry, *entity);
        }
        if (!queue->orders.empty()) {
            queue->orders.clear();
            ++queue->revision;
        }
        queue->externalRevision = externalRevision;
        ++assigned;
    }
    for (const ObjectId actor : aircraftActors) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(actor);
        if (!entity ||
            !m_world.m_objectSimulation.requestAircraftRepairAtAirfield(
                m_world.m_registry, m_world.m_objects, actor, structure,
                confirmedTick)) {
            continue;
        }
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(
                m_world.m_registry, *entity);
        if (!queue) {
            queue = &ecs::emplace<ObjectOrderQueueComponent>(
                m_world.m_registry, *entity);
        }
        queue->orders.clear();
        ++queue->revision;
        ++queue->externalRevision;
        if (queue->externalRevision == 0) ++queue->externalRevision;
        ++assigned;
    }
    return {.accepted = true, .actorCount = assigned};
}

} // namespace engine
