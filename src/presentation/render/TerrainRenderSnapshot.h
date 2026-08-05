#pragma once

#include "presentation/render/RenderWorldDescriptorContracts.h"

namespace engine::render {

struct TerrainRenderDirtyRegion {
    int32_t minX = 0;
    int32_t minY = 0;
    int32_t maxX = -1;
    int32_t maxY = -1;

    [[nodiscard]] bool isValid() const noexcept { return minX <= maxX && minY <= maxY; }
    void include(TerrainRenderDirtyRegion value) noexcept {
        if (!value.isValid()) return;
        if (!isValid()) {
            *this = value;
            return;
        }
        minX = minX < value.minX ? minX : value.minX;
        minY = minY < value.minY ? minY : value.minY;
        maxX = maxX > value.maxX ? maxX : value.maxX;
        maxY = maxY > value.maxY ? maxY : value.maxY;
    }
};

// Ordered change journal copied from TerrainMap. `revision` is the terrain
// revision after the region was written. Keeping this in the sealed snapshot
// lets a renderer combine all mutations since its own GPU revision even when
// intermediate logic frames were deliberately dropped by the render queue.
struct TerrainRenderDirtyRevision {
    uint64_t revision = 0;
    TerrainRenderDirtyRegion region;
};

// Detached material selectors copied out of a map's BlendTileData chunk.  The
// texture class names are intentionally value strings, rather than VFS,
// TextureManager, or loader references: the render thread owns resolution and
// GPU lifetime after a logic frame is sealed.
struct TerrainTextureClassRenderData {
    int32_t firstTile = 0;
    int32_t tileCount = 0;
    int32_t tileWidth = 0;
    container::String name;
};

struct TerrainBlendDefinitionRenderData {
    int32_t blendTileIndex = 0;
    uint8_t horizontal = 0;
    uint8_t vertical = 0;
    uint8_t rightDiagonal = 0;
    uint8_t leftDiagonal = 0;
    uint8_t inverted = 0;
    uint8_t longDiagonal = 0;
    int32_t customEdgeTextureClass = -1;
};

struct TerrainCliffDefinitionRenderData {
    int32_t tileIndex = 0;
    container::Array<float, 8> uv{};
    uint8_t flip = 0;
    uint8_t mutant = 0;
};

// The original terrain stores a tile selector per height sample.  A cell uses
// the selector at its lower-left sample; a tile index encodes both a 64px
// source tile and one of its four 32px quadrants.  Blend and cliff selectors
// remain intact even when a backend only implements a subset of the classic
// atlas compositor.
struct TerrainMaterialRenderData {
    uint16_t sourceVersion = 0;
    int32_t bitmapTileCount = 0;
    int32_t edgeTileCount = 0;
    container::Vector<int16_t> baseTileIndices;
    container::Vector<int16_t> blendTileIndices;
    container::Vector<int16_t> extraBlendTileIndices;
    container::Vector<int16_t> cliffInfoIndices;
    container::Vector<uint8_t> cliffCells;
    container::Vector<TerrainTextureClassRenderData> textureClasses;
    container::Vector<TerrainTextureClassRenderData> edgeTextureClasses;
    // Dense source-tile -> texture-class projection compiled once while the
    // detached snapshot is assembled.  Terrain overlays, scorches and chunk
    // builders all need this lookup; repeating a linear textureClasses scan
    // for every covered cell made large maps quadratic in authored classes.
    container::Vector<int32_t> textureClassBySourceTile;
    container::Vector<TerrainBlendDefinitionRenderData> blendDefinitions;
    container::Vector<TerrainCliffDefinitionRenderData> cliffDefinitions;

