#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"

#include "game/session/frame/GameSessionFxAnchorSnapshot.h"

namespace engine::detail {

void GameSessionWeaponEventDrain::emitTransportObjectFx(
    ObjectId object,
    const container::String& fxList) {
    if (fxList.empty()) return;
    const std::optional<game::FxInvocationAnchor> anchor =
        session_fx::snapshotAnchor(
            m_world.m_registry,
            m_world.m_objects, object);
    static_cast<void>(m_publication.emitFxInvocationEvent({
        .fxListName = fxList,
        .anchorKind = anchor
            ? game::FxInvocationAnchorKind::ObjectAttachment
            : game::FxInvocationAnchorKind::WorldPosition,
        .primary = anchor.value_or(session_fx::worldAnchor({}, object)),
    }));
}

bool GameSessionWeaponEventDrain::handleTransportTransaction(
    ObjectTransportBattleBusStartTransaction event) {
    ObjectSimulation& simulation =
        m_world.m_objectSimulation;
    if (!simulation.beginBattleBusUndeath(
            m_world.m_registry,
            m_world.m_objects,
            event.battleBus, event.ruleIndex, event.confirmedTick)) {
        return true;
    }
    emitTransportObjectFx(event.battleBus, event.fxList);
    if (event.objectCreationList) {
        if (!handleTransportTransaction(
                std::move(*event.objectCreationList))) {
            return false;
        }
    }
    if (!simulation.finishBattleBusUndeath(
            m_world.m_registry,
            m_world.m_objects,
            event.battleBus, event.ruleIndex, event.confirmedTick)) {
        return true;
    }
    for (ObjectDamageRequest& request : event.passengerDamage) {
        simulation.queueDamage(std::move(request));
        // ZH applies each passenger's Body reaction before advancing through
        // the current Contain list. Preserve that per-occurrence closure.
        closeCurrentReaction();
        if (m_frame.result().faulted()) return false;
    }
    return true;
}

bool GameSessionWeaponEventDrain::handleTransportTransaction(
    ObjectTransportBattleBusLandedTransaction event) {
    ObjectSimulation& simulation =
        m_world.m_objectSimulation;
    if (!simulation.beginBattleBusLanded(
            m_world.m_registry,
            m_world.m_objects,
            event.battleBus, event.ruleIndex, event.confirmedTick)) {
        return true;
    }
    emitTransportObjectFx(event.battleBus, event.fxList);
    if (event.objectCreationList) {
        if (!handleTransportTransaction(
                std::move(*event.objectCreationList))) {
            return false;
        }
    }
    static_cast<void>(simulation.finishBattleBusLanded(
        m_world.m_registry,
        m_world.m_objects,
        event.battleBus, event.ruleIndex, event.confirmedTick));
    return true;
}

bool GameSessionWeaponEventDrain::handleTransportTransaction(
    ObjectTransportHijackerReleaseTransaction event) {
    ObjectSimulation& simulation =
        m_world.m_objectSimulation;
    if (!simulation.beginHijackerRelease(
            m_world.m_registry,
            m_world.m_objects,
            event.hijacker, event.ruleIndex, event.confirmedTick)) {
        return true;
    }
    const std::optional<ecs::entity> hijacker =
        m_world.m_objects.entityFromId(
            event.hijacker);
    if (hijacker && !event.parachuteTemplate.empty()) {
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(
            m_world.m_registry, *hijacker);
        const PrimaryTeamComponent* team =
            ecs::try_get<PrimaryTeamComponent>(
                m_world.m_registry, *hijacker);
        const ObjectFixedTransformComponent* transform =
            ecs::try_get<ObjectFixedTransformComponent>(
                m_world.m_registry, *hijacker);
        if (owner && team && transform &&
            m_content.m_contentSnapshot
                .findObjectArchetype(event.parachuteTemplate)) {
            ObjectSpawnRequest request;
            request.templateName = event.parachuteTemplate;
            request.owner = owner->player;
            request.primaryTeam = team->team;
            request.transform = *transform;
            request.origin = ObjectCreationOrigin::System;
            request.confirmedTick = event.confirmedTick;
            request.producer = event.hijacker;
            const GameSessionObjectSpawnResult parachute =
                m_lifecycle.spawnObject(std::move(request));
            if (parachute) {
                static_cast<void>(simulation.requestContainment(
                    m_world.m_registry,
                    m_world.m_objects,
                    {.kind = ObjectContainmentRequestKind::Attach,
                     .container = parachute.object,
                     .object = event.hijacker,
                     .confirmedTick = event.confirmedTick,
                     .force = true},
                    &m_content.m_players,
                    &m_content
                         .m_contentSnapshot));
            }
        }
    }
    closeCurrentReaction();
    if (m_frame.result().faulted()) return false;
    // Resource or containment failure must never leave the hijacker in the
    // half-released phase.
    static_cast<void>(simulation.finishHijackerRelease(
        m_world.m_registry,
        m_world.m_objects,
        event.hijacker, event.ruleIndex));
    return true;
}

} // namespace engine::detail
