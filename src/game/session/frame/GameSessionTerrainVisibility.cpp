#include "game/session/frame/GameSessionTerrainVisibility.h"

#include "game/player/PlayerList.h"
#include "game/render/ClientTerrainObjectStore.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::session_terrain {

game::terrain::MapVisibilityCellState visibilityAt(
    const game::terrain::MapVisibilitySnapshot& visibility,
    const PlayerRegistry& players,
    math::vec3 position) noexcept {
    const PlayerState* observer = players.localPlayer();
    if (!visibility.renderingActive || !observer ||
        !observer->isSimulationParticipant() || visibility.width <= 0 ||
        visibility.height <= 0 || !(visibility.cellWorldSize > 0.0f)) {
        return game::terrain::MapVisibilityCellState::Clear;
    }
    const double cellX = std::floor(
        (static_cast<double>(position.x()) - visibility.originX) /
        visibility.cellWorldSize);
    const double cellY = std::floor(
        (static_cast<double>(position.y()) - visibility.originY) /
        visibility.cellWorldSize);
    if (!std::isfinite(cellX) || !std::isfinite(cellY) ||
        cellX < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        cellX > static_cast<double>(std::numeric_limits<int32_t>::max()) ||
        cellY < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        cellY > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return game::terrain::MapVisibilityCellState::Shrouded;
    }
    const int32_t x = static_cast<int32_t>(cellX);
    const int32_t y = static_cast<int32_t>(cellY);
    game::terrain::MapVisibilityCellState combined =
        visibility.cellState(observer->id, x, y);
    for (const PlayerId player : players.activePlayerIds()) {
        if (player == observer->id ||
            players.relationship(observer->id, player) !=
                PlayerRelationship::Allies) {
            continue;
        }
        combined = std::max(combined, visibility.cellState(player, x, y));
    }
    return combined;
}

void refreshFallingTreeFog(
    ClientTerrainObjectStore& objects,
    const game::terrain::MapVisibilityAuthority& visibility,
    const PlayerRegistry& players) {
    const container::SharedPtr<const game::terrain::MapVisibilitySnapshot>
        snapshot = visibility.snapshot();
    for (const ClientTerrainObject& object : objects.objects()) {
        if (object.kind != ClientTerrainObjectKind::OptimizedTree ||
            object.treeState != ClientTerrainTreeState::Falling) {
            continue;
        }
        const bool fogged = snapshot &&
            visibilityAt(*snapshot, players, object.position) ==
                game::terrain::MapVisibilityCellState::Fogged;
        static_cast<void>(objects.setTreeFogged(object.id, fogged));
    }
}

} // namespace engine::session_terrain
