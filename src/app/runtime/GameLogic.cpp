#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "app/runtime/GameLogic.h"

#include "game/base/GameContinuationResolver.h"
#include "game/session/query/BeaconTextCommandComposer.h"
#include "game/session/query/WorldCommandComposer.h"
#include "game/base/VisualSpeedPolicy.h"
#include "game/base/ReplayStorage.h"
#include "debug/debug.h"
#include "debug/td_assert.h"
#include "core/platform/runtime_threads.h"
#include "game/content/loader/GameDataRegistry.h"
#include "game/ini/GameDataLoader.h"
#include "game/session/query/LocalSelectionPolicy.h"
#include "game/session/query/SessionPlayerQuery.h"
#include "game/session/query/SessionRuntimeQuery.h"
#include "game/session/query/GameSessionCommandQueryPort.h"
#include "game/session/ai/GameSessionAIQueryPort.h"
#include "game/session/core/GameSession.h"
#include "game/session/presentation/LocalPlacementPresentationPort.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

void assertLogicThreadOwnership() noexcept {
    const platform::runtime::ThreadRole role =
        platform::runtime::currentThreadRole();
    TD_ASSERT_MSG(
        role == platform::runtime::ThreadRole::Logic ||
            role == platform::runtime::ThreadRole::Unknown,
        "GameLogic deterministic state accessed outside the logic thread");
}

void advanceNonzeroRevision(uint64_t& revision) noexcept {
    ++revision;
    if (revision == 0u) ++revision;
}

} // namespace

[[nodiscard]] static constexpr const char* activationRejectionName(
    ConfirmedCommandActivationRejection rejection) noexcept {
    switch (rejection) {
    case ConfirmedCommandActivationRejection::None: return "None";
    case ConfirmedCommandActivationRejection::MalformedContext:
        return "MalformedContext";
    case ConfirmedCommandActivationRejection::DescriptorChanged:
        return "DescriptorChanged";
    case ConfirmedCommandActivationRejection::ActorUnavailable:
        return "ActorUnavailable";
    case ConfirmedCommandActivationRejection::AvailabilityChanged:
        return "AvailabilityChanged";
    case ConfirmedCommandActivationRejection::SingleUseConsumed:
        return "SingleUseConsumed";
    case ConfirmedCommandActivationRejection::ScienceUnavailable:
        return "ScienceUnavailable";
    }
    return "Unknown";
}

[[nodiscard]] static constexpr CommandOutcomeReason
activationOutcomeReason(ConfirmedCommandActivationRejection rejection) noexcept {
    switch (rejection) {
    case ConfirmedCommandActivationRejection::MalformedContext:
        return CommandOutcomeReason::DispatcherMalformedPayload;
    case ConfirmedCommandActivationRejection::DescriptorChanged:
        return CommandOutcomeReason::DescriptorChanged;
    case ConfirmedCommandActivationRejection::ActorUnavailable:
        return CommandOutcomeReason::SourceBecameUnavailable;
    case ConfirmedCommandActivationRejection::AvailabilityChanged:
        return CommandOutcomeReason::AvailabilityChanged;
    case ConfirmedCommandActivationRejection::SingleUseConsumed:
        return CommandOutcomeReason::SingleUseConsumed;
    case ConfirmedCommandActivationRejection::ScienceUnavailable:
        return CommandOutcomeReason::ScienceUnavailable;
    case ConfirmedCommandActivationRejection::None:
        return CommandOutcomeReason::DispatcherRejected;
    }
    return CommandOutcomeReason::DispatcherRejected;
}

struct GameLogic::PendingGameStart final {
    GameStartInfo startInfo;
    container::Vector<GameCommand> replayCommands;
    std::optional<ResolvedMatchSetup> replayMatchSetup;
    container::UniquePtr<GameSession> session;
};

[[nodiscard]] static uint32_t mixSimulationSyncChecksum(
    uint32_t commandChecksum,
    const ObjectAISimulationDigest& simulation) noexcept {
    constexpr uint32_t fnvPrime = 16777619u;
    uint32_t result = commandChecksum;
    const auto byte = [&result, fnvPrime](uint8_t value) noexcept {
        result ^= value;
        result *= fnvPrime;
    };
    const auto u64 = [&byte](uint64_t value) noexcept {
        for (uint32_t shift = 0; shift < 64; shift += 8)
            byte(static_cast<uint8_t>((value >> shift) & 0xffull));
    };
    // Domain/version keeps an old input-only sample from comparing equal to
    // a production world-state sample by coincidence.
    u64(0x4149574f524c4431ull); // "AIWORLD1"
    u64(simulation.aiRuntime);
    u64(simulation.navigation);
    u64(simulation.movement);
    u64(simulation.economy);
    u64(simulation.combined);
    return result;
}

[[nodiscard]] static uint32_t millisecondsToConfirmedTicks(
    uint32_t milliseconds, uint32_t ticksPerSecond) noexcept {
    if (milliseconds == 0 || ticksPerSecond == 0) return 0;
    const uint64_t numerator = static_cast<uint64_t>(milliseconds) *
        static_cast<uint64_t>(ticksPerSecond) + 999u;
    return static_cast<uint32_t>(std::min<uint64_t>(
        numerator / 1000u, std::numeric_limits<uint32_t>::max()));
}

[[nodiscard]] GameSessionStartDependencies makeSessionStartDependencies(
    const GameStartInfo& startInfo,
    std::optional<ResolvedMatchSetup> resolvedMatchSetup = std::nullopt) {
    GameDataRegistry startupData;
    const bool replayModeOverlay = resolvedMatchSetup.has_value();
    const uint32_t confirmedTicksPerSecond =
        static_cast<uint32_t>(std::max(1, startInfo.gameSpeedFPS));
    const AISimulationRules& aiRules =
        ::game::GameDataLoader::instance().aiSimulationRules();
    return {
        .ruleset = startupData.rulesetSnapshot(),
        .scienceCatalog = startupData.scienceCatalogSnapshot(),
        .upgradeCatalog = startupData.upgradeCatalogSnapshot(),
        .simulationContentFingerprint =
            startupData.simulationContentFingerprint(),
        .resolvedMatchSetup = std::move(resolvedMatchSetup),
        .allowReplayModeOverlay = replayModeOverlay,
        .objectSimulationRules = {
            .unitDamagedThresholdFixed =
                ::game::GameDataLoader::instance().unitDamagedThreshold(),
            .unitReallyDamagedThresholdFixed =
                ::game::GameDataLoader::instance()
                    .unitReallyDamagedThreshold(),
            .logicFramesPerSecond = confirmedTicksPerSecond,
            .maxTunnelCapacity =
                ::game::GameDataLoader::instance().maxTunnelCapacity(),
            .standardMinefieldDistance =
                ::game::GameDataLoader::instance().standardMinefieldDistance(),
            .standardMinefieldDensity =
                ::game::GameDataLoader::instance().standardMinefieldDensity(),
            .groupMoveClickToGatherFactor =
                ::game::GameDataLoader::instance()
                    .groupMoveClickToGatherFactor(),
            .specialPowerViewObject =
                ::game::GameDataLoader::instance()
                    .specialPowerViewObject(),
            .ai = aiRules,
            .baseRegeneration =
                ::game::GameDataLoader::instance().baseRegenerationRules(),
            .buildPlacement =
                ::game::GameDataLoader::instance()
                    .buildPlacementSimulationRules(),
            .energy = ::game::GameDataLoader::instance()
                          .energySimulationRules(),
            .economy = ::game::GameDataLoader::instance()
                           .economySimulationRules(),
            .difficulty = ::game::GameDataLoader::instance()
                              .difficultySimulationRules(),
            .veterancy = ::game::GameDataLoader::instance()
                             .veterancySimulationRules(),
            .physics =
                ::game::GameDataLoader::instance().physicsSimulationRules(),
        },
        .objectAIRuntimeConfig = {
            .maximumActors = 65536,
            .slotsPerBatch = 256,
            .membershipEventCapacity = 262144,
            .transientValueCapacity = 262144,
            .guardEnemyScanIntervalTicks = millisecondsToConfirmedTicks(
                aiRules.guardEnemyScanMilliseconds,
                confirmedTicksPerSecond),
            .guardReturnScanIntervalTicks = millisecondsToConfirmedTicks(
                aiRules.guardEnemyReturnScanMilliseconds,
                confirmedTicksPerSecond),
            .guardChaseDurationTicks = millisecondsToConfirmedTicks(
                aiRules.guardChaseDurationMilliseconds,
                confirmedTicksPerSecond),
            .idleTargetScanIntervalTicks = millisecondsToConfirmedTicks(
                aiRules.forceIdleMilliseconds,
                confirmedTicksPerSecond),
        },
    };
}

GameLogic& GameLogic::instance() {
    static GameLogic s_instance;
    return s_instance;
}

GameLogic::~GameLogic() = default;

bool GameLogic::appendLocalConstructionRouteNode(
    selection::LocalPlacementPreviewSnapshot node)
{
    assertLogicThreadOwnership();
    if (m_state != GameState::Running || !m_session ||
        !node.sourceObject || !node.hasPose ||
        !node.fixedPosition.valid || node.objectType.empty() ||
        node.backend != selection::LocalPlacementBackendKind::Build) {
        return false;
    }
    node.previewIdentity =
        selection::localConstructionRoutePreviewIdentity(
            m_nextLocalConstructionRoutePreviewIdentity++);
    if (m_nextLocalConstructionRoutePreviewIdentity == 0)
        ++m_nextLocalConstructionRoutePreviewIdentity;
    if (node.previewIdentity == 0) {
        node.previewIdentity =
            selection::localConstructionRoutePreviewIdentity(
                m_nextLocalConstructionRoutePreviewIdentity++);
        if (m_nextLocalConstructionRoutePreviewIdentity == 0)
            ++m_nextLocalConstructionRoutePreviewIdentity;
    }
    node.feedback = selection::LocalPlacementPreviewFeedback::Queued;
    // Placement animation starts when the cursor opens, while the short
    // command hint starts when the player confirms this route node.
    node.animationStartTick = m_currentTick;
    node.sourceSequence = 0;
    node.routeAnchorOnly = false;
    node.routeLocalOnly = true;
    node.activation = {};

    const auto found = std::find_if(
        m_localConstructionRoutes.begin(), m_localConstructionRoutes.end(),
        [&](const LocalConstructionRoute& route) noexcept {
            return route.builder == node.sourceObject;
        });
    if (found != m_localConstructionRoutes.end()) {
        found->nodes.push_back({.placement = std::move(node)});
    } else {
        LocalConstructionRoute route;
        route.builder = node.sourceObject;
        route.nodes.push_back({.placement = std::move(node)});
        m_localConstructionRoutes.push_back(std::move(route));
    }
    publishLocalConstructionRoutePreviews();
    return true;
}

size_t GameLogic::localConstructionRouteCount(ObjectId builder) const noexcept
{
    const auto found = std::find_if(
        m_localConstructionRoutes.begin(), m_localConstructionRoutes.end(),
        [builder](const LocalConstructionRoute& route) noexcept {
            return route.builder == builder;
        });
    return found == m_localConstructionRoutes.end()
        ? 0u : found->nodes.size();
}