    [[nodiscard]] bool isValidFor(size_t sampleCount) const noexcept;
};

inline constexpr size_t kTerrainRenderTimeOfDayCount = 4;
inline constexpr size_t kTerrainRenderGlobalLightCount = 3;

struct TerrainLightingRenderData {
    RenderVector ambient{};
    RenderVector diffuse{1.0f, 1.0f, 1.0f};
    RenderVector direction{0.0f, 0.0f, -1.0f};
};

// Keep the complete parsed GlobalLighting payload in the snapshot.  Today the
// terrain visual prelights against its selected terrain lights; retaining the
// object lights and shadow colour avoids a later parser/logic reach-back when
// the world environment pass is added.
struct TerrainGlobalLightingRenderData {
    uint16_t sourceVersion = 0;
    int32_t timeOfDay = 0;
    container::Array<container::Array<TerrainLightingRenderData, kTerrainRenderGlobalLightCount>,
               kTerrainRenderTimeOfDayCount> terrainLights{};
    container::Array<container::Array<TerrainLightingRenderData, kTerrainRenderGlobalLightCount>,
               kTerrainRenderTimeOfDayCount> objectLights{};
    std::optional<uint32_t> shadowColor;

    // RefCode's serialized enum is INVALID=0 followed by the four dense
    // morning/afternoon/evening/night array entries. Invalid/out-of-range
    // source values remain deterministic at the nearest supported slot.
    [[nodiscard]] size_t terrainLightSlot() const noexcept {
        if (timeOfDay <= 1) return 0;
        if (timeOfDay >= static_cast<int32_t>(kTerrainRenderTimeOfDayCount)) {
            return kTerrainRenderTimeOfDayCount - 1;
        }
        return static_cast<size_t>(timeOfDay - 1);
    }
};

// Logic publishes flood/water state as simple value data. The terrain/water
// renderer may choose its own GPU representation, but it cannot observe a
// live PolygonTrigger or TerrainLogic object after snapshot extraction.
struct TerrainWaterRenderArea {
    uint32_t triggerId = 0;
    container::String name;
    container::Vector<RenderVector> polygon;
    bool river = false;
    int32_t riverStart = 0;
    bool synthesizedLegacyWater = false;
    float surfaceHeight = 0.0f;
    float targetHeight = 0.0f;
    // Transition-only legacy pulse amount; rendering normally ignores it,
    // but exposing the correctly named value keeps debug/authoring views from
    // presenting water damage as a continuous DPS rate.
    float damageAmount = 0.0f;
    bool transitioning = false;
};

struct TerrainWaterMaterialRenderData final {
    container::String textureName = "TSWater.tga";
    container::String standingWaterTextureName = "TWWater01.tga";
    container::String skyTextureName;
    container::Array<math::vec4, 4> vertexColors{
        math::vec4{1.0f, 1.0f, 1.0f, 1.0f},
        math::vec4{1.0f, 1.0f, 1.0f, 1.0f},
        math::vec4{1.0f, 1.0f, 1.0f, 1.0f},
        math::vec4{1.0f, 1.0f, 1.0f, 1.0f},
    };
    math::vec4 diffuseColor{0.686f, 0.686f, 0.686f, 1.0f};
    math::vec4 transparentDiffuseColor{0.588f, 0.588f, 0.588f, 0.502f};
    math::vec4 standingWaterColor{1.0f, 1.0f, 1.0f, 1.0f};
    math::vec4 radarWaterColor{0.55f, 0.55f, 1.0f, 1.0f};
    float uScrollPerSecond = 2.0f;
    float vScrollPerSecond = 2.0f;
    float textureRepeatCount = 32.0f;
    float skyTexelsPerUnit = 0.0f;
    float minimumOpacity = 1.0f;
    float transparentWaterDepth = 3.0f;
    // GlobalData WaterType. Only type 2 owns the legacy 256x256 reflected
    // scene target; other types keep the existing translucent/grid paths.
    int32_t waterType = 0;
    float reflectionPlaneZ = 0.0f;
    bool hasReflectionPlane = false;
    bool showSoftEdge = true;
    bool additiveBlending = false;
};

// One selected GlobalData VertexWater slot.  TerrainLogic enables this only
// for maps carrying WaveGuide1; the renderer receives a complete frozen grid
// descriptor and never compares live map names or reads GameData.ini.
struct TerrainVertexWaterRenderData final {
    uint32_t sourceSlot = 0;
    RenderVector position{};
    float angleRadians = 0.0f;
    float heightClampLow = 0.0f;
    float heightClampHigh = 0.0f;
    uint32_t gridCellsX = 0;
    uint32_t gridCellsY = 0;
    float gridSize = 0.0f;
    float attenuationA = 0.0f;
    float attenuationB = 0.0f;
    float attenuationC = 0.0f;
    float attenuationRange = 0.0f;
    // Final row-major local heights from the session-owned legacy
    // oscillator. When present this is the authoritative presentation state
    // for all (gridCellsX+1)*(gridCellsY+1) vertices; the renderer must not
    // reconstruct motion from sparse impulse history.
    container::Vector<float> pointHeights;

