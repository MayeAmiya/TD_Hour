#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::d3d12 { class D3D12Device; }

namespace engine::render::detail {

// Per-render-frame admission policy for static terrain uploads. It reserves
// upload arena capacity for visible W3D, skin palettes, particles and UI,
// while guaranteeing that one oversized terrain product can still progress
// when the frame arena has enough physical space.
class TerrainGpuCommitBudget final {
public:
    explicit TerrainGpuCommitBudget(
        const d3d12::D3D12Device& device) noexcept;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool admitMesh(
        size_t vertexCount,
        size_t indexCount) noexcept;

private:
    uint64_t m_frameRemaining = 0;
    uint64_t m_ordinaryBudget = 0;
    uint64_t m_committed = 0;
};

} // namespace engine::render::detail
