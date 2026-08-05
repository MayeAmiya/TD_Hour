#pragma once

#include "core/container/container_types.h"
#include "presentation/camera/GameCameraInput.h"
#include "presentation/camera/GameCameraState.h"
#include "game/base/GameCameraDirector.h"
#include "game/script/presentation/ScriptCinematicPresentationControls.h"

#include <cstddef>
#include <cstdint>
#include <optional>

class RendererSubsystem;

namespace app::runtime {
struct GameUiProjection;
}

namespace app::input {

// Main/presentation-thread owner of the immediately displayed camera endpoint.
// Logic receives the same input for durable authority, but delayed simulation
// poses cannot overwrite a manually moving local view unless a script cut or
// session transition explicitly takes ownership.
class PresentationCameraInputState final {
public:
    explicit PresentationCameraInputState(RendererSubsystem& renderer)
        : m_renderer(renderer) {}

    void setProjection(const runtime::GameUiProjection& projection);
    void reset() noexcept;

    void saveView(size_t oneBasedSlot,
                  const engine::GameCameraState& fallback);
    [[nodiscard]] bool requestRestore(size_t oneBasedSlot);
    void applyPendingRestore(engine::GameCameraInput& input);

    [[nodiscard]] std::optional<
        engine::script::ScriptCameraPresentationCompletion> applyImmediate(
        const engine::GameCameraInput& input,
        const runtime::GameUiProjection& projection,
        float presentationDeltaSeconds);

private:
    RendererSubsystem& m_renderer;
    container::Array<std::optional<engine::GameCameraState>, 8> m_savedViews;
    std::optional<engine::GameCameraState> m_pendingRestore;
    std::optional<engine::GameCameraState> m_localCamera;
    engine::GameCameraDirector m_scriptDirector;
    engine::GameCameraState m_projectionBase;
    uint64_t m_sessionRevision = 0;
    uint64_t m_presentationEpoch = 0;
    uint64_t m_consumedScriptCameraSequence = 0;
    uint64_t m_latestScriptMovementRevision = 0;
    uint64_t m_acknowledgedScriptMovementRevision = 0;
    bool m_localDirty = false;
    bool m_overrideActive = false;
    bool m_scriptOverrideActive = false;
    bool m_scriptRigContinuous = false;
};

} // namespace app::input
