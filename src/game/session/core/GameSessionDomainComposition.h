#pragma once

#include "game/session/ai/GameSessionAIDomain.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/script/GameSessionScriptFrameTransactions.h"
#include "game/session/frame/GameSessionFramePort.h"
#include "game/session/frame/GameSessionDebrisPresentationPublisher.h"
#include "game/session/frame/GameSessionObjectFeedbackPublisher.h"
#include "game/session/frame/GameSessionConfirmedPresentationUpdater.h"
#include "game/session/frame/GameSessionClientTerrainPresentationUpdater.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/session/transaction/GameSessionPendingEvacuationTransactions.h"
#include "game/session/transaction/GameSessionObjectDamageTransactions.h"
#include "game/session/transaction/GameSessionObjectLifecycleTransactions.h"
#include "game/session/transaction/GameSessionObjectProductionTransactions.h"
#include "game/session/transaction/GameSessionCountermeasureTransactions.h"
#include "game/session/transaction/GameSessionScriptScenarioPlanTransactions.h"
#include "game/session/transaction/GameSessionAIAttackOrderTransactions.h"
#include "game/session/transaction/GameSessionAINavigationFrameTransactions.h"
#include "game/session/transaction/GameSessionAIInsertionTransactions.h"
#include "game/session/transaction/GameSessionAIMoveOrderTransactions.h"
#include "game/session/transaction/GameSessionAIShadowTransactions.h"
#include "game/session/transaction/GameSessionAISpecialCommandTransactions.h"
#include "game/session/transaction/GameSessionAIResolutionTransactions.h"
#include "game/session/transaction/GameSessionBuildPlacementEvaluator.h"
#include "game/session/weapon/GameSessionGameplayTransactionDrain.h"

#include <utility>

namespace engine {

class GameSession;

namespace detail {

struct GameSessionPostCombatFrameState final {
    struct ClientTerrainMovementSource final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
        math::vec3 previousPosition{};
    };

    container::SharedPtr<const game::terrain::MapVisibilitySnapshot>
        repairVisibilitySnapshot;
    ObjectUpgradeExecutionContext objectContext;
    container::Vector<ClientTerrainMovementSource>
        clientTerrainMovementSources;
};

// Private, non-virtual composition of session-owned state and fixed-frame
// domain services. PostCombat is one ordered stage of this composition, not
// the owner of the session itself.
class GameSessionDomainComposition {
    friend class GameSession;
    friend struct GameSessionStorage;
private:
    GameSessionDomainComposition() noexcept
        : m_aiDomain(m_state),
          m_content(m_state.contentState()),
          m_world(m_state.worldState()),
          m_ai(m_state.aiState()),
          m_presentation(m_state.presentationState()),
          m_objectEvents(m_state.objectEventState()),
          m_publication(
              m_content, m_world, m_presentation, m_state.frameState()),
          m_debris(m_content, m_world, m_presentation, m_publication),
          m_objectFeedback(
              m_content, m_world, m_presentation, m_publication),
          m_confirmedPresentation(
              m_content, m_world, m_presentation),
          m_clientTerrainPresentation(
              m_content, m_world, m_publication),
          m_pendingEvacuations(m_world, m_presentation),
          m_scriptFrames(
              m_content, m_world, m_ai, m_presentation, m_objectEvents,
              m_state.frameState()),
          m_frame(
              m_content, m_world, m_presentation, m_objectEvents,
              m_state.frameState(), m_scriptFrames),
          m_lifecycle(
              m_content, m_world, m_presentation,
              lifecyclePort(), &m_objectEvents),
          m_production(
              m_content, m_world, m_presentation,
              makeProductionPolicyPort(m_content, m_presentation),
              lifecyclePort()),
          m_countermeasures(
              m_world, m_presentation,
              lifecyclePort()),
          m_scenarioPlans(
              m_content, m_world, m_ai, m_presentation,
              makeScenarioTransactionPort(
                  m_content, m_world, m_ai, m_presentation,
                  lifecyclePort())),
          m_aiAttackOrders(
              m_content, m_world, m_ai, m_presentation),
          m_aiNavigation(
              m_content, m_world, m_ai, m_presentation, m_publication),
          m_aiInsertion(m_content, m_world, m_ai, m_presentation),
          m_aiMoveOrders(
              m_content, m_world, m_ai, m_presentation,
              m_publication, m_frame),
          m_aiShadow(
              m_content, m_world, m_ai, m_presentation,
              m_publication, m_frame),
          m_aiSpecialCommands(
              m_content, m_world, m_ai, m_presentation,
              m_publication, m_lifecycle),
          m_aiResolution(
              m_content, m_world, m_ai, m_presentation, m_publication),
          m_damage(
              m_content, m_world, m_presentation,
              lifecyclePort()),
          m_placement(m_content, m_ai, m_presentation, m_world) {}

