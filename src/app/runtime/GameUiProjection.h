#pragma once

#include "core/container/container_types.h"
#include "core/platform/runtime_mailbox.h"
#include "app/runtime/GameLogic.h"
#include "game/command/CommandOutcome.h"
#include "game/session/query/MatchResultSnapshot.h"
#include "game/network/DisconnectContract.h"
#include "game/selection/PendingWorldCommandMode.h"
#include "presentation/render/RenderGameDataSettings.h"
#include "presentation/camera/GameCameraState.h"
#include "game/script/presentation/ScriptCinematicPresentationControls.h"
#include "presentation/ui/MappedImageContentLayer.h"
#include "presentation/ui/MapStringContentLayer.h"
#include "ScriptUiProjection.h"
#include "game/session/query/InGameCommandProjection.h"
#include "game/session/query/InGameCommandQuerySource.h"
#include "game/session/query/PlayerUiProjection.h"
#include "game/render/LocalPlacementPresentationState.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace app::runtime {

enum class WorldCursorHint : uint8_t {
    Normal,
    Move,
    AttackObject,
    EnterFriendly,
    EnterAggressive,
    GetHealed,
    DoRepair,
    ResumeConstruction,
    GetRepaired,
    Invalid,
};

// Immutable value projection consumed by main-thread input/WND code. It owns
// no GameSession, ECS entity, registry storage, terrain or content pointers.
struct GameUiProjection final {
    uint64_t revision = 0;
    uint64_t sessionRevision = 0;
    engine::GameState gameState = engine::GameState::Idle;
    bool hasSession = false;
    bool gameplayInputEnabled = true;
    engine::DisconnectStatus disconnect;

    container::SharedPtr<const engine::GameStartInfo> startInfo;

    engine::GameLoadingStage loadingStage = engine::GameLoadingStage::None;
    uint64_t loadingRevision = 0;
    float loadingProgress = 0.0f;
    container::String loadingStatus;
    container::String loadingError;
    engine::StartupSceneProgress startupSceneProgress;
    container::String loadingLastProgress;
    uint64_t loadingDeadlineRemainingMilliseconds = 0;

    uint64_t confirmedTick = 0;
    uint64_t presentationEpoch = 0;
    uint64_t audioPresentationEpoch = 0;
    // Immutable map.ini/solo.ini image overlay. The logic thread only
    // publishes source values; main-thread WND/texture code owns activation.
    container::SharedPtr<
        const container::Vector<engine::ui::MappedImageContentLayer>>
        mappedImageContentLayers;
    // Candidate-owned map.str source. Main-thread activation occurs only when
    // this projection becomes the committed presentation epoch.
    container::SharedPtr<const engine::ui::MapStringContentLayer>
        mapStringContentLayer;
    engine::RenderCameraGameData cameraInputSettings;
    engine::RenderInputGameData inputSettings;
    // Logic-owned camera pose copied into the immutable UI projection. Input
    // may use it for local view slots without reading GameSession from the
    // SDL thread or relying on a possibly older presented render frame.
    engine::GameCameraState cameraState;
    bool hasCameraState = false;
    math::vec3 cameraPlayableMinimum{};
    math::vec3 cameraPlayableMaximum{};
    bool hasCameraPlayableExtent = false;
    // Snapshot of the logic-owned director policy. Presentation may predict
    // only while this is true; raw input is still sent to logic so the
    // director remains the final authority.
    bool cameraManualInputAllowed = true;
    // Script/cinematic camera is the only in-session authority allowed to
    // take the displayed camera back from presentation-local manual control.
    bool cameraScriptMovementActive = false;
    int32_t cameraVisualSpeedMultiplier = 1;
    container::Vector<engine::script::ScriptCameraPresentationCommand>
        scriptCameraCommands;
    uint64_t scriptCameraCommandsTrimmedThroughSequence = 0;
    uint64_t scriptCameraMovementRevision = 0;
    float tacticalViewportHeightScale = 1.0f;
    ScriptUiProjection scriptUi;
    engine::session_query::InGameCommandProjection commandUi;
    engine::CommandOutcomeProjection commandOutcomes;
    engine::session_query::SciencePurchaseProjection sciencePurchase;
    engine::session_query::SpecialPowerShortcutProjection specialPowerShortcuts;
    engine::session_query::PlayerPowerProjection power;
    engine::session_query::PlayerMoneyProjection money;
    engine::RenderControlBarPowerGameData powerBarSettings;

    bool localPlacementActive = false;
    uint64_t localPlacementPreviewIdentity = 0;
    engine::selection::LocalPlacementPreviewSnapshot localPlacement;
    container::Vector<engine::InGameBuilderConstructionReadModel>
        localBuilders;
    container::Vector<engine::GameLogic::LocalConstructionRouteNodeProjection>
        localConstructionRouteNodes;
    std::optional<engine::GameLogic::LocalConstructionRouteNodeProjection>
        selectedLocalConstructionRouteNode;
    container::Vector<engine::ObjectId> localSelectedObjects;
    engine::selection::PendingWorldCommandMode pendingWorldCommand;
    engine::ObjectId hoveredObject = engine::INVALID_OBJECT_ID;
    WorldCursorHint worldCursorHint = WorldCursorHint::Normal;
    bool hoveredObjectForceAttackableBySelection = false;
    // Relation/existence gate for the active pending target mode. The
    // presentation picker supplies only an ObjectId; gameplay remains the
    // authority for whether that object is an admissible target.
    bool pendingHoveredObjectValid = false;
    // Detached authored GameText label for the confirmed object under the
    // pointer. Presentation localizes it; an empty label intentionally means
    // that no world-hover tooltip is shown.
    container::String hoveredObjectDisplayNameLabel;
    bool hasCommandBarSelection = false;
    engine::ObjectId selectedCommandBarObject = engine::INVALID_OBJECT_ID;
    container::String selectedCommandBarObjectType;

    uint64_t matchResultRevision = 0;
    container::SharedPtr<const engine::MatchResultSnapshot> matchResult;
    bool canResultNext = false;
    bool canResultRetry = false;
    container::String resultTransitionError;
    std::optional<int32_t> scriptFrameRateLimit;
    bool localFastForwardActive = false;

    [[nodiscard]] bool isInGame() const noexcept {
        return gameState == engine::GameState::Running;
    }

    [[nodiscard]] bool isGameDomain() const noexcept {
        return gameState == engine::GameState::Loading ||
            gameState == engine::GameState::Running ||
            gameState == engine::GameState::Paused ||
            gameState == engine::GameState::Result ||
            gameState == engine::GameState::Transitioning;
    }
};

class GameUiProjectionPublisher final {
public:
    [[nodiscard]] GameUiProjection build(
        engine::GameLogic& gameLogic);

private:
    uint64_t m_nextRevision = 1;
    uint64_t m_cachedStartSessionRevision = UINT64_MAX;
    uint64_t m_cachedStartLoadingRevision = UINT64_MAX;
    container::SharedPtr<const engine::GameStartInfo> m_cachedStartInfo;
    uint64_t m_cachedMatchResultRevision = UINT64_MAX;
    container::SharedPtr<const engine::MatchResultSnapshot> m_cachedMatchResult;
    ScriptUiProjectionPublisher m_scriptUiPublisher;
    engine::session_query::InGameCommandProjectionPublisher m_commandUiPublisher;
};

using GameUiProjectionMailbox =
    platform::runtime::LatestValueMailbox<GameUiProjection>;

} // namespace app::runtime
