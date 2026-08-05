#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "core/platform/runtime_mailbox.h"
#include "app/runtime/GameLogic.h"
#include "game/base/GameSettings.h"
#include "game/command/GameCommand.h"
#include "game/render/LocalPlacementPresentationState.h"
#include "game/session/query/LocalSelectionPolicy.h"
#include "presentation/camera/GameCameraInput.h"
#include "game/script/presentation/ScriptCinematicPresentationControls.h"
#include "game/session/query/InGameCommandProjection.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <mutex>
#include <variant>

namespace app::runtime {

struct StartGameIntent final {
    engine::GameStartInfo info;
};

struct ClearGameIntent final {};

struct QueueCameraInputIntent final {
    engine::GameCameraInput input;
    bool replacePending = false;
};

struct SubmitGameCommandIntent final {
    engine::GameCommand command;
};

struct SubmitRepairTargetIntent final {
    engine::ObjectId target = engine::INVALID_OBJECT_ID;
};

// Presentation supplies only a visible object candidate and a screen-space
// terrain pick request.  The logic thread resolves diplomacy/capabilities and
// composes the final typed Move or Attack command against current state.
struct SubmitContextualWorldCommandIntent final {
    engine::ObjectId targetObject = engine::INVALID_OBJECT_ID;
    // Radar input already resolved a terrain point in presentation space;
    // when present, logic uses this fixed value instead of re-projecting a
    // normal screen ray through the tactical camera.
    std::optional<engine::CommandPosition> targetPosition;
    float screenX = 0.0f;
    float screenY = 0.0f;
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;
    std::optional<engine::GameCameraState> presentationCamera;
    bool queued = false;
    bool forceAttack = false;
    // Local CommandMap BEGIN_FORCEMOVE state.  It deliberately remains out
    // of GameCommand: RefCode dispatches MSG_DO_FORCEMOVETO through the same
    // groupMoveToPosition path as Move, changing only local context routing
    // and the order acknowledgement selected before submission.
    bool forceMove = false;
    bool attackMove = false;
    bool guardPosition = false;
    // Regular (non-alternate) mouse mode uses the same left click for
    // contextual orders and selection. Logic first composes the authoritative
    // context command; only a rejected context falls back to this gesture.
    std::optional<engine::selection::LocalSelectionGesture>
        fallbackSelection;
};

struct CancelPendingWorldCommandIntent final {
    uint64_t modeRevision = 0;
};

// Screen coordinates remain presentation input. The logic thread resolves
// terrain and revalidates the current typed mode before composing a replay-
// safe GameCommand.
struct SubmitPendingWorldCommandTargetIntent final {
    uint64_t modeRevision = 0;
    engine::ObjectId targetObject = engine::INVALID_OBJECT_ID;
    std::optional<engine::CommandPosition> targetPosition;
    float screenX = 0.0f;
    float screenY = 0.0f;
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;
    std::optional<engine::GameCameraState> presentationCamera;
    bool queued = false;
};

struct SubmitBeaconTextIntent final {
    container::String text;
};

struct PurchaseScienceIntent final {
    container::String commandButtonName;
    container::String science;
    uint64_t buttonStableId = 0;
};

struct ActivateSpecialPowerShortcutIntent final {
    container::String commandButtonName;
    uint64_t buttonStableId = 0;
    bool queued = false;
};

struct ResetLocalSelectionIntent final {};

struct ApplyLocalSelectionGestureIntent final {
    engine::selection::LocalSelectionGesture gesture;
};

struct SelectLocalOrderWaypointIntent final {
    engine::selection::LocalOrderWaypointSelection waypoint;
};

struct SelectLocalConstructionRouteNodeIntent final {
    uint64_t previewIdentity = 0;
};

struct CancelSelectedLocalConstructionRouteNodeIntent final {};

struct ApplyLocalControlGroupIntent final {
    engine::selection::LocalControlGroupRequest request;
};

struct ApplyLocalSelectionShortcutIntent final {
    engine::selection::LocalSelectionShortcut shortcut =
        engine::selection::LocalSelectionShortcut::NextUnit;
};

struct SubmitScatterIntent final {};
struct SubmitCreateFormationIntent final {};

struct SetHoveredObjectIntent final {
    engine::ObjectId object = engine::INVALID_OBJECT_ID;
};

struct CancelLocalPlacementIntent final {
    uint64_t previewIdentity = 0;
};

struct UpdateLocalPlacementPointerIntent final {
    float anchorStartX = 0.0f;
    float anchorStartY = 0.0f;
    float anchorEndX = 0.0f;
    float anchorEndY = 0.0f;
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;
    std::optional<engine::GameCameraState> presentationCamera;
    bool forceAttackSnap = false;
    bool confirm = false;
    // A queued construction click updates the shared cursor without taking
    // the ordinary one-shot command; a separate fixed-value intent publishes
    // the corresponding deterministic queued Build command immediately.
    bool queueConstruction = false;
    bool refreshLegality = false;
    bool fullHeightViewport = false;
};

// One main-thread-authored construction-route candidate. Logic stores only a
// local preview node here; it emits no Build until the builder reaches the
// current node and revalidates present deterministic state.
struct SubmitLocalConstructionWaypointIntent final {
    engine::selection::LocalPlacementPreviewSnapshot placement;
    engine::CommandPosition anchorStart;
    engine::CommandPosition anchorEnd;
    bool hasDirection = false;
    bool forceAttackSnap = false;
};

struct ActivateCommandBarSlotIntent final {
    engine::session_query::InGameCommandActionToken token;
    uint8_t repeatCount = 1;
    bool queued = false;
};

struct CancelProductionQueueItemIntent final {
    engine::session_query::InGameProductionQueueActionToken token;
    uint8_t repeatCount = 1;
};

struct QueueResultActionIntent final {
    engine::GameResultAction action = engine::GameResultAction::Exit;
};

struct SetScriptPresentationPausedIntent final {
    bool paused = false;
};

struct SetLocalPauseSourceIntent final {
    engine::LocalPauseSource source = engine::LocalPauseSource::InGameMenu;
    bool paused = false;
};

struct ReconnectIntent final {};

struct CancelReconnectIntent final {};

struct ExitDisconnectedSessionIntent final {};

struct DismissScriptPopupIntent final {
    uint64_t presentationEpoch = 0;
    uint64_t sequence = 0;
};

struct AcknowledgeScriptCameraCompletionIntent final {
    engine::script::ScriptCameraPresentationCompletion completion;
};

// A local presentation watermark. It never enters the deterministic command
// stream; GameLogic uses it only to retire already-displayed receipts.
struct AcknowledgeCommandOutcomesIntent final {
    uint64_t revision = 0;
};

struct NotifyLoadingScreenPresentedIntent final {
    uint64_t loadingRevision = 0;
};

struct NotifyLoadingScreenDismissedIntent final {
    uint64_t loadingRevision = 0;
};

struct NotifyRenderStartupFrameSubmittedIntent final {
    uint64_t loadingRevision = 0;
    uint64_t sessionRevision = 0;
};

struct NotifyRenderStartupProgressIntent final {
    uint64_t loadingRevision = 0;
    uint64_t sessionRevision = 0;
    engine::StartupSceneProgress progress;
};

struct NotifyRenderStartupFailureIntent final {
    uint64_t loadingRevision = 0;
    uint64_t sessionRevision = 0;
    container::String error;
};

using GameLogicIntent = std::variant<
    StartGameIntent,
    ClearGameIntent,
    QueueCameraInputIntent,
    SubmitGameCommandIntent,
    SubmitRepairTargetIntent,
    SubmitContextualWorldCommandIntent,
    CancelPendingWorldCommandIntent,
    SubmitPendingWorldCommandTargetIntent,
    SubmitBeaconTextIntent,
    PurchaseScienceIntent,
    ActivateSpecialPowerShortcutIntent,
    ResetLocalSelectionIntent,
    ApplyLocalSelectionGestureIntent,
    SelectLocalOrderWaypointIntent,
    SelectLocalConstructionRouteNodeIntent,
    CancelSelectedLocalConstructionRouteNodeIntent,
    ApplyLocalControlGroupIntent,
    ApplyLocalSelectionShortcutIntent,
    SubmitScatterIntent,
    SubmitCreateFormationIntent,
    SetHoveredObjectIntent,
    CancelLocalPlacementIntent,
    UpdateLocalPlacementPointerIntent,
    SubmitLocalConstructionWaypointIntent,
    ActivateCommandBarSlotIntent,
    CancelProductionQueueItemIntent,
    QueueResultActionIntent,
    SetScriptPresentationPausedIntent,
    SetLocalPauseSourceIntent,
    ReconnectIntent,
    CancelReconnectIntent,
    ExitDisconnectedSessionIntent,
    DismissScriptPopupIntent,
    AcknowledgeScriptCameraCompletionIntent,
    AcknowledgeCommandOutcomesIntent,
    NotifyLoadingScreenPresentedIntent,
    NotifyLoadingScreenDismissedIntent,
    NotifyRenderStartupProgressIntent,
    NotifyRenderStartupFrameSubmittedIntent,
    NotifyRenderStartupFailureIntent>;

struct GameLogicIntentEnvelope final {
    uint64_t sequence = 0;
    // Zero is lifecycle/global. A non-zero revision prevents input sampled
    // for an old match from mutating a newly installed GameSession.
    uint64_t expectedSessionRevision = 0;
    GameLogicIntent intent;
};

struct GameLogicIntentStats final {
    uint64_t posted = 0;
    uint64_t applied = 0;
    uint64_t rejectedStaleSession = 0;
    uint64_t rejectedLifecycle = 0;
    uint64_t rejectedOverflow = 0;
};

class GameLogicIntentMailbox final {
public:
    [[nodiscard]] bool post(
        GameLogicIntent intent, uint64_t expectedSessionRevision);
    [[nodiscard]] std::optional<uint64_t> postTracked(
        GameLogicIntent intent, uint64_t expectedSessionRevision);