    [[nodiscard]] bool isValid() const noexcept {
        const uint64_t expectedPointCount =
            (static_cast<uint64_t>(gridCellsX) + 1u) *
            (static_cast<uint64_t>(gridCellsY) + 1u);
        const bool validIntegratedHeights = pointHeights.empty() ||
            (expectedPointCount == pointHeights.size() &&
             std::all_of(pointHeights.begin(), pointHeights.end(),
                         [](float height) { return std::isfinite(height); }));
        return gridCellsX != 0u && gridCellsY != 0u &&
            std::isfinite(gridSize) && gridSize > 0.0f &&
            std::isfinite(position.x()) && std::isfinite(position.y()) &&
            std::isfinite(position.z()) && std::isfinite(angleRadians) &&
            std::isfinite(heightClampLow) &&
            std::isfinite(heightClampHigh) &&
            heightClampHigh >= heightClampLow && validIntegratedHeights;
    }
};

struct TerrainScorchRenderData final {
    RenderVector position{};
    float radius = 0.0f;
    int32_t type = 0;
};

struct TerrainRoadRenderSegment final {
    container::String styleName;
    // Stable TerrainRoad catalog identity.  Zero is reserved for synthetic
    // renderer fixtures that intentionally use styleName as their key.
    uint32_t styleIdentity = 0;
    container::String textureName;
    RenderVector start{};
    RenderVector end{};
    // RefCode W3DRoadBuffer fallback for an unresolved road style.
    float width = 8.0f;
    float widthInTexture = 1.0f;
    bool startAngled = false;
    bool endAngled = false;
    bool startJoin = false;
    bool endJoin = false;
    bool tightCorner = false;
};

enum class TerrainBridgeDamageState : uint8_t {
    Pristine,
    Damaged,
    ReallyDamaged,
    Rubble,
};

struct TerrainBridgeRenderData final {
    uint32_t bridgeId = 0;
    // Stable CkMp point-1 record identity. Runtime GenericBridge authority is
    // joined by this value, never by transient ECS entity or nearest-distance
    // guesses, so replay/epoch rebuilds retain deterministic ownership.
    uint64_t sourceRecordIndex = 0;
    uint64_t authoritativeObjectId = 0;
    container::String styleName;
    RenderVector start{};
    RenderVector end{};
    float scale = 0.7f;
    float bridgeWidth = 23.8f;
    // TerrainRoadType defaults RadarColor to black. Keep an explicit presence
    // bit so authored/default black is not confused with a missing bridge
    // template (which RefCode renders white as its diagnostic fallback).
    container::Array<float, 3> radarColor{};
    bool hasRadarColor = false;
    TerrainBridgeDamageState damageState =
        TerrainBridgeDamageState::Pristine;
    container::Array<container::String, 4> modelNames;
    container::Array<container::String, 4> textureNames;
    container::Array<container::String, 4> towerObjectNames;
    container::Array<container::String, 4> towerModelAssets;
    container::String scaffoldObjectName;
    container::String scaffoldSupportObjectName;
    RenderVector boundsCenter{};
    RenderVector boundsExtents{};
    bool pickable = true;
};

enum class TerrainBibKind : uint8_t {
    Building,
    FactoryExit,
    PlacementPreview,
};

enum class TerrainBibTint : uint8_t {
    Default,
    Blue,
    Green,
    Yellow,
    Red,
};

struct TerrainBibRenderData final {
    RenderEntityId ownerObjectId = 0;
    TerrainBibKind kind = TerrainBibKind::Building;
    container::Array<RenderVector, 4> corners{};
    container::String textureName = "TBBib.tga";
    bool red = false;
    TerrainBibTint tint = TerrainBibTint::Default;
    bool receivesVisibility = true;
};

struct TerrainPointLightRenderData final {
    RenderVector position{};
    RenderVector ambient{};
    RenderVector diffuse{1.0f, 1.0f, 1.0f};
    float innerRadius = 0.0f;
    float outerRadius = 0.0f;
};

// Map/terrain logic publishes this immutable data as part of a logic-frame
// snapshot.  It deliberately has no file/VFS/ECS/GPU handle: the renderer
// owns its chunk meshes, and terrain logic can later replace this payload when
// flooding or deformation changes the height field.
struct TerrainRenderSnapshot {
    uint64_t revision = 0;
    // Changes when a new map/layout/material source is loaded. A height-only
    // deformation keeps this stable and can therefore replace just affected
    // renderer-owned chunks.
    uint64_t layoutRevision = 0;
    TerrainRenderDirtyRegion dirtyRegion;
    container::Vector<TerrainRenderDirtyRevision> dirtyHistory;
    uint64_t waterRevision = 0;
    // Changes when a map-backed GenericBridge BodyDamageState projection
    // changes. Bridge buffers can then update independently from heightfield
    // and water revisions.
    uint64_t bridgeRevision = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t borderSize = 0;
    // Logical playable boundary selected by MAP_SWITCH_BORDER. The physical
    // heightfield remains fully renderable; a script can choose whether the
    // unused rim is darkened like RefCode's Display border shroud.
    RenderVector playableMinimum{};
    RenderVector playableMaximum{};
    bool borderShroudEnabled = false;
    // Presentation revision independent from terrain deformation. It keeps
    // renderer-side border policy/cache identity current without pretending
    // that a shroud toggle changed height or material geometry.
    uint64_t borderShroudRevision = 0;
    float cellWorldSize = 10.0f;
    float heightWorldScale = 0.625f;
    // Captured from the logic-owned graphics configuration with the map
    // snapshot.  When false, classic authored cliff UV deformation is
    // disabled and the regular source-tile mapping is used instead.
    bool adjustCliffTextures = true;
    container::Vector<uint8_t> heights;
    std::optional<TerrainMaterialRenderData> materials;
    std::optional<TerrainGlobalLightingRenderData> globalLighting;
    std::optional<TerrainWaterMaterialRenderData> waterMaterial;
    std::optional<TerrainVertexWaterRenderData> vertexWater;
    container::Vector<TerrainWaterRenderArea> waterAreas;
    container::Vector<TerrainScorchRenderData> scorches;
    container::Vector<TerrainRoadRenderSegment> roads;
    container::Vector<TerrainBridgeRenderData> bridges;
    container::Vector<TerrainBibRenderData> bibs;
    container::Vector<TerrainPointLightRenderData> pointLights;

