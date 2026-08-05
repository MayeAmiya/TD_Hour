#include "core/container/container_types.h"
#include "core/math/fixed/q32_32.h"
#include "TerrainMap.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace game::terrain {
namespace {

bool finite(float value) noexcept {
    return std::isfinite(value);
}

constexpr int32_t kMaxLogicalBoundaryExtent = 1'000'000;
// Keep enough individual mutations for a renderer that is temporarily behind
// the simulation, without allowing repeated explosions/deformations to grow
// logic memory without bound. A consumer older than this window explicitly
// takes the full-upload path.
constexpr size_t kMaxDirtyHistoryEntries = 256;

using Fixed = math::q32_32;
constexpr int64_t kFixedOneRaw = int64_t{1} << 32u;

[[nodiscard]] int64_t fixedFloorToInteger(Fixed value) noexcept {
    int64_t quotient = value.raw() / kFixedOneRaw;
    if (value.raw() < 0 && value.raw() % kFixedOneRaw != 0) --quotient;
    return quotient;
}

[[nodiscard]] int64_t fixedCeilToInteger(Fixed value) noexcept {
    int64_t quotient = value.raw() / kFixedOneRaw;
    if (value.raw() > 0 && value.raw() % kFixedOneRaw != 0) ++quotient;
    return quotient;
}

[[nodiscard]] int32_t fixedRoundToInt32(Fixed value) noexcept {
    constexpr uint64_t kHalf = uint64_t{1} << 31u;
    const int64_t raw = value.raw();
    const bool negative = raw < 0;
    const uint64_t magnitude = negative
        ? static_cast<uint64_t>(-(raw + 1)) + uint64_t{1}
        : static_cast<uint64_t>(raw);
    const uint64_t roundedMagnitude = (magnitude + kHalf) >> 32u;
    const int64_t rounded = negative
        ? -static_cast<int64_t>(roundedMagnitude)
        : static_cast<int64_t>(roundedMagnitude);
    return static_cast<int32_t>(std::clamp<int64_t>(
        rounded, std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max()));
}

bool isRepresentableBoundary(const TerrainBoundary& boundary) noexcept {
    return boundary.width >= -kMaxLogicalBoundaryExtent &&
           boundary.width <= kMaxLogicalBoundaryExtent &&
           boundary.height >= -kMaxLogicalBoundaryExtent &&
           boundary.height <= kMaxLogicalBoundaryExtent;
}

bool isUsableBoundary(const TerrainBoundary& boundary) noexcept {
    return boundary.width > 0 && boundary.height > 0;
}

} // namespace

void TerrainDirtyRegion::include(int32_t x, int32_t y) noexcept {
    if (!isValid()) {
        minX = maxX = x;
        minY = maxY = y;
        return;
    }
    minX = std::min(minX, x);
    minY = std::min(minY, y);
    maxX = std::max(maxX, x);
    maxY = std::max(maxY, y);
}

void TerrainDirtyRegion::include(TerrainDirtyRegion region) noexcept {
    if (!region.isValid()) return;
    include(region.minX, region.minY);
    include(region.maxX, region.maxY);
}

bool TerrainMap::load(TerrainHeightfieldData heightfield) {
    if (!heightfield.isValid()) return false;

    if (heightfield.borderSize < 0 || heightfield.borderSize > heightfield.width ||
        heightfield.borderSize > heightfield.height || heightfield.boundaries.empty()) {
        return false;
    }
    for (const TerrainBoundary& boundary : heightfield.boundaries) {
        // v4 boundaries are script-selectable logical map extents.  RefCode
        // keeps zero/negative placeholders and permits values which are not
        // a strict subset of the physical height samples.  Preserve that
        // disk semantics; sampleSurface/cellAt still clamp independently to
        // the actual heightfield before any array access.
        if (!isRepresentableBoundary(boundary)) return false;
    }

    const bool blendTilesValid = heightfield.blendTiles &&
        heightfield.blendTiles->isValidFor(heightfield.heights.size());
    m_heightfield = std::move(heightfield);
    m_blendTilesValid = blendTilesValid;
    // Keep index zero when it is usable (the legacy default), otherwise use
    // the first positive entry. Campaign maps can reserve leading boundary
    // slots with -1/0 sentinels for scripts that later switch the boundary.
    m_activeBoundary = 0;
    for (size_t index = 0; index < m_heightfield.boundaries.size(); ++index) {
        if (isUsableBoundary(m_heightfield.boundaries[index])) {
            m_activeBoundary = index;
            break;
        }
    }
    recalculateHeightRange();
    ++m_revision;
    if (m_revision == 0) ++m_revision;
    ++m_layoutRevision;
    if (m_layoutRevision == 0) ++m_layoutRevision;
    m_dirtyHistory.clear();
    m_dirtyRegion.clear();
    appendDirtyRevision({0, 0, m_heightfield.width - 1, m_heightfield.height - 1});
    return true;
}

bool TerrainMap::loadFromFile(container::StringView path, container::String* error) {
    MapHeightfieldLoader loader;
    if (!loader.loadFromFile(path)) {
        if (error) *error = loader.error();
        return false;
    }
    if (!load(loader.takeResult())) {
        if (error) *error = "Loaded map heightfield is invalid";
        return false;
    }
    if (error) error->clear();
    return true;
}

void TerrainMap::clear() noexcept {
    m_heightfield = {};
    m_blendTilesValid = false;
    m_activeBoundary = 0;
    m_minHeight = 0.0f;
    m_maxHeight = 0.0f;
    m_heightSampleCounts.fill(0u);
    m_dirtyRegion.clear();
    m_dirtyHistory.clear();
    m_heightMutationBatchDepth = 0;
    m_batchedHeightDirty.clear();
    ++m_revision;
    if (m_revision == 0) ++m_revision;
    ++m_layoutRevision;
    if (m_layoutRevision == 0) ++m_layoutRevision;
}

bool TerrainMap::setHeightSample(int32_t x, int32_t y, uint8_t sample) noexcept {
    if (!isLoaded() || x < 0 || y < 0 || x >= m_heightfield.width || y >= m_heightfield.height) {
        return false;
    }
    const size_t index = static_cast<size_t>(y) * static_cast<size_t>(m_heightfield.width) +
                         static_cast<size_t>(x);
    if (m_heightfield.heights[index] == sample) return false;
    replaceHeightSample(index, sample);
    markHeightMutation({x, y, x, y});
    return true;
}