container::Vector<GameLogic::LocalConstructionRouteNodeProjection>
GameLogic::localConstructionRouteNodes() const
{
    container::Vector<LocalConstructionRouteNodeProjection> result;
    for (const LocalConstructionRoute& route : m_localConstructionRoutes) {
        for (const LocalConstructionRouteNode& node : route.nodes) {
            // A submitted Build is now owned by the confirmed dispatcher;
            // Building is represented by the actual construction site. Only
            // local, still-cancellable ghosts are exposed to SDL input.
            if (node.phase != LocalConstructionRoutePhase::AwaitingMove &&
                node.phase != LocalConstructionRoutePhase::Traveling) {
                continue;
            }
            const selection::LocalPlacementPreviewSnapshot& placement =
                node.placement;
            LocalConstructionRouteNodeProjection value{
                .previewIdentity = placement.previewIdentity,
                .builder = route.builder,
                .objectType = placement.objectType,
                .position = placement.position,
                .selectionRadius = placement.selectionRadius,
            };
            if (value.valid()) result.push_back(std::move(value));
        }
    }
    return result;
}

std::optional<GameLogic::LocalConstructionRouteNodeProjection>
GameLogic::selectedLocalConstructionRouteNode() const
{
    if (m_selectedLocalConstructionRoutePreviewIdentity == 0) {
        return std::nullopt;
    }
    for (const LocalConstructionRoute& route : m_localConstructionRoutes) {
        for (const LocalConstructionRouteNode& node : route.nodes) {
            if (node.placement.previewIdentity !=
                    m_selectedLocalConstructionRoutePreviewIdentity ||
                (node.phase != LocalConstructionRoutePhase::AwaitingMove &&
                 node.phase != LocalConstructionRoutePhase::Traveling)) {
                continue;
            }
            LocalConstructionRouteNodeProjection value{
                .previewIdentity = node.placement.previewIdentity,
                .builder = route.builder,
                .objectType = node.placement.objectType,
                .position = node.placement.position,
                .selectionRadius = node.placement.selectionRadius,
            };
            if (value.valid()) return value;
        }
    }
    return std::nullopt;
}

bool GameLogic::selectLocalConstructionRouteNode(
    uint64_t previewIdentity) noexcept
{
    assertLogicThreadOwnership();
    if (previewIdentity == 0) return false;
    for (const LocalConstructionRoute& route : m_localConstructionRoutes) {
        for (const LocalConstructionRouteNode& node : route.nodes) {
            if (node.placement.previewIdentity != previewIdentity ||
                (node.phase != LocalConstructionRoutePhase::AwaitingMove &&
                 node.phase != LocalConstructionRoutePhase::Traveling)) {
                continue;
            }
            m_selectedLocalConstructionRoutePreviewIdentity = previewIdentity;
            return true;
        }
    }
    return false;
}

bool GameLogic::cancelSelectedLocalConstructionRouteNode()
{
    assertLogicThreadOwnership();
    const uint64_t selected = m_selectedLocalConstructionRoutePreviewIdentity;
    if (selected == 0) return false;

    for (auto route = m_localConstructionRoutes.begin();
         route != m_localConstructionRoutes.end(); ++route) {
        const auto node = std::find_if(
            route->nodes.begin(), route->nodes.end(),
            [selected](const LocalConstructionRouteNode& value) noexcept {
                return value.placement.previewIdentity == selected;
            });
        if (node == route->nodes.end() ||
            (node->phase != LocalConstructionRoutePhase::AwaitingMove &&
             node->phase != LocalConstructionRoutePhase::Traveling)) {
            continue;
        }

        const bool wasHead = node == route->nodes.begin();
        const ObjectId builder = route->builder;
        route->nodes.erase(node);
        m_selectedLocalConstructionRoutePreviewIdentity = 0;

        // If no subsequent local node remains, stop the already submitted
        // local Move rather than letting a cancelled ghost pull the dozer to
        // its old location. With a successor, the next local tick publishes
        // a non-queued replacement Move to that successor.
        if (wasHead && route->nodes.empty() && builder) {
            GameCommand stop;
            stop.tick = m_currentTick;
            stop.type = GameCommandType::Stop;
            stop.actors.push_back(builder);
            static_cast<void>(submitCommand(std::move(stop)));
        }
        if (route->nodes.empty() && route->rejected.empty()) {
            m_localConstructionRoutes.erase(route);
        }
        publishLocalConstructionRoutePreviews();
        return true;
    }
    m_selectedLocalConstructionRoutePreviewIdentity = 0;
    return false;
}

size_t GameLogic::cancelLocalConstructionRoutes(
    container::Span<const ObjectId> builders)
{
    assertLogicThreadOwnership();
    if (builders.empty() || m_localConstructionRoutes.empty()) return 0;
    size_t removed = 0;
    std::erase_if(
        m_localConstructionRoutes,
        [&](const LocalConstructionRoute& route) {
            if (std::find(builders.begin(), builders.end(), route.builder) ==
                builders.end()) {
                return false;
            }
            ++removed;
            const bool selectedBelongsToRoute =
                m_selectedLocalConstructionRoutePreviewIdentity != 0 &&
                std::any_of(
                    route.nodes.begin(), route.nodes.end(),
                    [&](const LocalConstructionRouteNode& node) noexcept {
                        return node.placement.previewIdentity ==
                            m_selectedLocalConstructionRoutePreviewIdentity;
                    });
            if (selectedBelongsToRoute) {
                m_selectedLocalConstructionRoutePreviewIdentity = 0;
            }
            return true;
        });
    if (removed != 0) publishLocalConstructionRoutePreviews();
    return removed;
}

void GameLogic::publishLocalConstructionRoutePreviews()
{
    if (!m_session) return;
    container::Vector<selection::LocalPlacementPreviewSnapshot> previews;
    for (LocalConstructionRoute& route : m_localConstructionRoutes) {
        for (const LocalConstructionRouteNode& node : route.nodes) {
            selection::LocalPlacementPreviewSnapshot preview = node.placement;
            preview.feedback = selection::LocalPlacementPreviewFeedback::Queued;
            preview.routeLocalOnly = true;
            // A live construction site owns its own model/bib. Retain only
            // this route anchor so extraction still connects it to the next
            // local node without drawing a duplicate ghost over the site.
            preview.routeAnchorOnly =
                node.phase == LocalConstructionRoutePhase::Building;
            previews.push_back(std::move(preview));
        }
        for (const LocalConstructionRejectedNode& rejected : route.rejected) {
            if (rejected.expiresAfterTick <= m_currentTick) continue;
            selection::LocalPlacementPreviewSnapshot preview =
                rejected.placement;
            preview.feedback = selection::LocalPlacementPreviewFeedback::Rejected;
            preview.legality = selection::LocalPlacementLegality::Illegal;
            preview.routeAnchorOnly = false;
            preview.routeLocalOnly = true;
            previews.push_back(std::move(preview));
        }
    }
    m_session->localPlacementPort().setLocalConstructionRoutePreviews(
        previews);
}

void GameLogic::advanceLocalConstructionRoutes()
{
    if (m_state != GameState::Running || !m_session) return;

    constexpr GameTick RejectedPreviewLifetimeTicks = 30;
    const auto rejectCurrent = [&](LocalConstructionRoute& route) {
        if (route.nodes.empty()) return;
        if (route.nodes.front().placement.previewIdentity ==
            m_selectedLocalConstructionRoutePreviewIdentity) {
            m_selectedLocalConstructionRoutePreviewIdentity = 0;
        }
        selection::LocalPlacementPreviewSnapshot rejected =
            route.nodes.front().placement;
        rejected.feedback = selection::LocalPlacementPreviewFeedback::Rejected;
        rejected.legality = selection::LocalPlacementLegality::Illegal;
        rejected.routeAnchorOnly = false;
        rejected.routeLocalOnly = true;
        const GameTick expiresAfter = m_currentTick >
                std::numeric_limits<GameTick>::max() -
                    RejectedPreviewLifetimeTicks
            ? std::numeric_limits<GameTick>::max()
            : m_currentTick + RejectedPreviewLifetimeTicks;
        route.rejected.push_back({
            .placement = std::move(rejected),
            .expiresAfterTick = expiresAfter,
        });
        route.nodes.erase(route.nodes.begin());
    };

    for (auto route = m_localConstructionRoutes.begin();
         route != m_localConstructionRoutes.end();) {
        std::erase_if(
            route->rejected,
            [&](const LocalConstructionRejectedNode& rejected) noexcept {
                return rejected.expiresAfterTick <= m_currentTick;
            });
        if (!route->nodes.empty()) {
            LocalConstructionRouteNode& node = route->nodes.front();
            LocalPlacementPresentationPort placement =
                m_session->localPlacementPort();
            switch (node.phase) {
            case LocalConstructionRoutePhase::AwaitingMove: {
                const std::optional<CommandPosition> approach =
                    placement.localConstructionRouteApproachTarget(
                        route->builder, node.placement);
                if (!approach) {
                    rejectCurrent(*route);
                    break;
                }
                GameCommand move;
                move.tick = m_currentTick;
                move.type = GameCommandType::Move;
                move.actors.push_back(route->builder);
                move.targetPosition = *approach;
                if (submitCommand(std::move(move)).admitted) {
                    node.phase = LocalConstructionRoutePhase::Traveling;
                }
                break;
            }
            case LocalConstructionRoutePhase::Traveling: {
                if (!placement.builderReachedLocalConstructionRouteNode(
                        route->builder, node.placement)) {
                    break;
                }
                std::optional<GameCommand> build =
                    placement.composeLocalConstructionRouteBuildCommand(
                        node.placement);
                if (!build) {
                    rejectCurrent(*route);
                    break;
                }
                const GameCommandSubmissionResult submitted =
                    submitCommand(std::move(*build));
                if (!submitted.admitted) {
                    rejectCurrent(*route);
                    break;
                }
                if (submitted.commandSequence == 0) {
                    // A route node has no other identity which can safely
                    // correlate its local state with the confirmed command.
                    // Treat a malformed admission as a local rejection
                    // instead of leaving an unresolvable node pending.
                    rejectCurrent(*route);
                    break;
                }
                node.phase = LocalConstructionRoutePhase::BuildSubmitted;
                if (node.placement.previewIdentity ==
                    m_selectedLocalConstructionRoutePreviewIdentity) {
                    m_selectedLocalConstructionRoutePreviewIdentity = 0;
                }
                node.buildCommandSequence = submitted.commandSequence;
                node.buildDispatchTick = 0;
                node.buildDispatch =
                    LocalConstructionBuildDispatch::Pending;
                break;
            }
            case LocalConstructionRoutePhase::BuildSubmitted:
                if (placement.builderHasPendingConstruction(route->builder)) {
                    node.phase = LocalConstructionRoutePhase::Building;
                } else if (node.buildDispatch ==
                    LocalConstructionBuildDispatch::Rejected) {
                    // The actual confirmed dispatcher rejected this exact
                    // Build.  A deferred FREEZE_TIME command remains Pending
                    // until it reaches this edge after unfreeze.
                    rejectCurrent(*route);
                } else if (node.buildDispatch ==
                           LocalConstructionBuildDispatch::Accepted &&
                           m_currentTick > node.buildDispatchTick) {
                    // An accepted build normally creates the task in the
                    // same confirmed world turn.  Keep the old one-tick
                    // diagnostic only after actual dispatch, never after
                    // initial ingress/admission.
                    rejectCurrent(*route);
                }
                break;
            case LocalConstructionRoutePhase::Building:
                if (!placement.builderHasPendingConstruction(route->builder)) {
                    if (node.placement.previewIdentity ==
                        m_selectedLocalConstructionRoutePreviewIdentity) {
                        m_selectedLocalConstructionRoutePreviewIdentity = 0;
                    }
                    route->nodes.erase(route->nodes.begin());
                }
                break;
            }
        }
        if (route->nodes.empty() && route->rejected.empty()) {
            route = m_localConstructionRoutes.erase(route);
        } else {
            ++route;
        }
    }
    publishLocalConstructionRoutePreviews();
}

