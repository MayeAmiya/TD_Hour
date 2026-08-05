#pragma once

#include "core/container/container_types.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>

namespace engine::render {

enum class GpuParticlePipelineKind : uint8_t {
    Reset,
    ApplyRetire,
    ApplyBirth,
    Integrate,
    ResetAliveCompact,
    AliveCompact,
    ResetVisibleCompact,
    VisibleCompact,
    MaterialBinReset,
    MaterialBinCount,
    MaterialBinPrefix,
    MaterialBinScatter,
    Count,
};

// Immutable compute-program owner shared by every particle simulation pass.
// Particle buffers, counters, uploads and readbacks remain in the simulator.
class GpuParticlePipeline final {
public:
    [[nodiscard]] bool initialize(ID3D12Device* device);
    void shutdown() noexcept;

    [[nodiscard]] ID3D12RootSignature* rootSignature() const noexcept {
        return m_rootSignature.Get();
    }
    [[nodiscard]] ID3D12PipelineState* pipeline(
        GpuParticlePipelineKind kind) const noexcept {
        const size_t index = static_cast<size_t>(kind);
        return index < m_pipelineStates.size()
            ? m_pipelineStates[index].Get()
            : nullptr;
    }
    [[nodiscard]] bool valid() const noexcept {
        return m_rootSignature && pipeline(GpuParticlePipelineKind::Reset);
    }

private:
    [[nodiscard]] bool loadShaderPackage();
    [[nodiscard]] bool createRootSignature(ID3D12Device* device);
    [[nodiscard]] bool createPipelineStates(ID3D12Device* device);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    container::Array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,
        static_cast<size_t>(GpuParticlePipelineKind::Count)> m_pipelineStates;
    container::Array<container::Vector<uint8_t>,
        static_cast<size_t>(GpuParticlePipelineKind::Count)> m_shaderBytecode;
};

} // namespace engine::render
