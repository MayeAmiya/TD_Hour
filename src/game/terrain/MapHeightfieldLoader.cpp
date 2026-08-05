#include "core/container/hash_containers.h"
#include "MapHeightfieldLoader.h"

#include "core/compression/runtime/manager.h"
#include "math/fixed/q32_32.h"
#include "VFS.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
namespace game::terrain {
namespace {

class ByteReader {
public:
    explicit ByteReader(container::Span<const uint8_t> bytes) noexcept
        : m_bytes(bytes) {}

    template <typename T>
    bool read(T& value) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        if (sizeof(T) > remaining()) return false;
        std::memcpy(&value, m_bytes.data() + m_offset, sizeof(T));
        m_offset += sizeof(T);
        return true;
    }

    bool readBytes(container::Span<uint8_t> destination) noexcept {
        if (destination.size() > remaining()) return false;
        std::memcpy(destination.data(), m_bytes.data() + m_offset, destination.size());
        m_offset += destination.size();
        return true;
    }

    bool readVec3(math::vec3& value) noexcept {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!read(x) || !read(y) || !read(z)) return false;
        value = {x, y, z};
        return true;
    }
    bool readAsciiString(container::String& value) noexcept {
        uint16_t length = 0;
        if (!read(length) || remaining() < length) return false;
        const auto bytes = readSpan(length);
        value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }

    bool readUtf16String(container::U16String& value) noexcept {
        uint16_t length = 0;
        if (!read(length) || static_cast<size_t>(length) > remaining() / sizeof(char16_t)) {
            return false;
        }
        value.resize(length);
        for (char16_t& character : value) {
            uint16_t codeUnit = 0;
            if (!read(codeUnit)) return false;
            character = static_cast<char16_t>(codeUnit);
        }
        return true;
    }

    bool skip(size_t count) noexcept {
        if (count > remaining()) return false;
        m_offset += count;
        return true;
    }

    size_t remaining() const noexcept { return m_bytes.size() - m_offset; }
    bool atEnd() const noexcept { return m_offset == m_bytes.size(); }

    container::Span<const uint8_t> readSpan(size_t count) noexcept {
        if (count > remaining()) return {};
        const auto result = m_bytes.subspan(m_offset, count);
        m_offset += count;
        return result;
    }

private:
    container::Span<const uint8_t> m_bytes;
    size_t m_offset = 0;
};

bool checkedSampleCount(int32_t width, int32_t height, size_t& count) noexcept {
    if (width <= 0 || height <= 0) return false;
    const size_t unsignedWidth = static_cast<size_t>(width);
    const size_t unsignedHeight = static_cast<size_t>(height);
    if (unsignedWidth > std::numeric_limits<size_t>::max() / unsignedHeight) return false;
    count = unsignedWidth * unsignedHeight;
    // A map cannot reasonably exceed a 4096x4096 byte heightfield. This
    // bound prevents hostile CkMp dimensions from turning a small file into a
    // multi-gigabyte allocation while leaving ample room above shipped maps.
    return count <= static_cast<size_t>(4096) * 4096;
}

constexpr int32_t kMaxTextureClasses = 256;
constexpr int32_t kMaxBlendDefinitions = 16192;
constexpr int32_t kMaxCliffDefinitions = 32384;
constexpr int32_t kMaxBitmapTiles = 2047;
constexpr int32_t kMaxMapSymbols = 65536;
constexpr int32_t kMaxTerrainBoundaries = 4096;
constexpr size_t kMaxMapObjects = 250000;
constexpr size_t kMaxDeferredMapChunks = 65536;
constexpr size_t kMaxWaypointLinks = 500000;
constexpr size_t kMaxPolygonTriggers = 65536;
constexpr size_t kMaxPolygonPoints = 65536;
constexpr size_t kMaxTotalPolygonPoints = 1000000;
constexpr size_t kMaxUncompressedMapBytes = 256ull * 1024ull * 1024ull;
constexpr float kLegacyMinimumMapObjectZ = -1000.0f;
constexpr float kLegacyMaximumMapObjectZ = 1593.75f;

// std::isfinite screens NaN/Inf but says nothing about magnitude, and a
// huge-but-finite value saturates q32_32 to +/-INT64_MAX raw.  Downstream that
// becomes signed-overflow UB in raw int64 waypoint arithmetic
// (TerrainLogic::nearestWaypointRaw) and used to drive a non-terminating
// angle-normalization loop.  Bounding the magnitude here is what makes
// fixedTransformValid an actual transform guard rather than a NaN filter.
// Both limits are far outside any real Zero Hour map extent.
constexpr float kMaximumMapObjectHorizontal = 1.0e6f;
constexpr float kMaximumMapObjectAngle = 1.0e4f;
// The two health fields have different units, so they need different bounds.
// objectMaxHPs is absolute HP: it becomes ObjectHealthComponent::maximumFixed
// and initialFixed verbatim, then feeds the current/maximum ratio that picks
// the damage state plus every damage/heal product.  1e6 HP is orders of
// magnitude above the toughest shipped Zero Hour body, so no authored map is
// rejected, while leaving q32_32 (integer range ~2.1e9) ample headroom for
// those products instead of a saturated INT64_MAX maximum health.
// objectInitialHealth is the WorldBuilder Object Panel *percentage* (legacy
// default 100), divided by 100 into a fraction that both consumers apply as
// clamp(initialFixed * fraction, 0, maximumFixed).  Outside [0,100] it has no
// legacy meaning: a negative percentage installs a negative fraction and spawns
// the object dead/rubble, and a huge one saturates that product.
constexpr float kMaximumMapObjectHealthPoints = 1.0e6f;
constexpr float kMaximumMapObjectHealthPercentage = 100.0f;

[[nodiscard]] constexpr bool withinMagnitude(float value, float limit) noexcept {
    return value >= -limit && value <= limit;
}
// HeightMapData v4 stores script-selectable *logical* map extents, not
// strictly a rectangle of samples.  Shipped campaign maps use zero/negative
// placeholder entries and occasionally extend one cell beyond the sampled
// terrain.  Retain those raw values (as RefCode does), but bound absurd input
// so later world-space calculations remain finite and overflow-free.
constexpr int32_t kMaxTerrainBoundaryExtent = 1'000'000;
constexpr int32_t kBlendDefinitionFlag = 0x7ADA0000;

container::String propertyKeyName(uint32_t id,
                            const container::HashMap<uint32_t, container::String>& symbols) {
    if (const auto found = symbols.find(id); found != symbols.end()) {
        return found->second;
    }
    // Preserve an unknown key rather than dropping map data because a newer
    // map writer added a symbol this build does not understand yet.
    return "#" + std::to_string(id);
}

