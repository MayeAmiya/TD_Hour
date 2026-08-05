#pragma once

#include "core/container/container_types.h"

#include "engine/renderer/runtime/RendererStats.h"
#include "presentation/render/RenderOverlaySnapshot.h"
#include "presentation/render/RenderViewSnapshot.h"
#include "engine/renderer/world/pipeline/WorldCamera.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace engine::d3d12 {
class D3D12Device;
}

namespace engine::render {

// Owns the tactical-world post-process stage as one renderer-local unit.
// Device resources, retained scene color, effect clocks and command recording
// remain together so WorldRenderer does not need to mirror their lifetimes.
class WorldPostProcessRenderer final {
public:
    WorldPostProcessRenderer() = default;
    ~WorldPostProcessRenderer();

    WorldPostProcessRenderer(const WorldPostProcessRenderer&) = delete;
    WorldPostProcessRenderer& operator=(const WorldPostProcessRenderer&) = delete;
    WorldPostProcessRenderer(WorldPostProcessRenderer&&) = delete;
    WorldPostProcessRenderer& operator=(WorldPostProcessRenderer&&) = delete;

    [[nodiscard]] bool init(d3d12::D3D12Device& device);
    void shutdown();
    void resetPresentationEpoch(uint64_t presentationEpoch) noexcept;

    [[nodiscard]] const SceneColorRenderStats& sceneColorStats() const noexcept {
        return m_sceneColorStats;
    }

    void renderScreenFade(
        const ScreenFadeRenderState& fade, uint64_t simulationFrame = 0);

    [[nodiscard]] RenderCameraSnapshot prepareScriptViewFilters(
        const RenderCameraSnapshot& cameraSnapshot, uint64_t simulationFrame,
        const BlackAndWhiteRenderState& blackAndWhite,
        const MotionBlurRenderState& motionBlur) noexcept;
    void renderScriptViewFilters();

    [[nodiscard]] bool configureFxaa(
        bool enabled, float subpixel, float edgeThreshold,
        float edgeThresholdMin) noexcept;
    [[nodiscard]] bool fxaaAvailable() const noexcept {
        return m_fxaaAvailable;
    }
    [[nodiscard]] bool renderFxaa(float tacticalViewportHeightScale);

private:
    enum class ShaderBytecode : uint8_t {
        ScreenFadeVertex,
        ScreenFadePixel,
        BlackAndWhiteVertex,
        BlackAndWhitePixel,
        MotionBlurVertex,
        MotionBlurPixel,
        Count,
    };

    struct ScreenFadeConsumerCursor {
        uint64_t presentationEpoch = 0;
        uint64_t simulationFrame = 0;
        uint64_t presentationSequence = 0;
    };

    struct BlackAndWhiteConsumer {
        int32_t transitionFrames = 0;
        int64_t currentFrame = 0;
        int32_t direction = 0;
        float mix = 0.0f;
        bool active = false;
    };

    enum class ScriptViewFilter : uint8_t {
        None,
        BlackAndWhite,
        MotionBlur,
    };

    struct MotionBlurConsumer {
        MotionBlurRenderMode mode = MotionBlurRenderMode::ZoomIn;
        bool active = false;
        bool saturate = false;
        int32_t followAmount = 0;
        int32_t maxCount = 0;
        bool decrement = false;
        bool endAfterCurrentPass = false;
        bool jumpAfterCurrentPass = false;
        bool hasJumpTarget = false;
        math::vec3 jumpTarget{};
        math::vec3 jumpBaseTarget{};
        bool hasPresentationTranslation = false;
        math::vec3 presentationTranslation{};
        math::vec2 panUvDelta{};
        bool captureSceneNextPass = true;
        uint64_t lastSimulationFrame = 0;
        bool hasSimulationFrame = false;
    };

    [[nodiscard]] bool loadShaderPackages();
    [[nodiscard]] bool createScreenFadeResources();
    [[nodiscard]] bool createBlackAndWhiteResources();
    [[nodiscard]] bool createMotionBlurResources();
    [[nodiscard]] bool createFxaaResources();
    [[nodiscard]] bool ensureViewFilterSceneCopy();
    void releaseViewFilterSceneCopy() noexcept;
    [[nodiscard]] bool captureViewFilterScene();

    d3d12::D3D12Device* m_device = nullptr;
    container::Array<container::Vector<uint8_t>,
        static_cast<size_t>(ShaderBytecode::Count)> m_shaderBytecode;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_screenFadeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_blackAndWhiteRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_motionBlurRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_fxaaRootSignature;
    container::Array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,
        static_cast<size_t>(ScreenFadeBlendMode::Count)> m_screenFadePipelineStates;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_blackAndWhitePipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_motionBlurPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_fxaaPipelineState;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_viewFilterSceneCopy;
    uint32_t m_viewFilterSceneCopySrv = UINT32_MAX;
    uint32_t m_viewFilterSceneCopyWidth = 0;
    uint32_t m_viewFilterSceneCopyHeight = 0;
    uint64_t m_viewFilterSceneCopyAllocationBytes = 0;
    SceneColorRenderStats m_sceneColorStats;

    ScreenFadeConsumerCursor m_screenFadeCursor;
    uint64_t m_viewFilterPresentationEpoch = 0;
    uint64_t m_viewFilterPresentationSequence = 0;
    ScriptViewFilter m_activeViewFilter = ScriptViewFilter::None;
    BlackAndWhiteConsumer m_blackAndWhite;
    MotionBlurConsumer m_motionBlur;
    RenderCameraSnapshot m_lastViewFilterCamera;
    bool m_hasLastViewFilterCamera = false;

    bool m_fxaaAvailable = false;
    bool m_fxaaEnabled = false;
    float m_fxaaSubpixel = 0.75f;
    float m_fxaaEdgeThreshold = 0.166f;
    float m_fxaaEdgeThresholdMin = 0.0833f;
    bool m_initialized = false;
};

} // namespace engine::render
