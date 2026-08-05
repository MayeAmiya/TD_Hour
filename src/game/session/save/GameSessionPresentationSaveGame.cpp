#include "game/session/save/GameSessionPresentationSaveGame.h"

#include "game/session/save/GameSessionClientTerrainObjectSaveGame.h"
#include "game/session/save/GameSessionVertexWaterSaveGame.h"
#include "game/session/core/GameSession.h"
#include "game/render/ClientTerrainObjectStore.h"

#include <limits>
#include <utility>

namespace engine {
namespace {

constexpr uint32_t kMagic = 0x47535052u; // "RPSG"
constexpr uint16_t kVersion = 1;
constexpr uint16_t kHeaderBytes = 32;
constexpr uint32_t kRequiredChunks = 0x3u;
// VertexWater's authored hard ceiling is (4096 + 1)^2 points at 12 bytes
// each, so the envelope must not introduce a lower limit than its child.
constexpr uint32_t kMaximumChunkBytes = 256u * 1024u * 1024u;

void setError(container::String* output, container::String error) {
    if (output) *output = std::move(error);
}

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

container::Vector<uint8_t> GameSessionPresentationSaveGame::capture(
    const GameSession& session, container::String* error) {
    if (error) error->clear();

    container::String childError;
    container::Vector<uint8_t> vertexWater =
        GameSessionVertexWaterSaveGame::capture(session, &childError);
    if (vertexWater.empty()) {
        setError(error, "VertexWater chunk capture failed: " + childError);
        return {};
    }
    container::Vector<uint8_t> clientTerrain =
        GameSessionClientTerrainObjectSaveGame::capture(session, &childError);
    if (clientTerrain.empty()) {
        setError(error,
                 "ClientTerrainObjects chunk capture failed: " + childError);
        return {};
    }
    if (vertexWater.size() > kMaximumChunkBytes ||
        clientTerrain.size() > kMaximumChunkBytes ||
        vertexWater.size() > std::numeric_limits<uint32_t>::max() ||
        clientTerrain.size() > std::numeric_limits<uint32_t>::max()) {
        setError(error, "presentation SaveGame chunk exceeds hard limit");
        return {};
    }

    container::Vector<uint8_t> output;
    output.reserve(static_cast<std::size_t>(kHeaderBytes) +
                   vertexWater.size() + clientTerrain.size());
    writeU32(output, kMagic);
    writeU16(output, kVersion);
    writeU16(output, kHeaderBytes);
    writeU64(output, session.confirmedTick());
    writeU32(output, static_cast<uint32_t>(vertexWater.size()));
    writeU32(output, static_cast<uint32_t>(clientTerrain.size()));
    writeU32(output, kRequiredChunks);
    writeU32(output, 0u);
    output.insert(output.end(), vertexWater.begin(), vertexWater.end());
    output.insert(output.end(), clientTerrain.begin(), clientTerrain.end());
    return output;
}

GameSessionPresentationSaveGameRestoreResult
GameSessionPresentationSaveGame::restore(
    GameSession& session,
    const container::Vector<uint8_t>& transaction,
    uint64_t restoredSimulationTick) {
    GameSessionPresentationSaveGameRestoreResult result;
    if (transaction.empty()) {
        result.error = "empty presentation SaveGame transaction";
        return result;
    }

    Reader reader(transaction.data(), transaction.size());
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t headerBytes = 0;
    uint64_t confirmedTick = 0;
    uint32_t vertexWaterBytes = 0;
    uint32_t clientTerrainBytes = 0;
    uint32_t flags = 0;
    uint32_t reserved = 0;
    if (!reader.readU32(magic) || !reader.readU16(version) ||
        !reader.readU16(headerBytes) || !reader.readU64(confirmedTick) ||
        !reader.readU32(vertexWaterBytes) ||
        !reader.readU32(clientTerrainBytes) || !reader.readU32(flags) ||
        !reader.readU32(reserved)) {
        result.error = "truncated presentation SaveGame transaction header";
        return result;
    }
    result.confirmedTick = confirmedTick;
    if (magic != kMagic || version != kVersion ||
        headerBytes != kHeaderBytes || flags != kRequiredChunks ||
        reserved != 0u || vertexWaterBytes == 0u ||
        clientTerrainBytes == 0u ||
        vertexWaterBytes > kMaximumChunkBytes ||
        clientTerrainBytes > kMaximumChunkBytes ||
        static_cast<uint64_t>(vertexWaterBytes) + clientTerrainBytes !=
            reader.remaining()) {
        result.error = "invalid presentation SaveGame transaction layout";
        return result;
    }
    if (confirmedTick != restoredSimulationTick ||
        session.confirmedTick() != restoredSimulationTick) {
        result.error =
            "presentation SaveGame tick does not match the active restored simulation tick";
        return result;
    }

    container::Vector<uint8_t> vertexWater(
        reader.current(), reader.current() + vertexWaterBytes);
    if (!reader.skip(vertexWaterBytes)) {
        result.error = "truncated VertexWater presentation chunk";
        return result;
    }
    container::Vector<uint8_t> clientTerrain(
        reader.current(), reader.current() + clientTerrainBytes);
    if (!reader.skip(clientTerrainBytes) || reader.remaining() != 0u) {
        result.error = "truncated ClientTerrainObjects presentation chunk";
        return result;
    }

    // The store validates the complete overlay before mutating it. Preserve
    // its old value so a later VertexWater rejection cannot leave a half-load.
    const ClientTerrainObjectPersistentState previousClientTerrain =
        session.captureClientTerrainObjectPersistentState();
    const GameSessionClientTerrainObjectSaveGameRestoreResult clientResult =
        GameSessionClientTerrainObjectSaveGame::restore(
            session, clientTerrain, restoredSimulationTick);
    if (!clientResult.ok) {
        result.error = "ClientTerrainObjects chunk restore failed: " +
            clientResult.error;
        return result;
    }

    GameSessionVertexWaterSaveGameRestoreResult waterResult =
        GameSessionVertexWaterSaveGame::restore(
            session, vertexWater, restoredSimulationTick);
    if (!waterResult.ok) {
        if (!session.restoreClientTerrainObjectPersistentState(
                previousClientTerrain)) {
            result.error =
                "VertexWater chunk restore failed and ClientTerrainObjects rollback was rejected: " +
                waterResult.error;
        } else {
            result.error = "VertexWater chunk restore failed: " +
                waterResult.error;
        }
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace engine