bool readPropertyDict(ByteReader& reader,
                      const container::HashMap<uint32_t, container::String>& symbols,
                      MapPropertyDict& output,
                      container::String& error) {
    uint16_t count = 0;
    if (!reader.read(count)) {
        error = "Truncated map property dictionary";
        return false;
    }
    // Every entry contains at least a 32-bit key/type plus one payload byte.
    if (static_cast<size_t>(count) > reader.remaining() / (sizeof(uint32_t) + 1)) {
        error = "Invalid map property dictionary count";
        return false;
    }
    output.clear();
    output.reserve(count);
    for (uint16_t index = 0; index < count; ++index) {
        uint32_t keyAndType = 0;
        if (!reader.read(keyAndType)) {
            error = "Truncated map property dictionary entry";
            return false;
        }

        const container::String key = propertyKeyName(keyAndType >> 8, symbols);
        const uint8_t type = static_cast<uint8_t>(keyAndType & 0xff);
        switch (type) {
        case 0: {
            uint8_t value = 0;
            if (!reader.read(value)) {
                error = "Truncated boolean map property";
                return false;
            }
            output.insert_or_assign(key, value != 0);
            break;
        }
        case 1: {
            int32_t value = 0;
            if (!reader.read(value)) {
                error = "Truncated integer map property";
                return false;
            }
            output.insert_or_assign(key, value);
            break;
        }
        case 2: {
            float value = 0.0f;
            if (!reader.read(value)) {
                error = "Truncated real map property";
                return false;
            }
            output.insert_or_assign(key, value);
            break;
        }
        case 3: {
            container::String value;
            if (!reader.readAsciiString(value)) {
                error = "Truncated ASCII map property";
                return false;
            }
            output.insert_or_assign(key, std::move(value));
            break;
        }
        case 4: {
            container::U16String value;
            if (!reader.readUtf16String(value)) {
                error = "Truncated Unicode map property";
                return false;
            }
            output.insert_or_assign(key, std::move(value));
            break;
        }
        default:
            error = "Unsupported map property value type";
            return false;
        }
    }
    return true;
}

bool readTextureClass(ByteReader& reader, TerrainTextureClass& output,
                      bool containsLegacyGdf, container::String& error) {
    if (!reader.read(output.firstTile) || !reader.read(output.tileCount) ||
        !reader.read(output.tileWidth)) {
        error = "Truncated terrain texture class";
        return false;
    }
    // RefCode deliberately accepts a negative firstTile as a disabled class
    // sentinel. Such a class has no source-tile range and is skipped by the
    // terrain/material lookup, but it is still legitimate serialized map
    // data (often inherited from an editor/default terrain definition).
    if (output.tileCount < 0 ||
        (output.firstTile >= 0 && output.tileWidth <= 0)) {
        error = "Invalid terrain texture class";
        return false;
    }
    if (containsLegacyGdf) {
        if (!reader.read(output.legacyGdfValue)) {
            error = "Truncated terrain texture class legacy field";
            return false;
        }
    }
    if (!reader.readAsciiString(output.name)) {
        error = "Truncated terrain texture class name";
        return false;
    }
    return true;
}

bool textureClassRangeIsValid(const TerrainTextureClass& textureClass,
                              int32_t availableTiles) noexcept {
    if (availableTiles < 0 || textureClass.tileCount < 0) {
        return false;
    }
    // A negative first tile means this authored class is disabled. RefCode
    // checks this sentinel before every source-tile lookup, so there is no
    // range to validate and no reason to reject an otherwise valid map.
    if (textureClass.firstTile < 0) return true;
    if (textureClass.tileWidth <= 0 || textureClass.firstTile > availableTiles) return false;
    return textureClass.tileCount <= availableTiles - textureClass.firstTile;
}

bool textureClassesAreValid(const container::Vector<TerrainTextureClass>& textureClasses,
                            int32_t availableTiles) noexcept {
    return std::all_of(textureClasses.begin(), textureClasses.end(),
                       [availableTiles](const TerrainTextureClass& textureClass) {
                           return textureClassRangeIsValid(textureClass, availableTiles);
                       });
}

} // namespace

bool TerrainHeightfieldData::isValid() const noexcept {
    size_t sampleCount = 0;
    return checkedSampleCount(width, height, sampleCount) && heights.size() == sampleCount;
}

bool TerrainBlendTileData::isValidFor(size_t sampleCount) const noexcept {
    if (baseTileIndices.size() != sampleCount || blendTileIndices.size() != sampleCount ||
        extraBlendTileIndices.size() != sampleCount || cliffInfoIndices.size() != sampleCount ||
        cliffCells.size() != sampleCount || bitmapTileCount <= 0 ||
        bitmapTileCount > kMaxBitmapTiles || edgeTileCount < 0 ||
        edgeTileCount > kMaxBitmapTiles || textureClasses.empty() ||
        textureClasses.size() > kMaxTextureClasses || edgeTextureClasses.size() > kMaxTextureClasses ||
        blendDefinitions.empty() || blendDefinitions.size() > kMaxBlendDefinitions ||
        cliffDefinitions.empty() || cliffDefinitions.size() > kMaxCliffDefinitions ||
        !textureClassesAreValid(textureClasses, bitmapTileCount) ||
        !textureClassesAreValid(edgeTextureClasses, edgeTileCount)) {
        return false;
    }
    const auto validCellIndices = [](const container::Vector<int16_t>& values, size_t definitionCount) {
        return std::all_of(values.begin(), values.end(), [definitionCount](int16_t value) {
            return value >= 0 && static_cast<size_t>(value) < definitionCount;
        });
    };
    return validCellIndices(blendTileIndices, blendDefinitions.size()) &&
           validCellIndices(extraBlendTileIndices, blendDefinitions.size()) &&
           validCellIndices(cliffInfoIndices, cliffDefinitions.size()) &&
           std::all_of(blendDefinitions.begin(), blendDefinitions.end(),
                       [this](const TerrainBlendDefinition& definition) {
                           return definition.customEdgeTextureClass >= -1 &&
                               definition.customEdgeTextureClass <
                                   static_cast<int32_t>(edgeTextureClasses.size());
                       });
}