void GameLogic::recordLocalConstructionRouteBuildDispatch(
    uint32_t commandSequence, bool accepted, GameTick confirmedTick) {
    assertLogicThreadOwnership();
    if (commandSequence == 0) return;
    const LocalConstructionBuildDispatch dispatch = accepted
        ? LocalConstructionBuildDispatch::Accepted
        : LocalConstructionBuildDispatch::Rejected;
    for (LocalConstructionRoute& route : m_localConstructionRoutes) {
        if (route.nodes.empty()) continue;
        LocalConstructionRouteNode& node = route.nodes.front();
        if (node.phase != LocalConstructionRoutePhase::BuildSubmitted ||
            node.buildCommandSequence != commandSequence) {
            continue;
        }
        node.buildDispatch = dispatch;
        node.buildDispatchTick = confirmedTick;
    }
}

bool GameLogic::startNewGame(const GameStartInfo& info) {
    assertLogicThreadOwnership();
    if (m_state != GameState::Idle) {
        TD_LOG_WARN("[GameLogic] Cannot start new game: state={}", static_cast<int>(m_state));
        return false;
    }
    if (info.mode == GameMode::Invalid ||
        (info.mapName.empty() && info.replayFileName.empty())) {
        TD_LOG_ERROR("[GameLogic] Rejected game start without a playable mode/map or replay");
        return false;
    }

    m_pendingGameStart = std::make_unique<PendingGameStart>();
    m_pendingGameStart->startInfo = info;
    m_currentGame = info;
    m_currentTick = FirstConfirmedGameTick;
    m_localFrameAuthority.reset();
    m_commandRecorder.clear();
    m_commandPlayback.clear();
    m_commandSyncProbe.reset();
    m_localSelection.reset();
    m_localConstructionRoutes.clear();
    m_nextLocalConstructionRoutePreviewIdentity = 1;
    m_selectedLocalConstructionRoutePreviewIdentity = 0;
    m_deferredFrozenCommands.clear();
    m_commandOutcomes = {};
    m_pendingBackendCommandOutcomes.clear();
    m_nextCommandOutcomeRevision = 1;
    m_localPauseSources = 0;
    m_disconnectActionStatus = {};
    m_lockstepFrameBuffer.reset();
    m_enetTransport.shutdown();
    m_matchResult.reset();
    m_pendingResultAction.reset();
    m_resultTransitionError.clear();
    m_exitRequested = false;
    m_loadingStage = GameLoadingStage::AwaitingPresentation;
    m_loadingProgress = 0.0f;
    advanceNonzeroRevision(m_loadingRevision);
    m_loadingStatus = "Preparing loading screen";
    m_loadingError.clear();
    m_loadingScreenPresented = false;
    m_loadingScreenDismissed = false;
    m_renderStartupFrameSubmitted = false;
    m_continuationLoading = ContinuationLoadingKind::None;
    m_startupSceneProgress = {};
    m_loadingLastProgress.clear();
    m_loadingStartupDeadline = {};
    m_loadingStartupHeartbeatAt = {};
    m_loadingStartupLastProgressAt = {};
    m_lastFrameCommitResult = {};
    m_state = GameState::Loading;

    TD_LOG_INFO("[GameLogic] Accepted game start: mode={} map={} difficulty={} cash={}",
                static_cast<int>(info.mode), info.mapName, info.difficulty, info.startingMoney);
    TD_LOG_INFO("[GameLogic] Local player: template='{}' side='{}' baseSide='{}'",
                info.localPlayerTemplateName, info.localPlayerSide, info.localPlayerBaseSide);
    if (!info.saveFileName.empty() || !info.replayFileName.empty()) {
        TD_LOG_INFO("[GameLogic] Startup files: save='{}' replay='{}'",
                    info.saveFileName, info.replayFileName);
    }

    return true;
}

void GameLogic::notifyLoadingScreenPresented(uint64_t revision) noexcept {
    assertLogicThreadOwnership();
    if (m_state == GameState::Loading && revision == m_loadingRevision) {
        m_loadingScreenPresented = true;
    }
}

void GameLogic::notifyLoadingScreenDismissed(uint64_t revision) noexcept {
    assertLogicThreadOwnership();
    if (m_state == GameState::Loading && revision == m_loadingRevision &&
        m_loadingStage == GameLoadingStage::Ready) {
        m_loadingScreenDismissed = true;
    }
}

void GameLogic::notifyRenderStartupFrameSubmitted(
    uint64_t loadingRevision, uint64_t sessionRevision) noexcept {
    assertLogicThreadOwnership();
    if (m_state == GameState::Loading &&
        m_loadingStage == GameLoadingStage::Ready &&
        loadingRevision == m_loadingRevision &&
        sessionRevision == m_sessionRevision && m_session) {
        m_renderStartupFrameSubmitted = true;
        m_loadingProgress = 1.0f;
        m_loadingStatus = "Scene ready";
    }
}

void GameLogic::notifyRenderStartupFailure(
    uint64_t loadingRevision, uint64_t sessionRevision,
    container::String error) noexcept {
    assertLogicThreadOwnership();
    if (m_state != GameState::Loading ||
        m_loadingStage != GameLoadingStage::Ready ||
        loadingRevision != m_loadingRevision ||
        sessionRevision != m_sessionRevision || !m_session) {
        return;
    }
    if (error.empty()) error = "renderer failed to prepare the startup scene";
    failLoading(std::move(error));
}

uint64_t GameLogic::loadingDeadlineRemainingMilliseconds() const noexcept {
    if (m_loadingStartupDeadline ==
        std::chrono::steady_clock::time_point{}) {
        return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= m_loadingStartupDeadline) return 0;
    return static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(m_loadingStartupDeadline - now).count());
}

void GameLogic::notifyRenderStartupProgress(
    uint64_t loadingRevision, uint64_t sessionRevision,
    StartupSceneProgress progress) noexcept {
    assertLogicThreadOwnership();
    if (m_state != GameState::Loading ||
        m_loadingStage != GameLoadingStage::Ready ||
        loadingRevision != m_loadingRevision ||
        sessionRevision != m_sessionRevision || !m_session) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    m_loadingStartupHeartbeatAt = now;
    if (progress != m_startupSceneProgress) {
        m_startupSceneProgress = progress;
        m_loadingStartupLastProgressAt = now;
        m_loadingLastProgress =
            "required " + std::to_string(progress.requiredReady) + "/" +
            std::to_string(progress.requiredTotal) + ", optional pending " +
            std::to_string(progress.optionalPending) + ", degraded " +
            std::to_string(progress.optionalDegraded);
        if (progress.requiredTotal != 0u) {
            const float requiredRatio = std::clamp(
                static_cast<float>(progress.requiredReady) /
                    static_cast<float>(progress.requiredTotal),
                0.0f, 1.0f);
            m_loadingProgress = std::max(
                m_loadingProgress, 0.95f + requiredRatio * 0.04f);
        }
    }
    refreshLoadingStartupStatus();
}

void GameLogic::refreshLoadingStartupStatus() {
    if (m_loadingStage != GameLoadingStage::Ready) return;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t remainingSeconds =
        (loadingDeadlineRemainingMilliseconds() + 999u) / 1000u;
    container::String blocking = "waiting for startup scene ticket";
    if (m_startupSceneProgress.requiredFailed != 0u) {
        blocking = "required startup product failed";
    } else if (m_startupSceneProgress.requiredPending != 0u) {
        blocking = "required terrain/bib pending";
    } else if (!m_renderStartupFrameSubmitted) {
        blocking = "waiting for first submitted world frame";
    } else if (!m_loadingScreenDismissed) {
        blocking = "waiting for loading WND fade-out";
    }
    container::String heartbeat = "no renderer heartbeat";
    if (m_loadingStartupHeartbeatAt !=
        std::chrono::steady_clock::time_point{}) {
        const uint64_t age = static_cast<uint64_t>(std::max<int64_t>(
            0, std::chrono::duration_cast<std::chrono::milliseconds>(
                   now - m_loadingStartupHeartbeatAt).count()));
        heartbeat = "heartbeat " + std::to_string(age) + "ms ago";
    }
    m_loadingStatus = blocking + "; " + heartbeat + "; deadline " +
        std::to_string(remainingSeconds) + "s";
    if (!m_loadingLastProgress.empty()) {
        uint64_t progressAge = 0;
        if (m_loadingStartupLastProgressAt !=
            std::chrono::steady_clock::time_point{}) {
            progressAge = static_cast<uint64_t>(std::max<int64_t>(
                0, std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - m_loadingStartupLastProgressAt).count()));
        }
        m_loadingStatus += "; last " + std::to_string(progressAge) +
            "ms ago: " + m_loadingLastProgress;
    }
}