    [[nodiscard]] bool isValid() const noexcept;
    // Computes the union of every retained height mutation after
    // `appliedRevision`. Returns false when the journal has a gap, the caller
    // is already current, or the snapshot cannot prove a partial update is
    // complete. In those cases a full upload is the safe fallback.
    [[nodiscard]] bool dirtyRegionSince(uint64_t appliedRevision,
                                        TerrainRenderDirtyRegion& output) const noexcept;
    [[nodiscard]] uint8_t heightSample(int32_t x, int32_t y) const noexcept;
    [[nodiscard]] float heightWorld(int32_t x, int32_t y) const noexcept;
    [[nodiscard]] RenderVector worldPosition(int32_t x, int32_t y) const noexcept;
};

// One local observer's immutable visibility projection. It deliberately
// contains no PlayerRegistry, alliance matrix, ECS entity, or all-player
// grids. `cells` is row-major terrain-cell data suitable for an R8 texture;
// dirtyRegion identifies the changed cell rectangle for incremental upload.
struct LocalVisibilityRenderSnapshot final {
    uint64_t presentationEpoch = 0;
    uint64_t revision = 0;
    // Directed diplomacy changes can alter the observer+allies projection
    // without mutating any visibility cell. Track that policy separately so
    // the renderer never reuses a texture produced for an older alliance set.
    uint64_t policyRevision = 0;
    uint64_t terrainLayoutRevision = 0;
    uint8_t observerPlayer = UINT8_MAX;
    int32_t width = 0;
    int32_t height = 0;
    int32_t borderSize = 0;
    // Explicit origin decouples the original 40-unit shroud grid from the
    // heightfield's 10-unit border sample count. `borderSize` remains as map
    // metadata for diagnostics/backward-compatible aggregate fixtures.
    float originX = 0.0f;
    float originY = 0.0f;
    float cellWorldSize = 10.0f;
    bool enabled = false;
    // Active WorldBuilder boundary. This is a presentation visibility gate
    // independent of fog diplomacy: content outside the current map rectangle
    // is never visible, including to the local owner or an ally. Scripts may
    // disable the border shroud or switch the active boundary, in which case
    // the next immutable snapshot updates this contract atomically.
    RenderVector playableMinimum{};
    RenderVector playableMaximum{};
    bool playableBoundsEnabled = false;
    // Controls only the renderer-owned exterior terrain fade. The playable
    // rectangle remains an object/effect visibility boundary when disabled.
    bool borderShroudEnabled = true;
    TerrainRenderDirtyRegion dirtyRegion;
    // Production extraction interns these immutable planes by visibility and
    // diplomacy revisions. Value-owned vectors remain for focused fixtures
    // and compatibility builders; accessors select the shared plane first.
    container::SharedPtr<const container::Vector<uint8_t>> sharedCells;
    container::SharedPtr<const container::Vector<uint8_t>> sharedVisualLevels;
    container::Vector<uint8_t> cells;
    // Optional directly encoded R8 luminance. When present it is uploaded
    // verbatim while `cells` retains discrete gameplay/object visibility.
    container::Vector<uint8_t> visualLevels;
    uint32_t hiddenEntityCount = 0;
    uint32_t hiddenProjectileCount = 0;

    [[nodiscard]] container::Span<const uint8_t> cellValues() const noexcept {
        return sharedCells
            ? container::Span<const uint8_t>{*sharedCells}
            : container::Span<const uint8_t>{cells};
    }
    [[nodiscard]] container::Span<const uint8_t>
    visualLevelValues() const noexcept {
        return sharedVisualLevels
            ? container::Span<const uint8_t>{*sharedVisualLevels}
            : container::Span<const uint8_t>{visualLevels};
    }
    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] bool hasPlayableBounds() const noexcept;
    [[nodiscard]] bool isInsidePlayableBounds(
        RenderVector position) const noexcept;
    [[nodiscard]] LocalVisibilityRenderCellState cellState(
        int32_t x, int32_t y) const noexcept;
    [[nodiscard]] LocalVisibilityRenderCellState worldState(
        RenderVector position) const noexcept;
    [[nodiscard]] LocalVisibilityRenderCellState worldStateSphere(
        RenderVector center, float radius) const noexcept;
};

// Identity of one authoritative world endpoint. worldRevision is initially
// sourced from the confirmed simulation frame; it remains a separate field so
// presentation-only endpoint revisions can later advance without pretending
// that an interpolated/render frame is a logic tick.

} // namespace engine::render
