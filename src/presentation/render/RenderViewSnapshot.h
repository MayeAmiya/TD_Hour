#pragma once

#include "presentation/render/RenderWorldDescriptorContracts.h"

namespace engine::render {

struct RenderCameraSnapshot {
    // This is complete camera value data for one sealed logic frame.  It has
    // no renderer, ECS, or GPU reference: every backend reconstructs its own
    // view/projection state from this immutable payload.
    //
    // Keep position/visibility first for source compatibility with early
    // aggregate users while giving a default snapshot the long-standing
    // diagnostic WorldCamera view.
    RenderVector position{5.0f, -7.0f, 4.0f};
    // Preserve the established zero = unbounded culling behavior for ad-hoc
    // snapshots. GameCameraState and the debug producer both set 2000.
    float visibilityDistance = 0.0f;
    RenderVector target{0.0f, 0.0f, 0.0f};
    RenderVector up{0.0f, 0.0f, 1.0f};
    float verticalFovRadians = math::deg_to_rad(60.0f);
    float horizontalFovRadians = 0.0f;
    float tacticalViewportHeightScale = 1.0f;
    float nearClip = 0.1f;
    float farClip = 2000.0f;

    // Environment data is copied with the frame rather than read live from a
    // map or game object while the renderer consumes the snapshot.
    bool fogEnabled = false;
    RenderVector fogColor{};
    float fogStartDistance = 0.0f;
    float fogEndDistance = 0.0f;
    uint64_t cameraCutRevision = 0;
};

[[nodiscard]] inline float renderCameraEffectiveAspectRatio(
    const RenderCameraSnapshot& camera, float fullViewportAspectRatio) noexcept {
    const float fullAspect = std::isfinite(fullViewportAspectRatio) &&
            fullViewportAspectRatio > math::EPSILON
        ? fullViewportAspectRatio : 4.0f / 3.0f;
    const float heightScale = std::isfinite(camera.tacticalViewportHeightScale)
        ? std::clamp(camera.tacticalViewportHeightScale, 0.1f, 1.0f) : 1.0f;
    return fullAspect / heightScale;
}

[[nodiscard]] inline float renderCameraVerticalFovRadians(
    const RenderCameraSnapshot& camera, float fullViewportAspectRatio) noexcept {
    const float aspect = renderCameraEffectiveAspectRatio(
        camera, fullViewportAspectRatio);
    if (std::isfinite(camera.horizontalFovRadians) &&
        camera.horizontalFovRadians > 0.0f) {
        const float horizontal = std::clamp(
            camera.horizontalFovRadians, 0.01f, math::PI - 0.01f);
        return 2.0f * std::atan(std::tan(horizontal * 0.5f) / aspect);
    }
    return std::isfinite(camera.verticalFovRadians)
        ? std::clamp(camera.verticalFovRadians, 0.01f, math::PI - 0.01f)
        : math::deg_to_rad(60.0f);
}

// Backend-neutral projection of the legacy CAMERA_FADE_* screen blend. It is
// intentionally a world-frame value rather than UI state: RefCode applies
// the quad after tactical-world/object-icon drawing but before ControlBar and
// other InGameUI work. `intensity` is the legacy 8-bit diffuse component
// (floor(255 * authored curve), clamped at the safe UNORM edge).
struct WorldPreparationStamp final {
    uint64_t worldRevision = 0;
    uint64_t simulationFrame = 0;
    uint64_t presentationEpoch = 0;
    uint64_t sessionRevision = 0;
    uint64_t loadingRevision = 0;
};

// One displayed frame spans three coordinate spaces: D3D renders in physical
// pixels, SDL reports pointer positions in logical window pixels, and the
// classic UI draws in virtual pixels.  Keep all three extents beside the
// displayed camera so projection and picking cannot silently choose different
// aspect ratios or DPI conversions.
struct RenderViewportMetrics final {
    uint32_t pixelWidth = 4;
    uint32_t pixelHeight = 3;
    uint32_t logicalWidth = 4;
    uint32_t logicalHeight = 3;
    float virtualWidth = 4.0f;
    float virtualHeight = 3.0f;

    [[nodiscard]] bool valid() const noexcept {
        return pixelWidth != 0u && pixelHeight != 0u &&
            logicalWidth != 0u && logicalHeight != 0u &&
            std::isfinite(virtualWidth) && std::isfinite(virtualHeight) &&
            virtualWidth > math::EPSILON && virtualHeight > math::EPSILON;
    }

    [[nodiscard]] float fullAspectRatio() const noexcept {
        return pixelHeight != 0u
            ? static_cast<float>(pixelWidth) /
                  static_cast<float>(pixelHeight)
            : 4.0f / 3.0f;
    }

    [[nodiscard]] math::vec2 logicalToVirtual(
        math::vec2 logical) const noexcept {
        if (!valid()) return logical;
        return {
            logical.x() * virtualWidth /
                static_cast<float>(logicalWidth),
            logical.y() * virtualHeight /
                static_cast<float>(logicalHeight),
        };
    }
};

// Renderer-owned view sample. It carries no ECS/session pointer and may change
// many times while the referenced PreparedWorldState remains unchanged.
struct RenderViewState final {
    WorldPreparationStamp sourceWorld;
    uint64_t viewRevision = 0;
    uint64_t worldARevision = 0;
    uint64_t worldBRevision = 0;
    RenderCameraSnapshot camera;
    RenderViewportMetrics viewport;
    float interpolationAlpha = 1.0f;
};

// Main/input-thread camera motion is presentation-local and can advance while
// simulation is paused.  Keep it in a separate newest-value channel instead
// of reusing RenderViewState::viewRevision: world endpoints and local camera
// samples have different producers and therefore cannot share one monotonic
// revision counter.
struct PresentationCameraOverride final {
    WorldPreparationStamp sourceWorld;
    RenderCameraSnapshot camera;
    // Smooth script moves deliberately retain their camera-cut generation,
    // so a cut revision cannot prove that the immutable base view contains
    // the completed pose.  Keep the final override until the base camera
    // itself matches this complete endpoint.
    bool releaseWhenBaseMatches = false;
    bool active = false;
};


} // namespace engine::render
