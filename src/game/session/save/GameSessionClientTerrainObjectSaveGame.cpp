#include "game/session/save/GameSessionClientTerrainObjectSaveGame.h"

#include "game/render/ClientTerrainObjectSaveGameCodec.h"
#include "game/session/core/GameSession.h"
#include "game/terrain/TerrainLogic.h"

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

} // namespace

container::Vector<uint8_t>
GameSessionClientTerrainObjectSaveGame::capture(
    const GameSession& session, container::String* error) {
    if (error) error->clear();
    if (!session.persistenceAllowed()) {
        setError(error,
                 "ClientTerrainObjects SaveGame capture requires an active non-network, non-replay session");
        return {};
    }

    const game::terrain::TerrainLogic& terrain =
        session.terrainForPersistence();
    const ClientTerrainObjectPersistentState state =
        session.captureClientTerrainObjectPersistentState();
    if (state.contentIdentity == 0u ||
        state.contentIdentity != session.clientTerrainObjects().contentIdentity()) {
        setError(error,
                 "ClientTerrainObjects SaveGame capture requires a rebuilt map baseline");
        return {};
    }

    container::Vector<uint8_t> transaction =
        ClientTerrainObjectSaveGameCodec::encode(
            session.confirmedTick(), terrain.contentIdentity(), state);
    if (transaction.empty()) {
        setError(error,
                 "could not encode ClientTerrainObjects SaveGame transaction");
    }
    return transaction;
}

GameSessionClientTerrainObjectSaveGameRestoreResult
GameSessionClientTerrainObjectSaveGame::restore(
    GameSession& session,
    const container::Vector<uint8_t>& transaction,
    uint64_t restoredSimulationTick) {
    GameSessionClientTerrainObjectSaveGameRestoreResult result;
    if (!session.persistenceAllowed()) {
        result.error =
            "ClientTerrainObjects SaveGame restore requires an active non-network, non-replay session";
        return result;
    }

    ClientTerrainObjectSaveGameDecodeResult decoded =
        ClientTerrainObjectSaveGameCodec::decode(transaction);
    if (!decoded.ok) {
        result.error = std::move(decoded.error);
        return result;
    }
    result.confirmedTick = decoded.transaction.confirmedTick;
    if (decoded.transaction.confirmedTick != restoredSimulationTick ||
        session.confirmedTick() != restoredSimulationTick) {
        result.error =
            "ClientTerrainObjects SaveGame tick does not match the active restored simulation tick";
        return result;
    }

    if (!sameMapIdentity(decoded.transaction.mapIdentity,
                         session.terrainForPersistence().contentIdentity())) {
        result.error =
            "ClientTerrainObjects SaveGame map identity does not match the active session";
        return result;
    }

    const ClientTerrainObjectStore& store = session.clientTerrainObjects();
    if (store.contentIdentity() == 0u ||
        decoded.transaction.state.contentIdentity != store.contentIdentity()) {
        result.error =
            "ClientTerrainObjects SaveGame content identity does not match the rebuilt client baseline";
        return result;
    }
    if (!session.restoreClientTerrainObjectPersistentState(
            decoded.transaction.state)) {
        result.error =
            "ClientTerrainObjects SaveGame state restore was rejected";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace engine