bool TerrainMap::deformCircle(float worldX, float worldY, float radiusWorld,
                               float heightDeltaWorld) noexcept {
    // Thin conversion shell over the raw form. Simulation callers must not
    // route height mutation through float, because the derivation of these
    // arguments would then decide the resulting samples per machine.
    if (!finite(worldX) || !finite(worldY) || !finite(radiusWorld) ||
        !finite(heightDeltaWorld)) {
        return false;
    }
    return deformCircleRaw(
        Fixed(worldX).raw(), Fixed(worldY).raw(), Fixed(radiusWorld).raw(),
        Fixed(heightDeltaWorld).raw());
}

bool TerrainMap::deformCircleRaw(int64_t worldXRaw, int64_t worldYRaw,
                                 int64_t radiusWorldRaw,
                                 int64_t heightDeltaWorldRaw) noexcept {
    if (!isLoaded() || radiusWorldRaw <= 0 || heightDeltaWorldRaw == 0) {
        return false;
    }

    const Fixed cellWorldSize(kMapCellWorldSize);
    const Fixed heightWorldScale(kMapHeightWorldScale);
    const Fixed centerWorldX = Fixed::from_raw(worldXRaw);
    const Fixed centerWorldY = Fixed::from_raw(worldYRaw);
    const Fixed radius = Fixed::from_raw(radiusWorldRaw);
    const Fixed heightDelta = Fixed::from_raw(heightDeltaWorldRaw);
    const Fixed centerX = centerWorldX / cellWorldSize +
        Fixed(m_heightfield.borderSize);
    const Fixed centerY = centerWorldY / cellWorldSize +
        Fixed(m_heightfield.borderSize);
    const int64_t radiusSamples = fixedCeilToInteger(radius / cellWorldSize);
    const int32_t minX = static_cast<int32_t>(std::clamp<int64_t>(
        fixedFloorToInteger(centerX) - radiusSamples, 0,
        m_heightfield.width - 1));
    const int32_t minY = static_cast<int32_t>(std::clamp<int64_t>(
        fixedFloorToInteger(centerY) - radiusSamples, 0,
        m_heightfield.height - 1));
    const int32_t maxX = static_cast<int32_t>(std::clamp<int64_t>(
        fixedCeilToInteger(centerX) + radiusSamples, 0,
        m_heightfield.width - 1));
    const int32_t maxY = static_cast<int32_t>(std::clamp<int64_t>(
        fixedCeilToInteger(centerY) + radiusSamples, 0,
        m_heightfield.height - 1));
    const Fixed radiusSquared = radius * radius;

    TerrainDirtyRegion changed;
    for (int32_t y = minY; y <= maxY; ++y) {
        for (int32_t x = minX; x <= maxX; ++x) {
            const Fixed sampleWorldX =
                Fixed(x - m_heightfield.borderSize) * cellWorldSize;
            const Fixed sampleWorldY =
                Fixed(y - m_heightfield.borderSize) * cellWorldSize;
            const Fixed deltaX = sampleWorldX - centerWorldX;
            const Fixed deltaY = sampleWorldY - centerWorldY;
            const Fixed distanceSquared = deltaX * deltaX + deltaY * deltaY;
            if (distanceSquared > radiusSquared) continue;
            const Fixed distance = Fixed::sqrt(distanceSquared);
            const Fixed falloff = Fixed(1) - distance / radius;
            const int32_t delta = fixedRoundToInt32(
                heightDelta * falloff / heightWorldScale);
            if (delta == 0) continue;

            const size_t index = static_cast<size_t>(y) * static_cast<size_t>(m_heightfield.width) +
                                 static_cast<size_t>(x);
            const int32_t updated = std::clamp(static_cast<int32_t>(m_heightfield.heights[index]) + delta, 0, 255);
            if (updated == m_heightfield.heights[index]) continue;
            replaceHeightSample(index, static_cast<uint8_t>(updated));
            changed.include(x, y);
        }
    }
    if (!changed.isValid()) return false;
    markHeightMutation(changed);
    return true;
}

