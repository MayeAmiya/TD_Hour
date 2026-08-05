#pragma once

#include "core/math/wwmath/vector/float3.h"
#include "game/terrain/MapVisibilityAuthority.h"

namespace engine {

class ClientTerrainObjectStore;
class PlayerRegistry;

namespace session_terrain {

[[nodiscard]] game::terrain::MapVisibilityCellState visibilityAt(
    const game::terrain::MapVisibilitySnapshot& visibility,
    const PlayerRegistry& players,
    math::vec3 position) noexcept;

void refreshFallingTreeFog(
    ClientTerrainObjectStore& objects,
    const game::terrain::MapVisibilityAuthority& visibility,
    const PlayerRegistry& players);

} // namespace session_terrain
} // namespace engine
