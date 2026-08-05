#pragma once

#include "presentation/render/RenderFxSnapshot.h"

#include <cstdint>

namespace engine::render {

struct ProjectileStreamJoinPoint final {
    RenderVector worldPosition{};
    container::Array<float, 4> color{};
    float textureV = 0.0f;
};

struct ProjectileStreamJoinVertex final {
    RenderVector worldPosition{};
    container::Array<float, 4> color{};
    float textureU = 0.0f;
    float textureV = 0.0f;
};

struct ProjectileStreamJoinMesh final {
    // Non-indexed triangle list, ready to append to ProjectileTrailRenderer's
    // CPU draw list without changing its GPU input layout.
    container::Vector<ProjectileStreamJoinVertex> vertices;
    uint32_t logicalSegmentCount = 0;
    uint32_t foldCount = 0;
    uint32_t mergedEdgeIntersectionCount = 0;
};

struct ProjectileStreamExtractionPolicy final {
    bool retainProjectileSnapshot = false;
    bool trailEnabled = false;
    bool shadowEnabled = false;
};

// Pure renderer-side port of SegLineRendererClass's default
// MERGE_INTERSECTIONS path. Camera-relative world axes preserve the original
// eye-space dot/cross construction while keeping this helper independent of
// the D3D12 device and view-matrix convention.
[[nodiscard]] ProjectileStreamJoinMesh buildProjectileStreamJoinMesh(
    container::Span<const ProjectileStreamJoinPoint> points,
    const RenderVector& cameraPosition,
    float width);

// Hot-path form used by the projectile renderer. The output allocation is
// retained by its owner; the value-return overload remains for probes and
// infrequent callers.
void buildProjectileStreamJoinMeshInto(
    ProjectileStreamJoinMesh& output,
    container::Span<const ProjectileStreamJoinPoint> points,
    const RenderVector& cameraPosition,
    float width);

// RefCode applies shroud once to the independent ProjectileStream Drawable at
// its frozen source position. It never clips the line by projectile cells.
[[nodiscard]] bool projectileStreamOwnerVisible(
    LocalVisibilityRenderCellState ownerAnchorState,
    bool localVisibilityValid,
    bool visibilityExempt) noexcept;

[[nodiscard]] ProjectileStreamExtractionPolicy
projectileStreamExtractionPolicy(
    bool projectileVisible,
    bool streamAuthored,
    bool ownerAnchorVisible) noexcept;

[[nodiscard]] uint32_t projectileStreamLogicFramesPerSecond(
    int sessionFramesPerSecond) noexcept;

} // namespace engine::render
