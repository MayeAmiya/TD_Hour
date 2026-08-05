#include "game/session/core/GameSession.h"
#include "game/session/core/GameSessionStorage.h"
#include "game/session/state/GameSessionDomainState.h"

#include "game/session/ai/GameSessionAIQueryPort.h"
#include "game/session/ai/GameSessionStrategicAIService.h"
#include "game/session/lifecycle/GameSessionScenarioBootstrapService.h"
#include "game/session/frame/GameSessionDynamicGeometryEventPublisher.h"
#include "game/session/frame/GameSessionEvaEventPublisher.h"
#include "game/session/frame/GameSessionObjectEventPublisher.h"
#include "game/session/lifecycle/GameSessionWorldMaintenanceService.h"
#include "game/session/query/GameSessionCommandQueryPort.h"
#include "game/session/query/GameSessionEconomyQueryPort.h"
#include "game/session/query/GameSessionObjectQueryPort.h"
#include "game/session/query/GameSessionRulesetQueryPort.h"
#include "game/session/query/InGameCommandQuerySource.h"
#include "game/session/query/LocalSelectionQueryPort.h"
#include "game/session/query/PlayerUiProjection.h"
#include "game/session/query/SessionPlayerQuery.h"
#include "game/session/query/SessionRuntimeQuery.h"
#include "game/session/query/WorldCommandQueryPort.h"
#include "game/session/transaction/GameSessionObjectDamageTransactions.h"
#include "game/session/transaction/GameSessionObjectLifecycleTransactions.h"
#include "game/session/transaction/GameSessionMultiplayerVictoryTransactions.h"
#include "game/session/transaction/GameSessionObjectProductionTransactions.h"
#include "game/session/transaction/GameSessionObjectSaleTransactions.h"
#include "game/session/transaction/GameSessionScriptOrderAdmissionTransactions.h"
#include "game/session/transaction/GameSessionTransactionPorts.h"

namespace engine {

GameSession::GameSession()
    : m_storage(std::make_unique<detail::GameSessionStorage>()) {}

GameSession::~GameSession() = default;

detail::GameSessionDomainComposition& GameSession::domain() noexcept {
    return m_storage->domain;
}

const detail::GameSessionDomainComposition&
GameSession::domain() const noexcept {
    return m_storage->domain;
}

GameRenderExtractionCache&
GameSession::renderExtractionCache() const noexcept {
    return m_storage->renderExtraction;
}

GameSessionStateRoot& GameSession::domainState() noexcept {
    return domain().domainState();
}

const GameSessionStateRoot& GameSession::domainState() const noexcept {
    return domain().domainState();
}

bool GameSession::isActive() const noexcept {
    return domainState().contentState().m_active;
}

uint64_t GameSession::confirmedTick() const noexcept {
    return domainState().presentationState().m_confirmedTick;
}

GameSessionFramePort GameSession::framePort() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionFramePort{
        state.contentState(), state.worldState(),
        state.presentationState(), state.objectEventState(),
        state.frameState(), domain().scriptFrames()};
}

GameSessionPresentationPort GameSession::presentationPort() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionPresentationPort{
        state.contentState(), state.worldState(),
        state.presentationState(), state.frameState()};
}

LocalPlacementPresentationPort GameSession::localPlacementPort() noexcept {
    GameSessionStateRoot& state = domainState();
    return LocalPlacementPresentationPort{
        state.contentState(), state.worldState(), state.aiState(),
        state.presentationState()};
}

GameSessionConfirmedCommandPort
GameSession::confirmedCommandPort() noexcept {
    return GameSessionConfirmedCommandPort{
        domainState(), objectProductionTransactions(),
        scriptOrderAdmissionTransactions(), objectSaleTransactions()};
}

GameSessionGameplayPublicationPort
GameSession::gameplayPublicationPort() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionGameplayPublicationPort{
        state.contentState(), state.worldState(), state.presentationState(),
        state.frameState()};
}

GameSessionRenderExtractionPort
GameSession::renderExtractionPort() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionRenderExtractionPort{
        state.contentState(), state.worldState(), state.presentationState(),
        state.objectEventState(), renderExtractionCache()};
}

