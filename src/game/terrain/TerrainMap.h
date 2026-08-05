#pragma once

#include "core/container/container_types.h"

#include "MapHeightfieldLoader.h"

#include <cstdint>
#include <optional>
namespace game::terrain {

inline constexpr float kPathfindCliffSlopeLimit = 9.8f;

struct TerrainExtent {
    math::vec3 minimum{};
    math::vec3 maximum{};
};

struct TerrainExtentRaw final {
    int64_t minimumX = 0;
    int64_t minimumY = 0;
    int64_t maximumX = 0;
    int64_t maximumY = 0;
};

struct TerrainCell {
    int32_t x = 0;
    int32_t y = 0;
};

// Inclusive heightfield-sample rectangle. It is logic-owned metadata copied
// into a render snapshot; renderers may use it for partial uploads, while a
// skipped snapshot remains safe because the terrain revision is authoritative.
struct TerrainDirtyRegion {
    int32_t minX = 0;
    int32_t minY = 0;
    int32_t maxX = -1;
    int32_t maxY = -1;

    [[nodiscard]] bool isValid() const noexcept { return minX <= maxX && minY <= maxY; }
    void clear() noexcept { minX = minY = 0; maxX = maxY = -1; }
    void include(int32_t x, int32_t y) noexcept;
    void include(TerrainDirtyRegion region) noexcept;
};

// A heightfield revision and the inclusive source-sample rectangle changed by
// that revision.  The map retains a bounded sequence of these records so a
// renderer that skips one or more logic snapshots can still determine exactly
// which GPU chunks need replacement.  A consumer older than the retained
// history must safely fall back to a full terrain upload.
struct TerrainDirtyRevision {
    uint64_t revision = 0;
    TerrainDirtyRegion region;
};

enum class TerrainFlattenShape : uint8_t {
    Circle,
    OrientedBox,
};

// Fixed-only request used by structure placement and the legacy OCL
// LIKE_EXISTING path. The terrain owner performs the complete average/lower
// transaction and publishes one dirty revision; callers never edit samples
// or reconstruct gameplay geometry through float.
struct TerrainFlattenFootprint final {
    int64_t centerXRaw = 0;
    int64_t centerYRaw = 0;
    int64_t yawRadiansRaw = 0;
    int64_t majorRadiusRaw = 0;
    int64_t minorRadiusRaw = 0;
    TerrainFlattenShape shape = TerrainFlattenShape::Circle;
    bool isSmall = true;
};

struct TerrainFlattenResult final {
    bool evaluated = false;
    bool changed = false;
    int64_t centerHeightRaw = 0;
    // World-space height of the lowering plane selected for this footprint.
    // It may differ from a post-mutation centre sample when neighbouring low
    // vertices pull that triangle below the plane.
    int64_t flattenedPlaneHeightRaw = 0;
};

// Query result in world coordinates. Height is calculated using the original
// map diagonal split (not a bilinear approximation), so gameplay placement,
// collision and the terrain mesh agree on the same surface.
struct TerrainSurfaceSample {
    float height = 0.0f;
    math::vec3 normal{0.0f, 0.0f, 1.0f};
    TerrainCell cell;
};

// Device-independent runtime owner for one loaded heightfield. It contains no
// ECS, rendering, VFS or D3D objects; those systems interact through these
// value queries and through immutable render extraction.
class TerrainMap final {
public:
    bool load(TerrainHeightfieldData heightfield);
    bool loadFromFile(container::StringView path, container::String* error = nullptr);
    void clear() noexcept;

    [[nodiscard]] bool isLoaded() const noexcept { return m_heightfield.isValid(); }
    [[nodiscard]] uint64_t revision() const noexcept { return m_revision; }
    // Changes only when immutable map layout/material source changes. Height
    // mutations deliberately leave it stable, which lets renderers use the
    // dirty revision journal for chunk-local updates.
    [[nodiscard]] uint64_t layoutRevision() const noexcept { return m_layoutRevision; }
    [[nodiscard]] const TerrainHeightfieldData& heightfield() const noexcept { return m_heightfield; }
    // BlendTile arrays are immutable after load and height deformation never
    // changes their sample count. Validate the untrusted source once at the
    // loading boundary; hot cell queries must not rescan the complete arrays.
    [[nodiscard]] bool hasValidBlendTiles() const noexcept {
        return m_blendTilesValid;
    }

    [[nodiscard]] int32_t width() const noexcept { return m_heightfield.width; }
    [[nodiscard]] int32_t height() const noexcept { return m_heightfield.height; }
    [[nodiscard]] int32_t borderSize() const noexcept { return m_heightfield.borderSize; }
    [[nodiscard]] const TerrainDirtyRegion& dirtyRegion() const noexcept { return m_dirtyRegion; }
    [[nodiscard]] const container::Deque<TerrainDirtyRevision>& dirtyHistory() const noexcept {
        return m_dirtyHistory;
    }

    // All mutations remain in the logic owner. They never touch renderer
    // buffers; a later snapshot observes the incremented revision and copied
    // height data. This is the bridge used by crater/flatten mechanics.
    bool setHeightSample(int32_t x, int32_t y, uint8_t sample) noexcept;
    // Authoritative deformation entry point: centre, radius and height delta
    // are signed Q32.32 raw values, so the mutated samples - and therefore
    // groundHeightRaw, cliff classification and the navigation raster derived
    // from them - are bit-identical on every machine. Simulation callers must
    // use this form, exactly as they must for flattenFootprintRaw.
    bool deformCircleRaw(int64_t worldXRaw, int64_t worldYRaw,
                         int64_t radiusWorldRaw,
                         int64_t heightDeltaWorldRaw) noexcept;
    // Float convenience shell for tools, map import and diagnostics only. It
    // converts and forwards to deformCircleRaw; it introduces no separate
    // mutation logic.
    bool deformCircle(float worldX, float worldY, float radiusWorld,
                      float heightDeltaWorld) noexcept;
    [[nodiscard]] TerrainFlattenResult flattenFootprintRaw(
        const TerrainFlattenFootprint& footprint) noexcept;
    // Map import may apply many authored structure footprints before any
    // consumer can observe the terrain. Samples still change immediately so
    // later placements see the preceding result, while range maintenance and
    // the externally visible revision are committed once at batch end.
    void beginHeightMutationBatch() noexcept;
    void endHeightMutationBatch() noexcept;