TerrainFlattenResult TerrainMap::flattenFootprintRaw(
    const TerrainFlattenFootprint& footprint) noexcept {
    TerrainFlattenResult result;
    if (!isLoaded()) return result;

    const Fixed centerX = Fixed::from_raw(footprint.centerXRaw);
    const Fixed centerY = Fixed::from_raw(footprint.centerYRaw);
    const Fixed majorRadius = Fixed::from_raw(footprint.majorRadiusRaw);
    const Fixed minorRadius = footprint.shape ==
            TerrainFlattenShape::OrientedBox
        ? Fixed::from_raw(footprint.minorRadiusRaw)
        : majorRadius;
    if (majorRadius <= Fixed{} || minorRadius <= Fixed{}) return result;

    result.evaluated = true;
    result.centerHeightRaw = groundHeightRaw(
        footprint.centerXRaw, footprint.centerYRaw, true);
    result.flattenedPlaneHeightRaw = result.centerHeightRaw;
    if (footprint.isSmall) return result;

    const Fixed cellWorldSize{int32_t{10}};
    const math::q32_32_sincos rotation = math::fixed_sincos(
        Fixed::from_raw(footprint.yawRadiansRaw));
    Fixed extentX = majorRadius;
    Fixed extentY = majorRadius;
    if (footprint.shape == TerrainFlattenShape::OrientedBox) {
        extentX = Fixed::abs(majorRadius * rotation.cosine) +
            Fixed::abs(minorRadius * rotation.sine);
        extentY = Fixed::abs(majorRadius * rotation.sine) +
            Fixed::abs(minorRadius * rotation.cosine);
    }

    const auto sampleCoordinate = [&](Fixed world) noexcept {
        return fixedFloorToInteger(world / cellWorldSize) +
            m_heightfield.borderSize;
    };
    const int32_t minimumX = static_cast<int32_t>(std::clamp<int64_t>(
        sampleCoordinate(centerX - extentX), 0,
        m_heightfield.width - 1));
    const int32_t minimumY = static_cast<int32_t>(std::clamp<int64_t>(
        sampleCoordinate(centerY - extentY), 0,
        m_heightfield.height - 1));
    const int32_t maximumX = static_cast<int32_t>(std::clamp<int64_t>(
        sampleCoordinate(centerX + extentX), 0,
        m_heightfield.width - 1));
    const int32_t maximumY = static_cast<int32_t>(std::clamp<int64_t>(
        sampleCoordinate(centerY + extentY), 0,
        m_heightfield.height - 1));

    const Fixed radiusSquared = majorRadius * majorRadius;
    const auto contains = [&](int32_t x, int32_t y) noexcept {
        const Fixed worldX =
            Fixed{x - m_heightfield.borderSize} * cellWorldSize;
        const Fixed worldY =
            Fixed{y - m_heightfield.borderSize} * cellWorldSize;
        const Fixed deltaX = worldX - centerX;
        const Fixed deltaY = worldY - centerY;
        if (footprint.shape == TerrainFlattenShape::Circle) {
            // RefCode uses a strict radius test for sphere/cylinder geometry.
            return deltaX * deltaX + deltaY * deltaY < radiusSquared;
        }
        const Fixed localX = deltaX * rotation.cosine +
            deltaY * rotation.sine;
        const Fixed localY = Fixed{} - deltaX * rotation.sine +
            deltaY * rotation.cosine;
        return Fixed::abs(localX) <= majorRadius &&
            Fixed::abs(localY) <= minorRadius;
    };

    uint64_t sampleSum = 0;
    uint64_t sampleCount = 0;
    for (int32_t y = minimumY; y <= maximumY; ++y) {
        for (int32_t x = minimumX; x <= maximumX; ++x) {
            if (!contains(x, y)) continue;
            const size_t index = static_cast<size_t>(y) *
                static_cast<size_t>(m_heightfield.width) +
                static_cast<size_t>(x);
            sampleSum += m_heightfield.heights[index];
            ++sampleCount;
        }
    }
    if (sampleCount == 0) return result;

    const Fixed heightScale = Fixed::from_fraction(10, 16);
    uint64_t targetSample =
        (sampleSum + sampleCount / 2u) / sampleCount;
    if (footprint.shape == TerrainFlattenShape::OrientedBox) {
        const int64_t centerSample = fixedFloorToInteger(
            Fixed::from_raw(result.centerHeightRaw) / heightScale);
        targetSample = std::min<uint64_t>(
            targetSample,
            static_cast<uint64_t>(std::clamp<int64_t>(
                centerSample, 0, 255)));
    }
    const uint8_t target = static_cast<uint8_t>(
        std::min<uint64_t>(targetSample, 255u));
    result.flattenedPlaneHeightRaw =
        (Fixed{static_cast<int32_t>(target)} * heightScale).raw();

    TerrainDirtyRegion changed;
    for (int32_t y = minimumY; y <= maximumY; ++y) {
        for (int32_t x = minimumX; x <= maximumX; ++x) {
            if (!contains(x, y)) continue;
            for (int32_t offsetY = -1; offsetY <= 1; ++offsetY) {
                const int32_t neighborY = y + offsetY;
                if (neighborY < 0 || neighborY >= m_heightfield.height)
                    continue;
                for (int32_t offsetX = -1; offsetX <= 1; ++offsetX) {
                    const int32_t neighborX = x + offsetX;
                    if (neighborX < 0 || neighborX >= m_heightfield.width)
                        continue;
                    const size_t index = static_cast<size_t>(neighborY) *
                        static_cast<size_t>(m_heightfield.width) +
                        static_cast<size_t>(neighborX);
                    // W3DTerrainVisual::setRawMapHeight only lowers the
                    // golden logic heightmap; flattening never raises terrain.
                    if (m_heightfield.heights[index] <= target) continue;
                    replaceHeightSample(index, target);
                    changed.include(neighborX, neighborY);
                }
            }
        }
    }
    if (changed.isValid()) {
        markHeightMutation(changed);
        result.changed = true;
        result.centerHeightRaw = groundHeightRaw(
            footprint.centerXRaw, footprint.centerYRaw, true);
    }
    return result;
}

bool TerrainMap::setActiveBoundary(size_t index) noexcept {
    if (index >= m_heightfield.boundaries.size() ||
        !isUsableBoundary(m_heightfield.boundaries[index])) {
        return false;
    }
    m_activeBoundary = index;
    return true;
}

TerrainExtent TerrainMap::makeExtent(int32_t gridWidth, int32_t gridHeight,
                                     int32_t originX, int32_t originY) const noexcept {
    if (!isLoaded() || gridWidth <= 0 || gridHeight <= 0) return {};

    // RefCode stores boundary dimensions as terrain-grid (cell) counts.  A
    // width of N spans [0, N * cellSize], rather than ending at vertex N-1.
    // This also makes the including-border extent agree with W3DTerrainLogic.
    const int64_t minGridX = static_cast<int64_t>(originX) - m_heightfield.borderSize;
    const int64_t minGridY = static_cast<int64_t>(originY) - m_heightfield.borderSize;
    const int64_t maxGridX = static_cast<int64_t>(originX) + gridWidth - m_heightfield.borderSize;
    const int64_t maxGridY = static_cast<int64_t>(originY) + gridHeight - m_heightfield.borderSize;
    const float minX = static_cast<float>(minGridX) * kMapCellWorldSize;
    const float minY = static_cast<float>(minGridY) * kMapCellWorldSize;
    const float maxX = static_cast<float>(maxGridX) * kMapCellWorldSize;
    const float maxY = static_cast<float>(maxGridY) * kMapCellWorldSize;
    return {
        {minX, minY, m_minHeight},
        {maxX, maxY, m_maxHeight},
    };
}

TerrainExtentRaw TerrainMap::makeExtentRaw(
    int32_t gridWidth, int32_t gridHeight,
    int32_t originX, int32_t originY) const noexcept {
    if (!isLoaded() || gridWidth <= 0 || gridHeight <= 0) return {};
    const int64_t minimumGridX =
        static_cast<int64_t>(originX) - m_heightfield.borderSize;
    const int64_t minimumGridY =
        static_cast<int64_t>(originY) - m_heightfield.borderSize;
    const int64_t maximumGridX =
        static_cast<int64_t>(originX) + gridWidth - m_heightfield.borderSize;
    const int64_t maximumGridY =
        static_cast<int64_t>(originY) + gridHeight - m_heightfield.borderSize;
    const Fixed cellWorldSize{int32_t{10}};
    const auto gridCoordinate = [cellWorldSize](int64_t grid) noexcept {
        return Fixed{static_cast<int32_t>(std::clamp<int64_t>(
            grid, std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int32_t>::max()))} * cellWorldSize;
    };
    return {
        .minimumX = gridCoordinate(minimumGridX).raw(),
        .minimumY = gridCoordinate(minimumGridY).raw(),
        .maximumX = gridCoordinate(maximumGridX).raw(),
        .maximumY = gridCoordinate(maximumGridY).raw(),
    };
}