    void drainGameplayTransactions() {
        detail::GameSessionGameplayTransactionDrain::run(
            m_content, m_world, m_ai, m_presentation, m_objectEvents,
            m_state.frameState(), lifecyclePort());
    }
    void publishObjectFeedback() { m_objectFeedback.publish(); }
    [[nodiscard]] bool advanceConfirmedProduction() {
        return m_production.advanceConfirmedProduction();
    }
    void updateConfirmedPresentation(float deltaSeconds) {
        m_confirmedPresentation.update(deltaSeconds);
    }
    void updateClientTerrainPresentation(float deltaSeconds) {
        m_clientTerrainPresentation.update(deltaSeconds);
    }
    [[nodiscard]] container::Vector<GameSessionReadyPlayerEvacuation>
    advancePendingPlayerEvacuations() {
        return m_pendingEvacuations.advance();
    }
    [[nodiscard]] GameSessionLifecycleTransactionPort lifecyclePort()
        noexcept {
        return makeLifecycleTransactionPort(
            m_content, m_world, m_ai, m_presentation, m_objectEvents,
            m_state.frameState());
    }
    [[nodiscard]] GameSessionOrderAdmissionPolicyPort orderPolicyPort()
        noexcept {
        return makeOrderAdmissionPolicyPort(
            m_content, m_world, m_presentation, lifecyclePort());
    }
    [[nodiscard]] GameSessionScenarioTransactionPort scenarioPort()
        noexcept {
        return makeScenarioTransactionPort(
            m_content, m_world, m_ai, m_presentation, lifecyclePort());
    }
    [[nodiscard]] GameSessionScriptFrameTransactions& scriptFrames()
        noexcept {
        return m_scriptFrames;
    }
    [[nodiscard]] GameSessionStateRoot& domainState() noexcept {
        return m_state;
    }
    [[nodiscard]] const GameSessionStateRoot& domainState() const noexcept {
        return m_state;
    }
    [[nodiscard]] GameSessionAIDomain& aiDomain() noexcept {
        return m_aiDomain;
    }
    [[nodiscard]] const GameSessionAIDomain& aiDomain() const noexcept {
        return m_aiDomain;
    }

    void resolvePostCommandCombat(GameSessionPostCombatFrameState& frame);
    void updatePostCommandObjectSimulation(
        GameSessionPostCombatFrameState& frame);
    void publishPostCommandHealthAndCollectSimulationEvents();
    void publishPostCommandObjectTransactions(
        const GameSessionPostCombatFrameState& frame);
    void publishPostCommandTransportEvents();
    void finalizePostCommandSimulationHandoff(
        const GameSessionPostCombatFrameState& frame);

    GameSessionStateRoot m_state;
    GameSessionAIDomain m_aiDomain;
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionObjectEventState& m_objectEvents;
    GameSessionGameplayPublicationPort m_publication;
    GameSessionDebrisPresentationPublisher m_debris;
    GameSessionObjectFeedbackPublisher m_objectFeedback;
    GameSessionConfirmedPresentationUpdater m_confirmedPresentation;
    GameSessionClientTerrainPresentationUpdater m_clientTerrainPresentation;
    GameSessionPendingEvacuationTransactions m_pendingEvacuations;
    GameSessionScriptFrameTransactions m_scriptFrames;
    GameSessionFramePort m_frame;
    GameSessionObjectLifecycleTransactions m_lifecycle;
    GameSessionObjectProductionTransactions m_production;
    GameSessionCountermeasureTransactions m_countermeasures;
    GameSessionScriptScenarioPlanTransactions m_scenarioPlans;
    GameSessionAIAttackOrderTransactions m_aiAttackOrders;
    GameSessionAINavigationFrameTransactions m_aiNavigation;
    GameSessionAIInsertionTransactions m_aiInsertion;
    GameSessionAIMoveOrderTransactions m_aiMoveOrders;
    GameSessionAIShadowTransactions m_aiShadow;
    GameSessionAISpecialCommandTransactions m_aiSpecialCommands;
    GameSessionAIResolutionTransactions m_aiResolution;
    GameSessionObjectDamageTransactions m_damage;
    GameSessionBuildPlacementEvaluator m_placement;
};

} // namespace detail
} // namespace engine
