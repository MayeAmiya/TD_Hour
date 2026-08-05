#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::render::world_renderer_detail {

inline constexpr uint32_t kMaximumSkinBones = 256;
inline constexpr float kW3dAlphaTestCutoff = 0x60 / 255.0f;

// Shared IA instance ABI for the main static-mesh and directional-shadow
// passes. Keep this private to the renderer pipeline: it is a GPU upload
// contract, not a presentation snapshot or a game-facing mesh type.
struct alignas(16) StaticMeshGpuInstance final {
    float world[16];
    float directionalLightScale = 1.0f;
    float scriptFlashTint[3]{};
    float scriptIndicatorColor[3]{};
    uint32_t houseColorFlags = 0;
    float heatVisionIntensity = 0.0f;
    uint32_t heatVisionMode = 0;
    float objectOpacity = 1.0f;
    uint32_t heatVisionPadding = 0;
    float treePushAsideDirection[2]{};
    float treePushAsideAmount = 0.0f;
    float treePushAsideDistanceFactor = 0.0f;
    float treePushAsideDarkeningFactor = 0.0f;
    float treePushAsidePadding[3]{};
    float previousWorld[16];
    float interpolationAlpha = 1.0f;
    float interpolationPadding[3]{};
};

static_assert(offsetof(StaticMeshGpuInstance, world) == 0);
static_assert(offsetof(StaticMeshGpuInstance, directionalLightScale) == 64);
static_assert(offsetof(StaticMeshGpuInstance, scriptFlashTint) == 68);
static_assert(offsetof(StaticMeshGpuInstance, scriptIndicatorColor) == 80);
static_assert(offsetof(StaticMeshGpuInstance, houseColorFlags) == 92);
static_assert(offsetof(StaticMeshGpuInstance, heatVisionIntensity) == 96);
static_assert(offsetof(StaticMeshGpuInstance, heatVisionMode) == 100);
static_assert(offsetof(StaticMeshGpuInstance, objectOpacity) == 104);
static_assert(offsetof(StaticMeshGpuInstance, treePushAsideDirection) == 112);
static_assert(offsetof(StaticMeshGpuInstance, treePushAsideDarkeningFactor) == 128);
static_assert(offsetof(StaticMeshGpuInstance, previousWorld) == 144);
static_assert(offsetof(StaticMeshGpuInstance, interpolationAlpha) == 208);
static_assert(sizeof(StaticMeshGpuInstance) == 224);

} // namespace engine::render::world_renderer_detail
