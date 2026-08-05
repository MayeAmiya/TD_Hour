#include "game/session/core/GameSession.h"
#include "game/session/core/GameSessionDomainComposition.h"
#include "game/session/weapon/GameSessionGameplayTransactionDrain.h"

#include <chrono>

namespace engine {

void GameSession::updatePostCommandCombatAndSimulation() {
#if TD_DEBUG_ENABLED
    const auto started = std::chrono::steady_clock::now();
#endif
    detail::GameSessionPostCombatFrameState frame;
    domain().resolvePostCommandCombat(frame);
    if (framePort().result().faulted()) return;
#if TD_DEBUG_ENABLED
    const auto combatFinished = std::chrono::steady_clock::now();
#endif
    domain().updatePostCommandObjectSimulation(frame);
    if (framePort().result().faulted()) return;
    // Close authoritative module work before any presentation collector can
    // consume sibling ledgers and before transaction/transport phases can
    // observe objects which PhysicsCrash or bridge collapse already retired.
    domain().drainGameplayTransactions();
    if (framePort().result().faulted()) return;
#if TD_DEBUG_ENABLED
    const auto simulationFinished = std::chrono::steady_clock::now();
#endif
    domain().publishPostCommandHealthAndCollectSimulationEvents();
    if (framePort().result().faulted()) return;
#if TD_DEBUG_ENABLED
    const auto healthFinished = std::chrono::steady_clock::now();
#endif
    domain().publishPostCommandObjectTransactions(frame);
    if (framePort().result().faulted()) return;
#if TD_DEBUG_ENABLED
    const auto transactionsFinished = std::chrono::steady_clock::now();
#endif
    domain().publishPostCommandTransportEvents();
    if (framePort().result().faulted()) return;
#if TD_DEBUG_ENABLED
    const auto transportFinished = std::chrono::steady_clock::now();
#endif
    domain().finalizePostCommandSimulationHandoff(frame);
#if TD_DEBUG_ENABLED
    const auto finished = std::chrono::steady_clock::now();
    if (confirmedTick() <= 16u) {
        const auto micros = [](auto begin, auto end) {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                end - begin).count();
        };
        TD_LOG_INFO(
            "[CombatStageTiming] tick={} resolve={}us objectSimulation={}us healthAndCollect={}us transactions={}us transport={}us handoff={}us total={}us",
            confirmedTick(), micros(started, combatFinished),
            micros(combatFinished, simulationFinished),
            micros(simulationFinished, healthFinished),
            micros(healthFinished, transactionsFinished),
            micros(transactionsFinished, transportFinished),
            micros(transportFinished, finished), micros(started, finished));
    }
#endif
}

} // namespace engine
