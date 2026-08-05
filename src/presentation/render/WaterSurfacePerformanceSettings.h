#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::water_surface::performance_limits {

// Transactional CPU meshing limits. Exceeding one rejects the affected water
// area before GPU upload; these limits never change water height or shoreline
// appearance.
inline constexpr size_t kMaximumPolygonVertices = 256;
inline constexpr uint64_t kMaximumCandidateTrianglePairs = 8'000'000;
inline constexpr size_t kMaximumGeneratedVertices = 4'000'000;
inline constexpr size_t kMaximumGeneratedIndices = 12'000'000;

// Session-owned VertexWater is a fixed authored grid, not polygon tessellation.
// RenderGameData historically accepts up to 4096 cells on either axis, which
// means 4097 x 4097 stored points. Keep its allocation/SaveGame ceiling
// separate from the transient polygon mesh budgets above.
inline constexpr uint32_t kMaximumVertexWaterGridCellsPerAxis = 4096u;
inline constexpr uint32_t kMaximumVertexWaterPoints =
    (kMaximumVertexWaterGridCellsPerAxis + 1u) *
    (kMaximumVertexWaterGridCellsPerAxis + 1u);

static_assert(kMaximumVertexWaterPoints == 16'785'409u);

} // namespace engine::water_surface::performance_limits
