#include "game/session/transaction/GameSessionCountermeasureTransactions.h"

#include "game/session/state/GameSessionDomainState.h"

namespace engine {

void GameSessionCountermeasureTransactions::updateAndResolveDiversions() {
    m_world.m_objectSimulation.updateCountermeasures(
        m_world.m_registry, m_world.m_objects,
        m_presentation.m_confirmedTick);
    m_barrier.drainGameplayTransactions();
    m_world.m_objectSimulation.resolveCountermeasureDiversions(
        m_world.m_registry, m_world.m_objects,
        m_presentation.m_confirmedTick);
}

} // namespace engine
