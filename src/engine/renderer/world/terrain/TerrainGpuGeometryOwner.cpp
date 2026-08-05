#include "engine/renderer/world/terrain/TerrainGpuGeometryOwner.h"

#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/world/terrain/TerrainGpuMaterialOwner.h"

#include <limits>
#include <utility>

namespace engine::render::detail {
namespace {

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

} // namespace

TerrainGpuGeometryOwner::TerrainGpuGeometryOwner(
    d3d12::D3D12Device& device) noexcept
    : m_device(&device) {}

void TerrainGpuGeometryOwner::setDevice(
    d3d12::D3D12Device& device) noexcept {
    m_device = &device;
}

bool TerrainGpuGeometryOwner::upload(
    const container::Vector<StaticMeshVertex>& vertices,
    const container::Vector<uint32_t>& indices,
    TerrainGpuGeometry& output,
    container::String* error) {
    if (!m_device || vertices.empty() || indices.empty()) {
        setError(error, "Terrain geometry has no vertices or indices");
        return false;
    }
    const uint64_t vertexBytes = vertices.size() * sizeof(StaticMeshVertex);
    const uint64_t indexBytes = indices.size() * sizeof(uint32_t);
    if (vertexBytes > std::numeric_limits<UINT>::max() ||
        indexBytes > std::numeric_limits<UINT>::max()) {
        setError(error, "Terrain geometry exceeds D3D12 view size limits");
        return false;
    }
    output.vertexBuffer = m_device->recordStaticBufferUpload(
        vertices.data(), vertexBytes,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    output.indexBuffer = m_device->recordStaticBufferUpload(
        indices.data(), indexBytes, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    if (!output.vertexBuffer || !output.indexBuffer) {
        m_device->retireStaticBufferAllocation(
            std::move(output.vertexBuffer));
        m_device->retireStaticBufferAllocation(
            std::move(output.indexBuffer));
        setError(error, "D3D12 terrain geometry upload failed");
        return false;
    }
    output.vertexView.BufferLocation = output.vertexBuffer.gpuAddress;
    output.vertexView.SizeInBytes = static_cast<UINT>(vertexBytes);
    output.vertexView.StrideInBytes = sizeof(StaticMeshVertex);
    output.indexView.BufferLocation = output.indexBuffer.gpuAddress;
    output.indexView.SizeInBytes = static_cast<UINT>(indexBytes);
    output.indexView.Format = DXGI_FORMAT_R32_UINT;
    output.indexCount = static_cast<uint32_t>(indices.size());
    return true;
}

bool TerrainGpuGeometryOwner::uploadChunk(
    TerrainTileMeshChunk& cpuChunk,
    const TerrainGpuMaterialOwner& materials,
    TerrainGpuChunk& output,
    container::String* error) {
    output = {};
    if (!m_device) {
        setError(error, "Terrain GPU geometry owner has no device");
        return false;
    }
    output.x0 = cpuChunk.x0;
    output.y0 = cpuChunk.y0;
    output.cellsX = cpuChunk.cellsX;
    output.cellsY = cpuChunk.cellsY;
    output.boundsCenter = cpuChunk.boundsCenter;
    output.boundsRadius = cpuChunk.boundsRadius;

    struct GeometryRange final {
        size_t cpuIndex = 0;
        size_t vertexOffset = 0;
        size_t indexOffset = 0;
    };
    container::Vector<StaticMeshVertex> combinedVertices;
    container::Vector<uint32_t> combinedIndices;
    container::Vector<GeometryRange> ranges;
    size_t totalVertices = 0;
    size_t totalIndices = 0;
    for (const TerrainTileMeshGeometry& cpu : cpuChunk.geometries) {
        totalVertices += cpu.vertices.size();
        totalIndices += cpu.indices.size();
    }
    if (totalVertices > std::numeric_limits<UINT>::max() /
                            sizeof(StaticMeshVertex) ||
        totalIndices > std::numeric_limits<UINT>::max() /
                           sizeof(uint32_t)) {
        setError(error,
            "Terrain chunk aggregate geometry exceeds D3D12 view limits");
        return false;
    }
    combinedVertices.reserve(totalVertices);
    combinedIndices.reserve(totalIndices);
    ranges.reserve(cpuChunk.geometries.size());
    for (size_t cpuIndex = 0; cpuIndex < cpuChunk.geometries.size();
         ++cpuIndex) {
        TerrainTileMeshGeometry& cpu = cpuChunk.geometries[cpuIndex];
        if (cpu.vertices.empty() || cpu.indices.empty()) continue;
        if (cpu.key.terrainEdgePhase !=
                StaticMeshTerrainEdgePhase::Disabled) {
            const uint32_t edgeMaterial = cpu.key.terrainEdgePhase ==
                    StaticMeshTerrainEdgePhase::BlendSource
                ? cpu.key.detailMaterialIndex
                : cpu.key.materialIndex;
            const TerrainGpuMaterial* edge = materials.at(edgeMaterial);
            if (!edge || edge->textureSrvIndex == 0u) continue;
        }
        ranges.push_back({
            .cpuIndex = cpuIndex,
            .vertexOffset = combinedVertices.size(),
            .indexOffset = combinedIndices.size(),
        });
        combinedVertices.insert(
            combinedVertices.end(), cpu.vertices.begin(), cpu.vertices.end());
        combinedIndices.insert(
            combinedIndices.end(), cpu.indices.begin(), cpu.indices.end());
    }
    if (ranges.empty()) {
        setError(error, "Terrain chunk produced no material geometry");
        return false;
    }

    const uint64_t vertexBytes = combinedVertices.size() *
        sizeof(StaticMeshVertex);
    const uint64_t indexBytes = combinedIndices.size() * sizeof(uint32_t);
    auto vertexBuffer = m_device->recordStaticBufferUpload(
        combinedVertices.data(), vertexBytes,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    auto indexBuffer = m_device->recordStaticBufferUpload(
        combinedIndices.data(), indexBytes,
        D3D12_RESOURCE_STATE_INDEX_BUFFER);
    if (!vertexBuffer || !indexBuffer) {
        m_device->retireStaticBufferAllocation(std::move(vertexBuffer));
        m_device->retireStaticBufferAllocation(std::move(indexBuffer));
        setError(error, "D3D12 aggregate terrain chunk upload failed");
        return false;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS vertexAddress = vertexBuffer.gpuAddress;
    const D3D12_GPU_VIRTUAL_ADDRESS indexAddress = indexBuffer.gpuAddress;

    output.geometries.reserve(ranges.size());
    for (const GeometryRange& range : ranges) {
        TerrainTileMeshGeometry& cpu = cpuChunk.geometries[range.cpuIndex];
        TerrainGpuGeometry geometry;
        geometry.materialIndex = cpu.key.materialIndex;
        geometry.detailMaterialIndex = cpu.key.detailMaterialIndex;
        geometry.materialPass = cpu.key.materialPass;
        geometry.alphaBlend = cpu.key.alphaBlend;
        geometry.twoSided = cpu.key.twoSided;
        geometry.terrainEdgePhase = cpu.key.terrainEdgePhase;
        geometry.samplerMode = cpu.key.samplerMode;
        geometry.detailSamplerMode = cpu.key.detailSamplerMode;
        geometry.vertexView.BufferLocation = vertexAddress +
            range.vertexOffset * sizeof(StaticMeshVertex);
        geometry.vertexView.SizeInBytes = static_cast<UINT>(
            cpu.vertices.size() * sizeof(StaticMeshVertex));
        geometry.vertexView.StrideInBytes = sizeof(StaticMeshVertex);
        geometry.indexView.BufferLocation = indexAddress +
            range.indexOffset * sizeof(uint32_t);
        geometry.indexView.SizeInBytes = static_cast<UINT>(
            cpu.indices.size() * sizeof(uint32_t));
        geometry.indexView.Format = DXGI_FORMAT_R32_UINT;
        geometry.indexCount = static_cast<uint32_t>(cpu.indices.size());
        if (output.geometries.empty()) {
            geometry.vertexBuffer = std::move(vertexBuffer);
            geometry.indexBuffer = std::move(indexBuffer);
        }
        output.geometries.push_back(std::move(geometry));
    }
    return true;
}

void TerrainGpuGeometryOwner::retire(TerrainGpuGeometry& geometry) noexcept {
    if (!m_device) return;
    m_device->retireStaticBufferAllocation(std::move(geometry.vertexBuffer));
    m_device->retireStaticBufferAllocation(std::move(geometry.indexBuffer));
    geometry = {};
}

void TerrainGpuGeometryOwner::retire(TerrainGpuChunk& chunk) noexcept {
    for (TerrainGpuGeometry& geometry : chunk.geometries) retire(geometry);
    chunk = {};
}

void TerrainGpuGeometryOwner::retire(
    container::Vector<TerrainGpuWaterChunk>& chunks) noexcept {
    for (TerrainGpuWaterChunk& chunk : chunks) retire(chunk.geometry);
    chunks.clear();
}

void TerrainGpuGeometryOwner::retire(
    container::Vector<TerrainGpuRoadChunk>& chunks) noexcept {
    for (TerrainGpuRoadChunk& chunk : chunks) retire(chunk.geometry);
    chunks.clear();
}

void TerrainGpuGeometryOwner::retire(
    container::Vector<TerrainGpuBridgeChunk>& chunks) noexcept {
    for (TerrainGpuBridgeChunk& chunk : chunks) retire(chunk.geometry);
    chunks.clear();
}

void TerrainGpuGeometryOwner::retire(
    container::Vector<TerrainGpuBibChunk>& chunks) noexcept {
    for (TerrainGpuBibChunk& chunk : chunks) retire(chunk.geometry);
    chunks.clear();
}

} // namespace engine::render::detail