TerrainExtent TerrainMap::playableExtent() const noexcept {
    if (!isLoaded() || m_heightfield.boundaries.empty()) return {};
    const TerrainBoundary& boundary = m_heightfield.boundaries[m_activeBoundary];
    if (isUsableBoundary(boundary)) {
        return makeExtent(boundary.width, boundary.height,
                          m_heightfield.borderSize, m_heightfield.borderSize);
    }

    // A map containing only disabled boundary slots is still renderable and
    // must remain query-safe.  Fall back to the physical terrain span rather
    // than presenting an inverted extent to gameplay/camera callers.
    return makeExtent(std::max(0, m_heightfield.width - m_heightfield.borderSize * 2),
                      std::max(0, m_heightfield.height - m_heightfield.borderSize * 2),
                      m_heightfield.borderSize, m_heightfield.borderSize);
}

TerrainExtent TerrainMap::extentIncludingBorder() const noexcept {
    return makeExtent(m_heightfield.width, m_heightfield.height, 0, 0);
}

TerrainExtentRaw TerrainMap::playableExtentRaw() const noexcept {
    if (!isLoaded() || m_heightfield.boundaries.empty()) return {};
    const TerrainBoundary& boundary = m_heightfield.boundaries[m_activeBoundary];
    if (isUsableBoundary(boundary)) {
        return makeExtentRaw(
            boundary.width, boundary.height,
            m_heightfield.borderSize, m_heightfield.borderSize);
    }
    return makeExtentRaw(
        std::max(0, m_heightfield.width - m_heightfield.borderSize * 2),
        std::max(0, m_heightfield.height - m_heightfield.borderSize * 2),
        m_heightfield.borderSize, m_heightfield.borderSize);
}

TerrainExtentRaw TerrainMap::extentIncludingBorderRaw() const noexcept {
    return makeExtentRaw(m_heightfield.width, m_heightfield.height, 0, 0);
}

bool TerrainMap::isInsidePlayable(float worldX, float worldY) const noexcept {
    if (!isLoaded() || !finite(worldX) || !finite(worldY)) return false;
    const TerrainExtent extent = playableExtent();
    return worldX >= extent.minimum.x() && worldY >= extent.minimum.y() &&
           worldX <= extent.maximum.x() && worldY <= extent.maximum.y();
}

bool TerrainMap::isInsidePlayableRaw(
    int64_t worldXRaw, int64_t worldYRaw) const noexcept {
    if (!isLoaded() || m_heightfield.boundaries.empty()) return false;
    const TerrainBoundary& selected = m_heightfield.boundaries[m_activeBoundary];
    const int32_t width = isUsableBoundary(selected)
        ? selected.width
        : std::max(0, m_heightfield.width - m_heightfield.borderSize * 2);
    const int32_t height = isUsableBoundary(selected)
        ? selected.height
        : std::max(0, m_heightfield.height - m_heightfield.borderSize * 2);
    const Fixed cellWorldSize{int32_t{10}};
    const Fixed x = Fixed::from_raw(worldXRaw);
    const Fixed y = Fixed::from_raw(worldYRaw);
    return x >= Fixed{} && y >= Fixed{} &&
           x <= Fixed{width} * cellWorldSize &&
           y <= Fixed{height} * cellWorldSize;
}

std::optional<float> TerrainMap::maxPhysicalCellHeightAlongLine(
    float startX, float startY, float endX, float endY) const noexcept {
    if (!isLoaded() || m_heightfield.width < 2 || m_heightfield.height < 2 ||
        !finite(startX) || !finite(startY) || !finite(endX) || !finite(endY)) {
        return std::nullopt;
    }

    // Clip to physical heightfield vertices, not playableExtent(): scenario
    // boundary changes must not change a ballistic path's terrain clearance.
    const float minimumX = -static_cast<float>(m_heightfield.borderSize) * kMapCellWorldSize;
    const float minimumY = -static_cast<float>(m_heightfield.borderSize) * kMapCellWorldSize;
    const float maximumX = static_cast<float>(m_heightfield.width - 1 - m_heightfield.borderSize) *
        kMapCellWorldSize;
    const float maximumY = static_cast<float>(m_heightfield.height - 1 - m_heightfield.borderSize) *
        kMapCellWorldSize;
    const float deltaX = endX - startX;
    const float deltaY = endY - startY;
    float entry = 0.0f;
    float exit = 1.0f;
    const auto clip = [&entry, &exit](float p, float q) noexcept {
        if (p == 0.0f) return q >= 0.0f;
        const float ratio = q / p;
        if (p < 0.0f) {
            if (ratio > exit) return false;
            entry = std::max(entry, ratio);
        } else {
            if (ratio < entry) return false;
            exit = std::min(exit, ratio);
        }
        return true;
    };
    if (!clip(-deltaX, startX - minimumX) || !clip(deltaX, maximumX - startX) ||
        !clip(-deltaY, startY - minimumY) || !clip(deltaY, maximumY - startY) || entry > exit) {
        return std::nullopt;
    }

    // A world point at the high vertex belongs to no cell. Nudge only that
    // endpoint inward so floor() selects the last physical cell consistently.
    const float interiorMaximumX = std::nextafter(maximumX, minimumX);
    const float interiorMaximumY = std::nextafter(maximumY, minimumY);
    const auto toCellX = [this, minimumX, interiorMaximumX](float worldX) noexcept {
        const float clipped = std::clamp(worldX, minimumX, interiorMaximumX);
        const int32_t grid = static_cast<int32_t>(std::floor(
            clipped / kMapCellWorldSize + static_cast<float>(m_heightfield.borderSize)));
        return std::clamp(grid, 0, m_heightfield.width - 2);
    };
    const auto toCellY = [this, minimumY, interiorMaximumY](float worldY) noexcept {
        const float clipped = std::clamp(worldY, minimumY, interiorMaximumY);
        const int32_t grid = static_cast<int32_t>(std::floor(
            clipped / kMapCellWorldSize + static_cast<float>(m_heightfield.borderSize)));
        return std::clamp(grid, 0, m_heightfield.height - 2);
    };
    const int32_t startCellX = toCellX(startX + deltaX * entry);
    const int32_t startCellY = toCellY(startY + deltaY * entry);
    const int32_t endCellX = toCellX(startX + deltaX * exit);
    const int32_t endCellY = toCellY(startY + deltaY * exit);

    // Preserve PartitionManager::iterateCellsAlongLine's exact Bresenham
    // tie-breaking.  A conventional symmetric variant visits the other cell
    // at a 2:1/1:2 diagonal tie, which in turn changes DumbProjectile's
    // terrain clearance and its deterministic curve samples.
    const int64_t distanceX = std::llabs(static_cast<int64_t>(endCellX) - startCellX);
    const int64_t distanceY = std::llabs(static_cast<int64_t>(endCellY) - startCellY);
    int32_t cellX = startCellX;
    int32_t cellY = startCellY;
    int32_t xIncrementWhenOver = endCellX >= startCellX ? 1 : -1;
    int32_t xIncrementAlways = xIncrementWhenOver;
    int32_t yIncrementWhenOver = endCellY >= startCellY ? 1 : -1;
    int32_t yIncrementAlways = yIncrementWhenOver;
    int64_t denominator = 0;
    int64_t numerator = 0;
    int64_t numeratorIncrement = 0;
    int64_t numberOfPixels = 0;
    if (distanceX >= distanceY) {
        xIncrementWhenOver = 0;
        yIncrementAlways = 0;
        denominator = distanceX;
        numerator = distanceX / 2;
        numeratorIncrement = distanceY;
        numberOfPixels = distanceX;
    } else {
        xIncrementAlways = 0;
        yIncrementWhenOver = 0;
        denominator = distanceY;
        numerator = distanceY / 2;
        numeratorIncrement = distanceX;
        numberOfPixels = distanceY;
    }
    float maximum = std::numeric_limits<float>::lowest();
    for (int64_t pixel = 0; pixel <= numberOfPixels; ++pixel) {
        maximum = std::max(maximum, std::max({sampleHeight(cellX, cellY),
                                              sampleHeight(cellX + 1, cellY),
                                              sampleHeight(cellX + 1, cellY + 1),
                                              sampleHeight(cellX, cellY + 1)}));
        numerator += numeratorIncrement;
        if (numerator >= denominator) {
            numerator -= denominator;
            cellX += xIncrementWhenOver;
            cellY += yIncrementWhenOver;
        }
        cellX += xIncrementAlways;
        cellY += yIncrementAlways;
    }
    return maximum;
}