uint8_t TerrainHeightfieldData::heightSample(int32_t x, int32_t y) const {
    if (!isValid() || x < 0 || y < 0 || x >= width || y >= height) return 0;
    return heights[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
}

float TerrainHeightfieldData::heightWorld(int32_t x, int32_t y) const {
    return static_cast<float>(heightSample(x, y)) * kMapHeightWorldScale;
}

math::vec3 TerrainHeightfieldData::worldPosition(int32_t x, int32_t y) const {
    return {
        static_cast<float>(x - borderSize) * kMapCellWorldSize,
        static_cast<float>(y - borderSize) * kMapCellWorldSize,
        heightWorld(x, y),
    };
}

void MapHeightfieldLoader::reset() {
    m_result = {};
    m_error.clear();
    m_legacyHeightMapWidth = 0;
    m_legacyHeightMapHeight = 0;
}

void MapHeightfieldLoader::setError(container::String message) {
    m_error = std::move(message);
}

void MapHeightfieldLoader::setLegacyWaterExtents(float x, float y) noexcept {
    // The legacy polygon stream stores integer world coordinates. Do not let
    // malformed configuration manufacture an overflowing synthetic trigger;
    // the caller can clear this override and use the bounded map fallback.
    if (std::isfinite(x) && std::isfinite(y) && x >= 0.0f && y >= 0.0f &&
        x <= static_cast<float>(std::numeric_limits<int32_t>::max()) &&
        y <= static_cast<float>(std::numeric_limits<int32_t>::max())) {
        m_legacyWaterExtents = MapHeightfieldLoader::LegacyWaterExtents{x, y};
    } else {
        m_legacyWaterExtents.reset();
    }
}

bool MapHeightfieldLoader::loadFromFile(container::StringView path) {
    try {
        container::Vector<uint8_t> bytes;
        if (!io::VFS::instance().readToBuffer(path, bytes)) {
            reset();
            setError("Failed to read map: " + container::String(path));
            return false;
        }
        return loadFromMemory(bytes);
    } catch (const std::bad_alloc&) {
        reset();
        setError("Map loading ran out of memory");
        return false;
    }
}

bool MapHeightfieldLoader::loadFromMemory(container::Span<const uint8_t> bytes) {
    reset();
    try {
        // Parsing is deliberately transactional from the caller's point of
        // view. Individual chunk parsers populate m_result as they validate
        // their input, but a later corrupt chunk must never leave a caller
        // with a plausible-looking partial map after loadFromMemory() has
        // returned false.
        const auto finish = [this](bool succeeded) {
            if (succeeded) return true;

            container::String failure = std::move(m_error);
            m_result = {};
            m_legacyHeightMapWidth = 0;
            m_legacyHeightMapHeight = 0;
            m_error = failure.empty() ? "Failed to parse CkMp map" : std::move(failure);
            return false;
        };
        if (bytes.empty() || bytes.size() > kMaxUncompressedMapBytes ||
            bytes.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            setError("Invalid map byte stream");
            return finish(false);
        }

        const auto compressedSize = static_cast<int32_t>(bytes.size());
        if (compression::manager::is_data_compressed(bytes.data(), compressedSize)) {
            const int32_t uncompressedSize = compression::manager::uncompressed_size(bytes.data(), compressedSize);
            if (uncompressedSize <= 0) {
                setError("Invalid compressed map header");
                return finish(false);
            }
            if (static_cast<size_t>(uncompressedSize) > kMaxUncompressedMapBytes) {
                setError("Compressed map expands beyond the supported size limit");
                return finish(false);
            }
            container::Vector<uint8_t> uncompressed(static_cast<size_t>(uncompressedSize));
            const int32_t decodedSize = compression::manager::decompress(
                uncompressed.data(), uncompressedSize, bytes.data(), compressedSize);
            if (decodedSize != uncompressedSize) {
                setError("Failed to decompress map");
                return finish(false);
            }
            return finish(parseUncompressed(uncompressed));
        }

        return finish(parseUncompressed(bytes));
    } catch (const std::bad_alloc&) {
        reset();
        setError("Map loading ran out of memory");
        return false;
    }
}

bool MapHeightfieldLoader::parseUncompressed(container::Span<const uint8_t> bytes) {
    ByteReader reader(bytes);
    char signature[4]{};
    int32_t symbolCount = 0;
    if (!reader.read(signature) || std::memcmp(signature, "CkMp", sizeof(signature)) != 0 ||
        !reader.read(symbolCount) || symbolCount < 0) {
        setError("Invalid CkMp map header");
        return false;
    }

    // Each symbol requires at least one length byte plus a 32-bit ID.
    if (symbolCount > kMaxMapSymbols ||
        static_cast<size_t>(symbolCount) > reader.remaining() / (sizeof(uint32_t) + 1)) {
        setError("Invalid CkMp symbol count");
        return false;
    }
    container::HashMap<uint32_t, container::String> symbols;
    symbols.reserve(static_cast<size_t>(symbolCount));
    for (int32_t index = 0; index < symbolCount; ++index) {
        uint8_t length = 0;
        uint32_t id = 0;
        if (!reader.read(length) || reader.remaining() < static_cast<size_t>(length)) {
            setError("Truncated CkMp symbol table");
            return false;
        }
        const auto nameBytes = reader.readSpan(length);
        if (!reader.read(id)) {
            setError("Truncated CkMp symbol ID");
            return false;
        }
        const container::String name(reinterpret_cast<const char*>(nameBytes.data()), nameBytes.size());
        if (!symbols.emplace(id, name).second) {
            setError("Duplicate CkMp symbol ID");
            return false;
        }
    }

    struct DeferredChunk {
        uint16_t version = 0;
        container::Vector<uint8_t> payload;
    };
    std::optional<DeferredChunk> heightMap;
    std::optional<DeferredChunk> globalLighting;
    std::optional<DeferredChunk> blendTiles;
    std::optional<DeferredChunk> worldInfo;
    container::Vector<DeferredChunk> objects;
    container::Vector<DeferredChunk> waypointLinks;
    container::Vector<DeferredChunk> polygonTriggers;
    while (!reader.atEnd()) {
        uint32_t id = 0;
        uint16_t version = 0;
        int32_t payloadSize = 0;
        if (!reader.read(id) || !reader.read(version) || !reader.read(payloadSize) || payloadSize < 0 ||
            reader.remaining() < static_cast<size_t>(payloadSize)) {
            setError("Truncated or invalid CkMp data chunk");
            return false;
        }

        const auto payload = reader.readSpan(static_cast<size_t>(payloadSize));
        const auto symbol = symbols.find(id);
        if (symbol == symbols.end()) continue;
        const auto retain = [&payload, version] {
            DeferredChunk result;
            result.version = version;
            result.payload.assign(payload.begin(), payload.end());
            return result;
        };
        if (symbol->second == "HeightMapData") {
            if (heightMap) {
                setError("Map contains more than one HeightMapData chunk");
                return false;
            }
            heightMap = retain();
        } else if (symbol->second == "GlobalLighting") {
            if (globalLighting) {
                setError("Map contains more than one GlobalLighting chunk");
                return false;
            }
            globalLighting = retain();
        } else if (symbol->second == "BlendTileData") {
            if (blendTiles) {
                setError("Map contains more than one BlendTileData chunk");
                return false;
            }
            blendTiles = retain();
        } else if (symbol->second == "WorldInfo") {
            if (worldInfo) {
                setError("Map contains more than one WorldInfo chunk");
                return false;
            }
            worldInfo = retain();
        } else if (symbol->second == "ObjectsList") {
            if (objects.size() >= kMaxDeferredMapChunks) {
                setError("Map contains too many ObjectsList chunks");
                return false;
            }
            objects.push_back(retain());
        } else if (symbol->second == "WaypointsList") {
            if (waypointLinks.size() >= kMaxDeferredMapChunks) {
                setError("Map contains too many WaypointsList chunks");
                return false;
            }
            waypointLinks.push_back(retain());
        } else if (symbol->second == "PolygonTriggers") {
            if (polygonTriggers.size() >= kMaxDeferredMapChunks) {
                setError("Map contains too many PolygonTriggers chunks");
                return false;
            }
            polygonTriggers.push_back(retain());
        }
    }

    if (!heightMap) {
        setError("Map has no HeightMapData chunk");
        return false;
    }
    if (!parseHeightMapData(heightMap->payload, heightMap->version)) {
        return false;
    }
    if (globalLighting && !parseGlobalLighting(globalLighting->payload, globalLighting->version)) return false;
    if (blendTiles && !parseBlendTileData(blendTiles->payload, blendTiles->version)) return false;
    if (worldInfo && !parseWorldInfo(worldInfo->payload, symbols)) return false;
    for (const DeferredChunk& chunk : objects) {
        if (!parseObjects(chunk.payload, symbols)) return false;
    }
    for (const DeferredChunk& chunk : waypointLinks) {
        if (!parseWaypointLinks(chunk.payload)) return false;
    }
    bool needsLegacyDefaultWater = false;
    for (const DeferredChunk& chunk : polygonTriggers) {
        if (!parsePolygonTriggers(chunk.payload, chunk.version)) return false;
        needsLegacyDefaultWater = needsLegacyDefaultWater || chunk.version == 1;
    }
    if (needsLegacyDefaultWater && !addLegacyDefaultWaterArea()) {
        return false;
    }
    return true;
}

bool MapHeightfieldLoader::parseHeightMapData(container::Span<const uint8_t> payload, uint16_t version) {
    ByteReader reader(payload);
    int32_t width = 0;
    int32_t height = 0;
    int32_t borderSize = 0;
    if (!reader.read(width) || !reader.read(height)) {
        setError("Truncated HeightMapData dimensions");
        return false;
    }

    size_t sampleCount = 0;
    if (!checkedSampleCount(width, height, sampleCount)) {
        setError("Invalid HeightMapData dimensions");
        return false;
    }

    if (version >= 3 && !reader.read(borderSize)) {
        setError("Truncated HeightMapData border size");
        return false;
    }
    if (borderSize < 0 || borderSize > width / 2 || borderSize > height / 2) {
        setError("Invalid HeightMapData border size");
        return false;
    }

    container::Vector<TerrainBoundary> boundaries;
    if (version >= 4) {
        int32_t boundaryCount = 0;
        if (!reader.read(boundaryCount) || boundaryCount < 0 ||
            boundaryCount > kMaxTerrainBoundaries ||
            static_cast<size_t>(boundaryCount) > reader.remaining() / (sizeof(int32_t) * 2)) {
            setError("Invalid HeightMapData boundary list");
            return false;
        }
        boundaries.resize(static_cast<size_t>(boundaryCount));
        for (auto& boundary : boundaries) {
            if (!reader.read(boundary.width) || !reader.read(boundary.height)) {
                setError("Truncated HeightMapData boundary");
                return false;
            }
            if (boundary.width < -kMaxTerrainBoundaryExtent ||
                boundary.width > kMaxTerrainBoundaryExtent ||
                boundary.height < -kMaxTerrainBoundaryExtent ||
                boundary.height > kMaxTerrainBoundaryExtent) {
                setError("Invalid HeightMapData boundary extent");
                return false;
            }
        }
    } else {
        boundaries.push_back({width - borderSize * 2, height - borderSize * 2});
    }

    int32_t dataSize = 0;
    if (!reader.read(dataSize) || dataSize <= 0 || static_cast<size_t>(dataSize) != sampleCount ||
        reader.remaining() != sampleCount) {
        setError("Invalid HeightMapData sample payload");
        return false;
    }

    TerrainHeightfieldData parsed;
    parsed.width = width;
    parsed.height = height;
    parsed.borderSize = borderSize;
    parsed.sourceVersion = version;
    parsed.boundaries = std::move(boundaries);
    parsed.heights.resize(sampleCount);
    if (!reader.readBytes(parsed.heights) || !reader.atEnd()) {
        setError("Truncated HeightMapData samples");
        return false;
    }

    // Version 1 stored 5-unit cells.  RefCode samples every other value and
    // then treats the output as the normal 10-unit grid; do the same while
    // making the compact output explicit and bounds-safe.
    if (version == 1) {
        const int32_t legacyWidth = parsed.width;
        const int32_t legacyHeight = parsed.height;
        const int32_t newWidth = (legacyWidth + 1) / 2;
        const int32_t newHeight = (legacyHeight + 1) / 2;
        container::Vector<uint8_t> downsampled(static_cast<size_t>(newWidth) * static_cast<size_t>(newHeight));
        for (int32_t y = 0; y < newHeight; ++y) {
            for (int32_t x = 0; x < newWidth; ++x) {
                downsampled[static_cast<size_t>(y) * static_cast<size_t>(newWidth) + static_cast<size_t>(x)] =
                    parsed.heights[static_cast<size_t>(y * 2) * static_cast<size_t>(legacyWidth) + static_cast<size_t>(x * 2)];
            }
        }
        parsed.width = newWidth;
        parsed.height = newHeight;
        parsed.heights = std::move(downsampled);
        parsed.boundaries = {{newWidth, newHeight}};
    }

    m_result = std::move(parsed);
    m_legacyHeightMapWidth = version == 1 ? width : 0;
    m_legacyHeightMapHeight = version == 1 ? height : 0;
    return true;
}

bool MapHeightfieldLoader::parseGlobalLighting(container::Span<const uint8_t> payload, uint16_t version) {
    ByteReader reader(payload);
    TerrainGlobalLighting lighting;
    lighting.sourceVersion = version;
    if (!reader.read(lighting.timeOfDay)) {
        setError("Truncated GlobalLighting time of day");
        return false;
    }
    for (auto& period : lighting.terrainLights) {
        for (TerrainLighting& entry : period) {
            entry = {{}, {}, {0.0f, 0.0f, -1.0f}};
        }
    }
    for (auto& period : lighting.objectLights) {
        for (TerrainLighting& entry : period) {
            entry = {{}, {}, {0.0f, 0.0f, -1.0f}};
        }
    }

    for (size_t timeSlot = 0; timeSlot < kTerrainTimeOfDayCount; ++timeSlot) {
        TerrainLighting& terrainPrimary = lighting.terrainLights[timeSlot][0];
        TerrainLighting& objectPrimary = lighting.objectLights[timeSlot][0];
        if (!reader.readVec3(terrainPrimary.ambient) || !reader.readVec3(terrainPrimary.diffuse) ||
            !reader.readVec3(terrainPrimary.direction) ||
            !reader.readVec3(objectPrimary.ambient) || !reader.readVec3(objectPrimary.diffuse) ||
            !reader.readVec3(objectPrimary.direction)) {
            setError("Truncated GlobalLighting primary lights");
            return false;
        }
        if (version >= 2) {
            for (size_t lightIndex = 1; lightIndex < kTerrainGlobalLightCount; ++lightIndex) {
                TerrainLighting& objectLight = lighting.objectLights[timeSlot][lightIndex];
                if (!reader.readVec3(objectLight.ambient) || !reader.readVec3(objectLight.diffuse) ||
                    !reader.readVec3(objectLight.direction)) {
                    setError("Truncated GlobalLighting object fill lights");
                    return false;
                }
            }
        }
        if (version >= 3) {
            for (size_t lightIndex = 1; lightIndex < kTerrainGlobalLightCount; ++lightIndex) {
                TerrainLighting& terrainLight = lighting.terrainLights[timeSlot][lightIndex];
                if (!reader.readVec3(terrainLight.ambient) || !reader.readVec3(terrainLight.diffuse) ||
                    !reader.readVec3(terrainLight.direction)) {
                    setError("Truncated GlobalLighting terrain fill lights");
                    return false;
                }
            }
        }
    }
    if (reader.remaining() == sizeof(uint32_t)) {
        uint32_t shadowColor = 0;
        if (!reader.read(shadowColor)) {
            setError("Truncated GlobalLighting shadow color");
            return false;
        }
        lighting.shadowColor = shadowColor;
    }
    if (!reader.atEnd()) {
        setError("Unexpected bytes in GlobalLighting");
        return false;
    }
    m_result.globalLighting = std::move(lighting);
    return true;
}

bool MapHeightfieldLoader::parseWorldInfo(
    container::Span<const uint8_t> payload,
    const container::HashMap<uint32_t, container::String>& symbols) {
    ByteReader reader(payload);
    MapPropertyDict parsed;
    container::String parseError;
    if (!readPropertyDict(reader, symbols, parsed, parseError)) {
        setError("Invalid WorldInfo: " + parseError);
        return false;
    }
    if (!reader.atEnd()) {
        setError("Unexpected bytes in WorldInfo");
        return false;
    }
    m_result.worldInfo = std::move(parsed);
    return true;
}

bool MapHeightfieldLoader::parseBlendTileData(container::Span<const uint8_t> payload, uint16_t version) {
    if (!m_result.isValid()) {
        setError("BlendTileData encountered before a valid HeightMapData");
        return false;
    }

    // HeightMapData v1 is normalized to a 10-unit grid before deferred map
    // chunks are parsed. BlendTileData v1 remains serialized on the original
    // 5-unit grid, exactly as in RefCode, so read it with the retained source
    // dimensions and normalize its arrays after the payload is complete.
    const bool legacyV1 = version == 1;
    int32_t sourceWidth = m_result.width;
    int32_t sourceHeight = m_result.height;
    if (legacyV1) {
        if (m_result.sourceVersion != 1 || m_legacyHeightMapWidth <= 0 ||
            m_legacyHeightMapHeight <= 0) {
            setError("BlendTileData v1 does not match HeightMapData dimensions");
            return false;
        }
        sourceWidth = m_legacyHeightMapWidth;
        sourceHeight = m_legacyHeightMapHeight;
    }

    size_t sourceSampleCount = 0;
    if (!checkedSampleCount(sourceWidth, sourceHeight, sourceSampleCount)) {
        setError("Invalid BlendTileData dimensions");
        return false;
    }

    ByteReader reader(payload);
    int32_t dataSize = 0;
    if (!reader.read(dataSize) || dataSize <= 0 ||
        static_cast<size_t>(dataSize) != sourceSampleCount) {
        setError("BlendTileData sample count does not match HeightMapData");
        return false;
    }

    TerrainBlendTileData parsed;
    parsed.sourceVersion = version;
    const auto readIndices = [&reader, sourceSampleCount](container::Vector<int16_t>& output) {
        output.resize(sourceSampleCount);
        for (int16_t& value : output) {
            if (!reader.read(value)) return false;
        }
        return true;
    };
    if (!readIndices(parsed.baseTileIndices) || !readIndices(parsed.blendTileIndices)) {
        setError("Truncated BlendTileData base or blend indices");
        return false;
    }
    parsed.extraBlendTileIndices.assign(sourceSampleCount, 0);
    parsed.cliffInfoIndices.assign(sourceSampleCount, 0);
    if (version >= 6 && !readIndices(parsed.extraBlendTileIndices)) {
        setError("Truncated BlendTileData extra blend indices");
        return false;
    }
    if (version >= 5 && !readIndices(parsed.cliffInfoIndices)) {
        setError("Truncated BlendTileData cliff indices");
        return false;
    }

    parsed.cliffCells.assign(sourceSampleCount, 0);
    if (version >= 7) {
        const size_t serializedBytesPerRow = static_cast<size_t>(version == 7
            ? (sourceWidth + 1) / 8 : (sourceWidth + 7) / 8);
        const size_t packedBytes = serializedBytesPerRow * static_cast<size_t>(sourceHeight);
        const auto packed = reader.readSpan(packedBytes);
        if (packed.size() != packedBytes) {
            setError("Truncated BlendTileData cliff flags");
            return false;
        }
        const size_t normalizedBytesPerRow = static_cast<size_t>((sourceWidth + 7) / 8);
        container::Vector<uint8_t> normalizedFlags(normalizedBytesPerRow * static_cast<size_t>(sourceHeight), 0);
        const size_t bytesToCopy = std::min(serializedBytesPerRow, normalizedBytesPerRow);
        for (int32_t y = 0; y < sourceHeight; ++y) {
            if (bytesToCopy == 0) continue;
            std::memcpy(normalizedFlags.data() + static_cast<size_t>(y) * normalizedBytesPerRow,
                        packed.data() + static_cast<size_t>(y) * serializedBytesPerRow,
                        bytesToCopy);
        }
        for (int32_t y = 0; y < sourceHeight; ++y) {
            for (int32_t x = 0; x < sourceWidth; ++x) {
                parsed.cliffCells[static_cast<size_t>(y) * static_cast<size_t>(sourceWidth) + x] =
                    (normalizedFlags[static_cast<size_t>(y) * normalizedBytesPerRow + static_cast<size_t>(x) / 8] >> (x & 7)) & 1;
            }
        }
    } else if (!legacyV1) {
        for (int32_t y = 0; y < m_result.height - 1; ++y) {
            for (int32_t x = 0; x < m_result.width - 1; ++x) {
                const float minHeight = std::min({m_result.heightWorld(x, y), m_result.heightWorld(x + 1, y),
                                                  m_result.heightWorld(x + 1, y + 1), m_result.heightWorld(x, y + 1)});
                const float maxHeight = std::max({m_result.heightWorld(x, y), m_result.heightWorld(x + 1, y),
                                                  m_result.heightWorld(x + 1, y + 1), m_result.heightWorld(x, y + 1)});
                parsed.cliffCells[static_cast<size_t>(y) * static_cast<size_t>(m_result.width) + x] =
                    maxHeight - minHeight > 9.8f ? 1 : 0;
            }
        }
    }

    if (legacyV1) {
        const size_t normalizedSampleCount = m_result.heights.size();
        container::Vector<int16_t> normalizedBaseTiles(normalizedSampleCount);
        for (int32_t y = 0; y < m_result.height; ++y) {
            for (int32_t x = 0; x < m_result.width; ++x) {
                normalizedBaseTiles[static_cast<size_t>(y) * static_cast<size_t>(m_result.width) + x] =
                    parsed.baseTileIndices[static_cast<size_t>(y * 2) * static_cast<size_t>(sourceWidth) +
                                           static_cast<size_t>(x * 2)];
            }
        }
        parsed.baseTileIndices = std::move(normalizedBaseTiles);
        // RefCode's v1 post-load path keeps only the downsampled base tile;
        // all blend and cliff definitions are reset to modern defaults.
        parsed.blendTileIndices.assign(normalizedSampleCount, 0);
        parsed.extraBlendTileIndices.assign(normalizedSampleCount, 0);
        parsed.cliffInfoIndices.assign(normalizedSampleCount, 0);
        parsed.cliffCells.assign(normalizedSampleCount, 0);

        // Version 1 has no persisted cliff bitmap. Reconstruct the modern
        // cliff flags from the normalized heights, matching the existing
        // pre-v7 behavior rather than retaining stale 5-unit-grid flags.
        for (int32_t y = 0; y < m_result.height - 1; ++y) {
            for (int32_t x = 0; x < m_result.width - 1; ++x) {
                const float minHeight = std::min({m_result.heightWorld(x, y), m_result.heightWorld(x + 1, y),
                                                  m_result.heightWorld(x + 1, y + 1), m_result.heightWorld(x, y + 1)});
                const float maxHeight = std::max({m_result.heightWorld(x, y), m_result.heightWorld(x + 1, y),
                                                  m_result.heightWorld(x + 1, y + 1), m_result.heightWorld(x, y + 1)});
                parsed.cliffCells[static_cast<size_t>(y) * static_cast<size_t>(m_result.width) + x] =
                    maxHeight - minHeight > 9.8f ? 1 : 0;
            }
        }
    }

    if (!reader.read(parsed.bitmapTileCount) || parsed.bitmapTileCount <= 0 ||
        parsed.bitmapTileCount > kMaxBitmapTiles) {
        setError("Invalid BlendTileData bitmap tile count");
        return false;
    }

    int32_t blendDefinitionCount = 0;
    if (!reader.read(blendDefinitionCount) || blendDefinitionCount <= 0 ||
        blendDefinitionCount > kMaxBlendDefinitions) {
        setError("Invalid BlendTileData blend definition count");
        return false;
    }

    // Old writers legitimately emit zero here when a map has no authored
    // cliff records.  RefCode still treats index zero as the implicit default
    // and simply performs no serialized-record loop.  Keep that invariant in
    // the modern value model instead of rejecting otherwise valid maps.
    int32_t serializedCliffDefinitionCount = 1;
    if (version >= 5 && (!reader.read(serializedCliffDefinitionCount) ||
                         serializedCliffDefinitionCount < 0 ||
                         serializedCliffDefinitionCount > kMaxCliffDefinitions)) {
        setError("Invalid BlendTileData cliff definition count");
        return false;
    }
    const int32_t cliffDefinitionCount = std::max(1, serializedCliffDefinitionCount);

    int32_t textureClassCount = 0;
    if (!reader.read(textureClassCount) || textureClassCount <= 0 ||
        textureClassCount > kMaxTextureClasses) {
        setError("Invalid BlendTileData texture class count");
        return false;
    }
    parsed.textureClasses.resize(static_cast<size_t>(textureClassCount));
    for (TerrainTextureClass& textureClass : parsed.textureClasses) {
        container::String parseError;
        if (!readTextureClass(reader, textureClass, true, parseError)) {
            setError("Invalid BlendTileData texture class: " + parseError);
            return false;
        }
    }

    if (version >= 4) {
        int32_t edgeTextureClassCount = 0;
        if (!reader.read(parsed.edgeTileCount) || parsed.edgeTileCount < 0 ||
            parsed.edgeTileCount > kMaxBitmapTiles ||
            !reader.read(edgeTextureClassCount) || edgeTextureClassCount < 0 ||
            edgeTextureClassCount > kMaxTextureClasses) {
            setError("Invalid BlendTileData edge texture classes");
            return false;
        }
        parsed.edgeTextureClasses.resize(static_cast<size_t>(edgeTextureClassCount));
        for (TerrainTextureClass& textureClass : parsed.edgeTextureClasses) {
            container::String parseError;
            if (!readTextureClass(reader, textureClass, false, parseError)) {
                setError("Invalid BlendTileData edge texture class: " + parseError);
                return false;
            }
        }
    }
    if (!textureClassesAreValid(parsed.textureClasses, parsed.bitmapTileCount) ||
        !textureClassesAreValid(parsed.edgeTextureClasses, parsed.edgeTileCount)) {
        setError("Invalid BlendTileData texture class tile range");
        return false;
    }

    parsed.blendDefinitions.resize(static_cast<size_t>(blendDefinitionCount));
    for (int32_t index = 1; index < blendDefinitionCount; ++index) {
        TerrainBlendDefinition& definition = parsed.blendDefinitions[static_cast<size_t>(index)];
        if (!reader.read(definition.blendIndex) || !reader.read(definition.horizontal) ||
            !reader.read(definition.vertical) || !reader.read(definition.rightDiagonal) ||
            !reader.read(definition.leftDiagonal) || !reader.read(definition.inverted)) {
            setError("Truncated BlendTileData blend definition");
            return false;
        }
        if (version >= 3 && !reader.read(definition.longDiagonal)) {
            setError("Truncated BlendTileData long-diagonal definition");
            return false;
        }
        if (version >= 4 && !reader.read(definition.customEdgeTextureClass)) {
            setError("Truncated BlendTileData custom edge definition");
            return false;
        }
        int32_t flag = 0;
        if (!reader.read(flag) || flag != kBlendDefinitionFlag) {
            setError("Invalid BlendTileData blend definition marker");
            return false;
        }
    }

    parsed.cliffDefinitions.resize(static_cast<size_t>(cliffDefinitionCount));
    if (version >= 5) {
        for (int32_t index = 1; index < serializedCliffDefinitionCount; ++index) {
            TerrainCliffDefinition& definition = parsed.cliffDefinitions[static_cast<size_t>(index)];
            if (!reader.read(definition.tileIndex)) {
                setError("Truncated BlendTileData cliff definition tile");
                return false;
            }
            for (float& coordinate : definition.uv) {
                if (!reader.read(coordinate)) {
                    setError("Truncated BlendTileData cliff definition UV");
                    return false;
                }
            }
            if (!reader.read(definition.flip) || !reader.read(definition.mutant)) {
                setError("Truncated BlendTileData cliff definition flags");
                return false;
            }
        }
    }
    if (!reader.atEnd()) {
        setError("Unexpected bytes in BlendTileData");
        return false;
    }

    // Original maps in the wild can contain stale selection values after a
    // terrain class was edited. RefCode repairs these cells to their implicit
    // index zero after all definition counts are known; retain that forgiving
    // compatibility while making the resulting typed data self-validating.
    const auto repairCellIndices = [](container::Vector<int16_t>& values, size_t definitionCount) {
        for (int16_t& value : values) {
            if (value < 0 || static_cast<size_t>(value) >= definitionCount) value = 0;
        }
    };
    repairCellIndices(parsed.blendTileIndices, parsed.blendDefinitions.size());
    repairCellIndices(parsed.extraBlendTileIndices, parsed.blendDefinitions.size());
    repairCellIndices(parsed.cliffInfoIndices, parsed.cliffDefinitions.size());

    if (legacyV1) {
        // The source texture classes remain useful after normalization, but
        // RefCode explicitly discards old 5-unit blend and cliff definitions.
        parsed.blendDefinitions.resize(1);
        parsed.cliffDefinitions.resize(1);
    }
    if (!parsed.isValidFor(m_result.heights.size())) {
        setError("Invalid BlendTileData typed data");
        return false;
    }
    m_result.blendTiles = std::move(parsed);
    return true;
}

bool MapHeightfieldLoader::parseObjects(
    container::Span<const uint8_t> payload, const container::HashMap<uint32_t, container::String>& symbols) {
    ByteReader chunks(payload);
    size_t chunkCount = 0;
    while (!chunks.atEnd()) {
        if (++chunkCount > kMaxMapObjects) {
            setError("ObjectsList exceeds the supported object chunk limit");
            return false;
        }

        uint32_t id = 0;
        uint16_t version = 0;
        int32_t size = 0;
        if (!chunks.read(id) || !chunks.read(version) || !chunks.read(size) || size < 0 ||
            chunks.remaining() < static_cast<size_t>(size)) {
            setError("Invalid ObjectsList chunk");
            return false;
        }
        const auto data = chunks.readSpan(static_cast<size_t>(size));
        const auto type = symbols.find(id);
        if (type == symbols.end() || type->second != "Object") continue;

        ByteReader object(data);
        MapObjectRecord record;
        if (!object.readVec3(record.position) || !object.read(record.angle) || !object.read(record.flags) ||
            !object.readAsciiString(record.name)) {
            setError("Truncated map Object");
            return false;
        }
        if (version <= 2) record.position[2] = 0.0f;
        if (version >= 2) {
            container::String parseError;
            if (!readPropertyDict(object, symbols, record.properties, parseError)) {
                setError("Invalid Object dictionary: " + parseError);
                return false;
            }
        }
        if (!object.atEnd()) {
            setError("Unexpected bytes in map Object");
            return false;
        }

        // Keep convenience waypoint fields for the logic API, but derive them
        // from the complete typed dictionary rather than throwing all other
        // map-object metadata away.
        if (const auto found = record.properties.find("waypointID"); found != record.properties.end()) {
            if (const auto value = std::get_if<int32_t>(&found->second); value && *value >= 0) {
                record.waypointId = static_cast<uint32_t>(*value);
            }
        }
        if (const auto found = record.properties.find("waypointName"); found != record.properties.end()) {
            if (const auto value = std::get_if<container::String>(&found->second)) record.waypointName = *value;
        }
        constexpr container::Array<container::StringView, 3> waypointLabelKeys = {
            "waypointPathLabel1", "waypointPathLabel2", "waypointPathLabel3"};
        for (size_t index = 0; index < waypointLabelKeys.size(); ++index) {
            if (const auto found = record.properties.find(container::String(waypointLabelKeys[index]));
                found != record.properties.end()) {
                if (const auto value = std::get_if<container::String>(&found->second)) {
                    record.waypointPathLabels[index] = *value;
                }
            }
        }
        if (const auto found = record.properties.find("waypointPathBiDirectional");
            found != record.properties.end()) {
            if (const auto value = std::get_if<bool>(&found->second)) {
                record.waypointPathBiDirectional = *value;
            }
        }

        record.fixedTransformValid =
            std::isfinite(record.position.x()) &&
            std::isfinite(record.position.y()) &&
            std::isfinite(record.position.z()) &&
            std::isfinite(record.angle) &&
            withinMagnitude(record.position.x(), kMaximumMapObjectHorizontal) &&
            withinMagnitude(record.position.y(), kMaximumMapObjectHorizontal) &&
            withinMagnitude(record.position.z(), kMaximumMapObjectHorizontal) &&
            withinMagnitude(record.angle, kMaximumMapObjectAngle);
        if (record.fixedTransformValid) {
            record.positionRaw = {
                math::q32_32{record.position.x()}.raw(),
                math::q32_32{record.position.y()}.raw(),
                math::q32_32{record.position.z()}.raw(),
            };
            record.angleRaw = math::q32_32{record.angle}.raw();
        }
        const auto numericProperty = [&record](container::StringView key)
            -> std::optional<float> {
            const auto found = record.properties.find(container::String{key});
            if (found == record.properties.end()) return std::nullopt;
            if (const int32_t* value = std::get_if<int32_t>(&found->second))
                return static_cast<float>(*value);
            if (const float* value = std::get_if<float>(&found->second);
                value && std::isfinite(*value)) return *value;
            return std::nullopt;
        };
        // Same range rule as the transform: these two values reach a simulation
        // entry point (MapObjectImport::mapObjectInstanceOverrides), so an
        // unbounded one saturates the fixed-point conversion and a negative
        // percentage installs a negative initial-health fraction.  Both bounds
        // are one-sided because neither field has a legal negative value.
        if (const std::optional<float> maximum =
                numericProperty("objectMaxHPs");
            maximum && *maximum >= 0.0f &&
            *maximum <= kMaximumMapObjectHealthPoints) {
            record.maximumHealthOverrideRaw =
                math::q32_32{*maximum}.raw();
        }
        if (const std::optional<float> percentage =
                numericProperty("objectInitialHealth");
            percentage && *percentage >= 0.0f &&
            *percentage <= kMaximumMapObjectHealthPercentage) {
            record.initialHealthFractionRaw =
                (math::q32_32{*percentage} /
                 math::q32_32{int32_t{100}}).raw();
        }

        // Preserve RefCode's corrupt-map guard. Values outside the valid map
        // object vertical range cannot be placed safely in the logic world.
        if (record.position.z() < kLegacyMinimumMapObjectZ ||
            record.position.z() > kLegacyMaximumMapObjectZ) {
            continue;
        }
        if (m_result.objects.size() >= kMaxMapObjects) {
            setError("Map has too many valid Object records");
            return false;
        }
        m_result.objects.push_back(std::move(record));
    }
    return true;
}

bool MapHeightfieldLoader::parseWaypointLinks(container::Span<const uint8_t> payload) {
    ByteReader reader(payload);
    int32_t count = 0;
    if (!reader.read(count) || count < 0 || static_cast<size_t>(count) > reader.remaining() / 8 ||
        static_cast<size_t>(count) > kMaxWaypointLinks - m_result.waypointLinks.size()) {
        setError("Invalid WaypointsList");
        return false;
    }
    m_result.waypointLinks.reserve(m_result.waypointLinks.size() + static_cast<size_t>(count));
    for (int32_t index = 0; index < count; ++index) {
        WaypointLinkRecord link;
        if (!reader.read(link.from) || !reader.read(link.to)) {
            setError("Truncated WaypointsList");
            return false;
        }
        m_result.waypointLinks.push_back(link);
    }
    if (!reader.atEnd()) { setError("Unexpected bytes in WaypointsList"); return false; }
    return true;
}

bool MapHeightfieldLoader::parsePolygonTriggers(container::Span<const uint8_t> payload, uint16_t version) {
    ByteReader reader(payload);
    int32_t count = 0;
    if (!reader.read(count) || count < 0 || static_cast<size_t>(count) > reader.remaining() / 10 ||
        static_cast<size_t>(count) > kMaxPolygonTriggers - m_result.polygonTriggers.size()) {
        setError("Invalid PolygonTriggers count");
        return false;
    }

    size_t totalPointCount = 0;
    for (const PolygonTriggerRecord& trigger : m_result.polygonTriggers) {
        if (trigger.points.size() > kMaxTotalPolygonPoints - totalPointCount) {
            setError("Existing PolygonTriggers exceed the supported point limit");
            return false;
        }
        totalPointCount += trigger.points.size();
    }

    m_result.polygonTriggers.reserve(m_result.polygonTriggers.size() + static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        PolygonTriggerRecord trigger;
        int32_t pointCount = 0;
        if (!reader.readAsciiString(trigger.name) || (version >= 4 && !reader.readAsciiString(trigger.layerName)) ||
            !reader.read(trigger.id)) {
            setError("Truncated PolygonTrigger header");
            return false;
        }
        if (version >= 2) {
            uint8_t value = 0;
            if (!reader.read(value)) {
                setError("Truncated PolygonTrigger water flag");
                return false;
            }
            trigger.water = value != 0;
        }
        if (version >= 3) {
            uint8_t value = 0;
            if (!reader.read(value) || !reader.read(trigger.riverStart)) {
                setError("Truncated PolygonTrigger river data");
                return false;
            }
            trigger.river = value != 0;
        }
        if (!reader.read(pointCount) || pointCount < 0 ||
            static_cast<size_t>(pointCount) > kMaxPolygonPoints ||
            static_cast<size_t>(pointCount) > reader.remaining() / 12 ||
            static_cast<size_t>(pointCount) > kMaxTotalPolygonPoints - totalPointCount) {
            setError("Invalid PolygonTrigger points");
            return false;
        }
        trigger.points.resize(static_cast<size_t>(pointCount));
        for (math::int3& point : trigger.points) {
            int32_t x = 0;
            int32_t y = 0;
            int32_t z = 0;
            if (!reader.read(x) || !reader.read(y) || !reader.read(z)) {
                setError("Truncated PolygonTrigger point");
                return false;
            }
            point = {x, y, z};
        }
        if (trigger.points.size() >= 2) {
            totalPointCount += trigger.points.size();
            m_result.polygonTriggers.push_back(std::move(trigger));
        }
    }
    if (!reader.atEnd()) {
        setError("Unexpected bytes in PolygonTriggers");
        return false;
    }
    return true;
}

bool MapHeightfieldLoader::addLegacyDefaultWaterArea() {
    if (m_result.polygonTriggers.size() >= kMaxPolygonTriggers) {
        setError("Cannot append legacy default water polygon: trigger limit reached");
        return false;
    }

    uint32_t candidateId = 0;
    if (!m_result.polygonTriggers.empty()) {
        uint32_t maximumId = 0;
        for (const PolygonTriggerRecord& trigger : m_result.polygonTriggers) {
            maximumId = std::max(maximumId, trigger.id);
        }
        if (maximumId != std::numeric_limits<uint32_t>::max()) {
            candidateId = maximumId + 1;
        }
    }
    const auto idInUse = [this](uint32_t candidate) {
        return std::any_of(m_result.polygonTriggers.begin(), m_result.polygonTriggers.end(),
                           [candidate](const PolygonTriggerRecord& trigger) {
                               return trigger.id == candidate;
                           });
    };
    while (idInUse(candidateId)) {
        if (candidateId == std::numeric_limits<uint32_t>::max()) {
            setError("Cannot allocate an ID for legacy default water polygon");
            return false;
        }
        ++candidateId;
    }

    // RefCode used a 30-cell margin and GlobalData's water extents. The
    // production TerrainLogic supplies those values through the parser's
    // explicit context; standalone tools without a game configuration use a
    // bounded map-derived fallback instead of reading a process singleton.
    float extentX = 0.0f;
    float extentY = 0.0f;
    if (m_legacyWaterExtents) {
        extentX = m_legacyWaterExtents->x;
        extentY = m_legacyWaterExtents->y;
    } else {
        int32_t terrainWidth = m_result.width;
        int32_t terrainHeight = m_result.height;
        for (const TerrainBoundary& boundary : m_result.boundaries) {
            terrainWidth = std::max(terrainWidth, boundary.width);
            terrainHeight = std::max(terrainHeight, boundary.height);
        }
        extentX = static_cast<float>(terrainWidth) * kMapCellWorldSize;
        extentY = static_cast<float>(terrainHeight) * kMapCellWorldSize;
    }
    constexpr int32_t kLegacyWaterMarginCells = 30;
    constexpr int32_t kLegacyWaterHeight = 7;
    const int32_t margin = static_cast<int32_t>(kLegacyWaterMarginCells * kMapCellWorldSize);
    const auto legacyMaximumCoordinate = [margin](float extent, int32_t& output) {
        const double value = static_cast<double>(margin) + static_cast<double>(extent);
        if (!std::isfinite(value) || value < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
            value > static_cast<double>(std::numeric_limits<int32_t>::max())) {
            return false;
        }
        output = static_cast<int32_t>(value);
        return true;
    };
    int32_t maxX = 0;
    int32_t maxY = 0;
    if (!legacyMaximumCoordinate(extentX, maxX) || !legacyMaximumCoordinate(extentY, maxY)) {
        setError("Legacy default water extent is outside supported coordinates");
        return false;
    }

    PolygonTriggerRecord trigger;
    trigger.id = candidateId;
    trigger.name = "AutoAddedWaterAreaTrigger";
    trigger.water = true;
    trigger.synthesizedLegacyWater = true;
    trigger.points = {
        {-margin, -margin, kLegacyWaterHeight},
        {maxX, -margin, kLegacyWaterHeight},
        {maxX, maxY, kLegacyWaterHeight},
        {-margin, maxY, kLegacyWaterHeight},
    };
    m_result.polygonTriggers.push_back(std::move(trigger));
    return true;
}

} // namespace game::terrain
