#pragma once

#include "core/container/hash_containers.h"
#include "engine/renderer/world/overlay/ObjectIconOverlayPresentation.h"
#include "presentation/render/RenderOverlaySnapshot.h"
#include "presentation/render/RenderViewSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace engine {
class Renderer;
class TextureManager;
}

namespace engine::render {

struct ObjectUiHealthColors final {
    uint32_t fill = 0xff00ff00u;
    uint32_t outline = 0xff007f00u;
};

[[nodiscard]] ObjectUiHealthColors objectUiHealthColors(
    const ObjectUiRenderSnapshot& object) noexcept;

[[nodiscard]] bool objectUiMayReveal(
    const ObjectUiRenderSnapshot& object) noexcept;

// Exact source Animation2D template names selected by Drawable::drawIconUI.
// Keeping the decision pure lets extraction/status regressions prove policy
// without constructing a VFS, texture manager, or graphics device.
[[nodiscard]] container::Vector<container::StringView>
objectUiStatusAnimations(
    const ObjectUiRenderSnapshot& object,
    uint64_t simulationFrame = 0);

// RefCode starts BombTimed at `numFrames - ceil(secondsRemaining) - 1`,
// clamped to the Animation2D template's last frame.  Null means this object
// has no live timed StickyBomb marker.
[[nodiscard]] std::optional<size_t> objectUiStickyBombTimedFrameIndex(
    const ObjectUiRenderSnapshot& object, uint64_t simulationFrame,
    uint32_t logicFramesPerSecond, size_t frameCount) noexcept;

struct ObjectUiHoverHit final {
    RenderEntityId objectId = 0;
    float distanceSquared = 0.0f;
};

// Client-local world picker for hover feedback. It consumes only the sealed
// render values and the same camera projection as drawing; shrouded and
// IGNORED_IN_GUI objects are rejected before any screen-space comparison.
[[nodiscard]] std::optional<ObjectUiHoverHit> objectUiHoverHitTest(
    const ObjectUiRenderState& state,
    const RenderCameraSnapshot& camera,
    const RenderViewportMetrics& viewport,
    math::vec2 pointer) noexcept;

// Renderer-local replacement for Drawable::drawIconUI.  The ground selection
// ring remains at the owner's indicator colour; Drawable's short selection
// flash belongs to SelectionFlashPresentation and tints the W3D model.
class ObjectUiOverlayPresentation final {
public:
    void reset() noexcept;
    [[nodiscard]] ObjectIconAnimationLibrary& animations() noexcept {
        return m_animations;
    }
    [[nodiscard]] size_t render(
        const ObjectUiRenderState& incoming,
        const RenderCameraSnapshot& camera,
        const RenderViewportMetrics& viewport,
        uint64_t simulationFrame,
        float interpolationAlpha,
        const ClientOptionsRenderState& acceptedOptions,
        engine::Renderer& renderer,
        engine::TextureManager& textures);

private:
    [[nodiscard]] const ObjectUiRenderState& consume(
        const ObjectUiRenderState& incoming, uint64_t simulationFrame);

    struct EndpointAnchors final {
        RenderVector worldPosition{};
        RenderVector captionAnchor{};
        RenderVector healthAnchor{};
        float worldRadius = 1.0f;
        float healthBoxWorldWidth = 40.0f;
    };

    ObjectUiRenderState m_accepted;
    container::HashMap<RenderEntityId, EndpointAnchors> m_previousAnchors;
    uint64_t m_acceptedSimulationFrame = 0;
    ObjectIconAnimationLibrary m_animations;
    bool m_initialized = false;
    bool m_animationLoadAttempted = false;
};

} // namespace engine::render