std::optional<int64_t> TerrainMap::maxPhysicalCellHeightAlongLineRaw(
    int64_t startXRaw, int64_t startYRaw,
    int64_t endXRaw, int64_t endYRaw) const noexcept {
    if (!isLoaded() || m_heightfield.width < 2 || m_heightfield.height < 2) {
        return std::nullopt;
    }
    const Fixed cellSize{int32_t{10}};
    const Fixed minimumX = -Fixed{m_heightfield.borderSize} * cellSize;
    const Fixed minimumY = -Fixed{m_heightfield.borderSize} * cellSize;
    const Fixed maximumX =
        Fixed{m_heightfield.width - 1 - m_heightfield.borderSize} * cellSize;
    const Fixed maximumY =
        Fixed{m_heightfield.height - 1 - m_heightfield.borderSize} * cellSize;
    const Fixed startX = Fixed::from_raw(startXRaw);
    const Fixed startY = Fixed::from_raw(startYRaw);
    const Fixed deltaX = Fixed::from_raw(endXRaw) - startX;
    const Fixed deltaY = Fixed::from_raw(endYRaw) - startY;
    Fixed entry{};
    Fixed exit{int32_t{1}};
    const auto clip = [&entry, &exit](Fixed p, Fixed q) noexcept {
        if (p == Fixed{}) return q >= Fixed{};
        const Fixed ratio = q / p;
        if (p < Fixed{}) {
            if (ratio > exit) return false;
            entry = Fixed::max(entry, ratio);
        } else {
            if (ratio < entry) return false;
            exit = Fixed::min(exit, ratio);
        }
        return true;
    };
    if (!clip(-deltaX, startX - minimumX) ||
        !clip(deltaX, maximumX - startX) ||
        !clip(-deltaY, startY - minimumY) ||
        !clip(deltaY, maximumY - startY) || entry > exit) {
        return std::nullopt;
    }
    const Fixed interiorMaximumX = Fixed::from_raw(maximumX.raw() - 1);
    const Fixed interiorMaximumY = Fixed::from_raw(maximumY.raw() - 1);
    const auto toCell = [this, cellSize](Fixed world, Fixed minimum,
                                         Fixed maximum,
                                         int32_t dimension) noexcept {
        const Fixed clipped = Fixed::clamp(world, minimum, maximum);
        const int64_t grid = fixedFloorToInteger(clipped / cellSize) +
            m_heightfield.borderSize;
        return static_cast<int32_t>(std::clamp<int64_t>(
            grid, 0, dimension - 2));
    };
    const int32_t startCellX = toCell(
        startX + deltaX * entry, minimumX, interiorMaximumX,
        m_heightfield.width);
    const int32_t startCellY = toCell(
        startY + deltaY * entry, minimumY, interiorMaximumY,
        m_heightfield.height);
    const int32_t endCellX = toCell(
        startX + deltaX * exit, minimumX, interiorMaximumX,
        m_heightfield.width);
    const int32_t endCellY = toCell(
        startY + deltaY * exit, minimumY, interiorMaximumY,
        m_heightfield.height);

    const int64_t distanceX = std::llabs(
        static_cast<int64_t>(endCellX) - startCellX);
    const int64_t distanceY = std::llabs(
        static_cast<int64_t>(endCellY) - startCellY);
    int32_t cellX = startCellX;
    int32_t cellY = startCellY;
    int32_t xIncrementWhenOver = endCellX >= startCellX ? 1 : -1;
    int32_t xIncrementAlways = xIncrementWhenOver;
    int32_t yIncrementWhenOver = endCellY >= startCellY ? 1 : -1;
    int32_t yIncrementAlways = yIncrementWhenOver;
    int64_t denominator = 0;
    int64_t numerator = 0;
    int64_t numeratorIncrement = 0;
    int64_t numberOfPixels = 0;
    if (distanceX >= distanceY) {
        xIncrementWhenOver = 0;
        yIncrementAlways = 0;
        denominator = distanceX;
        numerator = distanceX / 2;
        numeratorIncrement = distanceY;
        numberOfPixels = distanceX;
    } else {
        xIncrementAlways = 0;
        yIncrementWhenOver = 0;
        denominator = distanceY;
        numerator = distanceY / 2;
        numeratorIncrement = distanceX;
        numberOfPixels = distanceY;
    }
    uint8_t maximum = 0;
    const auto sample = [&](int32_t x, int32_t y) noexcept {
        return m_heightfield.heights[
            static_cast<size_t>(y) * static_cast<size_t>(m_heightfield.width) +
            static_cast<size_t>(x)];
    };
    for (int64_t pixel = 0; pixel <= numberOfPixels; ++pixel) {
        maximum = std::max(maximum, std::max({
            sample(cellX, cellY), sample(cellX + 1, cellY),
            sample(cellX + 1, cellY + 1), sample(cellX, cellY + 1)}));
        numerator += numeratorIncrement;
        if (numerator >= denominator) {
            numerator -= denominator;
            cellX += xIncrementWhenOver;
            cellY += yIncrementWhenOver;
        }
        cellX += xIncrementAlways;
        cellY += yIncrementAlways;
    }
    return (Fixed{static_cast<int32_t>(maximum)} *
            Fixed::from_fraction(10, 16)).raw();
}

