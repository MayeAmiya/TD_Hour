#pragma once

#include "engine/renderer/world/model/W3dStaticModel.h"
#include "core/container/container_types.h"
#include "presentation/render/TerrainRenderSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine::render {

struct TerrainRoadJunctionTopology final {
    size_t threeWayCount = 0;
    size_t fourWayCount = 0;
    size_t multiWayCount = 0;
    size_t joinedEndpointCount = 0;
    size_t patchVertexCount = 0;
    size_t patchTriangleCount = 0;
};

struct TerrainRoadMeshInspectionVertex final {
    math::vec3 worldPosition{};
    math::vec2 texcoord{};
    float alpha = 0.0f;
};

struct TerrainRoadMeshInspection final {
    container::Vector<TerrainRoadMeshInspectionVertex> vertices;
    container::Vector<uint32_t> indices;
    uint32_t materialPass = 0;
    size_t authoredCornerCount = 0;
    size_t crossMaterialJoinCount = 0;
};

// Detached CPU geometry. Material ordering is part of the road build result,
// so the D3D12 consumer does not need to inspect topology-plan internals.
struct TerrainRoadMeshCpu final {
    container::Vector<StaticMeshVertex> vertices;
    container::Vector<uint32_t> indices;
    uint32_t materialPass = 0;
};

struct TerrainRoadMeshCpuBatch final {
    size_t beginIndex = 0;
    container::Vector<std::optional<TerrainRoadMeshCpu>> meshes;
};

// Immutable topology/trim plan shared by parallel per-road mesh jobs. Its
// representation remains private to the CPU builder: renderer code can only
// ask the builder for a complete road product.
class TerrainRoadMeshPlan final {
public:
    TerrainRoadMeshPlan();
    ~TerrainRoadMeshPlan();

    TerrainRoadMeshPlan(TerrainRoadMeshPlan&&) noexcept;
    TerrainRoadMeshPlan& operator=(TerrainRoadMeshPlan&&) noexcept;

    TerrainRoadMeshPlan(const TerrainRoadMeshPlan&) = delete;
    TerrainRoadMeshPlan& operator=(const TerrainRoadMeshPlan&) = delete;

private:
    struct Impl;
    container::UniquePtr<Impl> m_impl;

    friend TerrainRoadMeshPlan buildTerrainRoadMeshPlan(
        const TerrainRenderSnapshot&, bool);
    friend std::optional<TerrainRoadMeshCpu> buildTerrainRoadMesh(
        const TerrainRenderSnapshot&, const TerrainRoadMeshPlan&, size_t);
    friend TerrainRoadJunctionTopology analyzeTerrainRoadJunctions(
        const TerrainRenderSnapshot&);
    friend std::optional<TerrainRoadMeshInspection> inspectTerrainRoadMesh(
        const TerrainRenderSnapshot&, size_t);
};

[[nodiscard]] TerrainRoadMeshPlan buildTerrainRoadMeshPlan(
    const TerrainRenderSnapshot& terrain,
    bool requireTerrainConform = true);

// Bounded process-local cache keyed only by map layout and canonical road
// descriptors. Height deformation does not change endpoint connectivity or
// whether a planned sample lies inside the immutable map bounds.
[[nodiscard]] container::SharedPtr<const TerrainRoadMeshPlan>
findOrBuildCachedTerrainRoadMeshPlan(const TerrainRenderSnapshot& terrain);

// Immediately renderable independent strip used while the refined junction
// plan is still preparing. It keeps authored width/UV and terrain conformance,
// but intentionally has square untrimmed ends and no junction patches.
[[nodiscard]] std::optional<TerrainRoadMeshCpu> buildBasicTerrainRoadMesh(
    const TerrainRenderSnapshot& terrain,
    size_t roadIndex);

[[nodiscard]] std::optional<TerrainRoadMeshCpu> buildTerrainRoadMesh(
    const TerrainRenderSnapshot& terrain,
    const TerrainRoadMeshPlan& plan,
    size_t roadIndex);

[[nodiscard]] TerrainRoadJunctionTopology analyzeTerrainRoadJunctions(
    const TerrainRenderSnapshot& terrain);

[[nodiscard]] std::optional<TerrainRoadMeshInspection> inspectTerrainRoadMesh(
    const TerrainRenderSnapshot& terrain,
    size_t roadIndex);

} // namespace engine::render