    // Logic-thread only. FIFO order is preserved and every accepted intent is
    // applied before GameLogic::update() starts the next deterministic tick.
    size_t drainAndApply(engine::GameLogic& gameLogic);

    void close() noexcept;
    void reset();
    [[nodiscard]] GameLogicIntentStats stats() const noexcept;
    [[nodiscard]] bool overflowed() const noexcept {
        return m_rejectedOverflow.load(std::memory_order_acquire) != 0;
    }

private:
    // Human/discrete actions are lossless and normally tiny. They must not
    // share a hard capacity with high-frequency pointer/camera samples: a
    // mouse-up selection or command confirmation cannot be retried after its
    // capture has ended. Continuous channels below are independently
    // coalesced to newest value, keeping this FIFO bounded by user actions in
    // practice without ever blocking the SDL thread.
    mutable std::mutex m_discreteMutex;
    container::Deque<GameLogicIntentEnvelope> m_discrete;
    bool m_closed = false;
    // Camera axes are newest-value state, while wheel/reset/absolute fields
    // are discrete deltas. Coalesce both under the ingress mutex via
    // GameCameraInput::accumulate instead of allowing a later zero-axis
    // sample to erase an unconsumed reset.
    std::optional<GameLogicIntentEnvelope> m_cameraInput;
    platform::runtime::LatestValueMailbox<GameLogicIntentEnvelope>
        m_hoverInput;
    platform::runtime::LatestValueMailbox<GameLogicIntentEnvelope>
        m_placementPointerInput;
    platform::runtime::LatestValueMailbox<GameLogicIntentEnvelope>
        m_startupProgressInput;
    std::atomic<uint64_t> m_nextSequence{1};
    std::atomic<uint64_t> m_posted{0};
    std::atomic<uint64_t> m_applied{0};
    std::atomic<uint64_t> m_rejectedStaleSession{0};
    std::atomic<uint64_t> m_rejectedLifecycle{0};
    std::atomic<uint64_t> m_rejectedOverflow{0};
};

} // namespace app::runtime