float TerrainMap::sampleHeight(int32_t x, int32_t y) const noexcept {
    return m_heightfield.heightWorld(x, y);
}

void TerrainMap::recalculateHeightRange() noexcept {
    m_heightSampleCounts.fill(0u);
    if (m_heightfield.heights.empty()) {
        m_minHeight = 0.0f;
        m_maxHeight = 0.0f;
        return;
    }
    uint8_t minimum = std::numeric_limits<uint8_t>::max();
    uint8_t maximum = std::numeric_limits<uint8_t>::lowest();
    for (const uint8_t sample : m_heightfield.heights) {
        ++m_heightSampleCounts[sample];
        minimum = std::min(minimum, sample);
        maximum = std::max(maximum, sample);
    }
    m_minHeight = static_cast<float>(minimum) * kMapHeightWorldScale;
    m_maxHeight = static_cast<float>(maximum) * kMapHeightWorldScale;
}

void TerrainMap::refreshHeightRangeFromHistogram() noexcept {
    size_t minimum = 0;
    while (minimum < m_heightSampleCounts.size() &&
           m_heightSampleCounts[minimum] == 0u) {
        ++minimum;
    }
    if (minimum == m_heightSampleCounts.size()) {
        m_minHeight = 0.0f;
        m_maxHeight = 0.0f;
        return;
    }
    size_t maximum = m_heightSampleCounts.size() - 1u;
    while (maximum > minimum && m_heightSampleCounts[maximum] == 0u) {
        --maximum;
    }
    m_minHeight = static_cast<float>(minimum) * kMapHeightWorldScale;
    m_maxHeight = static_cast<float>(maximum) * kMapHeightWorldScale;
}

void TerrainMap::replaceHeightSample(
    size_t index, uint8_t sample) noexcept {
    const uint8_t previous = m_heightfield.heights[index];
    if (previous == sample) return;
    if (m_heightSampleCounts[previous] != 0u) {
        --m_heightSampleCounts[previous];
    }
    ++m_heightSampleCounts[sample];
    m_heightfield.heights[index] = sample;
}

void TerrainMap::markHeightMutation(TerrainDirtyRegion dirty) noexcept {
    if (!dirty.isValid()) return;
    if (m_heightMutationBatchDepth != 0) {
        m_batchedHeightDirty.include(dirty);
        return;
    }
    refreshHeightRangeFromHistogram();
    ++m_revision;
    if (m_revision == 0) ++m_revision;
    appendDirtyRevision(dirty);
}

void TerrainMap::beginHeightMutationBatch() noexcept {
    if (m_heightMutationBatchDepth != UINT32_MAX) {
        ++m_heightMutationBatchDepth;
    }
}

void TerrainMap::endHeightMutationBatch() noexcept {
    if (m_heightMutationBatchDepth == 0) return;
    --m_heightMutationBatchDepth;
    if (m_heightMutationBatchDepth != 0 ||
        !m_batchedHeightDirty.isValid()) {
        return;
    }
    const TerrainDirtyRegion committed = m_batchedHeightDirty;
    m_batchedHeightDirty.clear();
    refreshHeightRangeFromHistogram();
    ++m_revision;
    if (m_revision == 0) ++m_revision;
    appendDirtyRevision(committed);
}

void TerrainMap::appendDirtyRevision(TerrainDirtyRegion dirty) noexcept {
    if (!dirty.isValid() || m_revision == 0) return;
    m_dirtyHistory.push_back({m_revision, dirty});
    while (m_dirtyHistory.size() > kMaxDirtyHistoryEntries) {
        m_dirtyHistory.pop_front();
    }

    // Preserve the pre-existing aggregate query while making its bounds match
    // exactly the retained journal. The detailed journal, rather than this
    // aggregate, is what the renderer uses to bridge skipped snapshots.
    m_dirtyRegion.clear();
    for (const TerrainDirtyRevision& entry : m_dirtyHistory) {
        m_dirtyRegion.include(entry.region);
    }
}

std::optional<TerrainCell> TerrainMap::cellAt(float worldX, float worldY) const noexcept {
    if (!isLoaded() || !finite(worldX) || !finite(worldY)) return std::nullopt;
    const int32_t x = static_cast<int32_t>(std::floor(worldX / kMapCellWorldSize)) + m_heightfield.borderSize;
    const int32_t y = static_cast<int32_t>(std::floor(worldY / kMapCellWorldSize)) + m_heightfield.borderSize;
    if (x < 0 || y < 0 || x >= m_heightfield.width - 1 || y >= m_heightfield.height - 1) {
        return std::nullopt;
    }
    return TerrainCell{x, y};
}

std::optional<TerrainCell> TerrainMap::cellAtRaw(
    int64_t worldXRaw, int64_t worldYRaw) const noexcept {
    if (!isLoaded()) return std::nullopt;
    const Fixed cellWorldSize{int32_t{10}};
    const int64_t x64 = fixedFloorToInteger(
        Fixed::from_raw(worldXRaw) / cellWorldSize) + m_heightfield.borderSize;
    const int64_t y64 = fixedFloorToInteger(
        Fixed::from_raw(worldYRaw) / cellWorldSize) + m_heightfield.borderSize;
    if (x64 < 0 || y64 < 0 || x64 >= m_heightfield.width - 1 ||
        y64 >= m_heightfield.height - 1) {
        return std::nullopt;
    }
    return TerrainCell{static_cast<int32_t>(x64), static_cast<int32_t>(y64)};
}

