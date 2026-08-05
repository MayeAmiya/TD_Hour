#pragma once

#include <cstddef>

namespace game::road_surface {

// Presentation policy. These values affect authored road shape/placement,
// not the amount of work the renderer is allowed to spend.
// RefCode floats roads by MAP_HEIGHT_SCALE / 8 (0.625 / 8).
inline constexpr float kRoadSurfaceZOffset = 0.078125f;
inline constexpr float kRoadJunctionTrimWidthFactor = 1.15f;
inline constexpr float kRoadJunctionMinimumTrim = 0.25f;
inline constexpr float kRoadJunctionParallelDirectionDot = 0.985f;
inline constexpr float kRoadMaximumEndpointTrimFraction = 0.40f;
inline constexpr size_t kRoadMinimumJunctionDegree = 3u;
// The shipped 512x512 road textures are atlases, not repeatable whole-road
// images.  RefCode's SEGMENT path samples a narrow horizontal strip centered
// at V=85/512 and advances U by worldDistance/(RoadWidth*4).
inline constexpr float kRoadStraightAtlasU = 0.0f;
inline constexpr float kRoadStraightAtlasV = 85.0f / 512.0f;
inline constexpr float kRoadAtlasWorldScale = 4.0f;
// Authored 512x512 road atlases reserve these anchors for RefCode's dedicated
// tee/Y/four-way pieces. Their footprints and UV frames must stay paired so
// the shipped intersection artwork remains aligned with each authored patch.
inline constexpr float kRoadTeeAtlasU = 425.0f / 512.0f;
inline constexpr float kRoadTeeAtlasV = 255.0f / 512.0f;
inline constexpr float kRoadYAtlasU = 255.0f / 512.0f;
inline constexpr float kRoadYAtlasV = 226.0f / 512.0f;
inline constexpr float kRoadFourWayAtlasU = 425.0f / 512.0f;
inline constexpr float kRoadFourWayAtlasV = 425.0f / 512.0f;
inline constexpr float kRoadJunctionAtlasWorldScale = kRoadAtlasWorldScale;
inline constexpr float kRoadStraightPairDot = -0.85f;
inline constexpr float kRoadTeeWidthAdjustment = 1.03f;
inline constexpr float kRoadYRejectOppositeDot = -0.866f;
inline constexpr float kRoadYIdealLegDot = -0.707f;
inline constexpr float kRoadSlantedTeeDot = 0.5f;
inline constexpr float kRoadYLengthFactor = 1.59f;
inline constexpr float kRoadYTopOffsetFactor = 0.29f;
inline constexpr float kRoadYWidthFactor = 1.08f;
inline constexpr float kRoadHAtlasU = 202.0f / 512.0f;
inline constexpr float kRoadHAtlasV = 364.0f / 512.0f;
inline constexpr float kRoadHNormalFactor = 1.35f;
inline constexpr float kRoadHFlipBottomOffset = 0.20f;
inline constexpr float kRoadHBottomOffset = 0.80f;
inline constexpr float kRoadHExtensionFactor = 1.20f;
inline constexpr float kRoadHLongEndpointFactor = 2.05f;
inline constexpr float kRoadHShortEndpointFactor = 0.46f;
inline constexpr float kRoadHArmEndpointFactor = 2.10f;
inline constexpr float kRoadYStemEndpointFactor = 0.55f;
inline constexpr float kRoadYLegEndpointFactor = 1.10f;
// W3DRoadBuffer inserts 30-degree curve slices.  A normal corner turns around
// 1.5 authored road widths while ROAD_CORNER_TIGHT selects 0.5; an authored
// angled endpoint requests the sharp miter path instead of curve insertion.
inline constexpr float kRoadCurveStepRadians = 3.14159265358979323846f / 6.0f;
inline constexpr float kRoadNormalCornerRadiusInWidths = 1.5f;
inline constexpr float kRoadTightCornerRadiusInWidths = 0.5f;
inline constexpr float kRoadCurveAtlasU = 4.0f / 512.0f;
inline constexpr float kRoadCurveAtlasV = 255.0f / 512.0f;
inline constexpr float kRoadTightCurveAtlasV = 425.0f / 512.0f;
// W3DRoadBuffer::loadAlphaJoin uses a dedicated 48-pixel-long atlas patch,
// widened by eight pixels on both sides. These are authored geometry/UV
// values, not renderer work budgets.
inline constexpr float kRoadAlphaJoinAtlasU = 106.0f / 512.0f;
inline constexpr float kRoadAlphaJoinAtlasV = 425.0f / 512.0f;
inline constexpr float kRoadAlphaJoinLengthInRoadWidths = 48.0f / 128.0f;
inline constexpr float kRoadAlphaJoinWidthExpansion = 1.0f + 8.0f / 128.0f;
inline constexpr float kRoadAlphaJoinLongitudinalOffset = 0.65f;

} // namespace game::road_surface
