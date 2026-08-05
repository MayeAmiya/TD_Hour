#include "engine/renderer/world/overlay/WaypointRenderer.h"

#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/world/resource/WorldTextureCache.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace engine::render {
namespace {

constexpr float kWaypointWidth = 1.5f;
constexpr float kWaypointTextureWorldPeriod = 16.0f;

[[nodiscard]] bool finite(const RenderVector& value) noexcept {
    return std::isfinite(value.x()) && std::isfinite(value.y()) &&
        std::isfinite(value.z());
}

[[nodiscard]] StaticMeshVertex waypointVertex(
    RenderVector position, float u, float v,
    const math::vec4& color) noexcept {
    StaticMeshVertex vertex;
    vertex.position = position;
    vertex.normal = {0.0f, 0.0f, 1.0f};
    vertex.texcoord = {u, v};
    vertex.color = color;
    return vertex;
}

[[nodiscard]] math::vec4 waypointColor(
    OrderWaypointRenderColor color, bool rejected) noexcept {
    if (rejected) return {1.0f, 0.08f, 0.04f, 1.0f};
    switch (color) {
    case OrderWaypointRenderColor::Blue:
        return {0.15f, 0.48f, 1.0f, 1.0f};
    case OrderWaypointRenderColor::Orange:
        return {1.0f, 0.42f, 0.05f, 1.0f};
    case OrderWaypointRenderColor::Red:
        return {1.0f, 0.08f, 0.04f, 1.0f};
    case OrderWaypointRenderColor::Green:
        return {0.15f, 0.9f, 0.22f, 1.0f};
    case OrderWaypointRenderColor::Yellow:
        return {1.0f, 0.82f, 0.08f, 1.0f};
    }
    return {0.15f, 0.48f, 1.0f, 1.0f};
}

} // namespace

WaypointRenderer::WaypointRenderer(
    d3d12::D3D12Device& device,
    container::SharedPtr<WorldTextureCache> textures) {
    static_cast<void>(init(device, std::move(textures)));
}

WaypointRenderer::~WaypointRenderer() { shutdown(); }

bool WaypointRenderer::init(
    d3d12::D3D12Device& device,
    container::SharedPtr<WorldTextureCache> textures) {
    shutdown();
    m_device = &device;
    m_textures = std::move(textures);
    m_initialized = m_textures != nullptr;
    return m_initialized;
}

void WaypointRenderer::shutdown() {
    m_vertices.clear();
    m_indices.clear();
    m_textureSrv = 0;
    m_textures.reset();
    m_device = nullptr;
    m_initialized = false;
}

void WaypointRenderer::resetTextureCache() noexcept { m_textureSrv = 0; }

size_t WaypointRenderer::appendDrawPackets(
    const SharedSnapshotVector<OrderWaypointSegmentRenderSnapshot>& segments,
    const RenderCameraSnapshot& camera,
    container::Vector<StaticMeshDrawPacket>& output) {
    m_vertices.clear();
    m_indices.clear();
    if (!m_initialized || !m_device || !m_device->commandList() ||
        segments.empty()) return 0;

    const size_t count = std::min<size_t>(segments.size(), 512u);
    m_vertices.reserve(count * 4u);
    m_indices.reserve(count * 6u);
    RenderVector sortCenter{};
    size_t emitted = 0;
    for (size_t index = 0; index < count; ++index) {
        RenderVector start = segments[index].startWorldPosition;
        RenderVector end = segments[index].endWorldPosition;
        if (!finite(start) || !finite(end)) continue;
        RenderVector direction = end - start;
        const float length = direction.length();
        if (!std::isfinite(length) || length <= math::EPSILON) continue;
        direction = direction / length;
        const RenderVector midpoint = (start + end) * 0.5f;
        RenderVector view = camera.position - midpoint;
        const float viewLength = view.length();
        view = std::isfinite(viewLength) && viewLength > math::EPSILON
            ? view / viewLength : RenderVector{0.0f, 0.0f, 1.0f};
        RenderVector side = direction.cross(view);
        float sideLength = side.length();
        if (!std::isfinite(sideLength) || sideLength <= math::EPSILON) {
            side = {-direction.y(), direction.x(), 0.0f};
            sideLength = side.length();
        }
        if (!std::isfinite(sideLength) || sideLength <= math::EPSILON)
            continue;
        side = side / sideLength * (kWaypointWidth * 0.5f);

        const uint32_t base = static_cast<uint32_t>(m_vertices.size());
        const float textureEnd = length / kWaypointTextureWorldPeriod;
        const math::vec4 color = waypointColor(
            segments[index].color, segments[index].rejected);
        m_vertices.push_back(waypointVertex(
            start - side, 0.0f, 0.0f, color));
        m_vertices.push_back(waypointVertex(
            start + side, 1.0f, 0.0f, color));
        m_vertices.push_back(waypointVertex(
            end + side, 1.0f, textureEnd, color));
        m_vertices.push_back(waypointVertex(
            end - side, 0.0f, textureEnd, color));
        const container::Array<uint32_t, 6> indices{
            base, base + 2u, base + 1u,
            base, base + 3u, base + 2u};
        m_indices.insert(m_indices.end(), indices.begin(), indices.end());
        sortCenter += midpoint;
        ++emitted;
    }
    if (emitted == 0) return 0;

    if (m_textureSrv == 0) {
        const std::optional<uint32_t> texture =
            m_textures->acquire("EXLaser.tga");
        if (!texture) return 0;
        m_textureSrv = *texture;
    }
    const size_t vertexBytes64 =
        m_vertices.size() * sizeof(StaticMeshVertex);
    const size_t indexBytes64 = m_indices.size() * sizeof(uint32_t);
    if (vertexBytes64 > std::numeric_limits<uint32_t>::max() ||
        indexBytes64 > std::numeric_limits<uint32_t>::max()) return 0;
    const auto vertexUpload = m_device->allocateFrameUpload(
        m_vertices.data(), static_cast<uint32_t>(vertexBytes64),
        alignof(StaticMeshVertex));
    const auto indexUpload = m_device->allocateFrameUpload(
        m_indices.data(), static_cast<uint32_t>(indexBytes64),
        alignof(uint32_t));
    if (!vertexUpload || !indexUpload) return 0;

    StaticMeshDrawPacket packet;
    packet.vertexBuffer = {
        .BufferLocation = vertexUpload.gpuAddress,
        .SizeInBytes = static_cast<uint32_t>(vertexBytes64),
        .StrideInBytes = sizeof(StaticMeshVertex)};
    packet.indexBuffer = {
        .BufferLocation = indexUpload.gpuAddress,
        .SizeInBytes = static_cast<uint32_t>(indexBytes64),
        .Format = DXGI_FORMAT_R32_UINT};
    packet.textureSrv = m_device->getSrvGpuHandle(m_textureSrv);
    packet.worldTransform = math::transform::identity();
    packet.sortCenter = sortCenter / static_cast<float>(emitted);
    packet.hasExplicitSortCenter = true;
    packet.lightingEnabled = false;
    packet.castsShadow = false;
    packet.receivesShadow = false;
    packet.receivesVisibility = false;
    packet.receivesMapBorder = true;
    packet.twoSided = true;
    packet.depthWrite = false;
    packet.depthCompare = StaticMeshDepthCompare::Always;
    packet.blendMode = StaticMeshBlendMode::Additive;
    packet.worldLayer = StaticMeshWorldLayer::Waypoints;
    packet.indexCount = static_cast<uint32_t>(m_indices.size());
    output.push_back(packet);
    return 1;
}

} // namespace engine::render
