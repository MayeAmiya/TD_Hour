#pragma once

#include "core/container/container_types.h"

#include <cstddef>
#include <cstdint>

namespace engine {

// One authored GlobalData VertexWater grid.  Heights and velocities are local
// offsets, matching W3DWater::WaterMeshData; positionZ is retained so render
// extraction can later place those offsets without making this state own any
// renderer resource.
struct TerrainVertexWaterGridConfig final {
    float positionX = 0.0f;
    float positionY = 0.0f;
    float positionZ = 0.0f;
    float angleRadians = 0.0f;
    uint32_t cellsX = 0;
    uint32_t cellsY = 0;
    float gridSize = 0.0f;
    float influenceRange = 0.0f;
};

enum class TerrainVertexWaterPointStatus : uint8_t {
    AtRest = 0x00,
    InMotion = 0x01,
};

struct TerrainVertexWaterPoint final {
    float height = 0.0f;
    float velocity = 0.0f;
    // Deliberately not a float: this surprising byte storage is part of the
    // original W3DWater state/save contract.
    uint8_t preferredHeight = 0;
    TerrainVertexWaterPointStatus status =
        TerrainVertexWaterPointStatus::AtRest;

    [[nodiscard]] bool inMotion() const noexcept {
        return status == TerrainVertexWaterPointStatus::InMotion;
    }
};

// Session-owned presentation state for the legacy fixed vertex-water grid.
// Its stable row-major storage and fixed update traversal make equal accepted
// input sequences produce equal point states.  It is intentionally outside
// lockstep gameplay state and contains no renderer handles.
class TerrainVertexWaterState final {
public:
    // Invalid/non-finite configs are rejected without changing existing
    // state. A successful reconfiguration starts with every point at rest.
    [[nodiscard]] bool configure(
        const TerrainVertexWaterGridConfig& config);

    // Reset point motion but retain the authored grid. clear() removes both.
    void reset() noexcept;
    void clear() noexcept;

    // Matches W3DWater::addVelocity: transform the world point into the
    // rotated grid, affect a square local range, overwrite preferredHeight,
    // and accumulate velocity. preferredHeight is truncated toward zero only
    // after its finite [0, 255] boundary has been validated.
    [[nodiscard]] bool addVelocity(float worldX,
                                   float worldY,
                                   float velocity,
                                   float preferredHeight) noexcept;

    // Advance one legacy client update. gravityPerUpdate is the signed value
    // after legacy unit conversion (normally negative). A non-finite value is
    // rejected without mutating any point.
    [[nodiscard]] bool advance(float gravityPerUpdate) noexcept;

    // SaveGame restore boundary. The current session/map must already have
    // configured the same authored grid; a mismatch or malformed point set
    // is rejected atomically without exposing a decoded prefix.
    [[nodiscard]] bool restore(
        const TerrainVertexWaterGridConfig& expectedConfig,
        container::Vector<TerrainVertexWaterPoint> points);

    [[nodiscard]] bool configured() const noexcept { return configured_; }
    [[nodiscard]] bool inMotion() const noexcept { return inMotion_; }
    [[nodiscard]] const TerrainVertexWaterGridConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] const container::Vector<TerrainVertexWaterPoint>& points()
        const noexcept {
        return points_;
    }
    [[nodiscard]] const TerrainVertexWaterPoint* point(uint32_t x,
                                                       uint32_t y) const
        noexcept;

private:
    [[nodiscard]] std::size_t pointIndex(uint32_t x,
                                         uint32_t y) const noexcept;

    TerrainVertexWaterGridConfig config_{};
    container::Vector<TerrainVertexWaterPoint> points_;
    float directionXx_ = 1.0f;
    float directionXy_ = 0.0f;
    float directionYx_ = 0.0f;
    float directionYy_ = 1.0f;
    bool configured_ = false;
    bool inMotion_ = false;
};

} // namespace engine
