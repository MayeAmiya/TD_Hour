#include "engine/renderer/world/pipeline/WorldRenderPipeline.h"
#include "engine/renderer/world/pipeline/WorldRenderPipelineMath.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/container/container_types.h"

namespace engine::render
{
bool TerrainMaterialRenderData::isValidFor(size_t sampleCount) const noexcept {
    if (baseTileIndices.size() != sampleCount ||
        blendTileIndices.size() != sampleCount ||
        extraBlendTileIndices.size() != sampleCount ||
        cliffInfoIndices.size() != sampleCount ||
        cliffCells.size() != sampleCount) {
        return false;
    }

    const auto validClass = [](const TerrainTextureClassRenderData& value) {
        // RefCode reserves a negative firstTile as a disabled texture-class
        // sentinel. It remains valid detached data and is skipped by the
        // renderer's selector lookup rather than being sent to VFS/GPU.
        return value.tileCount >= 0 &&
               (value.firstTile < 0 || value.tileWidth > 0);
    };
    const auto validClassRanges = [&validClass](
                                      const container::Vector<TerrainTextureClassRenderData>& values,
                                      int32_t availableTiles) {
        return availableTiles >= 0 && std::all_of(values.begin(), values.end(),
            [availableTiles, &validClass](const TerrainTextureClassRenderData& value) {
                if (!validClass(value)) return false;
                if (value.firstTile < 0) return true;
                return value.firstTile <= availableTiles &&
                       value.tileCount <= availableTiles - value.firstTile;
            });
    };
    if (bitmapTileCount <= 0 || textureClasses.empty() || blendDefinitions.empty() ||
        cliffDefinitions.empty() || !validClassRanges(textureClasses, bitmapTileCount) ||
        !validClassRanges(edgeTextureClasses, edgeTileCount)) {
        return false;
    }
    const auto validSelector = [](const container::Vector<int16_t>& values, size_t definitionCount) {
        return std::all_of(values.begin(), values.end(), [definitionCount](int16_t value) {
            return value >= 0 && static_cast<size_t>(value) < definitionCount;
        });
    };
    return validSelector(blendTileIndices, blendDefinitions.size()) &&
           validSelector(extraBlendTileIndices, blendDefinitions.size()) &&
           validSelector(cliffInfoIndices, cliffDefinitions.size()) &&
           std::all_of(blendDefinitions.begin(), blendDefinitions.end(), [this](
                           const TerrainBlendDefinitionRenderData& definition) {
               return definition.customEdgeTextureClass >= -1 &&
                      definition.customEdgeTextureClass <
                          static_cast<int32_t>(edgeTextureClasses.size());
           });
}

bool TerrainRenderSnapshot::isValid() const noexcept {
    if (width <= 0 || height <= 0 || cellWorldSize <= 0.0f || heightWorldScale <= 0.0f) return false;
    const size_t unsignedWidth = static_cast<size_t>(width);
    const size_t unsignedHeight = static_cast<size_t>(height);
    return unsignedWidth <= std::numeric_limits<size_t>::max() / unsignedHeight &&
           heights.size() == unsignedWidth * unsignedHeight;
}

bool TerrainRenderSnapshot::dirtyRegionSince(uint64_t appliedRevision,
                                             TerrainRenderDirtyRegion& output) const noexcept {
    output = {};
    if (!isValid() || appliedRevision >= revision || dirtyHistory.empty()) return false;

    const TerrainRenderDirtyRevision& first = dirtyHistory.front();
    const TerrainRenderDirtyRevision& last = dirtyHistory.back();
    if (first.revision == 0 || last.revision != revision || !first.region.isValid()) return false;

    // A retained journal beginning after the next revision needed by the GPU
    // cannot prove that a partial replacement includes every mutation. Do not
    // guess: let the caller rebuild the full visual in that case.
    if (appliedRevision < first.revision && first.revision - appliedRevision > 1) return false;

    uint64_t previousRevision = 0;
    for (const TerrainRenderDirtyRevision& entry : dirtyHistory) {
        if (entry.revision == 0 || !entry.region.isValid() ||
            (previousRevision != 0 && entry.revision != previousRevision + 1)) {
            return false;
        }
        if (entry.revision > appliedRevision) output.include(entry.region);
        previousRevision = entry.revision;
    }
    return output.isValid();
}

uint8_t TerrainRenderSnapshot::heightSample(int32_t x, int32_t y) const noexcept {
    if (!isValid() || x < 0 || y < 0 || x >= width || y >= height) return 0;
    return heights[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
}

float TerrainRenderSnapshot::heightWorld(int32_t x, int32_t y) const noexcept {
    return static_cast<float>(heightSample(x, y)) * heightWorldScale;
}

RenderVector TerrainRenderSnapshot::worldPosition(int32_t x, int32_t y) const noexcept {
    return {
        static_cast<float>(x - borderSize) * cellWorldSize,
        static_cast<float>(y - borderSize) * cellWorldSize,
        heightWorld(x, y),
    };
}

bool LocalVisibilityRenderSnapshot::isValid() const noexcept {
    if (!enabled) return false;
    if (revision == 0 || width <= 0 || height <= 0 ||
        !std::isfinite(cellWorldSize) || cellWorldSize <= 0.0f) {
        return false;
    }
    const size_t unsignedWidth = static_cast<size_t>(width);
    const size_t unsignedHeight = static_cast<size_t>(height);
    const container::Span<const uint8_t> values = cellValues();
    return unsignedWidth <= std::numeric_limits<size_t>::max() / unsignedHeight &&
           values.size() == unsignedWidth * unsignedHeight;
}

bool LocalVisibilityRenderSnapshot::hasPlayableBounds() const noexcept {
    return playableBoundsEnabled &&
        world_pipeline_detail::finiteVector(playableMinimum) &&
        world_pipeline_detail::finiteVector(playableMaximum) &&
        playableMinimum.x() <= playableMaximum.x() &&
        playableMinimum.y() <= playableMaximum.y();
}

bool LocalVisibilityRenderSnapshot::isInsidePlayableBounds(
    RenderVector position) const noexcept {
    if (!world_pipeline_detail::finiteVector(position)) return false;
    if (!hasPlayableBounds()) return true;
    return position.x() >= playableMinimum.x() &&
        position.y() >= playableMinimum.y() &&
        position.x() <= playableMaximum.x() &&
        position.y() <= playableMaximum.y();
}

LocalVisibilityRenderCellState LocalVisibilityRenderSnapshot::cellState(
    int32_t x, int32_t y) const noexcept {
    if (!isValid()) return LocalVisibilityRenderCellState::Visible;
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return LocalVisibilityRenderCellState::Shrouded;
    }
    const size_t offset = static_cast<size_t>(y) * static_cast<size_t>(width) +
                          static_cast<size_t>(x);
    const container::Span<const uint8_t> values = cellValues();
    return static_cast<LocalVisibilityRenderCellState>(
        std::min<uint8_t>(values[offset],
            static_cast<uint8_t>(LocalVisibilityRenderCellState::Visible)));
}

LocalVisibilityRenderCellState LocalVisibilityRenderSnapshot::worldState(
    RenderVector position) const noexcept {
    if (!world_pipeline_detail::finiteVector(position)) {
        return LocalVisibilityRenderCellState::Shrouded;
    }
    if (!isInsidePlayableBounds(position)) {
        return LocalVisibilityRenderCellState::Shrouded;
    }
    if (!isValid()) return LocalVisibilityRenderCellState::Visible;
    const double cellX = (static_cast<double>(position.x()) -
                          static_cast<double>(originX)) /
                         static_cast<double>(cellWorldSize);
    const double cellY = (static_cast<double>(position.y()) -
                          static_cast<double>(originY)) /
                         static_cast<double>(cellWorldSize);
    if (cellX < 0.0 || cellY < 0.0 ||
        cellX >= static_cast<double>(width) ||
        cellY >= static_cast<double>(height)) {
        return LocalVisibilityRenderCellState::Shrouded;
    }
    const int32_t x = static_cast<int32_t>(std::floor(cellX));
    const int32_t y = static_cast<int32_t>(std::floor(cellY));
    return cellState(x, y);
}

LocalVisibilityRenderCellState LocalVisibilityRenderSnapshot::worldStateSphere(
    RenderVector center, float radius) const noexcept {
    if (!world_pipeline_detail::finiteVector(center) ||
        !std::isfinite(radius)) {
        return LocalVisibilityRenderCellState::Shrouded;
    }
    // Object visibility follows its authoritative centre. A large object may
    // overlap the boundary while its centre remains legal, but a staging-area
    // object centred outside must never leak into the playable scene merely
    // because its conservative render sphere crosses the rectangle.
    if (!isInsidePlayableBounds(center)) {
        return LocalVisibilityRenderCellState::Shrouded;
    }
    if (!isValid()) return LocalVisibilityRenderCellState::Visible;
    if (radius <= 0.0f) return worldState(center);

    const double inverseCell = 1.0 / static_cast<double>(cellWorldSize);
    const double firstCellX = (static_cast<double>(center.x()) - radius -
                               static_cast<double>(originX)) * inverseCell;
    const double lastCellX = (static_cast<double>(center.x()) + radius -
                              static_cast<double>(originX)) * inverseCell;
    const double firstCellY = (static_cast<double>(center.y()) - radius -
                               static_cast<double>(originY)) * inverseCell;
    const double lastCellY = (static_cast<double>(center.y()) + radius -
                              static_cast<double>(originY)) * inverseCell;
    if (lastCellX < 0.0 || lastCellY < 0.0 ||
        firstCellX >= static_cast<double>(width) ||
        firstCellY >= static_cast<double>(height)) {
        return LocalVisibilityRenderCellState::Shrouded;
    }
    const int32_t firstX = std::max<int32_t>(
        0, static_cast<int32_t>(std::floor(std::max(firstCellX, 0.0))));
    const int32_t lastX = std::min<int32_t>(
        width - 1, static_cast<int32_t>(std::floor(std::min(
            lastCellX, static_cast<double>(width - 1)))));
    const int32_t firstY = std::max<int32_t>(
        0, static_cast<int32_t>(std::floor(std::max(firstCellY, 0.0))));
    const int32_t lastY = std::min<int32_t>(
        height - 1, static_cast<int32_t>(std::floor(std::min(
            lastCellY, static_cast<double>(height - 1)))));

    LocalVisibilityRenderCellState result =
        LocalVisibilityRenderCellState::Shrouded;
    for (int32_t y = firstY; y <= lastY; ++y) {
        for (int32_t x = firstX; x <= lastX; ++x) {
            const LocalVisibilityRenderCellState state = cellState(x, y);
            if (state == LocalVisibilityRenderCellState::Visible) return state;
            if (state == LocalVisibilityRenderCellState::Explored) result = state;
        }
    }
    return result;
}





} // namespace engine::render