std::optional<TerrainSurfaceSample> TerrainMap::sampleSurface(
    float worldX, float worldY, bool clip) const noexcept {
    if (!isLoaded() || m_heightfield.width < 2 || m_heightfield.height < 2 ||
        !finite(worldX) || !finite(worldY)) {
        return std::nullopt;
    }

    float gridX = worldX / kMapCellWorldSize + static_cast<float>(m_heightfield.borderSize);
    float gridY = worldY / kMapCellWorldSize + static_cast<float>(m_heightfield.borderSize);
    const float maxX = static_cast<float>(m_heightfield.width - 1);
    const float maxY = static_cast<float>(m_heightfield.height - 1);
    if (!clip && (gridX < 0.0f || gridY < 0.0f || gridX > maxX || gridY > maxY)) {
        return std::nullopt;
    }
    gridX = std::clamp(gridX, 0.0f, maxX);
    gridY = std::clamp(gridY, 0.0f, maxY);
    const int32_t cellX = std::min(static_cast<int32_t>(std::floor(gridX)), m_heightfield.width - 2);
    const int32_t cellY = std::min(static_cast<int32_t>(std::floor(gridY)), m_heightfield.height - 2);
    const float fractionX = gridX - static_cast<float>(cellX);
    const float fractionY = gridY - static_cast<float>(cellY);

    // RefCode's height query splits every quad along 0->2:
    // 3---2       0=(x,y), 1=(x+1,y), 2=(x+1,y+1), 3=(x,y+1)
    // |  /|       It interpolates the containing triangle plane rather than
    // |/  |       using a bilinear height approximation.
    const float p0 = sampleHeight(cellX, cellY);
    const float p1 = sampleHeight(cellX + 1, cellY);
    const float p2 = sampleHeight(cellX + 1, cellY + 1);
    const float p3 = sampleHeight(cellX, cellY + 1);

    TerrainSurfaceSample result;
    result.cell = {cellX, cellY};
    if (fractionY > fractionX) {
        result.height = p3 + (1.0f - fractionY) * (p0 - p3) + fractionX * (p2 - p3);
        result.normal = math::vec3{kMapCellWorldSize, kMapCellWorldSize, p2 - p0}
            .cross({0.0f, kMapCellWorldSize, p3 - p0}).normalized();
    } else {
        result.height = p1 + fractionY * (p2 - p1) + (1.0f - fractionX) * (p0 - p1);
        result.normal = math::vec3{kMapCellWorldSize, 0.0f, p1 - p0}
            .cross({kMapCellWorldSize, kMapCellWorldSize, p2 - p0}).normalized();
    }
    return result;
}

float TerrainMap::groundHeight(float worldX, float worldY, bool clip) const noexcept {
    const auto sample = sampleSurface(worldX, worldY, clip);
    return sample ? sample->height : 0.0f;
}

int64_t TerrainMap::groundHeightRaw(
    int64_t worldXRaw, int64_t worldYRaw, bool clip) const noexcept {
    if (!isLoaded() || m_heightfield.width < 2 || m_heightfield.height < 2) {
        return 0;
    }

    const Fixed cellWorldSize{int32_t{10}};
    Fixed gridX = Fixed::from_raw(worldXRaw) / cellWorldSize +
        Fixed{m_heightfield.borderSize};
    Fixed gridY = Fixed::from_raw(worldYRaw) / cellWorldSize +
        Fixed{m_heightfield.borderSize};
    const Fixed maxX{m_heightfield.width - 1};
    const Fixed maxY{m_heightfield.height - 1};
    if (!clip && (gridX < Fixed{} || gridY < Fixed{} ||
                  gridX > maxX || gridY > maxY)) {
        return 0;
    }
    gridX = Fixed::clamp(gridX, Fixed{}, maxX);
    gridY = Fixed::clamp(gridY, Fixed{}, maxY);
    const int32_t cellX = std::min(
        static_cast<int32_t>(fixedFloorToInteger(gridX)),
        m_heightfield.width - 2);
    const int32_t cellY = std::min(
        static_cast<int32_t>(fixedFloorToInteger(gridY)),
        m_heightfield.height - 2);
    const Fixed fractionX = gridX - Fixed{cellX};
    const Fixed fractionY = gridY - Fixed{cellY};
    const Fixed heightWorldScale = Fixed::from_fraction(10, 16);
    const auto heightAt = [&](int32_t x, int32_t y) noexcept {
        const size_t index = static_cast<size_t>(y) *
            static_cast<size_t>(m_heightfield.width) + static_cast<size_t>(x);
        return Fixed{static_cast<int32_t>(m_heightfield.heights[index])} *
            heightWorldScale;
    };
    const Fixed p0 = heightAt(cellX, cellY);
    const Fixed p1 = heightAt(cellX + 1, cellY);
    const Fixed p2 = heightAt(cellX + 1, cellY + 1);
    const Fixed p3 = heightAt(cellX, cellY + 1);
    const Fixed result = fractionY > fractionX
        ? p3 + (Fixed{1} - fractionY) * (p0 - p3) +
              fractionX * (p2 - p3)
        : p1 + fractionY * (p2 - p1) +
              (Fixed{1} - fractionX) * (p0 - p1);
    return result.raw();
}

math::vec3 TerrainMap::groundNormal(float worldX, float worldY, bool clip) const noexcept {
    const auto sample = sampleSurface(worldX, worldY, clip);
    return sample ? sample->normal : math::vec3{0.0f, 0.0f, 1.0f};
}

