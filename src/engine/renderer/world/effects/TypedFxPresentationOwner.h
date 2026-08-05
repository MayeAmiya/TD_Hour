#pragma once

#include "core/container/container_types.h"
#include "core/math/wwmath/vector/float2.h"
#include "engine/renderer/world/effects/DynamicPointLightRuntime.h"
#include "engine/renderer/world/effects/TypedFxWorldRenderer.h"

#include <cstddef>
#include <cstdint>

namespace engine::render {

class W3dAssetCache;

// Render-thread owner for the complete detached typed-FX presentation stream.
// GPU beam/model-ray state, light lifetimes, terrain scorches, audio handoff
// and view-shake decay share one epoch and are reset atomically.
class TypedFxPresentationOwner final {
public:
    TypedFxPresentationOwner(
        d3d12::D3D12Device& device,
        container::SharedPtr<WorldTextureCache> textures);

    void reset(uint64_t sessionEpoch = 0);
    void consume(
        fx::FxPresentationCommandBatch commands,
        W3dAssetCache& assets,
        size_t maximumOwnerCommands);

    [[nodiscard]] container::Vector<fx::FxSoundCommand> takeSounds();
    [[nodiscard]] RenderCameraSnapshot applyViewShake(
        const RenderCameraSnapshot& camera,
        uint64_t simulationFrame);

    [[nodiscard]] container::Span<const fx::FxTerrainScorchCommand>
    terrainScorches() const noexcept {
        return m_terrainScorches;
    }
    [[nodiscard]] container::Span<const DynamicPointLightRenderData>
    advanceLights(uint64_t simulationFrame) {
        return m_dynamicPointLights.advance(
            m_sessionEpoch, simulationFrame);
    }
    [[nodiscard]] const DynamicPointLightRuntimeStats& lightStats() const
        noexcept {
        return m_dynamicPointLights.stats();
    }

    [[nodiscard]] TypedFxWorldRenderer& worldRenderer() noexcept {
        return m_worldRenderer;
    }
    [[nodiscard]] const TypedFxWorldRenderer& worldRenderer() const noexcept {
        return m_worldRenderer;
    }
    [[nodiscard]] uint64_t sessionEpoch() const noexcept {
        return m_sessionEpoch;
    }
    [[nodiscard]] uint64_t rejectedCommands() const noexcept {
        return m_rejectedCommands;
    }
    [[nodiscard]] uint64_t retainedQueueCapacityBytes() const noexcept;
    void noteRejected(size_t count = 1) noexcept {
        m_rejectedCommands += count;
    }

private:
    TypedFxWorldRenderer m_worldRenderer;
    DynamicPointLightRuntime m_dynamicPointLights;
    container::Vector<fx::FxTerrainScorchCommand> m_terrainScorches;
    container::Vector<fx::FxViewShakeCommand> m_viewShakes;
    container::Vector<fx::FxSoundCommand> m_pendingSounds;
    uint64_t m_sessionEpoch = 0;
    uint64_t m_lastShakeSimulationFrame = 0;
    math::vec2 m_shakeDirection{};
    math::vec2 m_shakeOffset{};
    float m_shakeIntensity = 0.0f;
    uint64_t m_rejectedCommands = 0;
};

} // namespace engine::render
