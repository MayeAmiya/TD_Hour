#pragma once

#include "game/session/frame/GameSessionFramePort.h"
#include "game/session/command/GameSessionConfirmedCommandPort.h"
#include "game/session/presentation/GameSessionPresentationPort.h"
#include "game/session/presentation/GameSessionScriptUiPort.h"
#include "game/session/state/GameSessionStartDependencies.h"
#include "game/session/presentation/LocalPlacementPresentationPort.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/session/integration/GameSessionRenderExtractionPort.h"
#include "game/session/integration/GameSessionMediaPresentationPort.h"

#include <cstdint>
#include <memory>

namespace game::terrain {
class TerrainLogic;
}

namespace engine {

namespace render {
struct RenderAnimationCompletionFeedback;
}

class GameRenderExtraction;
class GameSession;
class GameSessionStateRoot;
struct GameRenderExtractionCache;
class ClientTerrainObjectStore;
struct ClientTerrainObjectPersistentState;
struct ResolvedMatchSetup;
class GameSessionAIQueryPort;
class GameSessionCommandQueryPort;
class GameSessionEconomyQueryPort;
class GameSessionDynamicGeometryEventPublisher;
class GameSessionEvaEventPublisher;
class GameSessionObjectEventPublisher;
class GameSessionMapImportPort;
class GameSessionObjectDamageTransactions;
class GameSessionObjectLifecycleTransactions;
class GameSessionMultiplayerVictoryTransactions;
class GameSessionObjectProductionTransactions;
class GameSessionObjectQueryPort;
class GameSessionObjectSaleTransactions;
class GameSessionRulesetQueryPort;
class GameSessionRuntimeQueryPort;
class GameSessionScenarioBootstrapService;
class GameSessionScriptScenarioPlanTransactions;
class GameSessionScriptOrderAdmissionTransactions;
class GameSessionStrategicAIService;
class GameSessionWorldMaintenanceService;
class MapObjectImport;
class GameSessionClientTerrainObjectSaveGame;
class GameSessionPresentationSaveGame;
class GameSessionVertexWaterSaveGame;
struct MatchResultSnapshot;
namespace selection {
class LocalSelectionCommandBarPresentationConsumer;
class LocalSelectionQueryPort;
class WorldCommandQueryPort;
}
namespace session_query {
class InGameCommandQuerySource;
class PlayerUiQueryPort;
class SessionPlayerQueryPort;
[[nodiscard]] InGameCommandQuerySource inGameCommandQuerySource(
    const GameSession& session) noexcept;
}
namespace detail {
class GameSessionDomainComposition;
struct GameSessionStorage;
}

// Ownership root and fixed-frame coordinator. Domain behavior and disposable
// presentation memoization live in opaque storage and are exposed only through
// explicit domain ports.
class GameSession final {
public:
    GameSession();
    ~GameSession();
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;
    GameSession(GameSession&&) = delete;
    GameSession& operator=(GameSession&&) = delete;

    [[nodiscard]] bool start(const GameStartInfo& info,
                             GameSessionStartDependencies dependencies);
    void shutdown();
    // Reissues only the local presentation domain for a frozen Result
    // session retained across a failed Next/Retry candidate. Authoritative
    // simulation, players, objects, scripts and mission outcome are kept.
    [[nodiscard]] uint64_t rebindResultPresentationEpoch() noexcept;

    void updatePreCommandSystems(float deltaSeconds);
    void updatePostCommandSystems(float deltaSeconds);

    [[nodiscard]] bool isActive() const noexcept;
    [[nodiscard]] uint64_t confirmedTick() const noexcept;
    [[nodiscard]] GameSessionFramePort framePort() noexcept;
    [[nodiscard]] GameSessionPresentationPort presentationPort() noexcept;
    [[nodiscard]] LocalPlacementPresentationPort localPlacementPort() noexcept;
    [[nodiscard]] GameSessionConfirmedCommandPort
    confirmedCommandPort() noexcept;
    [[nodiscard]] GameSessionRenderExtractionPort
    renderExtractionPort() noexcept;
    [[nodiscard]] GameSessionMediaPresentationPort
    mediaPresentationPort() noexcept;
    [[nodiscard]] GameSessionScriptUiPort scriptUiPort() noexcept;
    [[nodiscard]] GameSessionObjectQueryPort objectQuery() const noexcept;
    [[nodiscard]] GameSessionEconomyQueryPort economyQuery() const noexcept;
    [[nodiscard]] GameSessionRulesetQueryPort rulesetQuery() const noexcept;
    [[nodiscard]] GameSessionCommandQueryPort commandQuery() const noexcept;
    [[nodiscard]] GameSessionRuntimeQueryPort runtimeQuery() const noexcept;
    [[nodiscard]] session_query::SessionPlayerQueryPort playerQuery()
        const noexcept;
    [[nodiscard]] selection::WorldCommandQueryPort worldCommandQuery()
        const noexcept;
    [[nodiscard]] selection::LocalSelectionQueryPort localSelectionQuery()
        const noexcept;
    [[nodiscard]] GameSessionAIQueryPort aiQuery() const noexcept;
    [[nodiscard]] session_query::PlayerUiQueryPort playerUiQuery()
        const noexcept;
    [[nodiscard]] uint32_t logicFramesPerSecond() const noexcept;

private:
    friend class GameRenderExtraction;
    friend class MapObjectImport;
    friend class GameSessionClientTerrainObjectSaveGame;
    friend class GameSessionPresentationSaveGame;
    friend class GameSessionVertexWaterSaveGame;
    friend struct MatchResultSnapshot;
    friend class selection::LocalSelectionCommandBarPresentationConsumer;
    friend class GameSessionConfirmedCommandPort;
    friend class GameSessionCommandQueryPort;
    friend session_query::InGameCommandQuerySource
        session_query::inGameCommandQuerySource(
            const GameSession& session) noexcept;
    friend class GameSessionRenderExtractionPort;
    friend class detail::GameSessionDomainComposition;

