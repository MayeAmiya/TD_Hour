#include "game/render/TerrainVertexWaterSaveGameCodec.h"

#include <limits>
#include <utility>

namespace engine {
namespace {

void writeU16(container::Vector<uint8_t>& output, uint16_t value) {
    output.push_back(static_cast<uint8_t>(value));
    output.push_back(static_cast<uint8_t>(value >> 8u));
}

void writeU32(container::Vector<uint8_t>& output, uint32_t value) {
    for (uint32_t shift = 0; shift < 32u; shift += 8u) {
        output.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void writeU64(container::Vector<uint8_t>& output, uint64_t value) {
    for (uint32_t shift = 0; shift < 64u; shift += 8u) {
        output.push_back(static_cast<uint8_t>(value >> shift));
    }
}

class Reader final {
public:
    Reader(const uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    [[nodiscard]] bool readU16(uint16_t& value) noexcept {
        if (remaining() < 2u) return false;
        value = static_cast<uint16_t>(data_[offset_]) |
            static_cast<uint16_t>(data_[offset_ + 1u] << 8u);
        offset_ += 2u;
        return true;
    }

    [[nodiscard]] bool readU32(uint32_t& value) noexcept {
        if (remaining() < 4u) return false;
        value = 0;
        for (uint32_t shift = 0; shift < 32u; shift += 8u) {
            value |= static_cast<uint32_t>(data_[offset_++]) << shift;
        }
        return true;
    }

    [[nodiscard]] bool readU64(uint64_t& value) noexcept {
        if (remaining() < 8u) return false;
        value = 0;
        for (uint32_t shift = 0; shift < 64u; shift += 8u) {
            value |= static_cast<uint64_t>(data_[offset_++]) << shift;
        }
        return true;
    }

    [[nodiscard]] const uint8_t* current() const noexcept {
        return data_ + offset_;
    }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return size_ - offset_;
    }
    [[nodiscard]] bool skip(std::size_t bytes) noexcept {
        if (bytes > remaining()) return false;
        offset_ += bytes;
        return true;
    }

private:
    const uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
};

} // namespace

container::Vector<uint8_t> TerrainVertexWaterSaveGameCodec::encode(
    uint64_t confirmedTick,
    const game::MapContentIdentity& mapIdentity,
    const TerrainVertexWaterState* vertexWater) {
    if (!mapIdentity.isKnown() || mapIdentity.size == 0u ||
        mapIdentity.resolvedPath.size() > MaximumMapPathBytes ||
        mapIdentity.resolvedPath.size() >
            std::numeric_limits<uint32_t>::max()) {
        return {};
    }

    container::Vector<uint8_t> payload;
    uint32_t flags = 0u;
    if (vertexWater && vertexWater->configured()) {
        payload = TerrainVertexWaterPersistentStateCodec::encode(*vertexWater);
        if (payload.empty() ||
            payload.size() > std::numeric_limits<uint32_t>::max()) {
            return {};
        }
        flags |= VertexWaterPresent;
    }

    const uint32_t pathBytes =
        static_cast<uint32_t>(mapIdentity.resolvedPath.size());
    const uint32_t payloadBytes = static_cast<uint32_t>(payload.size());
    container::Vector<uint8_t> output;
    output.reserve(static_cast<std::size_t>(HeaderBytes) + pathBytes +
                   payloadBytes);
    writeU32(output, Magic);
    writeU16(output, Version);
    writeU16(output, HeaderBytes);
    writeU64(output, confirmedTick);
    writeU32(output, mapIdentity.crc);
    writeU32(output, mapIdentity.size);
    writeU32(output, pathBytes);
    writeU32(output, payloadBytes);
    writeU32(output, flags);
    writeU32(output, 0u);
    output.insert(output.end(), mapIdentity.resolvedPath.begin(),
                  mapIdentity.resolvedPath.end());
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

TerrainVertexWaterSaveGameDecodeResult
TerrainVertexWaterSaveGameCodec::decode(
    const uint8_t* data, std::size_t size) {
    TerrainVertexWaterSaveGameDecodeResult result;
    if (!data || size == 0u) {
        result.error = "empty VertexWater SaveGame transaction";
        return result;
    }

    Reader reader(data, size);
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t headerBytes = 0;
    uint32_t mapCrc = 0;
    uint32_t mapSize = 0;
    uint32_t pathBytes = 0;
    uint32_t payloadBytes = 0;
    uint32_t flags = 0;
    uint32_t reserved = 0;
    if (!reader.readU32(magic) || !reader.readU16(version) ||
        !reader.readU16(headerBytes) ||
        !reader.readU64(result.transaction.confirmedTick) ||
        !reader.readU32(mapCrc) || !reader.readU32(mapSize) ||
        !reader.readU32(pathBytes) || !reader.readU32(payloadBytes) ||
        !reader.readU32(flags) || !reader.readU32(reserved)) {
        result.error = "truncated VertexWater SaveGame transaction header";
        return result;
    }
    if (magic != Magic || version != Version ||
        headerBytes != HeaderBytes || reserved != 0u ||
        (flags & ~VertexWaterPresent) != 0u || mapSize == 0u ||
        pathBytes == 0u ||
        pathBytes > MaximumMapPathBytes ||
        ((flags & VertexWaterPresent) == 0u && payloadBytes != 0u) ||
        ((flags & VertexWaterPresent) != 0u && payloadBytes == 0u) ||
        static_cast<uint64_t>(pathBytes) + payloadBytes !=
            reader.remaining()) {
        result.error = "invalid VertexWater SaveGame transaction layout";
        return result;
    }

    result.transaction.mapIdentity.resolvedPath.assign(
        reinterpret_cast<const char*>(reader.current()), pathBytes);
    result.transaction.mapIdentity.crc = mapCrc;
    result.transaction.mapIdentity.size = mapSize;
    if (!reader.skip(pathBytes)) {
        result.error = "truncated VertexWater SaveGame map identity";
        return result;
    }
    if ((flags & VertexWaterPresent) != 0u) {
        TerrainVertexWaterPersistentStateDecodeResult decoded =
            TerrainVertexWaterPersistentStateCodec::decode(
                reader.current(), payloadBytes);
        if (!decoded.ok) {
            result.error = "invalid VertexWater SaveGame payload: " +
                decoded.error;
            return result;
        }
        result.transaction.vertexWater = std::move(decoded.state);
    }
    if (!reader.skip(payloadBytes) || reader.remaining() != 0u) {
        result.error = "unexpected trailing VertexWater SaveGame data";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace engine
