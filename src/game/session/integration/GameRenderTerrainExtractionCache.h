#pragma once

#include "core/container/container_types.h"

#include <cstddef>
#include <cstdint>

namespace engine {
namespace render {
struct TerrainRenderSnapshot;
}

// Presentation-owned memoization for terrain extraction. Keeping these values
// together prevents the Session composition root from exposing a parallel set
// of cache fields to every terrain helper.
struct GameRenderTerrainExtractionCache final {
    container::SharedPtr<const render::TerrainRenderSnapshot> snapshot;
    uint64_t mapRevision = 0;
    uint64_t layoutRevision = 0;
    uint64_t waterRevision = 0;
    uint64_t borderRevision = 0;
    uint64_t presentationEpoch = 0;
    size_t activeBoundary = 0;
    uint64_t bridgeStateIdentity = 0;
    uint64_t presentationRevision = 0;
};

} // namespace engine
