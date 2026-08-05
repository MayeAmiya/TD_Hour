#include "engine/renderer/world/resource/WorldTextureCache.h"

#include <limits>
#include <utility>

namespace engine::render {

std::optional<container::Vector<uint8_t>> WorldTextureCache::buildTerrainAlphaEdgePixels(
    container::Span<const uint8_t> rgbaPixels, uint32_t width, uint32_t height) {
    const uint64_t byteCount = static_cast<uint64_t>(width) * height * 4u;
    if (width == 0 || height == 0 ||
        byteCount > std::numeric_limits<size_t>::max() ||
        byteCount > static_cast<uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
        rgbaPixels.size() != static_cast<size_t>(byteCount)) {
        return std::nullopt;
    }

    // TextureManager's TGA decoder normalizes data to top-to-bottom rows.
    // RefCode read the bottom-up source into TileData and then inverted each
    // tile while writing AlphaEdgeTextureClass' D3D texture, so preserving
    // this normalized row order is the equivalent direct-source operation.
    container::Vector<uint8_t> output(rgbaPixels.begin(), rgbaPixels.end());
    for (size_t offset = 0; offset < output.size(); offset += 4) {
        const uint8_t red = output[offset + 0];
        const uint8_t green = output[offset + 1];
        const uint8_t blue = output[offset + 2];
        output[offset + 3] = red == 0 && green == 0 && blue == 0
            ? 0x80
            : red == 0xff && green == 0xff && blue == 0xff ? 0x00 : 0xff;
    }
    return output;
}

std::optional<WorldTextureCache::TerrainColorMipPayload>
WorldTextureCache::buildTerrainColorMipPayload(
    container::Span<const uint8_t> rgbaPixels,
    uint32_t width,
    uint32_t height,
    uint32_t sourceTileGridWidth) {
    constexpr uint32_t kChannels = 4u;
    constexpr uint32_t kTerrainMipLevels = 3u;
    if (width == 0u || height == 0u || width != height ||
        sourceTileGridWidth == 0u ||
        sourceTileGridWidth > std::numeric_limits<uint32_t>::max() / 2u) {
        return std::nullopt;
    }

    const uint32_t selectorsPerAxis = sourceTileGridWidth * 2u;
    if (width % selectorsPerAxis != 0u ||
        height % selectorsPerAxis != 0u) {
        return std::nullopt;
    }
    const uint32_t selectorWidth = width / selectorsPerAxis;
    const uint32_t selectorHeight = height / selectorsPerAxis;
    // All selector boundaries must remain integral through mip2. This is the
    // classic 32px quadrant contract and is what makes every downsample
    // footprint stay inside its owning selector.
    constexpr uint32_t kDeepestMipScale = 1u << (kTerrainMipLevels - 1u);
    if (selectorWidth < kDeepestMipScale ||
        selectorHeight < kDeepestMipScale ||
        selectorWidth % kDeepestMipScale != 0u ||
        selectorHeight % kDeepestMipScale != 0u) {
        return std::nullopt;
    }

    const uint64_t sourceByteCount = static_cast<uint64_t>(width) *
        height * kChannels;
    if (sourceByteCount > std::numeric_limits<size_t>::max() ||
        sourceByteCount > static_cast<uint64_t>(
            std::numeric_limits<std::ptrdiff_t>::max()) ||
        rgbaPixels.size() != static_cast<size_t>(sourceByteCount)) {
        return std::nullopt;
    }

    TerrainColorMipPayload output;
    output.mips.reserve(kTerrainMipLevels);
    container::Vector<uint8_t> level(rgbaPixels.begin(), rgbaPixels.end());
    uint32_t levelWidth = width;
    uint32_t levelHeight = height;
    uint32_t levelSelectorWidth = selectorWidth;
    uint32_t levelSelectorHeight = selectorHeight;

    for (uint32_t mipLevel = 0; mipLevel < kTerrainMipLevels; ++mipLevel) {
        const uint64_t rowPitch64 = static_cast<uint64_t>(levelWidth) * kChannels;
        const uint64_t slicePitch64 = rowPitch64 * levelHeight;
        if (rowPitch64 > std::numeric_limits<uint32_t>::max() ||
            slicePitch64 > std::numeric_limits<uint32_t>::max() ||
            output.rgbaPixels.size() > std::numeric_limits<uint32_t>::max() ||
            slicePitch64 > std::numeric_limits<uint32_t>::max() -
                output.rgbaPixels.size() ||
            level.size() != static_cast<size_t>(slicePitch64)) {
            return std::nullopt;
        }
        output.mips.push_back({
            .width = levelWidth,
            .height = levelHeight,
            .byteOffset = static_cast<uint32_t>(output.rgbaPixels.size()),
            .rowPitch = static_cast<uint32_t>(rowPitch64),
            .slicePitch = static_cast<uint32_t>(slicePitch64),
        });
        output.rgbaPixels.insert(
            output.rgbaPixels.end(), level.begin(), level.end());
        if (mipLevel + 1u == kTerrainMipLevels) break;

        const uint32_t nextWidth = levelWidth / 2u;
        const uint32_t nextHeight = levelHeight / 2u;
        const uint32_t nextSelectorWidth = levelSelectorWidth / 2u;
        const uint32_t nextSelectorHeight = levelSelectorHeight / 2u;
        const uint64_t nextByteCount = static_cast<uint64_t>(nextWidth) *
            nextHeight * kChannels;
        if (nextByteCount > std::numeric_limits<size_t>::max()) {
            return std::nullopt;
        }
        container::Vector<uint8_t> next(
            static_cast<size_t>(nextByteCount));
        for (uint32_t selectorY = 0; selectorY < selectorsPerAxis; ++selectorY) {
            for (uint32_t selectorX = 0; selectorX < selectorsPerAxis; ++selectorX) {
                const uint32_t sourceOriginX = selectorX * levelSelectorWidth;
                const uint32_t sourceOriginY = selectorY * levelSelectorHeight;
                const uint32_t targetOriginX = selectorX * nextSelectorWidth;
                const uint32_t targetOriginY = selectorY * nextSelectorHeight;
                for (uint32_t y = 0; y < nextSelectorHeight; ++y) {
                    for (uint32_t x = 0; x < nextSelectorWidth; ++x) {
                        const uint32_t sourceX = sourceOriginX + x * 2u;
                        const uint32_t sourceY = sourceOriginY + y * 2u;
                        const size_t targetOffset =
                            (static_cast<size_t>(targetOriginY + y) * nextWidth +
                             targetOriginX + x) * kChannels;
                        for (uint32_t channel = 0; channel < kChannels; ++channel) {
                            const size_t topLeft =
                                (static_cast<size_t>(sourceY) * levelWidth +
                                 sourceX) * kChannels + channel;
                            const uint32_t sum =
                                level[topLeft] +
                                level[topLeft + kChannels] +
                                level[topLeft + static_cast<size_t>(levelWidth) * kChannels] +
                                level[topLeft + static_cast<size_t>(levelWidth) * kChannels +
                                      kChannels];
                            next[targetOffset + channel] =
                                static_cast<uint8_t>((sum + 2u) / 4u);
                        }
                    }
                }
            }
        }
        level = std::move(next);
        levelWidth = nextWidth;
        levelHeight = nextHeight;
        levelSelectorWidth = nextSelectorWidth;
        levelSelectorHeight = nextSelectorHeight;
    }
    return output;
}

} // namespace engine::render
