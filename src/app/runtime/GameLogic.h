#pragma once

#include "core/container/container_types.h"

#include "game/base/GameSettings.h"
#include "game/base/FrameCommitResult.h"
#include "game/session/query/MatchResultSnapshot.h"
#include "game/base/StartupSceneProgress.h"
#include "app/runtime/CommandDispatcher.h"
#include "game/command/CommandOutcome.h"
#include "game/command/CommandPlayback.h"
#include "game/command/CommandRecorder.h"
#include "game/command/CommandSyncProbe.h"
#include "game/command/DeferredFrozenCommandBuffer.h"
#include "game/command/LocalFrameAuthority.h"
#include "game/command/LockstepFrameBuffer.h"
#include "game/network/EnetGameTransport.h"
#include "game/network/DisconnectContract.h"
#include "game/render/LocalPlacementPresentationState.h"
#include "game/selection/LocalSelectionState.h"
#include <optional>
#include <chrono>
#include <utility>
namespace engine {

struct GameCommandSubmissionResult final {
    bool admitted = false;
    uint32_t commandSequence = 0;
    GameTick executionTick = 0;
    CommandOutcomeReason rejection = CommandOutcomeReason::None;
};

class GameSession;
struct GameCameraInput;

enum class GameState : uint8_t {
    Idle,
    Loading,
    Running,
    Paused,
    Result,
    Transitioning
};

enum class GameResultAction : uint8_t {
    Next,
    Retry,
    Exit
};

enum class ContinuationLoadingKind : uint8_t {
    None,
    Candidate,
    ResultRollback,
};

enum class GameLoadingStage : uint8_t {
    None,
    AwaitingPresentation,
    ResolvingReplay,
    LoadingGameData,
    StartingSession,
    StartingTransport,
    Ready,
};

enum class LocalPauseSource : uint8_t {
    ScriptPopup = 0,
    InGameMenu = 1,
};

class GameLogic {
public:
    static GameLogic& instance();

    bool startNewGame(const GameStartInfo& info);
    void clearGameData();
    void update();
    // Result callbacks enqueue only an intent. update() owns any session
    // teardown/start at the main-loop safe boundary.
    [[nodiscard]] bool canResultAction(GameResultAction action) const;
    [[nodiscard]] bool queueResultAction(GameResultAction action);
    [[nodiscard]] const std::optional<MatchResultSnapshot>& matchResult() const noexcept {
        return m_matchResult;
    }
    [[nodiscard]] uint64_t matchResultRevision() const noexcept {
        return m_matchResultRevision;
    }
    [[nodiscard]] uint64_t sessionRevision() const noexcept {
        return m_sessionRevision;
    }
    [[nodiscard]] const FrameCommitResult& lastFrameCommitResult() const noexcept {
        return m_lastFrameCommitResult;
    }
    [[nodiscard]] const container::String& resultTransitionError() const noexcept {
        return m_resultTransitionError;
    }
    [[nodiscard]] bool takeExitRequest() noexcept;
    [[nodiscard]] GameLoadingStage loadingStage() const noexcept {
        return m_loadingStage;
    }
    [[nodiscard]] float loadingProgress() const noexcept {
        return m_loadingProgress;
    }
    [[nodiscard]] uint64_t loadingRevision() const noexcept {
        return m_loadingRevision;
    }
    [[nodiscard]] const container::String& loadingStatus() const noexcept {
        return m_loadingStatus;
    }
    [[nodiscard]] const container::String& loadingError() const noexcept {
        return m_loadingError;
    }
    void notifyLoadingScreenPresented(uint64_t revision) noexcept;
    void notifyLoadingScreenDismissed(uint64_t revision) noexcept;
    void notifyRenderStartupFrameSubmitted(
        uint64_t loadingRevision, uint64_t sessionRevision) noexcept;
    void notifyRenderStartupFailure(
        uint64_t loadingRevision, uint64_t sessionRevision,
        container::String error) noexcept;
    void notifyRenderStartupProgress(
        uint64_t loadingRevision, uint64_t sessionRevision,
        StartupSceneProgress progress) noexcept;
    [[nodiscard]] const StartupSceneProgress& startupSceneProgress()
        const noexcept { return m_startupSceneProgress; }
    [[nodiscard]] const container::String& loadingLastProgress()
        const noexcept { return m_loadingLastProgress; }
    [[nodiscard]] uint64_t loadingDeadlineRemainingMilliseconds()
        const noexcept;
    // Local presentation input is intentionally outside gameplay command
    // replication, but is consumed by GameSession on its next confirmed tick.
    void queueCameraInput(const GameCameraInput& input);
    // Client-local selection belongs neither to PlayerRegistry nor to the
    // replicated command stream. It may include view-only enemy/neutral
    // objects; command composition performs its own authority filter.
    selection::LocalSelectionState& localSelection() noexcept { return m_localSelection; }
    const selection::LocalSelectionState& localSelection() const noexcept {
        return m_localSelection;
    }
    // Client-local construction-route authoring. Nodes here never enter the
    // deterministic order queue until the current builder has reached them.
    [[nodiscard]] bool appendLocalConstructionRouteNode(
        selection::LocalPlacementPreviewSnapshot node);
    [[nodiscard]] size_t localConstructionRouteCount(
        ObjectId builder) const noexcept;
    // Client-local route ghosts never enter the confirmed order queue.  This
    // compact value view is the only route data published back to the input
    // thread for hit testing and the ControlBar cancel affordance.
    struct LocalConstructionRouteNodeProjection final {
        uint64_t previewIdentity = 0;
        ObjectId builder = INVALID_OBJECT_ID;
        container::String objectType;
        render::RenderVector position{};
        float selectionRadius = 0.0f;