    [[nodiscard]] GameSessionStateRoot& domainState() noexcept;
    [[nodiscard]] const GameSessionStateRoot& domainState() const noexcept;
    [[nodiscard]] detail::GameSessionDomainComposition& domain() noexcept;
    [[nodiscard]] const detail::GameSessionDomainComposition& domain()
        const noexcept;
    [[nodiscard]] GameRenderExtractionCache& renderExtractionCache()
        const noexcept;
    [[nodiscard]] GameSessionGameplayPublicationPort
    gameplayPublicationPort() noexcept;
    [[nodiscard]] const ResolvedMatchSetup* resolvedMatchSetup() const
        noexcept;
    [[nodiscard]] session_query::InGameCommandQuerySource
    makeInGameCommandQuerySource() const noexcept;
    [[nodiscard]] GameSessionScenarioBootstrapService
    scenarioBootstrapService() noexcept;
    [[nodiscard]] GameSessionStrategicAIService
    strategicAIService() noexcept;
    [[nodiscard]] GameSessionScriptScenarioPlanTransactions
    scenarioPlanTransactions() noexcept;
    [[nodiscard]] GameSessionObjectProductionTransactions
    objectProductionTransactions() noexcept;
    [[nodiscard]] GameSessionScriptOrderAdmissionTransactions
    scriptOrderAdmissionTransactions() noexcept;
    [[nodiscard]] GameSessionWorldMaintenanceService
    worldMaintenanceService() noexcept;
    [[nodiscard]] GameSessionObjectSaleTransactions
    objectSaleTransactions() noexcept;
    [[nodiscard]] GameSessionObjectDamageTransactions
    objectDamageTransactions() noexcept;
    [[nodiscard]] GameSessionObjectLifecycleTransactions
    objectLifecycleTransactions() noexcept;
    [[nodiscard]] GameSessionMultiplayerVictoryTransactions
    multiplayerVictoryTransactions() noexcept;
    [[nodiscard]] GameSessionDynamicGeometryEventPublisher
    dynamicGeometryEventPublisher() noexcept;
    [[nodiscard]] GameSessionEvaEventPublisher
    evaEventPublisher() noexcept;
    [[nodiscard]] GameSessionObjectEventPublisher
    objectEventPublisher() noexcept;
    [[nodiscard]] GameSessionMapImportPort mapImportPort() noexcept;
    [[nodiscard]] game::terrain::TerrainLogic&
    mutableTerrainForPersistence() noexcept;
    [[nodiscard]] const game::terrain::TerrainLogic&
    terrainForPersistence() const noexcept;
    [[nodiscard]] bool persistenceAllowed() const noexcept;
    [[nodiscard]] ClientTerrainObjectStore& clientTerrainObjects()
        noexcept;
    [[nodiscard]] const ClientTerrainObjectStore& clientTerrainObjects()
        const noexcept;
    [[nodiscard]] ClientTerrainObjectPersistentState
    captureClientTerrainObjectPersistentState() const;
    [[nodiscard]] bool restoreClientTerrainObjectPersistentState(
        const ClientTerrainObjectPersistentState& state) noexcept;
    static constexpr int32_t kDefaultScriptRankLevelLimit = 1000;

    [[nodiscard]] ObjectId objectIdFromEntity(ecs::entity entity) const;

    void updatePostCommandCombatAndSimulation();
    void updatePostCommandSimulationEvents();
    void updatePostCommandFinalize(float deltaSeconds);

    std::unique_ptr<detail::GameSessionStorage> m_storage;

};

} // namespace engine
