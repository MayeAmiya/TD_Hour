#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"

#include "game/player/PlayerRegistry.h"

#include <utility>

namespace engine::detail {

bool GameSessionWeaponEventDrain::handleDynamicGeometry(WorkItem item) {
    ObjectDynamicGeometryGameplayEvent& event = item.dynamicGeometry;
    if (!event.object || event.confirmedTick !=
            m_presentation.m_confirmedTick) {
        return true;
    }

    // FirestormDynamicGeometryInfoUpdate records creation before applying the
    // same authored occurrence's radius damage. Keep that order in the common
    // causal stack; presentation Start/Radius/Scorch remains an independent
    // value stream and cannot suppress either gameplay effect.
    if (event.firestormCreated && event.owner) {
        static_cast<void>(m_content.m_players
            .recordAcademyEvent(
                event.owner, PlayerAcademyEvent::FirestormCreated));
    }
    for (ObjectDamageRequest& request : event.damage) {
        m_world.m_objectSimulation.queueDamage(
            std::move(request));
    }
    // This route also consumes DestroyRequested/DeleteWalk descendants before
    // the next Firestorm occurrence, preserving the original synchronous
    // Object::attemptDamage call chain rather than merely draining Body rows.
    m_lifecycle.resolveQueuedObjectDamage();
    return true;
}

} // namespace engine::detail