    [[nodiscard]] size_t activeBoundary() const noexcept { return m_activeBoundary; }
    bool setActiveBoundary(size_t index) noexcept;
    [[nodiscard]] TerrainExtent playableExtent() const noexcept;
    [[nodiscard]] TerrainExtent extentIncludingBorder() const noexcept;
    [[nodiscard]] TerrainExtentRaw playableExtentRaw() const noexcept;
    [[nodiscard]] TerrainExtentRaw extentIncludingBorderRaw() const noexcept;
    [[nodiscard]] bool isInsidePlayable(float worldX, float worldY) const noexcept;
    // Authoritative simulation counterpart. Coordinates and the returned
    // height are signed Q32.32 raw values; no float projection is performed.
    [[nodiscard]] bool isInsidePlayableRaw(
        int64_t worldXRaw, int64_t worldYRaw) const noexcept;

    // RefCode's projectile trajectory query walks physical partition cells,
    // not the currently selected logical boundary. This value query mirrors
    // that distinction with a Bresenham walk over the physical heightfield
    // (including its border) and returns the maximum of every touched cell's
    // four height samples. A line completely outside the physical map has no
    // valid terrain cell and returns nullopt.
    [[nodiscard]] std::optional<float> maxPhysicalCellHeightAlongLine(
        float startX, float startY, float endX, float endY) const noexcept;
    [[nodiscard]] std::optional<int64_t> maxPhysicalCellHeightAlongLineRaw(
        int64_t startXRaw, int64_t startYRaw,
        int64_t endXRaw, int64_t endYRaw) const noexcept;

    // With clip=false, positions outside the heightfield return nullopt.
    // With clip=true, they are clamped to the nearest map edge, mirroring the
    // legacy query's safe edge behavior without exposing a renderer object.
    [[nodiscard]] std::optional<TerrainSurfaceSample> sampleSurface(
        float worldX, float worldY, bool clip = true) const noexcept;
    [[nodiscard]] float groundHeight(float worldX, float worldY, bool clip = true) const noexcept;
    [[nodiscard]] int64_t groundHeightRaw(
        int64_t worldXRaw, int64_t worldYRaw,
        bool clip = true) const noexcept;
    [[nodiscard]] math::vec3 groundNormal(float worldX, float worldY, bool clip = true) const noexcept;
    [[nodiscard]] container::Array<int64_t, 3> groundNormalRaw(
        int64_t worldXRaw, int64_t worldYRaw,
        bool clip = true) const noexcept;
    [[nodiscard]] std::optional<TerrainCell> cellAt(float worldX, float worldY) const noexcept;
    [[nodiscard]] std::optional<TerrainCell> cellAtRaw(
        int64_t worldXRaw, int64_t worldYRaw) const noexcept;
    [[nodiscard]] bool isCliffCell(float worldX, float worldY) const noexcept;
    [[nodiscard]] bool isCliffCellRaw(
        int64_t worldXRaw, int64_t worldYRaw) const noexcept;
    // Navigation grids are aligned one-to-one with TerrainMap quads. These
    // index-based queries avoid repeating two Q32.32 world-to-grid divisions
    // for every cell in a dirty terrain publication.
    [[nodiscard]] int64_t navigationCellCenterHeightRaw(
        int32_t cellX, int32_t cellY) const noexcept;
    [[nodiscard]] bool navigationCellIsCliff(
        int32_t cellX, int32_t cellY) const noexcept;

private:
    [[nodiscard]] float sampleHeight(int32_t x, int32_t y) const noexcept;
    void recalculateHeightRange() noexcept;
    void refreshHeightRangeFromHistogram() noexcept;
    void replaceHeightSample(size_t index, uint8_t sample) noexcept;
    void markHeightMutation(TerrainDirtyRegion dirty) noexcept;
    void appendDirtyRevision(TerrainDirtyRegion dirty) noexcept;
    [[nodiscard]] TerrainExtent makeExtent(int32_t gridWidth, int32_t gridHeight,
                                             int32_t originX, int32_t originY) const noexcept;
    [[nodiscard]] TerrainExtentRaw makeExtentRaw(
        int32_t gridWidth, int32_t gridHeight,
        int32_t originX, int32_t originY) const noexcept;

    TerrainHeightfieldData m_heightfield;
    bool m_blendTilesValid = false;
    uint64_t m_revision = 0;
    uint64_t m_layoutRevision = 0;
    size_t m_activeBoundary = 0;
    // These are map-wide extrema, calculated once when immutable source data
    // enters the logic owner.  Extent queries are hot-path value queries and
    // must not rescan a potentially 400k-sample heightfield every time.
    float m_minHeight = 0.0f;
    float m_maxHeight = 0.0f;
    container::Array<uint32_t, 256> m_heightSampleCounts{};
    TerrainDirtyRegion m_dirtyRegion;
    container::Deque<TerrainDirtyRevision> m_dirtyHistory;
    uint32_t m_heightMutationBatchDepth = 0;
    TerrainDirtyRegion m_batchedHeightDirty;
};

} // namespace game::terrain
