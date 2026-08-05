#include "TerrainVertexWaterPersistentStateCodec.h"

#include <bit>
#include <cmath>
#include <limits>
#include <optional>

namespace engine {
namespace {

constexpr size_t kHeaderBytes = 48;
constexpr size_t kPointBytes = 12;

void writeU8(container::Vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

void writeU16(container::Vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8u));
}

void writeU32(container::Vector<uint8_t>& out, uint32_t value) {
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void writeFloat(container::Vector<uint8_t>& out, float value) {
    writeU32(out, std::bit_cast<uint32_t>(value));
}

class Reader final {
public:
    Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

    bool readU8(uint8_t& value) {
        if (remaining() < 1) return false;
        value = m_data[m_offset++];
        return true;
    }
    bool readU16(uint16_t& value) {
        if (remaining() < 2) return false;
        value = static_cast<uint16_t>(m_data[m_offset]) |
            (static_cast<uint16_t>(m_data[m_offset + 1u]) << 8u);
        m_offset += 2;
        return true;
    }
    bool readU32(uint32_t& value) {
        if (remaining() < 4) return false;
        value = 0;
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            value |= static_cast<uint32_t>(m_data[m_offset++]) << shift;
        }
        return true;
    }
    bool readFloat(float& value) {
        uint32_t bits = 0;
        if (!readU32(bits)) return false;
        value = std::bit_cast<float>(bits);
        return true;
    }
    [[nodiscard]] size_t remaining() const noexcept {
        return m_size - m_offset;
    }

private:
    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_offset = 0;
};

[[nodiscard]] bool validConfig(
    const TerrainVertexWaterGridConfig& config) noexcept {
    return config.cellsX != 0 && config.cellsY != 0 &&
        config.cellsX <= water_surface::performance_limits::
            kMaximumVertexWaterGridCellsPerAxis &&
        config.cellsY <= water_surface::performance_limits::
            kMaximumVertexWaterGridCellsPerAxis &&
        std::isfinite(config.positionX) &&
        std::isfinite(config.positionY) &&
        std::isfinite(config.positionZ) &&
        std::isfinite(config.angleRadians) &&
        std::isfinite(config.gridSize) && config.gridSize > 0.0f &&
        std::isfinite(config.influenceRange) && config.influenceRange >= 0.0f;
}

[[nodiscard]] bool validPoint(
    const TerrainVertexWaterPoint& point) noexcept {
    return std::isfinite(point.height) && std::isfinite(point.velocity) &&
        static_cast<uint8_t>(point.status) <= static_cast<uint8_t>(
            TerrainVertexWaterPointStatus::InMotion);
}

[[nodiscard]] std::optional<uint32_t> expectedPointCount(
    const TerrainVertexWaterGridConfig& config) noexcept {
    const uint64_t count =
        (static_cast<uint64_t>(config.cellsX) + 1u) *
        (static_cast<uint64_t>(config.cellsY) + 1u);
    if (count > TerrainVertexWaterPersistentStateCodec::MaximumPoints ||
        count > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(count);
}

} // namespace

container::Vector<uint8_t> TerrainVertexWaterPersistentStateCodec::encode(
    const TerrainVertexWaterState& state) {
    if (!state.configured() || !validConfig(state.config())) return {};
    const std::optional<uint32_t> expected = expectedPointCount(state.config());
    if (!expected || state.points().size() != *expected) return {};
    for (const TerrainVertexWaterPoint& point : state.points()) {
        if (!validPoint(point)) return {};
    }

    container::Vector<uint8_t> output;
    output.reserve(kHeaderBytes + state.points().size() * kPointBytes);
    writeU32(output, Magic);
    writeU16(output, Version);
    writeU16(output, static_cast<uint16_t>(kHeaderBytes));
    writeU32(output, state.config().cellsX);
    writeU32(output, state.config().cellsY);
    writeFloat(output, state.config().positionX);
    writeFloat(output, state.config().positionY);
    writeFloat(output, state.config().positionZ);
    writeFloat(output, state.config().angleRadians);
    writeFloat(output, state.config().gridSize);
    writeFloat(output, state.config().influenceRange);
    writeU32(output, *expected);
    writeU32(output, state.inMotion() ? 1u : 0u);
    for (const TerrainVertexWaterPoint& point : state.points()) {
        writeFloat(output, point.height);
        writeFloat(output, point.velocity);
        writeU8(output, point.preferredHeight);
        writeU8(output, static_cast<uint8_t>(point.status));
        writeU16(output, 0);
    }
    return output;
}

TerrainVertexWaterPersistentStateDecodeResult
TerrainVertexWaterPersistentStateCodec::decode(
    const uint8_t* data, size_t size) {
    TerrainVertexWaterPersistentStateDecodeResult result;
    if (!data || size == 0) {
        result.error = "empty VertexWater SaveGame payload";
        return result;
    }
    Reader reader(data, size);
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t headerBytes = 0;
    uint32_t pointCount = 0;
    uint32_t flags = 0;
    TerrainVertexWaterPersistentState decoded;
    if (!reader.readU32(magic) || !reader.readU16(version) ||
        !reader.readU16(headerBytes) ||
        !reader.readU32(decoded.config.cellsX) ||
        !reader.readU32(decoded.config.cellsY) ||
        !reader.readFloat(decoded.config.positionX) ||
        !reader.readFloat(decoded.config.positionY) ||
        !reader.readFloat(decoded.config.positionZ) ||
        !reader.readFloat(decoded.config.angleRadians) ||
        !reader.readFloat(decoded.config.gridSize) ||
        !reader.readFloat(decoded.config.influenceRange) ||
        !reader.readU32(pointCount) || !reader.readU32(flags)) {
        result.error = "truncated VertexWater SaveGame header";
        return result;
    }
    const std::optional<uint32_t> expected =
        expectedPointCount(decoded.config);
    if (magic != Magic || version != Version ||
        headerBytes != kHeaderBytes || !validConfig(decoded.config) ||
        !expected || pointCount != *expected || (flags & ~1u) != 0 ||
        reader.remaining() != static_cast<size_t>(pointCount) * kPointBytes) {
        result.error = "invalid VertexWater SaveGame layout";
        return result;
    }
    decoded.points.reserve(pointCount);
    bool anyMotion = false;
    for (uint32_t index = 0; index < pointCount; ++index) {
        TerrainVertexWaterPoint point;
        uint8_t status = 0;
        uint16_t reserved = 0;
        if (!reader.readFloat(point.height) ||
            !reader.readFloat(point.velocity) ||
            !reader.readU8(point.preferredHeight) ||
            !reader.readU8(status) || !reader.readU16(reserved)) {
            result.error = "truncated VertexWater SaveGame point";
            return result;
        }
        point.status = static_cast<TerrainVertexWaterPointStatus>(status);
        if (reserved != 0 || !validPoint(point)) {
            result.error = "invalid VertexWater SaveGame point";
            return result;
        }
        anyMotion = anyMotion || point.inMotion();
        decoded.points.push_back(point);
    }
    if (((flags & 1u) != 0) != anyMotion) {
        result.error = "VertexWater SaveGame motion flag disagrees with points";
        return result;
    }
    result.ok = true;
    result.state = std::move(decoded);
    return result;
}

} // namespace engine