        [[nodiscard]] bool valid() const noexcept {
            return previewIdentity != 0 && builder &&
                !objectType.empty() && selectionRadius > 0.0f;
        }
    };
    [[nodiscard]] container::Vector<LocalConstructionRouteNodeProjection>
        localConstructionRouteNodes() const;
    [[nodiscard]] std::optional<LocalConstructionRouteNodeProjection>
        selectedLocalConstructionRouteNode() const;
    [[nodiscard]] bool selectLocalConstructionRouteNode(
        uint64_t previewIdentity) noexcept;
    void clearLocalConstructionRouteNodeSelection() noexcept {
        m_selectedLocalConstructionRoutePreviewIdentity = 0;
    }
    [[nodiscard]] bool cancelSelectedLocalConstructionRouteNode();
    size_t cancelLocalConstructionRoutes(
        container::Span<const ObjectId> builders);
    GameCommandSubmissionResult submitCommand(
        GameCommand command);
    void publishCommandOutcome(CommandOutcome outcome);
    void acknowledgeCommandOutcomes(uint64_t revision);
    [[nodiscard]] const CommandOutcomeProjection& commandOutcomes()
        const noexcept { return m_commandOutcomes; }
    // Live UI entry point corresponding to MSG_SET_BEACON_TEXT. It snapshots
    // the exact current local selection, filters/canonicalizes the caption,
    // and submits an ordinary replay/network command.
    [[nodiscard]] bool submitBeaconText(
        container::StringView caption, container::String* error = nullptr);
    // Live world-context entry point corresponding to retail MSG_DO_REPAIR.
    // It snapshots the local selection before entering lockstep/replay.
    [[nodiscard]] bool submitRepairTarget(
        ObjectId structure, container::String* error = nullptr);
    bool saveRecordedReplay(container::StringView fileName) const;
    // A script popup may request a local single-player presentation pause.
    // This deliberately cannot pause a lockstep/replay match and is separate
    // from the deterministic FREEZE_TIME session gate.
    [[nodiscard]] bool setScriptPresentationPaused(bool paused);
    [[nodiscard]] bool setLocalPauseSource(
        LocalPauseSource source, bool paused);
    [[nodiscard]] bool localPauseSourceActive(
        LocalPauseSource source) const noexcept;
    [[nodiscard]] bool scriptPresentationPaused() const noexcept {
        return localPauseSourceActive(LocalPauseSource::ScriptPopup);
    }

    GameState getState() const { return m_state; }
    bool isInGame() const { return m_state == GameState::Running; }
    bool isLoading() const { return m_state == GameState::Loading; }
    bool isGameDomain() const {
        return m_state == GameState::Loading ||
               m_state == GameState::Running ||
               m_state == GameState::Paused ||
               m_state == GameState::Result ||
               m_state == GameState::Transitioning;
    }

    const GameStartInfo& getCurrentGameInfo() const { return m_currentGame; }
    GameTick currentTick() const { return m_currentTick; }
    bool isNetworkSession() const { return m_currentGame.network.enabled; }
    EnetGameTransportState networkState() const { return m_enetTransport.state(); }
    const container::String& networkError() const { return m_enetTransport.error(); }
    [[nodiscard]] DisconnectStatus disconnectStatus() const;
    // The current ENet backend has no recovery/resync implementation. These
    // actions still acknowledge a typed UI request, explicitly as
    // Unsupported, so presentation can never infer a successful reconnect.
    [[nodiscard]] bool requestDisconnectAction(DisconnectAction action);
    GameSession* currentSession() { return m_session.get(); }
    const GameSession* currentSession() const { return m_session.get(); }

private:
    GameLogic() = default;
    ~GameLogic();

    struct PendingGameStart;