GameSessionMediaPresentationPort
GameSession::mediaPresentationPort() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionMediaPresentationPort{
        state.contentState(), state.worldState(), state.presentationState()};
}

GameSessionScriptUiPort GameSession::scriptUiPort() noexcept {
    return GameSessionScriptUiPort{domainState().presentationState()};
}

uint32_t GameSession::logicFramesPerSecond() const noexcept {
    return domainState().worldState().m_objectSimulation.rules()
        .logicFramesPerSecond;
}

const ResolvedMatchSetup* GameSession::resolvedMatchSetup() const noexcept {
    const auto& setup = domainState().contentState().m_resolvedMatchSetup;
    return setup ? &*setup : nullptr;
}

game::terrain::TerrainLogic&
GameSession::mutableTerrainForPersistence() noexcept {
    return domainState().contentState().m_terrain;
}

const game::terrain::TerrainLogic&
GameSession::terrainForPersistence() const noexcept {
    return domainState().contentState().m_terrain;
}

bool GameSession::persistenceAllowed() const noexcept {
    const GameSessionContentStartState& content =
        domainState().contentState();
    return content.m_active && !content.m_startInfo.network.enabled &&
        content.m_startInfo.mode != GameMode::Replay;
}

ClientTerrainObjectStore& GameSession::clientTerrainObjects() noexcept {
    return domainState().worldState().m_clientTerrainObjects;
}

const ClientTerrainObjectStore&
GameSession::clientTerrainObjects() const noexcept {
    return domainState().worldState().m_clientTerrainObjects;
}

ClientTerrainObjectPersistentState
GameSession::captureClientTerrainObjectPersistentState() const {
    return domainState().worldState().m_clientTerrainObjects
        .capturePersistentState();
}

bool GameSession::restoreClientTerrainObjectPersistentState(
    const ClientTerrainObjectPersistentState& state) noexcept {
    return domainState().worldState().m_clientTerrainObjects
        .restorePersistentState(state);
}

GameSessionObjectQueryPort GameSession::objectQuery() const noexcept {
    return GameSessionObjectQueryPort{
        domainState().worldState().m_objects};
}

GameSessionEconomyQueryPort GameSession::economyQuery() const noexcept {
    return GameSessionEconomyQueryPort{
        domainState().worldState().m_registry,
        domainState().worldState().m_objects};
}

GameSessionRulesetQueryPort GameSession::rulesetQuery() const noexcept {
    return GameSessionRulesetQueryPort{
        domainState().contentState().m_ruleset.get()};
}

GameSessionCommandQueryPort GameSession::commandQuery() const noexcept {
    return GameSessionCommandQueryPort{
        domainState().contentState(), domainState().worldState(),
        domainState().aiState()};
}

GameSessionRuntimeQueryPort GameSession::runtimeQuery() const noexcept {
    return GameSessionRuntimeQueryPort{
        domainState().contentState().m_startInfo,
        domainState().contentState().m_resolvedMatchSetup,
        domainState().presentationState().m_missionState,
        domainState().worldState().m_objects};
}

session_query::SessionPlayerQueryPort
GameSession::playerQuery() const noexcept {
    return session_query::SessionPlayerQueryPort{
        domainState().contentState().m_players};
}

selection::WorldCommandQueryPort
GameSession::worldCommandQuery() const noexcept {
    const GameSessionWorldState& world = domainState().worldState();
    return selection::WorldCommandQueryPort{
        world.m_registry,
        domainState().contentState().m_players,
        world.m_ownership,
        world.m_objects,
        world.m_objectSimulation,
        domainState().contentState().m_contentSnapshot,
        domainState().aiState().m_objectAI};
}

selection::LocalSelectionQueryPort
GameSession::localSelectionQuery() const noexcept {
    const GameSessionWorldState& world = domainState().worldState();
    return selection::LocalSelectionQueryPort{
        world.m_registry,
        world.m_objects,
        world.m_ownership,
        domainState().contentState().m_contentSnapshot,
        domain().aiDomain()};
}

GameSessionAIQueryPort GameSession::aiQuery() const noexcept {
    return GameSessionAIQueryPort{
        domain().aiDomain()};
}