void GameLogic::updateLoading() {
    if (m_state != GameState::Loading || !m_pendingGameStart) return;

    switch (m_loadingStage) {
    case GameLoadingStage::AwaitingPresentation:
        if (!m_loadingScreenPresented) return;
        m_loadingProgress = 0.05f;
        m_loadingStatus = "Resolving launch data";
        m_loadingStage = GameLoadingStage::ResolvingReplay;
        return;

    case GameLoadingStage::ResolvingReplay: {
        GameStartInfo& startInfo = m_pendingGameStart->startInfo;
        if (!startInfo.replayFileName.empty()) {
            const container::String replayFileName = startInfo.replayFileName;
            auto replay = ::game::ReplayStorage::instance().readReplayCommandStream(replayFileName);
            if (!replay.ok) {
                failLoading("failed to load replay '" + replayFileName + "': " + replay.error);
                return;
            }
            startInfo = std::move(replay.startInfo);
            m_pendingGameStart->replayMatchSetup = std::move(replay.resolvedMatchSetup);
            startInfo.mode = GameMode::Replay;
            startInfo.replayFileName = replayFileName;
            startInfo.network.reset();
            m_pendingGameStart->replayCommands = std::move(replay.commands);
            m_currentGame = startInfo;
        }
        m_loadingProgress = 0.15f;
        m_loadingStatus = "Loading game data";
        m_loadingStage = GameLoadingStage::LoadingGameData;
        return;
    }

    case GameLoadingStage::LoadingGameData:
        if (!::game::GameDataLoader::instance().loadAll()) {
            failLoading("failed to load game data");
            return;
        }
        m_loadingProgress = 0.45f;
        m_loadingStatus = "Starting game session";
        m_loadingStage = GameLoadingStage::StartingSession;
        return;

    case GameLoadingStage::StartingSession: {
        GameStartInfo& startInfo = m_pendingGameStart->startInfo;
        GameSessionStartDependencies sessionDependencies =
            makeSessionStartDependencies(
                startInfo,
                std::move(m_pendingGameStart->replayMatchSetup));
        m_pendingGameStart->session = std::make_unique<GameSession>();
        if (!m_pendingGameStart->session->start(startInfo, std::move(sessionDependencies))) {
            failLoading("failed to start game session");
            return;
        }
        startInfo = m_pendingGameStart->session->runtimeQuery().startInfo();
        m_currentGame = startInfo;
        m_loadingProgress = 0.85f;
        m_loadingStatus = "Starting runtime services";
        m_loadingStage = GameLoadingStage::StartingTransport;
        return;
    }

    case GameLoadingStage::StartingTransport: {
        const GameStartInfo& startInfo = m_pendingGameStart->startInfo;
        GameSession& loadingSession = *m_pendingGameStart->session;
        if (!startInfo.replayFileName.empty()) {
            m_commandPlayback.load(std::move(m_pendingGameStart->replayCommands),
                                   CommandSource::Replay);
            TD_LOG_INFO("[GameLogic] Replay playback source loaded: file='{}' commands={}",
                        startInfo.replayFileName, m_commandPlayback.commandCount());
        } else if (startInfo.network.enabled) {
            const engine::session_query::LockstepSessionIdentitySnapshot identity =
                engine::session_query::lockstepIdentity(loadingSession);
            if (!identity.localPlayer || !identity.localPlayer.commandPlayer) {
                failLoading("cannot start command lockstep without a local simulation participant");
                return;
            }
            const LockstepMatchIdentity matchIdentity{
                .simulationContentFingerprint = identity.simulationContentFingerprint,
                .resolvedSetupSimulationDigest = identity.resolvedSetupSimulationDigest,
            };
            m_lockstepFrameBuffer.configure(
                identity.localPlayer.id, startInfo.network.frameSendRate);
            if (!m_enetTransport.start(startInfo, matchIdentity)) {
                failLoading("failed to start in-game network transport: " + m_enetTransport.error());
                return;
            }
        }
        // Publish the fully bootstrapped session while remaining in Loading.
        // Simulation is still stopped by GameLogic::update(), but the
        // presentation extractor can now build the first immutable world
        // snapshot and warm required renderer resources behind the Loading
        // WND instead of discovering the whole scene after Running begins.
        advanceNonzeroRevision(m_sessionRevision);
        m_session = std::move(m_pendingGameStart->session);
        // Keep the WND visible and animated while the renderer prepares the
        // required world.  The final 100% and fade-out are released only by
        // a successfully submitted, generation-matched startup frame.
        m_loadingProgress = 0.95f;
        m_loadingStatus = "Preparing scene";
        m_loadingStage = GameLoadingStage::Ready;
        const auto now = std::chrono::steady_clock::now();
        m_loadingStartupDeadline = now + std::chrono::seconds(120);
        m_loadingStartupHeartbeatAt = {};
        m_loadingStartupLastProgressAt = now;
        m_startupSceneProgress = {};
        m_loadingLastProgress = "startup scene ticket created";
        refreshLoadingStartupStatus();
        return;
    }

    case GameLoadingStage::Ready:
        refreshLoadingStartupStatus();
        if (!m_renderStartupFrameSubmitted &&
            m_loadingStartupDeadline !=
                std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() >=
                m_loadingStartupDeadline) {
            failLoading(
                "renderer startup deadline exceeded; last progress: " +
                (m_loadingLastProgress.empty()
                     ? container::String{"none"}
                     : m_loadingLastProgress));
            return;
        }
        if (!m_loadingScreenDismissed ||
            !m_renderStartupFrameSubmitted) return;
        TD_LOG_INFO("[GameLogic] Loading presentation complete, transitioning to Running");
        m_pendingGameStart.reset();
        if (m_continuationLoading ==
            ContinuationLoadingKind::Candidate) {
            if (m_retainedResultSession) {
                m_retainedResultSession->shutdown();
                m_retainedResultSession.reset();
            }
            m_matchResult.reset();
            advanceNonzeroRevision(m_matchResultRevision);
            m_resultTransitionError.clear();
            m_continuationLoading = ContinuationLoadingKind::None;
            m_loadingStage = GameLoadingStage::None;
            m_loadingStatus.clear();
            m_state = GameState::Running;
            return;
        }
        if (m_continuationLoading ==
            ContinuationLoadingKind::ResultRollback) {
            m_continuationLoading = ContinuationLoadingKind::None;
            m_loadingStage = GameLoadingStage::None;
            m_loadingStatus.clear();
            m_state = GameState::Result;
            return;
        }
        m_loadingStage = GameLoadingStage::None;
        m_loadingStatus.clear();
        m_state = GameState::Running;
        return;

    case GameLoadingStage::None:
        return;
    }
}

void GameLogic::failLoading(container::String error) {
    TD_LOG_ERROR("[GameLogic] Loading failed: {}", error);
    if (m_continuationLoading == ContinuationLoadingKind::Candidate &&
        m_matchResult) {
        abortContinuationLoading(std::move(error));
        return;
    }
    if (m_continuationLoading ==
            ContinuationLoadingKind::ResultRollback &&
        m_matchResult) {
        m_loadingStage = GameLoadingStage::None;
        m_loadingProgress = 0.0f;
        m_loadingStatus.clear();
        m_loadingError.clear();
        m_loadingScreenPresented = false;
        m_loadingScreenDismissed = false;
        m_renderStartupFrameSubmitted = false;
        m_continuationLoading = ContinuationLoadingKind::None;
        m_resultTransitionError =
            "result presentation rollback failed: " + error;
        m_state = GameState::Result;
        return;
    }
    clearGameData();
    m_loadingError = std::move(error);
}

bool GameLogic::startContinuationLoading(
    GameStartInfo startInfo, container::String& error) {
    error.clear();
    m_state = GameState::Transitioning;

    auto candidate = std::make_unique<GameSession>();
    GameSessionStartDependencies dependencies =
        makeSessionStartDependencies(startInfo);
    if (!candidate->start(startInfo, std::move(dependencies))) {
        candidate->shutdown();
        error = "candidate game session rejected map '" +
            startInfo.mapName + "'";
        m_state = GameState::Result;
        return false;
    }

    // Candidate bootstrap succeeded while the old Result session remains a
    // complete rollback source. It is retired only after renderer readiness
    // and the Loading WND handoff both commit.
    m_retainedResultSession = std::move(m_session);
    m_currentGame = candidate->runtimeQuery().startInfo();
    m_currentTick = FirstConfirmedGameTick;
    m_localFrameAuthority.reset();
    m_commandRecorder.clear();
    m_commandPlayback.clear();
    m_commandSyncProbe.reset();
    m_localSelection.reset();
    m_localConstructionRoutes.clear();
    m_nextLocalConstructionRoutePreviewIdentity = 1;
    m_selectedLocalConstructionRoutePreviewIdentity = 0;
    m_deferredFrozenCommands.clear();
    m_commandOutcomes = {};
    m_pendingBackendCommandOutcomes.clear();
    m_nextCommandOutcomeRevision = 1;
    m_localPauseSources = 0;
    m_disconnectActionStatus = {};
    m_lockstepFrameBuffer.reset();
    m_enetTransport.shutdown();
    m_pendingResultAction.reset();
    m_exitRequested = false;
    m_lastFrameCommitResult = {};

    m_pendingGameStart = std::make_unique<PendingGameStart>();
    m_pendingGameStart->startInfo = m_currentGame;
    advanceNonzeroRevision(m_sessionRevision);
    m_session = std::move(candidate);
    advanceNonzeroRevision(m_loadingRevision);
    m_loadingStage = GameLoadingStage::Ready;
    m_loadingProgress = 0.95f;
    m_loadingStatus = "Preparing continuation scene";
    m_loadingError.clear();
    m_loadingScreenPresented = false;
    m_loadingScreenDismissed = false;
    m_renderStartupFrameSubmitted = false;
    m_continuationLoading = ContinuationLoadingKind::Candidate;
    m_startupSceneProgress = {};
    m_loadingLastProgress = "continuation candidate committed";
    const auto now = std::chrono::steady_clock::now();
    m_loadingStartupDeadline = now + std::chrono::seconds(120);
    m_loadingStartupHeartbeatAt = {};
    m_loadingStartupLastProgressAt = now;
    m_state = GameState::Loading;
    refreshLoadingStartupStatus();
    return true;
}

void GameLogic::abortContinuationLoading(container::String error) {
    if (m_session) {
        m_session->shutdown();
        m_session.reset();
    }
    if (m_pendingGameStart && m_pendingGameStart->session) {
        m_pendingGameStart->session->shutdown();
        m_pendingGameStart->session.reset();
    }
    m_session = std::move(m_retainedResultSession);
    const uint64_t rollbackEpoch = m_session
        ? m_session->rebindResultPresentationEpoch() : 0;
    advanceNonzeroRevision(m_sessionRevision);
    m_currentGame = m_matchResult
        ? m_matchResult->startInfo : GameStartInfo{};
    m_currentTick = m_matchResult
        ? static_cast<GameTick>(std::min<uint64_t>(
              m_matchResult->confirmedTick,
              std::numeric_limits<GameTick>::max()))
        : 0;
    m_localFrameAuthority.reset();
    m_commandRecorder.clear();
    m_commandPlayback.clear();
    m_commandSyncProbe.reset();
    m_localSelection.reset();
    m_deferredFrozenCommands.clear();
    m_commandOutcomes = {};
    m_pendingBackendCommandOutcomes.clear();
    m_nextCommandOutcomeRevision = 1;
    m_localPauseSources = 0;
    m_disconnectActionStatus = {};
    m_lockstepFrameBuffer.reset();
    m_enetTransport.shutdown();
    m_pendingResultAction.reset();
    m_pendingGameStart = std::make_unique<PendingGameStart>();
    m_pendingGameStart->startInfo = m_currentGame;
    advanceNonzeroRevision(m_loadingRevision);
    m_loadingStage = GameLoadingStage::Ready;
    m_loadingProgress = 0.95f;
    m_loadingStatus = "Restoring result scene";
    m_loadingError.clear();
    m_loadingScreenPresented = false;
    m_loadingScreenDismissed = false;
    m_renderStartupFrameSubmitted = false;
    m_continuationLoading = ContinuationLoadingKind::ResultRollback;
    m_startupSceneProgress = {};
    m_loadingLastProgress = "retained result session rebound";
    const auto now = std::chrono::steady_clock::now();
    m_loadingStartupDeadline = now + std::chrono::seconds(120);
    m_loadingStartupHeartbeatAt = {};
    m_loadingStartupLastProgressAt = now;
    m_lastFrameCommitResult = {};
    m_resultTransitionError = std::move(error);
    if (rollbackEpoch == 0) {
        m_pendingGameStart.reset();
        m_loadingStage = GameLoadingStage::None;
        m_loadingProgress = 0.0f;
        m_loadingStatus.clear();
        m_continuationLoading = ContinuationLoadingKind::None;
        m_resultTransitionError +=
            "; retained result session could not issue a new presentation epoch";
        m_state = GameState::Result;
        return;
    }
    m_state = GameState::Loading;
    refreshLoadingStartupStatus();
}

