#pragma once

#include <cstddef>
#include <cstdint>

namespace game::road_surface {

// Renderer budget. Changing these values trades road tessellation work and
// memory for terrain-conform fidelity without changing road connectivity.
inline constexpr float kRoadMeshSampleDistanceInCells = 1.0f;
inline constexpr uint32_t kRoadMeshMaximumSubdivisions = 4096u;
// Matches W3DRoadBuffer's fixed lateral sampling ceiling. This is a safety
// limit; authored road width remains a visual value in RoadSurface settings.
inline constexpr uint32_t kRoadMeshMaximumLateralSamples = 100u;
inline constexpr size_t kRoadMaximumJunctionDegree = 16u;

} // namespace game::road_surface