container::Array<int64_t, 3> TerrainMap::groundNormalRaw(
    int64_t worldXRaw, int64_t worldYRaw, bool clip) const noexcept {
    if (!isLoaded() || m_heightfield.width < 2 || m_heightfield.height < 2) {
        return {0, 0, Fixed{int32_t{1}}.raw()};
    }
    const Fixed cellSize{int32_t{10}};
    Fixed gridX = Fixed::from_raw(worldXRaw) / cellSize +
        Fixed{m_heightfield.borderSize};
    Fixed gridY = Fixed::from_raw(worldYRaw) / cellSize +
        Fixed{m_heightfield.borderSize};
    const Fixed maxX{m_heightfield.width - 1};
    const Fixed maxY{m_heightfield.height - 1};
    if (!clip && (gridX < Fixed{} || gridY < Fixed{} ||
                  gridX > maxX || gridY > maxY)) {
        return {0, 0, Fixed{int32_t{1}}.raw()};
    }
    gridX = Fixed::clamp(gridX, Fixed{}, maxX);
    gridY = Fixed::clamp(gridY, Fixed{}, maxY);
    const int32_t cellX = std::min(
        static_cast<int32_t>(fixedFloorToInteger(gridX)),
        m_heightfield.width - 2);
    const int32_t cellY = std::min(
        static_cast<int32_t>(fixedFloorToInteger(gridY)),
        m_heightfield.height - 2);
    const Fixed fractionX = gridX - Fixed{cellX};
    const Fixed fractionY = gridY - Fixed{cellY};
    const Fixed heightScale = Fixed::from_fraction(10, 16);
    const auto heightAt = [&](int32_t x, int32_t y) noexcept {
        const uint8_t sample = m_heightfield.heights[
            static_cast<size_t>(y) * static_cast<size_t>(m_heightfield.width) +
            static_cast<size_t>(x)];
        return Fixed{static_cast<int32_t>(sample)} * heightScale;
    };
    const Fixed p0 = heightAt(cellX, cellY);
    const Fixed p1 = heightAt(cellX + 1, cellY);
    const Fixed p2 = heightAt(cellX + 1, cellY + 1);
    const Fixed p3 = heightAt(cellX, cellY + 1);
    Fixed nx;
    Fixed ny;
    const Fixed nz = cellSize * cellSize;
    if (fractionY > fractionX) {
        nx = cellSize * (p3 - p2);
        ny = -cellSize * (p3 - p0);
    } else {
        nx = -cellSize * (p1 - p0);
        ny = cellSize * (p1 - p2);
    }
    const Fixed length = Fixed::sqrt(nx * nx + ny * ny + nz * nz);
    if (length <= Fixed{}) return {0, 0, Fixed{int32_t{1}}.raw()};
    return {(nx / length).raw(), (ny / length).raw(),
            (nz / length).raw()};
}

bool TerrainMap::isCliffCell(float worldX, float worldY) const noexcept {
    const auto cell = cellAt(worldX, worldY);
    if (!cell) return false;
    if (m_blendTilesValid) {
        return m_heightfield.blendTiles->cliffCells[
            static_cast<size_t>(cell->y) * static_cast<size_t>(m_heightfield.width) + cell->x] != 0;
    }
    const float p0 = sampleHeight(cell->x, cell->y);
    const float p1 = sampleHeight(cell->x + 1, cell->y);
    const float p2 = sampleHeight(cell->x + 1, cell->y + 1);
    const float p3 = sampleHeight(cell->x, cell->y + 1);
    const float minimum = std::min({p0, p1, p2, p3});
    const float maximum = std::max({p0, p1, p2, p3});
    return maximum - minimum > kPathfindCliffSlopeLimit;
}

bool TerrainMap::isCliffCellRaw(
    int64_t worldXRaw, int64_t worldYRaw) const noexcept {
    const std::optional<TerrainCell> cell = cellAtRaw(worldXRaw, worldYRaw);
    if (!cell) return false;
    if (m_blendTilesValid) {
        return m_heightfield.blendTiles->cliffCells[
            static_cast<size_t>(cell->y) *
                static_cast<size_t>(m_heightfield.width) + cell->x] != 0;
    }
    const auto sample = [&](int32_t x, int32_t y) noexcept {
        return m_heightfield.heights[
            static_cast<size_t>(y) * static_cast<size_t>(m_heightfield.width) +
            static_cast<size_t>(x)];
    };
    const uint8_t p0 = sample(cell->x, cell->y);
    const uint8_t p1 = sample(cell->x + 1, cell->y);
    const uint8_t p2 = sample(cell->x + 1, cell->y + 1);
    const uint8_t p3 = sample(cell->x, cell->y + 1);
    const uint8_t minimum = std::min({p0, p1, p2, p3});
    const uint8_t maximum = std::max({p0, p1, p2, p3});
    const Fixed slope = Fixed{static_cast<int32_t>(maximum - minimum)} *
        Fixed::from_fraction(10, 16);
    return slope > Fixed::from_fraction(49, 5);
}

int64_t TerrainMap::navigationCellCenterHeightRaw(
    int32_t cellX, int32_t cellY) const noexcept {
    if (!isLoaded() || cellX < 0 || cellY < 0 ||
        cellX >= m_heightfield.width - 1 ||
        cellY >= m_heightfield.height - 1) {
        return 0;
    }
    const size_t row = static_cast<size_t>(cellY) *
        static_cast<size_t>(m_heightfield.width);
    const int32_t diagonalSum =
        static_cast<int32_t>(m_heightfield.heights[
            row + static_cast<size_t>(cellX)]) +
        static_cast<int32_t>(m_heightfield.heights[
            row + static_cast<size_t>(m_heightfield.width) +
            static_cast<size_t>(cellX + 1)]);
    // At the exact quad center RefCode's 0->2 triangle split yields
    // (p0+p2)/2. One authored height unit is 10/16 world units, therefore
    // the exact Q32.32 result is diagonalSum * 5/16.
    return static_cast<int64_t>(diagonalSum) *
        (int64_t{5} << 32u) / int64_t{16};
}

bool TerrainMap::navigationCellIsCliff(
    int32_t cellX, int32_t cellY) const noexcept {
    if (!isLoaded() || cellX < 0 || cellY < 0 ||
        cellX >= m_heightfield.width - 1 ||
        cellY >= m_heightfield.height - 1) {
        return false;
    }
    const size_t row = static_cast<size_t>(cellY) *
        static_cast<size_t>(m_heightfield.width);
    const size_t index = row + static_cast<size_t>(cellX);
    if (m_blendTilesValid) {
        return m_heightfield.blendTiles->cliffCells[index] != 0;
    }
    const auto sample = [&](int32_t x, int32_t y) noexcept {
        return m_heightfield.heights[
            static_cast<size_t>(y) *
                static_cast<size_t>(m_heightfield.width) +
            static_cast<size_t>(x)];
    };
    const uint8_t p0 = sample(cellX, cellY);
    const uint8_t p1 = sample(cellX + 1, cellY);
    const uint8_t p2 = sample(cellX + 1, cellY + 1);
    const uint8_t p3 = sample(cellX, cellY + 1);
    const uint8_t minimum = std::min({p0, p1, p2, p3});
    const uint8_t maximum = std::max({p0, p1, p2, p3});
    // (maximum-minimum) * 10/16 > 9.8 is exactly true for an integer
    // authored delta of at least 16.
    return static_cast<uint32_t>(maximum - minimum) >= 16u;
}

} // namespace game::terrain