void GameLogic::clearGameData() {
    assertLogicThreadOwnership();
    if (m_state == GameState::Idle) return;

    TD_LOG_INFO("[GameLogic] Clearing game data");
    const bool retiringSession = static_cast<bool>(m_session) ||
        static_cast<bool>(m_retainedResultSession) ||
        (m_pendingGameStart &&
         static_cast<bool>(m_pendingGameStart->session));
    if (m_session) {
        m_session->shutdown();
        m_session.reset();
    }
    if (m_pendingGameStart && m_pendingGameStart->session) {
        m_pendingGameStart->session->shutdown();
        m_pendingGameStart->session.reset();
    }
    if (m_retainedResultSession) {
        m_retainedResultSession->shutdown();
        m_retainedResultSession.reset();
    }
    ::game::GameDataLoader::instance().clear();
    m_currentGame.reset();
    m_currentTick = 0;
    m_localFrameAuthority.reset();
    m_commandRecorder.clear();
    m_commandPlayback.clear();
    m_commandSyncProbe.reset();
    m_localSelection.reset();
    m_deferredFrozenCommands.clear();
    m_commandOutcomes = {};
    m_pendingBackendCommandOutcomes.clear();
    m_nextCommandOutcomeRevision = 1;
    m_localPauseSources = 0;
    m_disconnectActionStatus = {};
    m_lockstepFrameBuffer.reset();
    m_enetTransport.shutdown();
    if (m_matchResult) {
        m_matchResult.reset();
        advanceNonzeroRevision(m_matchResultRevision);
    }
    m_pendingResultAction.reset();
    m_resultTransitionError.clear();
    m_pendingGameStart.reset();
    m_loadingStage = GameLoadingStage::None;
    m_loadingProgress = 0.0f;
    m_loadingStatus.clear();
    m_loadingError.clear();
    m_loadingScreenPresented = false;
    m_loadingScreenDismissed = false;
    m_renderStartupFrameSubmitted = false;
    m_continuationLoading = ContinuationLoadingKind::None;
    m_startupSceneProgress = {};
    m_loadingLastProgress.clear();
    m_loadingStartupDeadline = {};
    m_loadingStartupHeartbeatAt = {};
    m_loadingStartupLastProgressAt = {};
    m_lastFrameCommitResult = {};
    if (retiringSession) advanceNonzeroRevision(m_sessionRevision);
    m_state = GameState::Idle;
}

void GameLogic::update() {
    assertLogicThreadOwnership();
    if (m_state == GameState::Loading) {
        updateLoading();
        // Installing a session and exposing Running are one observable
        // boundary.  Execute tick zero in this same logic iteration so the
        // first published Running projection/world frame already contains
        // map startup scripts such as CN01's DISABLE_INPUT and camera setup.
        if (m_state != GameState::Running || !m_session) return;
    }
    if (m_state == GameState::Result) {
        processResultAction();
        return;
    }
    if (m_state != GameState::Running || !m_session) {
        return;
    }
    // A structural simulation fault is terminal for this session generation.
    // Keep presenting the last committed endpoint, but never advance another
    // tick whose inputs would be based on a partially committed predecessor.
    if (m_lastFrameCommitResult.faulted()) return;

    if (m_currentGame.network.enabled) {
        m_enetTransport.update(m_lockstepFrameBuffer);
        if (const auto confirmedRate = m_enetTransport.confirmedFrameSendRate()) {
            if (*confirmedRate != m_lockstepFrameBuffer.frameSendRate()) {
                m_lockstepFrameBuffer.setFrameSendRate(*confirmedRate);
                TD_LOG_INFO("[GameLogic] Server confirmed frameSendRate={}", *confirmedRate);
            }
        }

        if (!m_enetTransport.isReady()) {
            return;
        }
        const GameTick frameSendRate = m_lockstepFrameBuffer.frameSendRate();
        if (frameSendRate == 0) {
            TD_LOG_ERROR("[GameLogic] Network session has an invalid frameSendRate of 0");
            return;
        }
        m_enetTransport.queueFrames(
            m_lockstepFrameBuffer.sealLocalFramesThrough(m_currentTick + frameSendRate - 1));

        container::Vector<GameCommand> commands;
        container::Vector<GameCommand> rejectedLocalCommands;
        if (!m_lockstepFrameBuffer.takeReadyFrame(
                m_currentTick, commands, &rejectedLocalCommands)) {
            return;
        }
        for (const GameCommand& rejected : rejectedLocalCommands) {
            publishTerminalCommandOutcome(
                rejected, false, CommandOutcomeReason::AuthorityRejected,
                m_currentTick);
        }
        ConfirmedCommandFrame frame;
        frame.tick = m_currentTick;
        frame.includesLocalCommands = true;
        frame.commands = std::move(commands);
        m_lastFrameCommitResult = executeConfirmedFrame(frame);
        if (m_lastFrameCommitResult.faulted()) return;
        if (const auto sample = m_commandSyncProbe.finishTick(m_currentTick)) {
            const ObjectAISimulationDigest digest =
                m_session->aiQuery().objectSimulationDigest();
            m_enetTransport.sendSyncSample({
                .tick = sample->tick,
                .commandChecksum = sample->checksum,
                .combinedChecksum = mixSimulationSyncChecksum(
                    sample->checksum, digest),
                .aiRuntime = digest.aiRuntime,
                .navigation = digest.navigation,
                .movement = digest.movement,
                .economy = digest.economy,
                .players = digest.players,
                .worldCombined = digest.combined,
            });
        }
        ++m_currentTick;
        advanceLocalConstructionRoutes();
        static_cast<void>(captureMissionOutcome());
        return;
    }

    // RefCode's m_timeMultiplier is a visual fast-forward request. Its
    // modern consumer is a bounded batch of *ordinary* fixed confirmed
    // frames: every timer, camera track, command and script still observes
    // the same deterministic 1/FPS delta. Sample once per outer update so a
    // script action emitted by the first frame takes effect on the next
    // presentation turn rather than making this loop self-expanding.
    const uint32_t frameBudget = visualSpeedConfirmedFrameBudget(
        m_session->presentationPort().snapshot().visualSpeedMultiplier,
        false);
    for (uint32_t frameIndex = 0; frameIndex < frameBudget; ++frameIndex) {
        m_commandPlayback.submitDueCommands(m_currentTick, m_localFrameAuthority.commandQueue());
        if (const size_t expiredReplayCommands = m_commandPlayback.takeExpiredCount();
            expiredReplayCommands != 0) {
            TD_LOG_WARN("[GameLogic] Dropped {} replay command(s) that missed confirmed tick {}",
                        expiredReplayCommands, m_currentTick);
        }
        const auto frame = m_localFrameAuthority.confirmFrame(m_currentTick);
        if (frame.expiredCommandCount != 0) {
            TD_LOG_WARN("[GameLogic] Dropped {} command(s) that missed confirmed tick {}",
                        frame.expiredCommandCount, m_currentTick);
        }
        if (frame.rejectedCommandCount != 0) {
            TD_LOG_WARN("[GameLogic] Rejected {} queued command(s) at local/replay ingress before confirmed tick {}",
                        frame.rejectedCommandCount, m_currentTick);
        }
        m_lastFrameCommitResult = executeConfirmedFrame(frame);
        if (m_lastFrameCommitResult.faulted()) break;
        m_commandSyncProbe.finishTick(m_currentTick);
        ++m_currentTick;
        advanceLocalConstructionRoutes();
        if (captureMissionOutcome()) break;
    }
}

bool GameLogic::canResultAction(GameResultAction action) const {
    if (m_state != GameState::Result || !m_matchResult ||
        m_pendingResultAction.has_value()) {
        return false;
    }
    // Network match restart/continuation needs a server-authored result and a
    // fresh session ticket.  Until that deferred protocol exists, only Exit
    // is a valid local result action.
    if (action == GameResultAction::Exit) return true;
    if (m_matchResult->startInfo.network.enabled ||
        m_matchResult->startInfo.mode == GameMode::Replay) {
        return false;
    }
    const bool victory = m_matchResult->localVictory();
    if (action == GameResultAction::Retry) return !victory;
    if (!victory ||
        (m_matchResult->startInfo.sequence.type !=
             GameSequenceType::Campaign &&
         m_matchResult->startInfo.sequence.type !=
             GameSequenceType::Challenge)) {
        return false;
    }
    return static_cast<bool>(GameContinuationResolver::resolve(
        m_matchResult->startInfo, GameContinuationAction::Next));
}

bool GameLogic::queueResultAction(GameResultAction action) {
    assertLogicThreadOwnership();
    if (!canResultAction(action)) return false;
    m_pendingResultAction = action;
    m_resultTransitionError.clear();
    return true;
}

bool GameLogic::takeExitRequest() noexcept {
    assertLogicThreadOwnership();
    const bool requested = m_exitRequested;
    m_exitRequested = false;
    return requested;
}

bool GameLogic::captureMissionOutcome() {
    if (m_state != GameState::Running || !m_session || m_matchResult) return false;
    const std::optional<scenario::MissionOutcome> outcome =
        m_session->runtimeQuery().missionOutcome();
    if (!outcome) return false;

    m_matchResult = MatchResultSnapshot::capture(*m_session, *outcome);
    advanceNonzeroRevision(m_matchResultRevision);
    m_pendingResultAction.reset();
    m_resultTransitionError.clear();
    m_localPauseSources = 0;
    m_state = GameState::Result;
    TD_LOG_INFO("[GameLogic] Mission result sealed: state={} tick={} rows={}",
                static_cast<int>(outcome->state), outcome->confirmedTick,
                m_matchResult->players.size());
    return true;
}

void GameLogic::processResultAction() {
    if (!m_pendingResultAction || !m_matchResult) return;

    const GameResultAction action = *m_pendingResultAction;
    m_pendingResultAction.reset();
    if (action == GameResultAction::Exit) {
        m_state = GameState::Transitioning;
        clearGameData();
        m_exitRequested = true;
        return;
    }

    const GameContinuationAction continuationAction =
        action == GameResultAction::Next
            ? GameContinuationAction::Next
            : GameContinuationAction::Retry;
    GameContinuationResult continuation =
        GameContinuationResolver::resolve(m_matchResult->startInfo, continuationAction);
    if (!continuation) {
        m_resultTransitionError = std::move(continuation.error);
        TD_LOG_WARN("[GameLogic] Result transition rejected: {}", m_resultTransitionError);
        return;
    }

    GameStartInfo nextStartInfo = std::move(continuation.startInfo);
    container::String error;
    if (!startContinuationLoading(std::move(nextStartInfo), error)) {
        m_resultTransitionError = std::move(error);
        TD_LOG_WARN("[GameLogic] Continuation candidate rejected: {}",
                    m_resultTransitionError);
    }
}

