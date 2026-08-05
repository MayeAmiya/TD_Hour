#pragma once

#include "core/container/hash_containers.h"

#include "core/math/wwmath/vector/float3.h"
#include "core/math/wwmath/vector/int3.h"
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
namespace game::terrain {

// These are the world-space units used by the original map format.  The
// renderer and simulation consume this neutral representation rather than
// reading a .map file themselves.
inline constexpr float kMapCellWorldSize = 10.0f;
inline constexpr float kMapHeightWorldScale = kMapCellWorldSize / 16.0f;

struct TerrainBoundary {
    int32_t width = 0;
    int32_t height = 0;
};

// One of the classic global-light records.  The source calls the final vector
// ``lightPos`` even though terrain shading consumes it as a light direction.
struct TerrainLighting {
    math::vec3 ambient{};
    math::vec3 diffuse{1.0f, 1.0f, 1.0f};
    math::vec3 direction{0.0f, 0.0f, -1.0f};
};

// Map dictionaries are serialized with a symbol-table key and one of these
// five scalar payload types.  Keep the source value typed and ownership-free
// so gameplay can consume map metadata without depending on RefCode's global
// NameKey/Dict singletons. Unicode stays UTF-16 rather than being silently
// converted through the process locale.
using MapPropertyValue = std::variant<bool, int32_t, float, container::String, container::U16String>;
using MapPropertyDict = container::HashMap<container::String, MapPropertyValue>;

inline constexpr size_t kTerrainTimeOfDayCount = 4;
inline constexpr size_t kTerrainGlobalLightCount = 3;

// Full GlobalLighting chunk retention. The first terrain light is enough for
// the current simple terrain pass, but object and fill lights must survive
// loading so a later environment extractor does not need to re-read a map.
struct TerrainGlobalLighting {
    uint16_t sourceVersion = 0;
    int32_t timeOfDay = 0;
    container::Array<container::Array<TerrainLighting, kTerrainGlobalLightCount>, kTerrainTimeOfDayCount> terrainLights{};
    container::Array<container::Array<TerrainLighting, kTerrainGlobalLightCount>, kTerrainTimeOfDayCount> objectLights{};
    std::optional<uint32_t> shadowColor;
};

struct TerrainTextureClass {
    int32_t firstTile = 0;
    int32_t tileCount = 0;
    int32_t tileWidth = 0;
    // Base terrain classes serialize this legacy GDF field.  It no longer
    // affects runtime behaviour, but retaining it makes the detached source
    // model lossless for all currently understood BlendTileData fields.
    int32_t legacyGdfValue = 0;
    container::String name;
};

struct TerrainBlendDefinition {
    int32_t blendIndex = 0;
    uint8_t horizontal = 0;
    uint8_t vertical = 0;
    uint8_t rightDiagonal = 0;
    uint8_t leftDiagonal = 0;
    uint8_t inverted = 0;
    uint8_t longDiagonal = 0;
    int32_t customEdgeTextureClass = -1;
};

struct TerrainCliffDefinition {
    // The in-memory RefCode field is Short, but the on-disk chunk writes an
    // Int. Keep the serialized width so malformed/truncated tails cannot
    // shift every following UV record and so a future writer can round-trip.
    int32_t tileIndex = 0;
    container::Array<float, 8> uv{};
    uint8_t flip = 0;
    uint8_t mutant = 0;
};

// Cell-addressed and material-source data from BlendTileData. The variable
// tail is parsed into bounded, typed records rather than retained as opaque
// bytes, letting a future material atlas builder consume it without reparsing
// untrusted map input on the render thread.
struct TerrainBlendTileData {
    uint16_t sourceVersion = 0;
    container::Vector<int16_t> baseTileIndices;
    container::Vector<int16_t> blendTileIndices;
    container::Vector<int16_t> extraBlendTileIndices;
    container::Vector<int16_t> cliffInfoIndices;
    container::Vector<uint8_t> cliffCells;
    int32_t bitmapTileCount = 0;
    int32_t edgeTileCount = 0;
    container::Vector<TerrainTextureClass> textureClasses;
    container::Vector<TerrainTextureClass> edgeTextureClasses;
    // Index zero is the implicit no-blend/no-cliff record, matching map cell
    // indices; entries after it were serialized by the map.
    container::Vector<TerrainBlendDefinition> blendDefinitions;
    container::Vector<TerrainCliffDefinition> cliffDefinitions;

