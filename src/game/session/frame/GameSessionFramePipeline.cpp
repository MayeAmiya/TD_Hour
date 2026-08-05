#include "game/session/core/GameSession.h"
#include "game/session/core/GameSessionDomainComposition.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/ai/GameSessionStrategicAIService.h"
#include "game/session/transaction/GameSessionScriptScenarioPlanTransactions.h"
#include "game/session/transaction/GameSessionObjectDamageTransactions.h"
#include "game/session/transaction/GameSessionNavigationTransactions.h"
#include "game/session/lifecycle/GameSessionWorldMaintenanceService.h"
#include "debug/debug.h"

#include <algorithm>
#include <chrono>

namespace engine {

void GameSession::updatePreCommandSystems(float /*deltaSeconds*/) {
    if (!domainState().contentState().m_active) return;
    // The caller has already advanced the camera and sampled the freeze gate.
    // Do not re-query a value possibly changed by this same frame's scripts:
    // RefCode lets FREEZE_TIME/UNFREEZE_TIME affect the following world frame.
    worldMaintenanceService().updateTerrainLogic(
        domainState().presentationState().m_confirmedTick,
        static_cast<uint32_t>(std::max(
            1, domainState().contentState().m_startInfo.gameSpeedFPS)));
    const navigation::NavigationSystemStatus terrainNavigation =
        GameSessionNavigationTransactions{
            domainState().contentState(),
            domainState().presentationState()}
            .synchronizeTerrainAuthority();
    if (terrainNavigation != navigation::NavigationSystemStatus::Success &&
        terrainNavigation !=
            navigation::NavigationSystemStatus::PublicationPending) {
        static_cast<void>(gameplayPublicationPort().raiseSimulationFault({
            .domain = SimulationFaultDomain::Navigation,
            .code = SimulationFaultCode::AtomicCommitFailed,
            .confirmedTick =
                domainState().presentationState().m_confirmedTick,
        }));
        return;
    }
    // RefCode's TerrainLogic update runs after ScriptEngine but before
    // commands/object modules. Flood transitions therefore resolve their
    // normal WATER damage here, not at the next object's update or visual
    // extraction frame.
    objectDamageTransactions().resolveTerrainWaterDamage();
    for (const GameSessionReadyPlayerEvacuation& ready :
         domain().advancePendingPlayerEvacuations()) {
        static_cast<void>(confirmedCommandPort().evacuate(
            ready.container, ready.player,
            domainState().presentationState().m_confirmedTick));
    }
}

void GameSession::updatePostCommandSystems(float deltaSeconds) {
    // GameLogic/single-session update have already sampled the time gate.  A
    // ScriptTimeControlEffect flushed earlier in this same frame must not
    // retroactively suppress the post-command world phase.
    if (!domainState().contentState().m_active) return;
#if TD_DEBUG_ENABLED
    const auto started = std::chrono::steady_clock::now();
#endif
    updatePostCommandCombatAndSimulation();
#if TD_DEBUG_ENABLED
    const auto combatFinished = std::chrono::steady_clock::now();
#endif
    if (framePort().result().faulted()) return;
    updatePostCommandSimulationEvents();
#if TD_DEBUG_ENABLED
    const auto eventsFinished = std::chrono::steady_clock::now();
#endif
    if (framePort().result().faulted()) return;
    updatePostCommandFinalize(deltaSeconds);
    if (framePort().result().faulted()) return;
    // RefCode runs the global AI and BuildAssistant only after the current
    // object's UpdateModules have observed commands, production, deaths and
    // released builders.  Decisions made here can append durable queues or
    // construction entries, but no object/production phase remains in this
    // confirmed frame to consume them; their first consumer is the next
    // world tick.
    scenarioPlanTransactions().processPriorityBuildEntries();
    if (framePort().result().faulted()) return;
    strategicAIService().update();
#if TD_DEBUG_ENABLED
    const auto finalized = std::chrono::steady_clock::now();
    if (confirmedTick() <= 16u) {
        const auto micros = [](auto begin, auto end) {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                end - begin).count();
        };
        TD_LOG_INFO(
            "[SessionStageTiming] tick={} combatAndSimulation={}us simulationEvents={}us finalize={}us total={}us",
            confirmedTick(), micros(started, combatFinished),
            micros(combatFinished, eventsFinished),
            micros(eventsFinished, finalized), micros(started, finalized));
    }
#endif
}






} // namespace engine