void GameLogic::queueCameraInput(const GameCameraInput& input) {
    assertLogicThreadOwnership();
    if ((m_state != GameState::Running && m_state != GameState::Paused) ||
        !m_session) {
        return;
    }
    m_session->presentationPort().queueCameraInput(input);
}

bool GameLogic::setScriptPresentationPaused(bool paused) {
    return setLocalPauseSource(LocalPauseSource::ScriptPopup, paused);
}

bool GameLogic::localPauseSourceActive(LocalPauseSource source) const noexcept {
    const uint8_t index = static_cast<uint8_t>(source);
    if (index > static_cast<uint8_t>(LocalPauseSource::InGameMenu)) {
        return false;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << index);
    return (m_localPauseSources & bit) != 0;
}

bool GameLogic::setLocalPauseSource(LocalPauseSource source, bool paused) {
    assertLogicThreadOwnership();
    // A client-local modal cannot halt replay progression or network lockstep.
    // The original single-player popup pause is nevertheless useful for
    // campaign scripting, so expose it as an explicit, narrowly scoped
    // GameLogic state transition instead of smuggling it through FREEZE_TIME.
    if (!m_session || m_currentGame.network.enabled ||
        m_currentGame.mode == GameMode::Replay) {
        return false;
    }
    const uint8_t index = static_cast<uint8_t>(source);
    if (index > static_cast<uint8_t>(LocalPauseSource::InGameMenu)) {
        return false;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << index);
    if (paused) {
        if ((m_localPauseSources & bit) != 0) return true;
        if (m_state != GameState::Running &&
            !(m_state == GameState::Paused && m_localPauseSources != 0)) {
            return false;
        }
        m_localPauseSources = static_cast<uint8_t>(
            m_localPauseSources | bit);
        m_state = GameState::Paused;
        return true;
    }
    if ((m_localPauseSources & bit) == 0) return true;
    m_localPauseSources = static_cast<uint8_t>(
        m_localPauseSources & static_cast<uint8_t>(~bit));
    if (m_localPauseSources == 0 && m_state == GameState::Paused) {
        m_state = GameState::Running;
    }
    return true;
}

DisconnectStatus GameLogic::transportDisconnectStatus() const {
    DisconnectStatus status;
    if (!m_currentGame.network.enabled) return status;

    switch (m_enetTransport.state()) {
    case EnetGameTransportState::Disconnected:
        if (m_session && m_state == GameState::Running) {
            status.state = DisconnectState::Lost;
            status.reason = "Network transport is disconnected";
        }
        break;
    case EnetGameTransportState::Connecting:
    case EnetGameTransportState::AwaitingHello:
        // These states currently occur only during the initial handshake.
        // Do not mislabel them as recovery; a future backend must explicitly
        // publish Lost/Reconnecting/Resync together with its real attempt and
        // deadline data.
        status.state = DisconnectState::Connected;
        break;
    case EnetGameTransportState::Ready:
        status.state = DisconnectState::Connected;
        break;
    case EnetGameTransportState::Failed:
        status.state = DisconnectState::Terminal;
        status.reason = m_enetTransport.error().empty()
            ? "Network connection failed"
            : m_enetTransport.error();
        // The current transport fails closed and cannot issue a resume
        // ticket. Explicitly keep retryability false until U-005 exists.
        status.retryable = false;
        break;
    }
    return status;
}

DisconnectStatus GameLogic::disconnectStatus() const {
    DisconnectStatus status = transportDisconnectStatus();
    if (m_disconnectActionStatus.actionRevision == 0) return status;

    // Transport state remains live after an action acknowledgement. A stale
    // Rejected/Connected ack must never mask a later real Lost/Failed state.
    status.actionRevision = m_disconnectActionStatus.actionRevision;
    status.lastAction = m_disconnectActionStatus.lastAction;
    status.lastActionResult = m_disconnectActionStatus.lastActionResult;
    status.actionError = m_disconnectActionStatus.actionError;
    if (status.lastActionResult == DisconnectActionResult::Unsupported &&
        status.state != DisconnectState::Connected &&
        status.state != DisconnectState::Ready) {
        status.state = DisconnectState::Terminal;
        status.retryable = false;
        if (status.reason.empty()) status.reason = status.actionError;
    }
    return status;
}

bool GameLogic::requestDisconnectAction(DisconnectAction action) {
    assertLogicThreadOwnership();
    if (action == DisconnectAction::None) return false;

    uint64_t actionRevision = m_disconnectActionStatus.actionRevision + 1;
    if (actionRevision == 0) actionRevision = 1;
    m_disconnectActionStatus = transportDisconnectStatus();
    const bool hasDisconnect =
        m_disconnectActionStatus.state != DisconnectState::Connected &&
        m_disconnectActionStatus.state != DisconnectState::Ready;
    m_disconnectActionStatus.actionRevision = actionRevision;
    m_disconnectActionStatus.lastAction = action;
    if (hasDisconnect && action == DisconnectAction::Exit) {
        // The in-game executable is launcher-owned. Leaving a terminally
        // disconnected match therefore follows the same clean application
        // exit path as the Result screen; host shutdown retires transport,
        // session and presentation domains in their normal order.
        m_disconnectActionStatus.lastActionResult =
            DisconnectActionResult::Accepted;
        m_disconnectActionStatus.actionError.clear();
        m_disconnectActionStatus.retryable = false;
        m_exitRequested = true;
        return true;
    }
    m_disconnectActionStatus.lastActionResult = hasDisconnect
        ? DisconnectActionResult::Unsupported
        : DisconnectActionResult::Rejected;
    m_disconnectActionStatus.actionError = !hasDisconnect
        ? "There is no disconnected session"
        : action == DisconnectAction::Reconnect
            ? "Reconnect is unsupported by the current network transport"
            : action == DisconnectAction::Cancel
                ? "Cancel reconnect is unsupported because no recovery backend is active"
                : "Disconnect exit routing is not implemented";
    // Preserve a pre-existing terminal transport reason while still making
    // the action failure independently visible to the UI.
    if (hasDisconnect &&
        m_disconnectActionStatus.state != DisconnectState::Terminal) {
        m_disconnectActionStatus.state = DisconnectState::Terminal;
        m_disconnectActionStatus.reason =
            m_disconnectActionStatus.actionError;
    }
    m_disconnectActionStatus.retryable = false;
    return false;
}

void GameLogic::publishCommandOutcome(CommandOutcome outcome) {
    assertLogicThreadOwnership();
    if (outcome.requestSequence == 0) return;
    if (m_nextCommandOutcomeRevision == 0) {
        m_nextCommandOutcomeRevision = 1;
    }
    outcome.revision = m_nextCommandOutcomeRevision++;
    m_commandOutcomes.revision = outcome.revision;
    m_commandOutcomes.latest = outcome;
    m_commandOutcomes.records.push_back(std::move(outcome));
}

void GameLogic::acknowledgeCommandOutcomes(uint64_t revision) {
    assertLogicThreadOwnership();
    if (revision == 0 ||
        revision <= m_commandOutcomes.acknowledgedRevision) {
        return;
    }
    const uint64_t acknowledged = std::min(
        revision, m_commandOutcomes.revision);
    if (acknowledged <= m_commandOutcomes.acknowledgedRevision) return;
    const auto retained = std::upper_bound(
        m_commandOutcomes.records.begin(),
        m_commandOutcomes.records.end(), acknowledged,
        [](uint64_t value, const CommandOutcome& outcome) {
            return value < outcome.revision;
        });
    m_commandOutcomes.records.erase(
        m_commandOutcomes.records.begin(), retained);
    m_commandOutcomes.acknowledgedRevision = acknowledged;
}

void GameLogic::publishTerminalCommandOutcome(
    const GameCommand& command, bool accepted,
    CommandOutcomeReason rejection, GameTick confirmedTick) {
    if (!command.activation.present() || command.source == CommandSource::Replay ||
        !m_session) {
        return;
    }
    const auto localPlayer = engine::session_query::localPlayer(*m_session);
    if (!localPlayer || command.player != localPlayer->id) return;
    // Waypoint selection is presentation-local.  Keep it selected until the
    // confirmed executor accepts this exact cancellation; a stale node must
    // remain visible/selectable when confirmation rejects the command.
    if (accepted && command.type == GameCommandType::CancelOrderWaypoint &&
        command.actors.size() == 1u) {
        const selection::LocalOrderWaypointSelection selected =
            m_localSelection.selectedOrderWaypoint();
        if (selected && selected.actor == command.actors.front() &&
            selected.sourceSequence == command.productionId) {
            static_cast<void>(m_localSelection.clear());
        }
    }
    publishCommandOutcome({
        .requestSequence = command.activation.requestSequence,
        .commandSequence = command.sequence,
        .buttonStableId = command.activation.buttonStableId,
        .commandKind = command.activation.commandKind,
        .state = accepted
            ? CommandOutcomeState::Accepted
            : CommandOutcomeState::Rejected,
        .reason = accepted ? CommandOutcomeReason::None : rejection,
        .voice = accepted
            ? CommandVoiceDisposition::Accepted
            : CommandVoiceDisposition::Rejected,
        .cursor = accepted
            ? CommandCursorDisposition::Accepted
            : CommandCursorDisposition::Rejected,
        .confirmedTick = confirmedTick,
    });
}