    [[nodiscard]] bool isValidFor(size_t sampleCount) const noexcept;
};

struct MapObjectRecord {
    math::vec3 position{};
    float angle = 0.0f;
    // Canonical simulation ingress, quantized after the complete map Object
    // dictionary has been read. The float members above remain the legacy
    // and presentation projection only.
    container::Array<int64_t, 3> positionRaw{};
    int64_t angleRaw = 0;
    std::optional<int64_t> maximumHealthOverrideRaw;
    std::optional<int64_t> initialHealthFractionRaw;
    bool fixedTransformValid = false;
    int32_t flags = 0;
    container::String name;
    MapPropertyDict properties;
    std::optional<uint32_t> waypointId;
    container::String waypointName;
    container::Array<container::String, 3> waypointPathLabels;
    bool waypointPathBiDirectional = false;
};

struct WaypointLinkRecord { uint32_t from = 0; uint32_t to = 0; };
struct PolygonTriggerRecord {
    uint32_t id = 0;
    container::String name;
    container::String layerName;
    bool water = false;
    bool river = false;
    int32_t riverStart = 0;
    container::Vector<math::int3> points;
    // PolygonTriggers v1 had no water flag. RefCode synthesizes one global
    // water polygon while loading it; expose that provenance to modern users.
    bool synthesizedLegacyWater = false;
};

// Immutable map-source data.  Dynamic terrain deformation and water state
// belong in a later TerrainRenderSnapshot, not in this disk-format object.
struct TerrainHeightfieldData {
    int32_t width = 0;
    int32_t height = 0;
    int32_t borderSize = 0;
    uint16_t sourceVersion = 0;
    std::optional<TerrainGlobalLighting> globalLighting;
    std::optional<MapPropertyDict> worldInfo;
    std::optional<TerrainBlendTileData> blendTiles;
    container::Vector<TerrainBoundary> boundaries;
    container::Vector<uint8_t> heights;
    container::Vector<MapObjectRecord> objects;
    container::Vector<WaypointLinkRecord> waypointLinks;
    container::Vector<PolygonTriggerRecord> polygonTriggers;

    bool isValid() const noexcept;
    uint8_t heightSample(int32_t x, int32_t y) const;
    float heightWorld(int32_t x, int32_t y) const;
    math::vec3 worldPosition(int32_t x, int32_t y) const;
    [[nodiscard]] bool hasGlobalLighting() const noexcept { return globalLighting.has_value(); }
};

class MapHeightfieldLoader {
public:
    // PolygonTriggers v1 did not serialize its default water rectangle.  The
    // original engine supplied its extent from GlobalData; keep that runtime
    // configuration injectable rather than giving this detached parser a
    // global singleton dependency. When unset, old standalone tools retain a
    // bounded terrain-extent fallback.
    void setLegacyWaterExtents(float x, float y) noexcept;
    void clearLegacyWaterExtents() noexcept { m_legacyWaterExtents.reset(); }

    // Parses the CkMp terrain/map-source chunks into detached, typed data.
    bool loadFromMemory(container::Span<const uint8_t> bytes);
    bool loadFromFile(container::StringView path);

    const TerrainHeightfieldData& result() const noexcept { return m_result; }
    TerrainHeightfieldData takeResult() noexcept { return std::move(m_result); }
    const container::String& error() const noexcept { return m_error; }

    void reset();

private:
    bool parseUncompressed(container::Span<const uint8_t> bytes);
    bool parseHeightMapData(container::Span<const uint8_t> payload, uint16_t version);
    bool parseGlobalLighting(container::Span<const uint8_t> payload, uint16_t version);
    bool parseBlendTileData(container::Span<const uint8_t> payload, uint16_t version);
    bool parseWorldInfo(container::Span<const uint8_t> payload,
                        const container::HashMap<uint32_t, container::String>& symbols);
    bool parseObjects(container::Span<const uint8_t> payload,
                      const container::HashMap<uint32_t, container::String>& symbols);
    bool parseWaypointLinks(container::Span<const uint8_t> payload);
    bool parsePolygonTriggers(container::Span<const uint8_t> payload, uint16_t version);
    bool addLegacyDefaultWaterArea();
    void setError(container::String message);

    TerrainHeightfieldData m_result;
    container::String m_error;
    // HeightMapData v1 is stored on a 5-unit grid, while the public result is
    // normalized to the modern 10-unit grid. BlendTileData v1 still uses the
    // original dimensions, so retain them until its deferred parse completes.
    int32_t m_legacyHeightMapWidth = 0;
    int32_t m_legacyHeightMapHeight = 0;
    struct LegacyWaterExtents {
        float x = 0.0f;
        float y = 0.0f;
    };
    std::optional<LegacyWaterExtents> m_legacyWaterExtents;
};

} // namespace game::terrain
