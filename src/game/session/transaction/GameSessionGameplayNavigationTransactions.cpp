#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"

#include "game/session/frame/GameSessionNavigationPresentationRules.h"

namespace engine::detail {

bool GameSessionWeaponEventDrain::handleCheckpointNavigation(
    WorkItem item) {
    const ObjectCheckpointNavigationEvent& event =
        item.checkpointNavigation;
    const std::optional<ecs::entity> entity =
        m_world.m_objects
            .entityFromIdIncludingPending(event.object);
    if (!entity) return true;
    const bool rubbleStructure = session_navigation::isRubbleBlocker(
        m_world.m_registry, *entity);
    const bool blocksGround =
        session_navigation::blocksGround(
            m_world.m_registry, *entity);
    const bool blocksAir =
        session_navigation::blocksAircraft(
            m_world.m_registry, *entity);
    if (m_world.m_objects.isPendingDestroy(
            event.object) ||
        (!rubbleStructure && !blocksGround && !blocksAir)) {
        return true;
    }
    const ObjectStatusComponent* status = ecs::try_get<ObjectStatusComponent>(
        m_world.m_registry, *entity);
    const bool underConstruction = status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction));
    if (m_navigationFootprints.submitBuildingFootprint(
            event.object, *entity, event.confirmedTick,
            navigation::NavigationDynamicEventReason::FootprintChanged,
            underConstruction
                ? navigation::NavigationBuildingState::Placed
                : navigation::NavigationBuildingState::Complete,
            std::optional<bool>{rubbleStructure ? false : blocksGround},
            std::optional<bool>{rubbleStructure ? false : blocksAir})) {
        return true;
    }
    static_cast<void>(m_publication.raiseSimulationFault({
        .domain = SimulationFaultDomain::Navigation,
        .code = SimulationFaultCode::AtomicCommitFailed,
        .confirmedTick = event.confirmedTick,
        .subject = event.object.value,
        .sequence = event.authoredOrder,
    }));
    return true;
}

bool GameSessionWeaponEventDrain::handleTensileNavigation(WorkItem item) {
    const ObjectTensileFormationEvent& event = item.tensileNavigation;
    if (!event.object || event.confirmedTick !=
            m_presentation.m_confirmedTick) {
        return true;
    }

    bool submitted = true;
    switch (event.kind) {
    case ObjectTensileFormationEventKind::NavigationWallRemove:
        submitted = m_navigationFootprints.submitBuildingState(
            event.object, event.confirmedTick,
            navigation::NavigationDynamicEventReason::FootprintChanged,
            navigation::NavigationBuildingState::Absent, false);
        break;
    case ObjectTensileFormationEventKind::NavigationWallCreate:
    case ObjectTensileFormationEventKind::TerminalRubble: {
        const std::optional<ecs::entity> entity =
            m_world.m_objects
                .entityFromIdIncludingPending(event.object);
        if (!entity) return true;
        submitted = m_navigationFootprints.submitBuildingFootprint(
            event.object, *entity, event.confirmedTick,
            navigation::NavigationDynamicEventReason::FootprintChanged,
            navigation::NavigationBuildingState::Complete);
        break;
    }
    case ObjectTensileFormationEventKind::CrackSound:
        // CrackSound belongs exclusively to the presentation stream. Seeing
        // it in the gameplay journal is a malformed producer route.
        return false;
    }

    if (!submitted) {
        static_cast<void>(m_publication.raiseSimulationFault({
            .domain = SimulationFaultDomain::Navigation,
            .code = SimulationFaultCode::AtomicCommitFailed,
            .confirmedTick = event.confirmedTick,
            .subject = event.object.value,
            .sequence = event.authoredOrder,
        }));
    }
    return true;
}

} // namespace engine::detail