GameCommandSubmissionResult GameLogic::submitCommand(GameCommand command) {
    assertLogicThreadOwnership();
    const CommandActivationContext activation = command.activation;
    const auto reject = [&](CommandOutcomeReason reason) {
        if (activation.present()) {
            publishCommandOutcome({
                .requestSequence = activation.requestSequence,
                .buttonStableId = activation.buttonStableId,
                .commandKind = activation.commandKind,
                .state = CommandOutcomeState::Rejected,
                .reason = reason,
                .voice = CommandVoiceDisposition::Rejected,
                .cursor = CommandCursorDisposition::Rejected,
                .confirmedTick = m_session
                    ? static_cast<GameTick>(m_session->confirmedTick()) : 0,
            });
        }
        return GameCommandSubmissionResult{
            .rejection = reason,
        };
    };
    const auto admitted = [&](const GameCommand& resolved) {
        if (activation.present()) {
            publishCommandOutcome({
                .requestSequence = activation.requestSequence,
                .commandSequence = resolved.sequence,
                .buttonStableId = activation.buttonStableId,
                .commandKind = activation.commandKind,
                .state = CommandOutcomeState::PendingConfirmation,
                .reason = CommandOutcomeReason::None,
                .voice = CommandVoiceDisposition::AwaitConfirmation,
                .cursor = CommandCursorDisposition::AwaitConfirmation,
            });
        }
        return GameCommandSubmissionResult{
            .admitted = true,
            .commandSequence = resolved.sequence,
            .executionTick = resolved.tick,
        };
    };
    if (m_state != GameState::Running || !m_session) {
        TD_LOG_WARN("[GameLogic] Ignoring command outside an active game session");
        return reject(CommandOutcomeReason::GameNotRunning);
    }
    if (m_currentGame.network.enabled) {
        if (!m_enetTransport.isReady()) {
            TD_LOG_WARN("[GameLogic] Ignoring local command before network session is ready");
            return reject(CommandOutcomeReason::NetworkNotReady);
        }
        LockstepLocalSubmitResult submitted =
            m_lockstepFrameBuffer.submitLocalResolved(
                std::move(command), m_currentTick);
        if (!submitted.command) {
            TD_LOG_WARN("[GameLogic] Local command arrived after its execution frame was sealed");
            const CommandOutcomeReason reason =
                submitted.rejection == LockstepLocalSubmitRejection::FrameSealed
                    ? CommandOutcomeReason::FrameSealed
                    : submitted.rejection ==
                            LockstepLocalSubmitRejection::CapacityExceeded
                        ? CommandOutcomeReason::QueueCapacityExceeded
                        : CommandOutcomeReason::AuthorityRejected;
            return reject(reason);
        }
        return admitted(*submitted.command);
    }
    const auto localPlayer = engine::session_query::localPlayer(*m_session);
    if (!localPlayer || !localPlayer->commandPlayer) {
        TD_LOG_WARN("[GameLogic] Ignoring local command without a local command player");
        return reject(CommandOutcomeReason::MissingLocalCommandPlayer);
    }
    // UI input is always authored by this process's resolved local player.
    // Replay/remote commands enter through their dedicated frame sources, so
    // a caller cannot forge another slot on the local fixed-step path.
    command.source = CommandSource::Local;
    command.player = localPlayer->id;
    if (command.tick == 0) {
        command.tick = m_currentTick;
    }
    // Local/replay ingress shares one rejection counter. Surface it once with
    // the sealed confirmed frame below, rather than logging the same rejected
    // key both here and again at confirmation.
    GameCommandQueueSubmitResult submitted =
        m_localFrameAuthority.submitResolved(std::move(command));
    if (!submitted.command) {
        const CommandOutcomeReason reason = submitted.rejection ==
                GameCommandQueueRejection::CapacityExceeded
            ? CommandOutcomeReason::QueueCapacityExceeded
            : submitted.rejection ==
                    GameCommandQueueRejection::DuplicateSequence
                ? CommandOutcomeReason::DuplicateSequence
                : CommandOutcomeReason::AuthorityRejected;
        return reject(reason);
    }
    return admitted(*submitted.command);
}

bool GameLogic::submitBeaconText(
    container::StringView caption, container::String* error) {
    assertLogicThreadOwnership();
    if (m_state != GameState::Running || !m_session) {
        if (error) *error = "beacon text was submitted outside an active game";
        return false;
    }
    if (m_currentGame.network.enabled && !m_enetTransport.isReady()) {
        if (error) *error = "network session is not ready for local commands";
        return false;
    }
    const auto localPlayer = engine::session_query::localPlayer(*m_session);
    if (!localPlayer || !localPlayer->commandPlayer) {
        if (error) *error = "local view has no live command player";
        return false;
    }

    const GameSessionCommandQueryPort commandQuery =
        m_session->commandQuery();
    selection::BeaconTextComposeResult composed =
        selection::BeaconTextCommandComposer::compose(
            m_localSelection, commandQuery, localPlayer->id,
            m_currentTick, 0, caption);
    if (!composed) {
        if (error) *error = std::move(composed.message);
        return false;
    }
    submitCommand(std::move(*composed.command));
    return true;
}

bool GameLogic::submitRepairTarget(
    ObjectId structure, container::String* error) {
    assertLogicThreadOwnership();
    if (m_state != GameState::Running || !m_session) {
        if (error) *error = "repair was submitted outside an active game";
        return false;
    }
    if (m_currentGame.network.enabled && !m_enetTransport.isReady()) {
        if (error) *error = "network session is not ready for local commands";
        return false;
    }
    const auto localPlayer = engine::session_query::localPlayer(*m_session);
    if (!localPlayer || !localPlayer->commandPlayer) {
        if (error) *error = "local view has no live command player";
        return false;
    }
    if (!m_session->commandQuery().canRepairSelectionTarget(
            localPlayer->id, m_localSelection.selected(), structure)) {
        if (error) *error = "current selection cannot repair this target";
        return false;
    }
    selection::WorldCommandRequest request;
    request.tick = m_currentTick;
    request.type = GameCommandType::Repair;
    request.targetObject = structure;
    selection::WorldCommandComposeResult composed =
        selection::WorldCommandComposer::compose(
            *m_session, m_localSelection, localPlayer->id,
            std::move(request));
    if (!composed) {
        if (error) *error = std::move(composed.message);
        return false;
    }
    submitCommand(std::move(*composed.command));
    return true;
}

