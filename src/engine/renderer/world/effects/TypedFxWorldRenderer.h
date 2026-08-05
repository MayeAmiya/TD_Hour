#pragma once

#include "core/container/hash_containers.h"

#include "engine/fx/runtime/FxRuntime.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "presentation/fx/runtime/LegacyBeamPresentation.h"
#include "engine/renderer/world/model/D3D12W3dModel.h"
#include "engine/renderer/world/effects/TypedFxBeamPresentation.h"
#include "engine/renderer/world/resource/WorldTextureCache.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine::render {

// Render-thread owner for typed RayEffect, Laser, Rope and Tracer state.
// Commands are detached at admission, while PSO, texture leases and active
// beam/model-ray lifetime remain local to this owner.
class TypedFxWorldRenderer final {
public:
    struct Stats final {
        uint32_t activeBeams = 0;
        uint32_t activeModelRays = 0;
        uint32_t renderedTriangles = 0;
        uint32_t renderedModelPackets = 0;
        uint32_t rejectedCommands = 0;
        uint32_t highWaterEffects = 0;
        uint32_t drawCalls = 0;
    };

    explicit TypedFxWorldRenderer(
        d3d12::D3D12Device& device,
        container::SharedPtr<WorldTextureCache> textures);
    ~TypedFxWorldRenderer();

    TypedFxWorldRenderer(const TypedFxWorldRenderer&) = delete;
    TypedFxWorldRenderer& operator=(const TypedFxWorldRenderer&) = delete;

    bool init(d3d12::D3D12Device& device);
    void shutdown();
    void resetTextureCache();
    bool configureSampleCount(uint32_t sampleCount);
    void reset();
    void submit(const fx::FxPresentationCommandBatch& commands);

    [[nodiscard]] size_t render(
        const RenderCameraSnapshot& camera,
        float deltaSeconds,
        const TerrainRenderSnapshot* terrain,
        const fx::FxRuntime* fxRuntime,
        uint64_t simulationFrame,
        const LocalVisibilityRenderSnapshot& localVisibility = {});
    [[nodiscard]] const Stats& stats() const noexcept;
    [[nodiscard]] bool isInitialized() const noexcept;
    void appendActiveBonePoseDemands(
        container::Vector<fx::FxBonePoseDemand>& output) const;

    [[nodiscard]] size_t appendModelRayDrawPackets(
        W3dAssetCache& assets,
        container::Vector<StaticMeshDrawPacket>& output,
        W3dRestPaletteFrameCache& restPalettes,
        float visualTimeSeconds,
        uint64_t simulationFrame,
        W3dModelGraphTraversalStats* traversalStats);

private:
    struct ModelRay final {
        fx::LegacyModelRayState state;
        uint64_t admittedFrame = 0;
    };

    struct Beam final {
        RenderVector start{};
        RenderVector end{};
        std::optional<fx::FxTypedAnchor> startAnchor;
        std::optional<fx::FxTypedAnchor> endAnchor;
        bool targetAttachmentWasAlive = false;
        bool punchThroughApplied = false;
        float ageSeconds = 0.0f;
        float lifetimeSeconds = 0.10f;
        uint64_t admittedFrame = 0;
        uint64_t laserIdentity = 0;
        uint64_t widenStartFrame = 0;
        uint64_t widenFinishFrame = 0;
        uint64_t decayStartFrame = 0;
        uint64_t decayFinishFrame = 0;
        float widthScale = 1.0f;
        bool widening = false;
        bool decaying = false;
        bool controlledLaser = false;
        std::optional<TypedFxTracerState> tracer;
        std::optional<fx::LegacyBeamTemplate> laser;
        std::optional<fx::LegacyRopeState> rope;
        uint64_t ropeIdentity = 0;
        uint64_t lastRopeFrame = 0;
    };

    struct Vertex final {
        float position[3]{};
        float color[4]{};
        float uv[2]{};
    };

    struct DrawBatch final {
        uint32_t firstVertex = 0;
        uint32_t vertexCount = 0;
        uint32_t textureSrv = 0;
    };

    static void applyLaserRadiusCommand(
        Beam& beam, const fx::FxLaserCommand& command) noexcept;
    void appendLaser(const fx::FxLaserCommand& command);
    void age(float deltaSeconds, uint64_t simulationFrame);
    [[nodiscard]] static std::optional<float> terrainHeightAt(
        const TerrainRenderSnapshot* terrain,
        float worldX,
        float worldY) noexcept;
    [[nodiscard]] uint32_t textureSrv(container::StringView name);
    [[nodiscard]] float textureAspectRatio(
        container::StringView name) const noexcept;
    void releaseTextures();
    void buildVertices(
        const RenderCameraSnapshot& camera,
        const TerrainRenderSnapshot* terrain,
        const fx::FxRuntime* fxRuntime,
        uint64_t simulationFrame);
    bool createRootSignature();
    bool loadShaderPackage();
    bool createPipelineState();

    d3d12::D3D12Device* m_device = nullptr;
    container::SharedPtr<WorldTextureCache> m_textures;
    container::HashMap<container::String, uint32_t> m_textureSrvs;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;
    container::Array<container::Vector<uint8_t>, 2> m_shaderBytecode;
    container::Vector<Beam> m_beams;
    container::Vector<ModelRay> m_modelRays;
    container::Vector<fx::LegacyLaserSegment> m_segmentScratch;
    container::HashSet<container::String> m_reportedModelRayFailures;
    container::Vector<Vertex> m_vertices;
    container::Vector<DrawBatch> m_batches;
    Stats m_stats;
    uint32_t m_sampleCount = 1;
    bool m_initialized = false;
};

} // namespace engine::render
