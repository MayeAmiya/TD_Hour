#pragma once

#include "core/container/container_types.h"
#include "engine/renderer/world/model/W3dStaticModel.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"
#include "presentation/render/RenderOverlaySnapshot.h"

namespace engine::d3d12 { class D3D12Device; }

namespace engine::render {

class WorldTextureCache;

class WaypointRenderer final {
public:
    WaypointRenderer() = default;
    WaypointRenderer(d3d12::D3D12Device& device,
                     container::SharedPtr<WorldTextureCache> textures);
    ~WaypointRenderer();

    WaypointRenderer(const WaypointRenderer&) = delete;
    WaypointRenderer& operator=(const WaypointRenderer&) = delete;

    bool init(d3d12::D3D12Device& device,
              container::SharedPtr<WorldTextureCache> textures);
    void shutdown();
    void resetTextureCache() noexcept;
    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }
    [[nodiscard]] size_t appendDrawPackets(
        const SharedSnapshotVector<OrderWaypointSegmentRenderSnapshot>& segments,
        const RenderCameraSnapshot& camera,
        container::Vector<StaticMeshDrawPacket>& output);

private:
    d3d12::D3D12Device* m_device = nullptr;
    container::SharedPtr<WorldTextureCache> m_textures;
    container::Vector<StaticMeshVertex> m_vertices;
    container::Vector<uint32_t> m_indices;
    uint32_t m_textureSrv = 0;
    bool m_initialized = false;
};

} // namespace engine::render
