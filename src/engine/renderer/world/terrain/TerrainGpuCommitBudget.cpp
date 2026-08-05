#include "engine/renderer/world/terrain/TerrainGpuCommitBudget.h"

#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/world/model/W3dStaticModel.h"

#include <algorithm>

namespace engine::render::detail {
namespace {

constexpr uint64_t kTerrainUploadBudgetPerFrame = 12u * 1024u * 1024u;
constexpr uint64_t kReservedFrameUploadBytes = 8u * 1024u * 1024u;
constexpr uint64_t kAlignmentAllowance = 128u;

} // namespace

TerrainGpuCommitBudget::TerrainGpuCommitBudget(
    const d3d12::D3D12Device& device) noexcept
    : m_frameRemaining(device.frameUploadRemainingBytes()) {
    if (available()) {
        m_ordinaryBudget = std::min(
            kTerrainUploadBudgetPerFrame,
            m_frameRemaining - kReservedFrameUploadBytes);
    }
}

bool TerrainGpuCommitBudget::available() const noexcept {
    return m_frameRemaining > kReservedFrameUploadBytes;
}

bool TerrainGpuCommitBudget::admitMesh(
    size_t vertexCount,
    size_t indexCount) noexcept {
    const uint64_t byteSize = static_cast<uint64_t>(vertexCount) *
            sizeof(StaticMeshVertex) +
        static_cast<uint64_t>(indexCount) * sizeof(uint32_t);
    const uint64_t required = byteSize + kAlignmentAllowance;
    if (!available() ||
        required > m_frameRemaining - kReservedFrameUploadBytes -
                m_committed) {
        return false;
    }
    if (m_committed != 0u &&
        m_committed + required > m_ordinaryBudget) {
        return false;
    }
    m_committed += required;
    return true;
}

} // namespace engine::render::detail