session_query::PlayerUiQueryPort
GameSession::playerUiQuery() const noexcept {
    return session_query::PlayerUiQueryPort{
        domainState().contentState(), makeInGameCommandQuerySource(),
        commandQuery(), economyQuery(), rulesetQuery(), confirmedTick(),
        logicFramesPerSecond()};
}

session_query::InGameCommandQuerySource
GameSession::makeInGameCommandQuerySource() const noexcept {
    const GameSessionContentStartState& content =
        domainState().contentState();
    const GameSessionWorldState& world = domainState().worldState();
    const GameSessionScriptPresentationState& presentation =
        domainState().presentationState();
    return session_query::InGameCommandQuerySource{
        world.m_registry,
        content.m_players,
        world.m_ownership,
        world.m_objects,
        content.m_contentSnapshot,
        presentation.m_scriptCommandBarOverrides,
        presentation.m_scriptObjectBuildabilityOverrides};
}

GameSessionScenarioBootstrapService
GameSession::scenarioBootstrapService() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionScenarioBootstrapService{
        state.contentState(),
        state.aiState(),
        state.presentationState(),
        state.objectEventState(),
        state.worldState(),
        domain().lifecyclePort()};
}

GameSessionStrategicAIService GameSession::strategicAIService() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionStrategicAIService{
        state.contentState(),
        state.aiState(),
        state.presentationState(),
        state.worldState(),
        scenarioPlanTransactions(),
        objectProductionTransactions(),
        GameSessionObjectStateTransactions{
            state.worldState().m_registry,
            state.worldState().m_objects}};
}

GameSessionScriptScenarioPlanTransactions
GameSession::scenarioPlanTransactions() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionScriptScenarioPlanTransactions{
        state.contentState(),
        state.worldState(),
        state.aiState(),
        state.presentationState(),
        domain().scenarioPort()};
}

GameSessionObjectProductionTransactions
GameSession::objectProductionTransactions() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionObjectProductionTransactions{
        state.contentState(),
        state.worldState(),
        state.presentationState(),
        makeProductionPolicyPort(
            state.contentState(), state.presentationState()),
        domain().lifecyclePort()};
}

GameSessionScriptOrderAdmissionTransactions
GameSession::scriptOrderAdmissionTransactions() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionScriptOrderAdmissionTransactions{
        state.contentState(),
        state.worldState(),
        state.aiState(),
        state.presentationState(),
        domain().orderPolicyPort()};
}

GameSessionWorldMaintenanceService
GameSession::worldMaintenanceService() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionWorldMaintenanceService{
        state.contentState(), state.worldState()};
}

GameSessionObjectSaleTransactions
GameSession::objectSaleTransactions() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionObjectSaleTransactions{
        state.contentState(),
        state.worldState(),
        state.aiState(),
        state.presentationState(),
        domain().lifecyclePort()};
}

GameSessionObjectDamageTransactions
GameSession::objectDamageTransactions() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionObjectDamageTransactions{
        state.contentState(),
        state.worldState(),
        state.presentationState(),
        domain().lifecyclePort()};
}

GameSessionObjectLifecycleTransactions
GameSession::objectLifecycleTransactions() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionObjectLifecycleTransactions{
        state.contentState(),
        state.worldState(),
        state.presentationState(),
        domain().lifecyclePort(),
        &state.objectEventState()};
}

GameSessionMultiplayerVictoryTransactions
GameSession::multiplayerVictoryTransactions() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionMultiplayerVictoryTransactions{
        state.contentState(),
        state.worldState(),
        state.presentationState(),
        domain().lifecyclePort()};
}

GameSessionDynamicGeometryEventPublisher
GameSession::dynamicGeometryEventPublisher() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionDynamicGeometryEventPublisher{
        state.worldState(),
        gameplayPublicationPort()};
}

GameSessionEvaEventPublisher GameSession::evaEventPublisher() noexcept {
    return GameSessionEvaEventPublisher{
        domainState().contentState(), gameplayPublicationPort()};
}

GameSessionObjectEventPublisher GameSession::objectEventPublisher() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionObjectEventPublisher{
        state.contentState(),
        state.worldState(),
        state.presentationState(),
        gameplayPublicationPort()};
}

} // namespace engine