FrameCommitResult GameLogic::executeConfirmedFrame(
    const ConfirmedCommandFrame& frame)
{
#if TD_DEBUG_ENABLED
    const auto frameStarted = std::chrono::steady_clock::now();
#endif
    const int fixedRate = std::max(1, m_currentGame.gameSpeedFPS);
    const float fixedDeltaSeconds = 1.0f / static_cast<float>(fixedRate);
    GameSessionFramePort sessionFrame = m_session->framePort();
    GameSessionPresentationPort sessionPresentation =
        m_session->presentationPort();
    // RefCode updates the client camera first, then samples GameEngine's
    // frozen-time gate. Script updates keep their own monotonically
    // increasing input tick, while the authoritative world clock remains at
    // its last committed value for the entire freeze interval.
    sessionPresentation.updateCameraSystems(fixedDeltaSeconds);
    const bool worldWasFrozen = sessionFrame.simulationTimeFrozen();
    const uint64_t previousWorldTick = m_session->confirmedTick();
    const uint64_t worldTick = worldWasFrozen
        ? previousWorldTick
        : previousWorldTick == 0
            ? static_cast<uint64_t>(FirstConfirmedGameTick)
            : previousWorldTick + 1u;
#if TD_DEBUG_ENABLED
    const auto cameraFinished = std::chrono::steady_clock::now();
#endif

    // Audio/FX consequences use the world tick. beginConfirmedFrame accepts
    // an equal tick only for this explicit frozen script-only update and
    // retains per-frame ordinals across that interval.
    if (!sessionFrame.begin(worldTick, worldWasFrozen)) {
        TD_LOG_ERROR(
            "[GameLogic] Could not establish world frame {} for input frame {}",
            worldTick, frame.tick);
        return sessionFrame.result();
    }
    sessionFrame.noteCommandOutcome(
        false, frame.expiredCommandCount);
    sessionFrame.noteCommandOutcome(
        false, frame.rejectedCommandCount);
    for (const GameCommand& expired : frame.expiredLocalCommands) {
        publishTerminalCommandOutcome(
            expired, false,
            CommandOutcomeReason::ExpiredBeforeConfirmation,
            static_cast<GameTick>(worldTick));
    }
    size_t lifecyclePresentationCursor = 0;
    const auto consumeLifecyclePresentation = [&] {
        // A faulted confirmed transaction never publishes a partial local
        // presentation edge, even though synchronous gameplay consumers have
        // already fail-stopped the session at the structural boundary.
        if (sessionFrame.result().faulted()) return;
        const container::Vector<ObjectLifecycleEvent> events =
            sessionFrame.lifecyclePresentationEvents(
                lifecyclePresentationCursor);
        if (events.empty()) return;
        const selection::PendingWorldCommandMode pending =
            m_localSelection.pendingWorldCommand();
        static_cast<void>(m_localSelection.applyLifecycleEvents(
            events));
        if (pending.active() &&
            !m_localSelection.pendingWorldCommand().active()) {
            publishCommandOutcome({
                .requestSequence = pending.requestSequence,
                .buttonStableId = pending.buttonStableId,
                .commandKind = pending.commandKind,
                .state = CommandOutcomeState::Rejected,
                .reason = CommandOutcomeReason::SourceBecameUnavailable,
                .voice = CommandVoiceDisposition::None,
                .cursor = CommandCursorDisposition::Unchanged,
                .confirmedTick = static_cast<GameTick>(worldTick),
            });
        }
        lifecyclePresentationCursor += events.size();
    };

    // ScriptRuntime keeps advancing while world time is frozen so a counter,
    // timer or condition can issue UNFREEZE_TIME.  Script effects are flushed
    // synchronously, but their new time-control value is intentionally only
    // observed by the next sampled world gate.
    (void)sessionFrame.advanceScripts(
        frame.tick, m_localSelection.selected(), worldTick);
#if TD_DEBUG_ENABLED
    const auto scriptsFinished = std::chrono::steady_clock::now();
#endif
    if (sessionFrame.result().faulted()) {
        return sessionFrame.complete();
    }

    // OBJECT_FORCE_SELECT and MOVE_CAMERA_TO_SELECTION are both local
    // presentation paths. They share the session's source sequence, so merge
    // their two typed journals rather than applying all of one kind before
    // the other and accidentally reversing same-frame script order. Neither
    // consumer writes a command, replay payload, lockstep frame or ECS
    // selection component; m_localSelection belongs only to this process.
    const container::Vector<script::ScriptForceObjectSelectionPresentation>
        forceSelectionRequests = sessionFrame.takeForceSelectionPresentations();
    const container::Vector<script::ScriptMoveCameraToSelectionPresentation>
        selectionCameraRequests = sessionFrame.takeSelectionCameraPresentations();
    size_t forceSelectionIndex = 0;
    size_t selectionCameraIndex = 0;
    GameSessionPresentationPort selectionPresentation =
        m_session->presentationPort();
    const uint64_t selectionPresentationTick = m_session->confirmedTick();
    while (forceSelectionIndex < forceSelectionRequests.size() ||
           selectionCameraIndex < selectionCameraRequests.size()) {
        const bool consumeForceSelection =
            selectionCameraIndex >= selectionCameraRequests.size() ||
            (forceSelectionIndex < forceSelectionRequests.size() &&
             forceSelectionRequests[forceSelectionIndex].stamp.sequence <=
                 selectionCameraRequests[selectionCameraIndex].stamp.sequence);
        if (consumeForceSelection) {
            const auto oneRequest = container::Span<const script::ScriptForceObjectSelectionPresentation>{
                forceSelectionRequests.data() + forceSelectionIndex, 1};
            static_cast<void>(
                selectionPresentation.consumeForceObjectSelection(
                    oneRequest, m_localSelection, selectionPresentationTick));
            ++forceSelectionIndex;
        } else {
            const auto oneRequest = container::Span<const script::ScriptMoveCameraToSelectionPresentation>{
                selectionCameraRequests.data() + selectionCameraIndex, 1};
            static_cast<void>(
                selectionPresentation.consumeMoveCameraToSelection(
                    oneRequest, m_localSelection, selectionPresentationTick));
            ++selectionCameraIndex;
        }
    }
    // Script actions may create, destroy, replace, or transfer an object.
    // Consume those local presentation edges before a frozen world returns.
    consumeLifecyclePresentation();

    if (worldWasFrozen) {
        // The local authority has already admitted this confirmed input.  Do
        // not drop it simply because the normal world phases are paused: keep
        // chronological frame order and execute it at the first world frame
        // after unfreeze.  Recording/checksum still belongs to its original
        // confirmed input tick, so replay input is neither lost nor re-stamped.
        for (const GameCommand& command : frame.commands) {
            if (command.source != CommandSource::Replay) {
                m_commandRecorder.record(command);
            }
            m_commandSyncProbe.record(command);
        }
        m_deferredFrozenCommands.defer(frame.commands);
        sessionFrame.noteDeferredCommands(frame.commands.size());
        return sessionFrame.complete(true);
    }

    // updateCameraSystems() above already consumed the presentation phase.
    // This call now contains only the pre-command world work (terrain/flood).
    m_session->updatePreCommandSystems(fixedDeltaSeconds);
#if TD_DEBUG_ENABLED
    const auto preCommandFinished = std::chrono::steady_clock::now();
#endif
    if (sessionFrame.result().faulted()) {
        return sessionFrame.complete();
    }

    const auto dispatch = [this, &sessionFrame,
                           confirmedTick = static_cast<GameTick>(worldTick)](
                              const GameCommand& command, bool recordInput) {
        GameCommand executionCommand = command;
        executionCommand.tick = confirmedTick;
        CommandDispatchResult result;
        const ConfirmedCommandActivationValidation activationValidation =
            m_session->confirmedCommandPort().validateActivation(
                executionCommand);
        if (!activationValidation.accepted()) {
            result = {
                .accepted = false,
                .producedOrder = false,
                .rejection = CommandDispatchRejection::Rejected,
                .message = container::String{
                    "confirmed CommandButton activation rejected: "} +
                    activationRejectionName(activationValidation.rejection),
            };
        } else {
            result = m_commandDispatcher.dispatch(
                *m_session, executionCommand);
        }
        if (executionCommand.type == GameCommandType::Build) {
            recordLocalConstructionRouteBuildDispatch(
                executionCommand.sequence, result.accepted, confirmedTick);
        }
        if (result.accepted &&
            !m_session->confirmedCommandPort().applyPostAccept(
                executionCommand)) {
            TD_LOG_ERROR(
                "[GameLogic] Accepted command could not apply its validated post-accept transition at tick {}",
                confirmedTick);
        }
        sessionFrame.noteCommandOutcome(result.accepted);
        CommandOutcomeReason rejection = !activationValidation.accepted()
            ? activationOutcomeReason(activationValidation.rejection)
            : CommandOutcomeReason::DispatcherRejected;
        if (activationValidation.accepted() &&
            result.rejection == CommandDispatchRejection::MalformedPayload) {
            rejection = CommandOutcomeReason::DispatcherMalformedPayload;
        } else if (activationValidation.accepted() &&
                   result.rejection == CommandDispatchRejection::Unsupported) {
            rejection = CommandOutcomeReason::DispatcherUnsupported;
        }
        const bool waitsForBackend = result.accepted &&
            executionCommand.activation.present() &&
            executionCommand.source != CommandSource::Replay &&
            result.actorCount != 0 &&
            (executionCommand.type == GameCommandType::SpecialPower ||
             executionCommand.type == GameCommandType::CombatDrop);
        if (waitsForBackend) {
            m_pendingBackendCommandOutcomes.push_back({
                .command = executionCommand,
                .admittedTick = confirmedTick,
                .expectedActorCount = result.actorCount,
            });
        } else {
            publishTerminalCommandOutcome(
                executionCommand, result.accepted, rejection, confirmedTick);
        }
        if (!result.accepted && command.type != GameCommandType::UIAction) {
            TD_LOG_WARN(
                "[GameLogic] Confirmed command was rejected at tick {}: {} "
                "type={} sequence={} activation={}/{} kind={} actors={} "
                "activationReason={}",
                command.tick, result.message,
                static_cast<uint32_t>(command.type), command.sequence,
                command.activation.requestSequence,
                command.activation.buttonStableId,
                static_cast<uint32_t>(command.activation.commandKind),
                command.actors.size(),
                activationRejectionName(activationValidation.rejection));
        }
        if (recordInput && command.source != CommandSource::Replay) {
            m_commandRecorder.record(command);
        }
        if (recordInput) m_commandSyncProbe.record(command);
    };
    for (const GameCommand& executionCommand :
         m_deferredFrozenCommands.takeForExecution(frame.tick)) {
        // The command has already been recorded and checksummed under its
        // original confirmed input tick while the world was frozen.  Its
        // actual object-order execution must nevertheless use *this* live
        // world tick: GameSession correctly rejects an order whose tick does
        // not equal m_confirmedTick, and feeding the original timestamp here
        // would silently lose every deferred Move/Attack/Stop on unfreeze.
        //
        // Recording/checksum already retained the original authoring tick.
        // DeferredFrozenCommandBuffer changes only the execution timestamp;
        // source sequence, player, payload and deterministic vector order
        // remain intact. Deferred input is intentionally dispatched before
        // newly confirmed input below, matching its original frame order.
        dispatch(executionCommand, false);
    }
    for (const GameCommand& command : frame.commands) {
        dispatch(command, true);
    }
#if TD_DEBUG_ENABLED
    const auto commandsFinished = std::chrono::steady_clock::now();
#endif
    m_session->updatePostCommandSystems(fixedDeltaSeconds);
    for (const CommandBackendOutcome& outcome :
         sessionFrame.takeBackendOutcomes()) {
        const auto found = std::find_if(
            m_pendingBackendCommandOutcomes.begin(),
            m_pendingBackendCommandOutcomes.end(),
            [&outcome](const PendingBackendCommandOutcome& pending) {
                const CommandBackendKind expected =
                    pending.command.type == GameCommandType::CombatDrop
                    ? CommandBackendKind::CombatDrop
                    : CommandBackendKind::SpecialPower;
                return pending.command.player == outcome.player &&
                    pending.command.sequence == outcome.sourceSequence &&
                    expected == outcome.kind && outcome.source &&
                    std::find(pending.command.actors.begin(),
                              pending.command.actors.end(),
                              outcome.source) !=
                        pending.command.actors.end();
            });
        if (found == m_pendingBackendCommandOutcomes.end()) continue;
        if (outcome.accepted) {
            publishTerminalCommandOutcome(
                found->command, true, CommandOutcomeReason::None,
                outcome.confirmedTick);
            m_pendingBackendCommandOutcomes.erase(found);
            continue;
        }
        const auto insertion = std::lower_bound(
            found->rejectedActors.begin(), found->rejectedActors.end(),
            outcome.source);
        if (insertion == found->rejectedActors.end() ||
            *insertion != outcome.source) {
            found->rejectedActors.insert(insertion, outcome.source);
        }
        if (found->rejectedActors.size() >= found->expectedActorCount) {
            publishTerminalCommandOutcome(
                found->command, false,
                CommandOutcomeReason::BackendRejected,
                outcome.confirmedTick);
            m_pendingBackendCommandOutcomes.erase(found);
        }
    }
    const uint64_t backendTimeoutTicks =
        std::max<uint64_t>(1u, m_session->logicFramesPerSecond()) * 30u;
    for (auto it = m_pendingBackendCommandOutcomes.begin();
         it != m_pendingBackendCommandOutcomes.end();) {
        if (worldTick <
            static_cast<uint64_t>(it->admittedTick) +
                backendTimeoutTicks) {
            ++it;
            continue;
        }
        publishTerminalCommandOutcome(
            it->command, false, CommandOutcomeReason::BackendTimedOut,
            static_cast<GameTick>(worldTick));
        it = m_pendingBackendCommandOutcomes.erase(it);
    }
    // World simulation and command execution may add further lifecycle edges.
    // The frame journal is non-destructive, so this cursor consumes only the
    // suffix not already observed after scripts.
    consumeLifecyclePresentation();
    container::Vector<ObjectId> invalidSelectionObjects;
    const auto inspectSelectionObjects = [&](container::Span<const ObjectId> objects) {
        for (const ObjectId object : objects) {
            if (!selection::LocalSelectionPolicy::isRetainedSelectionObject(
                    *m_session, object, false)) {
                invalidSelectionObjects.push_back(object);
            }
        }
    };
    inspectSelectionObjects(m_localSelection.selected());
    for (size_t group = 0; group < selection::LOCAL_CONTROL_GROUP_COUNT;
         ++group) {
        for (const ObjectId object :
             m_localSelection.controlGroup(group)) {
            if (!selection::LocalSelectionPolicy::isRetainedSelectionObject(
                    *m_session, object, true)) {
                invalidSelectionObjects.push_back(object);
            }
        }
    }
    std::sort(invalidSelectionObjects.begin(),
              invalidSelectionObjects.end());
    invalidSelectionObjects.erase(
        std::unique(invalidSelectionObjects.begin(),
                    invalidSelectionObjects.end()),
        invalidSelectionObjects.end());
    if (!invalidSelectionObjects.empty()) {
        const selection::PendingWorldCommandMode pending =
            m_localSelection.pendingWorldCommand();
        static_cast<void>(m_localSelection.removeObjects(
            invalidSelectionObjects));
        if (pending.active() &&
            !m_localSelection.pendingWorldCommand().active()) {
            publishCommandOutcome({
                .requestSequence = pending.requestSequence,
                .buttonStableId = pending.buttonStableId,
                .commandKind = pending.commandKind,
                .state = CommandOutcomeState::Rejected,
                .reason = CommandOutcomeReason::
                    CancelledBySelectionChange,
                .voice = CommandVoiceDisposition::None,
                .cursor = CommandCursorDisposition::Unchanged,
                .confirmedTick = static_cast<GameTick>(worldTick),
            });
        }
    }
#if TD_DEBUG_ENABLED
    const auto postCommandFinished = std::chrono::steady_clock::now();
    if (frame.tick <= 16u) {
        const auto micros = [](auto begin, auto end) {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                end - begin).count();
        };
        TD_LOG_INFO(
            "[LogicStageTiming] tick={} beginCamera={}us scripts={}us selectionAndPre={}us commands={}us post={}us total={}us",
            frame.tick, micros(frameStarted, cameraFinished),
            micros(cameraFinished, scriptsFinished),
            micros(scriptsFinished, preCommandFinished),
            micros(preCommandFinished, commandsFinished),
            micros(commandsFinished, postCommandFinished),
            micros(frameStarted, postCommandFinished));
    }
#endif
    return sessionFrame.complete();
}

bool GameLogic::saveRecordedReplay(container::StringView fileName) const
{
    if (!isGameDomain() || !m_session || fileName.empty()) {
        return false;
    }
    const std::optional<ResolvedMatchSetup> resolvedMatchSetup =
        m_session->runtimeQuery().resolvedMatchSetup();
    if (!resolvedMatchSetup) return false;
    return ::game::ReplayStorage::instance().writeReplayData(container::String{fileName},
                                                            *resolvedMatchSetup,
                                                            m_commandRecorder.commands());
}

} // namespace engine
