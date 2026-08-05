#include "game/session/save/GameSessionVertexWaterSaveGame.h"

#include "game/render/TerrainVertexWaterSaveGameCodec.h"
#include "game/session/core/GameSession.h"
#include "game/terrain/TerrainLogic.h"

#include <bit>
#include <utility>

namespace engine {
namespace {

void setError(container::String* output, container::String error) {
    if (output) *output = std::move(error);
}

[[nodiscard]] bool sameMapIdentity(
    const game::MapContentIdentity& left,
    const game::MapContentIdentity& right) noexcept {
    return left.crc == right.crc && left.size == right.size &&
        left.resolvedPath == right.resolvedPath;
}

[[nodiscard]] bool sameFloatBits(float left, float right) noexcept {
    return std::bit_cast<uint32_t>(left) == std::bit_cast<uint32_t>(right);
}

[[nodiscard]] bool sameGrid(
    const TerrainVertexWaterGridConfig& left,
    const TerrainVertexWaterGridConfig& right) noexcept {
    return left.cellsX == right.cellsX && left.cellsY == right.cellsY &&
        sameFloatBits(left.positionX, right.positionX) &&
        sameFloatBits(left.positionY, right.positionY) &&
        sameFloatBits(left.positionZ, right.positionZ) &&
        sameFloatBits(left.angleRadians, right.angleRadians) &&
        sameFloatBits(left.gridSize, right.gridSize) &&
        sameFloatBits(left.influenceRange, right.influenceRange);
}

} // namespace

container::Vector<uint8_t> GameSessionVertexWaterSaveGame::capture(
    const GameSession& session, container::String* error) {
    if (error) error->clear();
    if (!session.persistenceAllowed()) {
        setError(error,
                 "VertexWater SaveGame capture requires an active non-network, non-replay session");
        return {};
    }
    const game::terrain::TerrainLogic& terrain =
        session.terrainForPersistence();
    const TerrainVertexWaterState& state = terrain.vertexWaterState();
    container::Vector<uint8_t> transaction =
        TerrainVertexWaterSaveGameCodec::encode(
            session.confirmedTick(), terrain.contentIdentity(),
            state.configured() ? &state : nullptr);
    if (transaction.empty()) {
        setError(error, "could not encode VertexWater SaveGame transaction");
    }
    return transaction;
}

GameSessionVertexWaterSaveGameRestoreResult
GameSessionVertexWaterSaveGame::restore(
    GameSession& session,
    const container::Vector<uint8_t>& transaction,
    uint64_t restoredSimulationTick) {
    GameSessionVertexWaterSaveGameRestoreResult result;
    if (!session.persistenceAllowed()) {
        result.error =
            "VertexWater SaveGame restore requires an active non-network, non-replay session";
        return result;
    }

    TerrainVertexWaterSaveGameDecodeResult decoded =
        TerrainVertexWaterSaveGameCodec::decode(transaction);
    if (!decoded.ok) {
        result.error = std::move(decoded.error);
        return result;
    }
    result.confirmedTick = decoded.transaction.confirmedTick;
    if (decoded.transaction.confirmedTick != restoredSimulationTick ||
        session.confirmedTick() != restoredSimulationTick) {
        result.error =
            "VertexWater SaveGame tick does not match the active restored simulation tick";
        return result;
    }

    game::terrain::TerrainLogic& terrain =
        session.mutableTerrainForPersistence();
    if (!sameMapIdentity(decoded.transaction.mapIdentity,
                         terrain.contentIdentity())) {
        result.error =
            "VertexWater SaveGame map identity does not match the active session";
        return result;
    }

    const TerrainVertexWaterState& current = terrain.vertexWaterState();
    if (decoded.transaction.vertexWater) {
        TerrainVertexWaterPersistentState& incoming =
            *decoded.transaction.vertexWater;
        if (!current.configured() ||
            !sameGrid(current.config(), incoming.config) ||
            current.points().size() != incoming.points.size()) {
            result.error =
                "VertexWater SaveGame grid does not match the active map";
            return result;
        }
        if (!terrain.restoreVertexWaterState(
                incoming.config, std::move(incoming.points))) {
            result.error = "VertexWater SaveGame state restore was rejected";
            return result;
        }
    } else if (current.configured()) {
        result.error =
            "VertexWater SaveGame omitted the active map's authored grid";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace engine
