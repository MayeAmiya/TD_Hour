#pragma once

#include "core/container/hash_containers.h"

#include "presentation/render/RenderOverlaySnapshot.h"
#include "presentation/render/RenderViewSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <optional>
namespace engine {
class Renderer;
class TextureManager;
}

namespace engine::render {

// The source Anim2D modes are retained as data rather than instantiating the
// old global Anim2D/Drawable graph.  A frame is sampled solely from a sealed
// confirmed tick, so a slow or fast renderer cannot advance an emoticon at a
// different rate from the rest of the game presentation.
enum class ObjectIconAnimationMode : uint8_t {
    Once,
    OnceBackwards,
    Loop,
    LoopBackwards,
    PingPong,
    PingPongBackwards,
};

struct ObjectIconAnimationTemplate final {
    container::String name;
    container::Vector<container::String> mappedImageFrames;
    uint32_t delayMilliseconds = 0;
    ObjectIconAnimationMode mode = ObjectIconAnimationMode::Loop;
    bool randomizedStartFrame = false;
};

// Presentation-owned VFS parser for RefCode's `Data/INI/Animation2D/*`
// files. It deliberately resolves only names to MappedImage frames: texture
// ownership stays with the caller-provided TextureManager and no singleton
// Anim2D collection or game-state pointer is introduced.
class ObjectIconAnimationLibrary final {
public:
    void clear() noexcept;
    [[nodiscard]] bool loadFromVfs(container::StringView directory = "data/ini/animation2d");
    // Public for deterministic probes and future hot-reload tooling. As in
    // ZH, duplicate case-insensitive template names are rejected and the
    // first definition remains authoritative.
    [[nodiscard]] bool parseDefinitionText(container::StringView text);

    [[nodiscard]] const ObjectIconAnimationTemplate* find(
        container::StringView name) const;
    [[nodiscard]] std::optional<container::StringView> frameName(
        const ObjectIconRenderSnapshot& icon, uint64_t simulationFrame) const;
    [[nodiscard]] size_t size() const noexcept { return m_templates.size(); }

private:
    static container::String canonicalName(container::StringView value);

    container::HashMap<container::String, ObjectIconAnimationTemplate> m_templates;
};

// Pure client-side consumer for the object's `Drawable::drawIconUI` layer.
// It projects value-only anchors after the world pass, resolves a MappedImage
// through TextureManager, and queues normal 2D quads. No ECS, replay state,
// lockstep state, W3D asset or D3D12 object crosses into this class.
class ObjectIconOverlayPresentation final {
public:
    void reset() noexcept;
    [[nodiscard]] ObjectIconAnimationLibrary& animations() noexcept { return m_animations; }
    [[nodiscard]] const ObjectIconAnimationLibrary& animations() const noexcept { return m_animations; }

    // The returned count is the number of mapped-image frames submitted to
    // Renderer. `acceptedOptions` is already guarded by the renderer-side
    // client-options consumer; DRAWICON_UI therefore gates real draw calls,
    // rather than ending as a detached script bool.
    [[nodiscard]] size_t render(const ObjectIconRenderState& incoming,
                                const RenderCameraSnapshot& camera,
                                const RenderViewportMetrics& viewport,
                                uint64_t simulationFrame,
                                const ClientOptionsRenderState& acceptedOptions,
                                engine::Renderer& renderer,
                                engine::TextureManager& textures);
    [[nodiscard]] size_t renderWorldFeedback(
        const WorldFeedbackRenderState& incoming,
        const RenderCameraSnapshot& camera,
        const RenderViewportMetrics& viewport,
        uint64_t simulationFrame,
        const ClientOptionsRenderState& acceptedOptions,
        engine::Renderer& renderer,
        engine::TextureManager& textures);

    // Exposed for the no-GPU probe. Coordinates are in Renderer virtual
    // pixels; null means the anchor is behind the camera or outside clip
    // space.
    [[nodiscard]] static std::optional<math::vec2> projectWorldAnchor(
        const math::vec3& anchor, const RenderCameraSnapshot& camera,
        const RenderViewportMetrics& viewport) noexcept;

private:
    [[nodiscard]] const ObjectIconRenderState& consume(
        const ObjectIconRenderState& incoming, uint64_t simulationFrame);
    [[nodiscard]] const WorldFeedbackRenderState& consumeWorldFeedback(
        const WorldFeedbackRenderState& incoming, uint64_t simulationFrame);

    ObjectIconAnimationLibrary m_animations;
    ObjectIconRenderState m_accepted;
    ObjectIconRenderState m_unscoped;
    WorldFeedbackRenderState m_acceptedWorldFeedback;
    WorldFeedbackRenderState m_unscopedWorldFeedback;
    uint64_t m_acceptedSimulationFrame = 0;
    uint64_t m_acceptedWorldFeedbackFrame = 0;
    bool m_initialized = false;
    bool m_worldFeedbackInitialized = false;
    bool m_animationLoadAttempted = false;
};

} // namespace engine::render
