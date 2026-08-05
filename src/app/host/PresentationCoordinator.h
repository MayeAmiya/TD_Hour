#pragma once

#include "engine/renderer/ui/UiDrawList.h"
#include "core/ecs/ObjectId.h"
#include "presentation/render/RenderWorldDescriptorContracts.h"
#include "presentation/camera/GameCameraState.h"
#include "game/selection/LocalSelectionState.h"

#include <functional>
#include <cstdint>
#include <memory>
#include <optional>

class InGameGuiSubsystem;
class RendererSubsystem;

namespace engine {
class AudioSubsystem;
class TextureManager;
}

namespace app {
namespace runtime {
class GameLogicIntentMailbox;
struct GameUiProjection;
}

enum class PresentedWorldInputSurface : uint8_t {
    None,
    Terrain,
    Radar,
};

// One value-only pointer resolution used by both cursor presentation and
// command capture.  Radar is an input surface over the same world; callers
// must not independently choose a radar object and a terrain position.
struct PresentedWorldInputTarget final {
    PresentedWorldInputSurface surface = PresentedWorldInputSurface::None;
    std::optional<engine::render::RenderVector> position;
    engine::ObjectId object = engine::INVALID_OBJECT_ID;

    [[nodiscard]] explicit operator bool() const noexcept {
        return surface != PresentedWorldInputSurface::None &&
            position.has_value();
    }
};

struct PresentedOrderWaypointTarget final {
    engine::ObjectId actor = engine::INVALID_OBJECT_ID;
    uint32_t sourceSequence = 0;
    engine::selection::LocalOrderWaypointKind kind =
        engine::selection::LocalOrderWaypointKind::Move;

    [[nodiscard]] explicit operator bool() const noexcept {
        return actor && sourceSequence != 0;
    }
};

class PresentationCoordinator final {
public:
    using WorldInputOcclusionQuery =
        std::function<bool(float virtualX, float virtualY)>;

    PresentationCoordinator(RendererSubsystem& renderer,
                            engine::AudioSubsystem& audio,
                            runtime::GameLogicIntentMailbox& logicIntents);
    ~PresentationCoordinator();

    PresentationCoordinator(const PresentationCoordinator&) = delete;
    PresentationCoordinator& operator=(const PresentationCoordinator&) = delete;

    void configureDebugOptions();
    void setGameProjection(const runtime::GameUiProjection& projection);
    // Main-thread command-mode presentation. It changes cursor/path overlays
    // only; confirmed commands remain the sole simulation write path.
    void setWaypointMode(bool enabled) noexcept;
    // The presentation picker owns object projection, while the WND layer
    // owns authored parent-chain opacity. Bind the two without copying WND
    // trees into logic/render snapshots.
    void setWorldInputOcclusionQuery(WorldInputOcclusionQuery query);
    void extractAndSubmit(int frameCount);
    void admitExtractedWorldFrame();
    // Closes ordered world-frame ingress before ApplicationHost joins logic;
    // late shutdown extraction is rejected without touching presentation.
    void closeWorldFrameIngress() noexcept;
    // Pull renderer animation feedback before the next confirmed logic
    // update. Endpoint admission is valid during Loading; resource/natural
    // completion remains deferred until gameplay is live.
    void admitRenderAnimationFeedback();
    void admitAudioCompletions();
    void admitRenderStartupReadiness();
    void applyNonSessionRenderQuality();
    [[nodiscard]] engine::UiDrawList recordUi(
        engine::TextureManager& textureManager,
        InGameGuiSubsystem& inGameGui, bool debugWorldOnly);
    void renderRecordedUi(engine::TextureManager& textureManager,
                          const engine::UiDrawList& uiDrawList);
    void render(engine::TextureManager& textureManager,
                InGameGuiSubsystem& inGameGui, bool debugWorldOnly);

    [[nodiscard]] std::optional<engine::render::RenderVector> radarWorldAt(
        float screenX, float screenY) const;
    [[nodiscard]] engine::ObjectId radarObjectAt(
        float screenX, float screenY) const;
    [[nodiscard]] std::optional<engine::render::RenderVector>
    lastRadarEventWorld() const;
    [[nodiscard]] std::optional<engine::render::RenderVector> terrainWorldAt(
        float screenX, float screenY) const;
    [[nodiscard]] std::optional<math::vec2> projectWorldToVirtual(
        engine::render::RenderVector world) const;
    [[nodiscard]] engine::ObjectId selectableObjectAt(
        float screenX, float screenY) const;
    [[nodiscard]] std::optional<PresentedOrderWaypointTarget>
    orderWaypointAt(float screenX, float screenY) const;
    [[nodiscard]] engine::ObjectId targetableObjectAt(
        float screenX, float screenY, bool allowShrubbery,
        bool allowMines, bool forceAttack) const;
    [[nodiscard]] PresentedWorldInputTarget worldInputTargetAt(
        float screenX, float screenY, bool allowShrubbery,
        bool allowMines, bool forceAttack) const;
    [[nodiscard]] std::optional<engine::render::RenderVector>
    objectWorldPosition(engine::ObjectId object) const;
    [[nodiscard]] std::optional<engine::GameCameraState>
    presentedCamera() const;
    [[nodiscard]] container::Vector<engine::ObjectId>
    selectableObjectsInRectangle(float startX, float startY,
                                 float endX, float endY) const;
    [[nodiscard]] bool hasRadarInput() const noexcept;
    [[nodiscard]] bool worldOnlyPresentation() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace app