    GameState m_state = GameState::Idle;
    GameStartInfo m_currentGame;
    container::UniquePtr<GameSession> m_session;
    // The terminal session remains fully alive until a Next/Retry candidate
    // has produced a presentable startup frame and the Loading handoff has
    // committed. It is the rollback source for candidate render failure.
    container::UniquePtr<GameSession> m_retainedResultSession;
    GameTick m_currentTick = 0;
    LocalFrameAuthority m_localFrameAuthority;
    CommandDispatcher m_commandDispatcher;
    CommandRecorder m_commandRecorder;
    CommandPlayback m_commandPlayback;
    CommandSyncProbe m_commandSyncProbe;
    selection::LocalSelectionState m_localSelection;
    enum class LocalConstructionRoutePhase : uint8_t {
        AwaitingMove,
        Traveling,
        BuildSubmitted,
        Building,
    };
    enum class LocalConstructionBuildDispatch : uint8_t {
        Pending,
        Accepted,
        Rejected,
    };
    struct LocalConstructionRouteNode final {
        selection::LocalPlacementPreviewSnapshot placement;
        LocalConstructionRoutePhase phase =
            LocalConstructionRoutePhase::AwaitingMove;
        // Admission is not execution: FREEZE_TIME can retain this command in
        // the confirmed deferred buffer for several script ticks.  The local
        // route must wait for the eventual dispatch with this stable sequence.
        uint32_t buildCommandSequence = 0;
        GameTick buildDispatchTick = 0;
        LocalConstructionBuildDispatch buildDispatch =
            LocalConstructionBuildDispatch::Pending;
    };
    struct LocalConstructionRejectedNode final {
        selection::LocalPlacementPreviewSnapshot placement;
        GameTick expiresAfterTick = 0;
    };
    struct LocalConstructionRoute final {
        ObjectId builder = INVALID_OBJECT_ID;
        container::Vector<LocalConstructionRouteNode> nodes;
        container::Vector<LocalConstructionRejectedNode> rejected;
    };
    container::Vector<LocalConstructionRoute> m_localConstructionRoutes;
    uint64_t m_nextLocalConstructionRoutePreviewIdentity = 1;
    // Input selection identity only.  This is not a GameCommand, ECS
    // component, replay value or save-state field; cancellation simply
    // removes an unsubmitted local route node before it reaches Build.
    uint64_t m_selectedLocalConstructionRoutePreviewIdentity = 0;
    // Script-driven time freeze deliberately leaves ScriptRuntime's confirmed
    // clock running.  Local commands admitted on those script-only ticks must
    // therefore survive until the next world-update tick instead of being
    // silently consumed by the frozen phase.
    DeferredFrozenCommandBuffer m_deferredFrozenCommands;
    CommandOutcomeProjection m_commandOutcomes;
    struct PendingBackendCommandOutcome final {
        GameCommand command;
        GameTick admittedTick = 0;
        size_t expectedActorCount = 0;
        container::Vector<ObjectId> rejectedActors;
    };
    container::Vector<PendingBackendCommandOutcome>
        m_pendingBackendCommandOutcomes;
    uint64_t m_nextCommandOutcomeRevision = 1;
    uint8_t m_localPauseSources = 0;
    LockstepFrameBuffer m_lockstepFrameBuffer;
    EnetGameTransport m_enetTransport;
    DisconnectStatus m_disconnectActionStatus;
    std::optional<MatchResultSnapshot> m_matchResult;
    std::optional<GameResultAction> m_pendingResultAction;
    uint64_t m_matchResultRevision = 0;
    uint64_t m_sessionRevision = 0;
    container::String m_resultTransitionError;
    bool m_exitRequested = false;
    container::UniquePtr<PendingGameStart> m_pendingGameStart;
    GameLoadingStage m_loadingStage = GameLoadingStage::None;
    float m_loadingProgress = 0.0f;
    uint64_t m_loadingRevision = 0;
    container::String m_loadingStatus;
    container::String m_loadingError;
    bool m_loadingScreenPresented = false;
    bool m_loadingScreenDismissed = false;
    bool m_renderStartupFrameSubmitted = false;
    ContinuationLoadingKind m_continuationLoading =
        ContinuationLoadingKind::None;
    StartupSceneProgress m_startupSceneProgress;
    container::String m_loadingLastProgress;
    std::chrono::steady_clock::time_point m_loadingStartupDeadline{};
    std::chrono::steady_clock::time_point m_loadingStartupHeartbeatAt{};
    std::chrono::steady_clock::time_point m_loadingStartupLastProgressAt{};
    FrameCommitResult m_lastFrameCommitResult;

    [[nodiscard]] FrameCommitResult executeConfirmedFrame(
        const ConfirmedCommandFrame& frame);
    void publishTerminalCommandOutcome(
        const GameCommand& command, bool accepted,
        CommandOutcomeReason rejection, GameTick confirmedTick);
    void updateLoading();
    void failLoading(container::String error);
    [[nodiscard]] bool startContinuationLoading(
        GameStartInfo startInfo, container::String& error);
    void abortContinuationLoading(container::String error);
    void refreshLoadingStartupStatus();
    void advanceLocalConstructionRoutes();
    void recordLocalConstructionRouteBuildDispatch(
        uint32_t commandSequence, bool accepted, GameTick confirmedTick);
    void publishLocalConstructionRoutePreviews();
    [[nodiscard]] DisconnectStatus transportDisconnectStatus() const;
    [[nodiscard]] bool captureMissionOutcome();
    void processResultAction();
};

} // namespace engine
