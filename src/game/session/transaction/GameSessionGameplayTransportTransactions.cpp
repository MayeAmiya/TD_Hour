#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/definition/ObjectArchetype.h"

#include "game/session/frame/GameSessionFxAnchorSnapshot.h"
#include "game/object/simulation/status/ObjectExperience.h"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>

namespace engine::detail {

bool GameSessionWeaponEventDrain::handleTransport(WorkItem item) {
    return std::visit(
        [this](auto&& event) {
            return handleTransportTransaction(
                std::forward<decltype(event)>(event));
        },
        std::move(item.transport.payload));
}

bool GameSessionWeaponEventDrain::handleTransportTransaction(
    ObjectTransportPayloadStrafeTransaction event) {
    const std::optional<ecs::entity> source =
        m_world.m_objects
            .entityFromIdIncludingPending(event.transport);
    const std::optional<game::WeaponSlot> slot =
        game::tryParseWeaponSlot(event.weaponSlot);
    if (source && slot && setObjectWeaponLock(
            m_world.m_registry, *source, *slot,
            ObjectWeaponLockType::Temporary)) {
        container::Vector<ObjectSystemWeaponFireCommand> commands;
        if (tryQueueObjectSlotWeaponFireAtPosition(
                m_world.m_registry, *source,
                event.transport, *slot, {event.x, event.y, event.z},
                m_content.m_contentSnapshot,
                m_content.m_simulationRandom,
                std::max<uint32_t>(
                    1u, m_world
                            .m_objectSimulation.rules()
                            .logicFramesPerSecond),
                event.authoredOrder,
                m_world.m_objectSimulation
                    .reserveGameplaySubmissionOrdinal(),
                event.confirmedTick, commands)) {
            for (ObjectSystemWeaponFireCommand& command : commands) {
                m_world.m_objectSimulation
                    .queueSystemWeaponFireCommand(std::move(command));
            }
        }
    }
    closeCurrentReaction();
    if (m_frame.result().faulted()) return false;
    if (!event.fxList.empty()) {
        static_cast<void>(m_publication.emitFxInvocationEvent({
            .fxListName = event.fxList,
            .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
            .primary = session_fx::worldAnchor(
                {event.x.to_float(), event.y.to_float(),
                 event.z.to_float()},
                event.transport),
        }));
    }
    return true;
}

bool GameSessionWeaponEventDrain::handleTransportTransaction(
    ObjectTransportPayloadWeaponTransaction event) {
    const std::optional<ecs::entity> source =
        m_world.m_objects
            .entityFromIdIncludingPending(event.transport);
    if (source && !event.weaponTemplate.empty()) {
        const game::WeaponContentId weapon =
            m_content.m_contentSnapshot
                .findWeaponId(event.weaponTemplate);
        container::Vector<ObjectSystemWeaponFireCommand> commands;
        if (weapon && queueObjectTransientWeaponFireAtPosition(
                weapon, m_world.m_registry,
                *source, event.transport, {event.x, event.y, event.z},
                m_content.m_contentSnapshot,
                m_content.m_simulationRandom,
                event.authoredOrder + 1u, event.authoredOrder,
                m_world.m_objectSimulation
                    .reserveGameplaySubmissionOrdinal(),
                event.confirmedTick, commands)) {
            for (ObjectSystemWeaponFireCommand& command : commands) {
                m_world.m_objectSimulation
                    .queueSystemWeaponFireCommand(std::move(command));
            }
        }
    } else if (source) {
        ObjectWeaponComponent* weapons = ecs::try_get<ObjectWeaponComponent>(
            m_world.m_registry, *source);
        const std::optional<game::WeaponSlot> current = weapons
            ? weapons->currentSlot : std::nullopt;
        if (current) {
            container::Vector<ObjectSystemWeaponFireCommand> commands;
            if (tryQueueObjectSlotWeaponFireAtPosition(
                    m_world.m_registry,
                    *source, event.transport, *current,
                    {event.x, event.y, event.z},
                    m_content.m_contentSnapshot,
                    m_content.m_simulationRandom,
                    std::max<uint32_t>(
                        1u, m_world
                                .m_objectSimulation.rules()
                                .logicFramesPerSecond),
                    event.authoredOrder,
                    m_world.m_objectSimulation
                        .reserveGameplaySubmissionOrdinal(),
                    event.confirmedTick, commands)) {
                for (ObjectSystemWeaponFireCommand& command : commands) {
                    m_world.m_objectSimulation
                        .queueSystemWeaponFireCommand(std::move(command));
                }
            }
        }
    }
    closeCurrentReaction();
    if (m_frame.result().faulted()) return false;
    if (event.payloadObject) {
        static_cast<void>(m_lifecycle.requestDestroyObject(
            event.payloadObject, ObjectDestroyReason::System,
            event.confirmedTick));
        closeCurrentReaction();
    }
    return !m_frame.result().faulted();
}

bool GameSessionWeaponEventDrain::handleTransportTransaction(
    ObjectTransportPayloadFinishedTransaction event) {
    static_cast<void>(m_world
        .m_objectSimulation.killRadiusDecal(
            m_world.m_registry,
            m_world.m_objects,
            event.transport, event.confirmedTick));
    const std::optional<ecs::entity> source =
        m_world.m_objects
            .entityFromIdIncludingPending(event.transport);
    if (source) {
        static_cast<void>(releaseObjectWeaponLock(
            m_world.m_registry, *source,
            ObjectWeaponLockType::Temporary));
    }
    const std::optional<ObjectTeamId> sourceTeam =
        m_world.m_objectTeams.teamOf(
            event.transport);
    const ObjectTeamRecord* teamRecord = sourceTeam
        ? m_world.m_objectTeams.find(*sourceTeam)
        : nullptr;
    const scenario::ScriptTeamDefinition* teamDefinition =
        teamRecord && teamRecord->scenarioDefinition &&
                m_presentation.m_scenarioDefinition
            ? m_presentation.m_scenarioDefinition
                  ->findScriptTeam(teamRecord->scenarioDefinition)
            : nullptr;
    const ThingTemplateComponent* sourceType = source
        ? ecs::try_get<ThingTemplateComponent>(
              m_world.m_registry, *source)
        : nullptr;
    const container::SharedPtr<const game::ObjectArchetype> authoredTransport =
        teamDefinition && !teamDefinition->plan.reinforcementTransport.empty()
        ? m_content.m_contentSnapshot
              .findObjectArchetype(
                  teamDefinition->plan.reinforcementTransport)
        : container::SharedPtr<const game::ObjectArchetype>{};
    if (sourceTeam && teamRecord && !teamRecord->active && sourceType &&
        sourceType->archetype && authoredTransport &&
        game::legacyThingTemplatesEquivalent(
            sourceType->archetype->templateData,
            authoredTransport->templateData)) {
        static_cast<void>(m_world
            .m_objectTeams.activate(*sourceTeam, event.confirmedTick));
    }
    if (event.destroyTransport) {
        static_cast<void>(m_lifecycle.requestDestroyObject(
            event.transport, ObjectDestroyReason::System,
            event.confirmedTick));
        closeCurrentReaction();
    }
    return !m_frame.result().faulted();
}

bool GameSessionWeaponEventDrain::handleTransportTransaction(
    ObjectTransportOclTransaction event) {
    if (event.objectCreationList.empty() || !event.hasFrozenSource) {
        return true;
    }
    const game::ObjectCreationListContentId content =
        m_content.m_contentSnapshot
            .findObjectCreationListId(event.objectCreationList);
    if (!content) return true;
    m_world.m_objectSimulation
        .queueObjectCreationListInvocation({
            .content = content,
            .source = event.source,
            .owner = event.owner,
            .primaryTeam = event.primaryTeam,
            .primaryPosition = {
                event.primaryX, event.primaryY, event.primaryZ},
            .sourceVelocity = {
                event.sourceVelocityX, event.sourceVelocityY,
                event.sourceVelocityZ},
            .orientationRadians = event.orientationRadians,
            .pitchRadians = event.pitchRadians,
            .rollRadians = event.rollRadians,
            .authoredOrder = event.authoredOrder,
            .emissionSequence = m_world
                .m_objectSimulation.reserveGameplaySubmissionOrdinal(),
            .confirmedTick = event.confirmedTick,
            .sourcePathfindLayer = event.sourcePathfindLayer,
            .sourceAirborne = event.sourceAirborne,
            .sourceOwnsFullAttitude = event.sourceOwnsFullAttitude,
        });
    closeCurrentReaction();
    return !m_frame.result().faulted();
}

bool GameSessionWeaponEventDrain::handleTransportTransaction(
    ObjectTransportWeaponAtPositionTransaction event) {
    const std::optional<ecs::entity> source =
        m_world.m_objects
            .entityFromIdIncludingPending(event.source);
    if (!source || event.weaponTemplate.empty()) return true;
    const game::WeaponContentId weapon =
        m_content.m_contentSnapshot
            .findWeaponId(event.weaponTemplate);
    container::Vector<ObjectSystemWeaponFireCommand> commands;
    if (weapon && queueObjectTransientWeaponFireAtPosition(
            weapon, m_world.m_registry, *source,
            event.source, {event.x, event.y, event.z},
            m_content.m_contentSnapshot,
            m_content.m_simulationRandom,
            event.authoredOrder + 1u, event.authoredOrder,
            m_world.m_objectSimulation
                .reserveGameplaySubmissionOrdinal(),
            event.confirmedTick, commands)) {
        for (ObjectSystemWeaponFireCommand& command : commands) {
            m_world.m_objectSimulation
                .queueSystemWeaponFireCommand(std::move(command));
        }
    }
    closeCurrentReaction();
    return !m_frame.result().faulted();
}

bool GameSessionWeaponEventDrain::handleTransportTransaction(
    ObjectTransportBunkerBustTransaction event) {
    ObjectSimulation& simulation =
        m_world.m_objectSimulation;
    for (ObjectTransportBunkerBustOccupant& occupant : event.occupants) {
        static_cast<void>(simulation.requestContainment(
            m_world.m_registry,
            m_world.m_objects,
            {.kind = ObjectContainmentRequestKind::Detach,
             .container = occupant.entrance,
             .object = occupant.damage.target,
             .confirmedTick = event.confirmedTick,
             .force = true},
            &m_content.m_players,
            &m_content.m_contentSnapshot));
        closeCurrentReaction();
        if (m_frame.result().faulted()) return false;
        occupant.damage.submissionOrdinal =
            simulation.reserveGameplaySubmissionOrdinal();
        simulation.queueDamage(std::move(occupant.damage));
        // harmAndForceExitAllContained is synchronous per passenger. A death
        // may mutate the network before the next frozen stable ObjectId is
        // considered, so close this reaction before advancing.
        closeCurrentReaction();
        if (m_frame.result().faulted()) return false;
    }
    if (!event.detonationFx.empty()) {
        const ObjectId anchorObject = event.target
            ? event.target : event.source;
        const std::optional<game::FxInvocationAnchor> anchor =
            session_fx::snapshotAnchor(
                m_world.m_registry,
                m_world.m_objects,
                anchorObject);
        static_cast<void>(m_publication.emitFxInvocationEvent({
            .fxListName = event.detonationFx,
            .anchorKind = anchor
                ? game::FxInvocationAnchorKind::ObjectAttachment
                : game::FxInvocationAnchorKind::WorldPosition,
            .primary = anchor.value_or(session_fx::worldAnchor(
                {event.x.to_float(), event.y.to_float(),
                 event.z.to_float()},
                anchorObject)),
        }));
    }
    // Seismic is an optional presentation-only build feature in ZH. Keep its
    // authored slot without allowing an absent renderer simulation to block
    // the following shockwave weapon.
    if (event.shockwaveWeapon.empty()) return true;
    return handleTransportTransaction(
        ObjectTransportWeaponAtPositionTransaction{
            .source = event.source,
            .weaponTemplate = std::move(event.shockwaveWeapon),
            .x = event.x,
            .y = event.y,
            .z = event.z,
            .authoredOrder = event.authoredOrder,
            .confirmedTick = event.confirmedTick,
        });
}

bool GameSessionWeaponEventDrain::handleTransportTransaction(
    ObjectTransportVeterancySyncTransaction event) {
    const std::optional<ecs::entity> lower =
        m_world.m_objects
            .entityFromIdIncludingPending(event.lower);
    if (!lower) return true;
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(
        m_world.m_registry, *lower);
    const PlayerState* player = owner
        ? m_content.m_players.get(owner->player)
        : nullptr;
    static_cast<void>(m_world
        .m_objectSimulation.setObjectVeterancyLevel(
            m_world.m_registry,
            m_world.m_objects, event.lower,
            event.level,
            player ? player->upgrades.completed : UpgradeMask{},
            event.confirmedTick,
            {.players = &m_content.m_players,
             .scienceCatalog = m_content
                 .m_contentSnapshot.scienceCatalog(),
             .content = &m_content
                 .m_contentSnapshot,
             .random = &m_content
                 .m_simulationRandom,
             .terrain = &m_content.m_terrain,
             .effects = &m_world
                 .m_objectSimulation}));
    return true;
}

} // namespace engine::detail
