#pragma once

#include <d3d12.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace engine::d3d12 {

struct TextureSamplingQuality final {
    D3D12_FILTER filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    uint32_t maximumAnisotropy = 1;
    float maximumLod = D3D12_FLOAT32_MAX;
};

// Options.ini preserves RefCode's TextureFilter values: none/point,
// bilinear, trilinear and anisotropic.  AnisotropyLevel is meaningful only
// for the final mode; keeping that rule here gives every renderer-owned
// static sampler exactly the same interpretation.
[[nodiscard]] constexpr TextureSamplingQuality textureSamplingQuality(
    uint32_t textureFilter, uint32_t anisotropyLevel) noexcept {
    switch (textureFilter) {
    case 0:
        return {D3D12_FILTER_MIN_MAG_MIP_POINT, 1, 0.0f};
    case 1:
        return {D3D12_FILTER_MIN_MAG_MIP_POINT, 1, D3D12_FLOAT32_MAX};
    case 2:
        return {D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, 1,
                D3D12_FLOAT32_MAX};
    case 3:
        return {D3D12_FILTER_MIN_MAG_MIP_LINEAR, 1, D3D12_FLOAT32_MAX};
    default:
        return {D3D12_FILTER_ANISOTROPIC,
                std::clamp(anisotropyLevel, 2u, 16u),
                D3D12_FLOAT32_MAX};
    }
}

// RefCode stores AA as the actual multisample count, not a combo-box index.
[[nodiscard]] constexpr uint32_t requestedMultisampleCount(
    uint32_t antiAliasing) noexcept {
    if (antiAliasing >= 8u) return 8u;
    if (antiAliasing >= 4u) return 4u;
    if (antiAliasing >= 2u) return 2u;
    return 1u;
}

struct ReducedTextureMipRange final {
    size_t firstMip = 0;
    size_t mipCount = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// TextureReductionFactor is the requested number of authored top mips.
// RefCode's GameClient installs a 32-pixel minimum dimension, so reduction
// stops as soon as either axis reaches that floor; a short chain also always
// retains its final authored level.
[[nodiscard]] constexpr ReducedTextureMipRange reducedTextureMipRange(
    uint32_t width, uint32_t height, size_t mipCount,
    uint32_t reductionFactor, uint32_t minimumDimension = 32u) noexcept {
    if (width == 0 || height == 0 || mipCount == 0) return {};
    const size_t requested = std::min<size_t>(
        reductionFactor, mipCount - 1u);
    size_t first = 0;
    while (first < requested && width > minimumDimension &&
           height > minimumDimension) {
        width = std::max(1u, width / 2u);
        height = std::max(1u, height / 2u);
        ++first;
    }
    return {
        .firstMip = first,
        .mipCount = mipCount - first,
        .width = width,
        .height = height,
    };
}

// RefCode's TerrainTextureClass owns at most MIP_LEVELS_3. TextureReduction
// discards authored top levels from that fixed source range; it must not make
// three more levels below the selected one visible.
[[nodiscard]] constexpr ReducedTextureMipRange terrainColorTextureMipRange(
    uint32_t width, uint32_t height, size_t sourceMipCount,
    uint32_t reductionFactor) noexcept {
    constexpr size_t kTerrainSourceMipLimit = 3u;
    return reducedTextureMipRange(
        width, height, std::min(sourceMipCount, kTerrainSourceMipLimit),
        reductionFactor);
}

static_assert(requestedMultisampleCount(0) == 1);
static_assert(requestedMultisampleCount(2) == 2);
static_assert(requestedMultisampleCount(3) == 2);
static_assert(requestedMultisampleCount(8) == 8);
static_assert(reducedTextureMipRange(256, 128, 9, 1).width == 128);
static_assert(reducedTextureMipRange(2, 1, 2, 8).firstMip == 0);
static_assert(terrainColorTextureMipRange(256, 128, 9, 0).mipCount == 3);
static_assert(terrainColorTextureMipRange(256, 128, 9, 1).firstMip == 1);
static_assert(terrainColorTextureMipRange(256, 128, 9, 1).mipCount == 2);
static_assert(terrainColorTextureMipRange(256, 128, 9, 2).firstMip == 2);
static_assert(terrainColorTextureMipRange(256, 128, 9, 2).mipCount == 1);
static_assert(terrainColorTextureMipRange(256, 128, 9, 8).firstMip == 2);
static_assert(terrainColorTextureMipRange(256, 128, 2, 0).mipCount == 2);

[[nodiscard]] constexpr size_t modelUploadBudgetForFrame(
    bool worldPresentationStarted, size_t activePlayBudget,
    size_t loadingBudget) noexcept {
    return worldPresentationStarted ? activePlayBudget : loadingBudget;
}

static_assert(modelUploadBudgetForFrame(false, 1, 16) == 16);
static_assert(modelUploadBudgetForFrame(true, 1, 16) == 1);

} // namespace engine::d3d12
