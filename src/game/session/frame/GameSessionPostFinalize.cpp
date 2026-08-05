#include "game/session/frame/GameSessionObjectEventPublisher.h"
#include "game/session/frame/GameSessionEvaEventPublisher.h"
#include "game/session/core/GameSession.h"
#include "game/session/core/GameSessionDomainComposition.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/lifecycle/GameSessionWorldMaintenanceService.h"
#include "game/session/transaction/GameSessionObjectSaleTransactions.h"
#include "game/session/transaction/GameSessionObjectLifecycleTransactions.h"
#include "game/session/transaction/GameSessionMultiplayerVictoryTransactions.h"
#include "game/session/transaction/GameSessionNavigationTransactions.h"

#include <chrono>

namespace engine {
void GameSession::updatePostCommandFinalize(float deltaSeconds) {
#if TD_DEBUG_ENABLED
    const auto finalizeStarted = std::chrono::steady_clock::now();
#endif
    domain().publishObjectFeedback();
    // Gameplay payloads carried by TransitionDamage/InstantDeath/SlowDeath
    // are extracted into the confirmed depth-first work stack first. FX
    // publication below is presentation-only and cannot gate OCL, Weapon or
    // rubble creation.
    domain().drainGameplayTransactions();
    GameSessionObjectEventPublisher eventPublisher = objectEventPublisher();
    eventPublisher.publishFx();
    eventPublisher.publishTechAndBeacon();
    // Sale is a confirmed delayed transaction rather than an immediate
    // delete.  Settle completed structures before player aggregates and
    // ProductionUpdate sample this frame, then let the normal lifecycle
    // boundary reclaim them together with combat/script destruction.
    objectSaleTransactions().settleDueSales(
        domainState().presentationState().m_confirmedTick);
    // Power aggregates only visit power-bearing ECS entities. Rebuild after
    // Body/Die has applied this frame's subdual/pending-destroy facts and
    // before ProductionUpdate recalculates its queue-head build time.
#if TD_DEBUG_ENABLED
    const auto preProductionFinished = std::chrono::steady_clock::now();
#endif
    worldMaintenanceService().refreshObjectDerivedPlayerAggregates(
        domainState().presentationState().m_confirmedTick);
#if TD_DEBUG_ENABLED
    const auto firstAggregatesFinished = std::chrono::steady_clock::now();
#endif
    worldMaintenanceService().updatePlayerPeriodicState(
        domainState().presentationState().m_confirmedTick);
    // Production runs after Body/Die has made this frame's pending-destroy
    // facts visible and before lifecycle flush removes factories.  It refunds
    // a dying producer's paid jobs, advances live FIFO heads, and emits only
    // detached spawn intents.  GameSession remains the sole object creation
    // transaction, so production cannot bypass team/name/archetype assembly.
    const bool productionChanged = domain().advanceConfirmedProduction();
    domain().drainGameplayTransactions();
    if (framePort().result().faulted()) return;
#if TD_DEBUG_ENABLED
    const auto productionFinished = std::chrono::steady_clock::now();
#endif
    // A produced object may itself produce or consume power. Its central
    // spawn transaction has already completed, so publish the resulting
    // aggregate in this same confirmed frame instead of a tick late.
    if (productionChanged) {
        worldMaintenanceService().refreshObjectDerivedPlayerAggregates(
            domainState().presentationState().m_confirmedTick);
    }
#if TD_DEBUG_ENABLED
    const auto secondAggregatesFinished = std::chrono::steady_clock::now();
#endif
    // RefCode's Eva::update runs on the client once per frame and re-polls its
    // predicate family. Sample it here, after both power-aggregate refreshes,
    // so a reactor completed or destroyed this frame is already reflected.
    evaEventPublisher().publishPolledObserverConditions(
        domainState().presentationState(),
        domainState().presentationState().m_confirmedTick);
    domain().updateConfirmedPresentation(deltaSeconds);
#if TD_DEBUG_ENABLED
    const auto modelConditionsFinished = std::chrono::steady_clock::now();
    const auto presentationClocksFinished = modelConditionsFinished;
#endif
    // RefCode runs VictoryConditions after ScriptEngine and object updates.
    // Latch this frame's elimination/alliance result here so scripts observe
    // it on the next confirmed tick instead of seeing a one-frame-early
    // result at ScriptRuntime ingress.  Any killPlayer-equivalent destroy
    // requests emitted by the owner are committed by the lifecycle flush
    // immediately below.
    multiplayerVictoryTransactions().refresh();
    static_cast<void>(objectLifecycleTransactions().flushPending());
#if TD_DEBUG_ENABLED
    const auto lifecycleFinished = std::chrono::steady_clock::now();
#endif
    // Spatial/selection consumers observe only the completed structural
    // state, never an entity marked PendingDestroy halfway through this tick.
    worldMaintenanceService().refreshSpatialIndex();
#if TD_DEBUG_ENABLED
    const auto spatialFinished = std::chrono::steady_clock::now();
#endif
    worldMaintenanceService().updateMapVisibilityLookers(
        domainState().presentationState().m_confirmedTick);
    // Announcements decided inside this frame's authoritative transactions
    // (structure completion, and anything else without its own publication
    // port) reach the detached audio stream here, after the lifecycle flush
    // that produced them.
    evaEventPublisher().publishPendingAnnouncements(
        domainState().presentationState());
    domain().updateClientTerrainPresentation(deltaSeconds);
    static_cast<void>(
        GameSessionNavigationTransactions{
            domainState().contentState(),
            domainState().presentationState()}
            .advanceConfirmedTickAndPublishFault(
                domainState().presentationState().m_confirmedTick,
                gameplayPublicationPort()));
#if TD_DEBUG_ENABLED
    const auto finalizeFinished = std::chrono::steady_clock::now();
    if (domainState().presentationState().m_confirmedTick <= 16u) {
        const auto micros = [](auto begin, auto end) {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                end - begin).count();
        };
        TD_LOG_INFO(
            "[FinalizeTiming] tick={} preProduction={}us firstAggregates={}us production={}us secondAggregates={}us modelConditions={}us presentationClocks={}us lifecycle={}us spatial={}us visibilityTreesNavigation={}us total={}us",
            domainState().presentationState().m_confirmedTick,
            micros(finalizeStarted, preProductionFinished),
            micros(preProductionFinished, firstAggregatesFinished),
            micros(firstAggregatesFinished, productionFinished),
            micros(productionFinished, secondAggregatesFinished),
            micros(secondAggregatesFinished, modelConditionsFinished),
            micros(modelConditionsFinished, presentationClocksFinished),
            micros(presentationClocksFinished, lifecycleFinished),
            micros(lifecycleFinished, spatialFinished),
            micros(spatialFinished, finalizeFinished),
            micros(finalizeStarted, finalizeFinished));
    }
#endif
}

} // namespace engine
