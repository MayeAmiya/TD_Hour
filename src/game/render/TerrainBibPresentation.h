#pragma once

#include "presentation/render/TerrainRenderSnapshot.h"

#include <optional>

namespace engine::render {

// Detached input shared by the future local placement controller and the
// obstruction-feedback projection. Preview callers pass template geometry;
// live obstruction callers pass the object's current GeometryInfo values.
struct TerrainBibFootprintInput final {
    RenderEntityId ownerIdentity = 0;
    TerrainBibKind kind = TerrainBibKind::PlacementPreview;
    RenderVector position{};
    float yawRadians = 0.0f;
    float geometryMajorRadius = 0.0f;
    float geometryMinorRadius = 0.0f;
    bool geometryIsBox = false;
    float factoryExitWidth = 0.0f;
    float factoryExtraBibWidth = 0.0f;
    float additionalExtraWidth = 0.0f;
    bool highlighted = false;
    TerrainBibTint tint = TerrainBibTint::Default;
    bool receivesVisibility = true;
};

// Exact value boundary of W3DTerrainVisual::addFactionBib{Drawable}. The
// factory exit extends the same quad along object-local +X; it is not emitted
// as a second overlapping projector.
[[nodiscard]] std::optional<TerrainBibRenderData>
buildTerrainBibFootprint(const TerrainBibFootprintInput& input) noexcept;

} // namespace engine::render
