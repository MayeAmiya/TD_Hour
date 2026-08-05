#pragma once

#include "core/container/container_types.h"
#include "game/render/LocalPlacementPresentationState.h"
#include "game/fx/runtime/GameFxEvents.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/script/presentation/ScriptCinematicPresentationControls.h"
#include "presentation/camera/GameCameraInput.h"
#include "presentation/camera/GameCameraState.h"
#include "presentation/render/RenderGameDataSettings.h"

#include <cstdint>
#include <optional>

namespace engine {

class GameSessionContentStartState;
class GameSessionFrameCommitState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

namespace selection {
class LocalSelectionState;
}

namespace script {
struct ScriptForceObjectSelectionPresentation;
struct ScriptMoveCameraToSelectionPresentation;
}

namespace render {
struct RenderAnimationCompletionFeedback;
}

struct GameSessionPresentationSnapshot final {
    uint64_t scriptEpoch = 0;
    uint64_t audioEpoch = 0;
    uint64_t fxEpoch = 0;
    container::SharedPtr<const RenderGameDataSettings> renderSettings;
    container::SharedPtr<const ResolvedRenderFeatureSnapshot> featureQuality;
    GameCameraState camera;
    math::vec3 cameraPlayableMinimum{};
    math::vec3 cameraPlayableMaximum{};
    selection::LocalPlacementPreviewSnapshot placement;
    std::optional<int32_t> frameRateLimit;
    int32_t gameSpeedFramesPerSecond = 30;
    int32_t visualSpeedMultiplier = 1;
    bool gameplayInputEnabled = true;
    bool cameraManualInputAllowed = true;
    bool cameraScriptMovementActive = false;
    bool hasCameraPlayableExtent = false;
    container::Vector<script::ScriptCameraPresentationCommand>
        scriptCameraCommands;
    // Highest source sequence removed from the bounded command journal.  A
    // presentation client that has not consumed through this boundary must
    // explicitly resynchronize instead of silently treating the retained tail
    // as a complete cinematic.
    uint64_t scriptCameraCommandsTrimmedThroughSequence = 0;
    uint64_t scriptCameraMovementRevision = 0;
    bool localPlacementActive = false;
    bool networkEnabled = false;
    bool localFastForwardActive = false;
};

// App-facing presentation capability. Reads are copied into one immutable
// value snapshot; writes are limited to camera input and renderer feedback.
class GameSessionPresentationPort final {
public:
    explicit GameSessionPresentationPort(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionFrameCommitState& frame) noexcept
        : m_content(content),
          m_world(world),
          m_presentation(presentation),
          m_publication(content, world, presentation, frame) {}

    [[nodiscard]] GameSessionPresentationSnapshot snapshot() const;
    void frameCameraOnTerrain() noexcept;
    void updateCameraSystems(float deltaSeconds);
    void queueCameraInput(const GameCameraInput& input) noexcept;
    [[nodiscard]] bool acknowledgeScriptCameraCompletion(
        const script::ScriptCameraPresentationCompletion& completion);
    [[nodiscard]] bool recordAnimationCompletion(
        const render::RenderAnimationCompletionFeedback& completion);
    [[nodiscard]] bool dismissPopup(
        uint64_t presentationEpoch,
        uint64_t presentationSequence) noexcept;
    [[nodiscard]] bool emitFxInvocation(game::FxInvocationEvent event);
    // Per-client unit speech: selection responses and order acknowledgements.
    // These are local presentation feedback, not authoritative gameplay, so
    // they enter the presentation audio journal directly and never travel
    // through a command, an ECS component, or the simulation event stream.
    // Emitting one cannot change confirmed state or SimulationRandom order.
    [[nodiscard]] bool emitLocalUnitVoice(
        container::StringView eventName, ObjectId object);
    [[nodiscard]] bool consumeForceObjectSelection(
        container::Span<
            const script::ScriptForceObjectSelectionPresentation> requests,
        selection::LocalSelectionState& selection,
        uint64_t confirmedTick);
    [[nodiscard]] bool consumeMoveCameraToSelection(
        container::Span<
            const script::ScriptMoveCameraToSelectionPresentation> requests,
        const selection::LocalSelectionState& selection,
        uint64_t confirmedTick);

private:
    [[nodiscard]] std::optional<ecs::entity> entityFromId(
        ObjectId object) const noexcept;

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionGameplayPublicationPort m_publication;
};

} // namespace engine
