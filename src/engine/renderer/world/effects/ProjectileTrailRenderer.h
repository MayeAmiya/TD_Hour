#pragma once

#include "core/container/hash_containers.h"
#include "presentation/render/ProjectileStreamJoinPresentation.h"

#include "engine/renderer/world/pipeline/WorldRenderPipeline.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
namespace engine::d3d12 {
class D3D12Device;
}

namespace engine::render {

class WorldTextureCache;

struct ProjectileTrailRenderStats final {
    uint32_t activeTrails = 0;
    uint32_t activePoints = 0;
    uint32_t renderedSegments = 0;
    uint32_t rejectedTrails = 0;
    uint32_t rejectedSegments = 0;
    uint32_t trailHighWater = 0;
    uint32_t drawCalls = 0;
    uint32_t cachedTextures = 0;
};

struct ProjectileTrailRenderVertex final {
    float position[3]{};
    float color[4]{};
    float uv[2]{};
};

struct ProjectileTrailRenderBatch final {
    container::String texture;
    uint32_t textureSrvIndex = 0;
    ProjectileTrailBlendMode blend = ProjectileTrailBlendMode::Additive;
    ProjectileTrailDepthMode depth = ProjectileTrailDepthMode::TestNoWrite;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t segmentCount = 0;
};

struct ProjectileTrailRenderDrawList final {
    uint64_t textureBindingGeneration = 0;
    container::Vector<ProjectileTrailRenderVertex> vertices;
    container::Vector<ProjectileTrailRenderBatch> batches;
    uint32_t streamCount = 0;
    uint32_t pointCount = 0;
    uint32_t segmentCount = 0;
    uint32_t rejectedSegments = 0;
};

// Renderer-local presentation for authored ProjectileStreamName content.
// Each stream connects the current authoritative positions of projectiles
// fired by one launcher/weapon slot, matching ProjectileStreamUpdate rather
// than inventing an individual history ribbon for every shell.
class ProjectileTrailRenderer final {
public:
    ProjectileTrailRenderer() = default;
    ProjectileTrailRenderer(d3d12::D3D12Device& device,
                            container::SharedPtr<WorldTextureCache> textures);
    ~ProjectileTrailRenderer();

    ProjectileTrailRenderer(const ProjectileTrailRenderer&) = delete;
    ProjectileTrailRenderer& operator=(const ProjectileTrailRenderer&) = delete;

    bool init(d3d12::D3D12Device& device,
              container::SharedPtr<WorldTextureCache> textures);
    void shutdown();
    void reset();
    void resetTextureCache();
    [[nodiscard]] bool configureTextureSampling(
        uint32_t textureFilter, uint32_t anisotropyLevel,
        uint32_t sampleCount);

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }
    [[nodiscard]] const ProjectileTrailRenderStats& stats() const noexcept {
        return m_stats;
    }
    [[nodiscard]] size_t cachedTextureCount() const noexcept {
        return m_textureSrvs.size();
    }

    // Pure CPU preparation plus confirmed-frame stream history. It is public
    // so focused probes can validate grouping, visibility, pause and replay
    // without reading back a GPU image.
    [[nodiscard]] ProjectileTrailRenderDrawList buildDrawList(
        container::Span<const PreparedProjectileRenderSnapshot> projectiles,
        const RenderCameraSnapshot& camera,
        uint64_t simulationFrame,
        const LocalVisibilityRenderSnapshot& localVisibility = {});
    void buildDrawListIntoRetained(
        ProjectileTrailRenderDrawList& output,
        container::Span<const PreparedProjectileRenderSnapshot> projectiles,
        const RenderCameraSnapshot& camera,
        uint64_t simulationFrame,
        const LocalVisibilityRenderSnapshot& localVisibility = {});
    void prepareTextureBindings(ProjectileTrailRenderDrawList& drawList);

    [[nodiscard]] size_t render(const ProjectileTrailRenderDrawList& drawList,
                                const RenderCameraSnapshot& camera,
                                const LocalVisibilityRenderSnapshot&
                                    localVisibility = {});
    [[nodiscard]] size_t render(
        container::Span<const PreparedProjectileRenderSnapshot> projectiles,
        const RenderCameraSnapshot& camera,
        uint64_t simulationFrame,
        float deltaSeconds,
        const LocalVisibilityRenderSnapshot& localVisibility = {});

private:
    struct StreamPoint final {
        RenderEntityId objectId = 0;
        RenderEntityId intendedTargetId = 0;
        uint32_t sourceShotSequence = 0;
        uint64_t spawnedTick = 0;
        uint64_t lastObservedSimulationFrame = 0;
        RenderVector position{};
        bool visibilityExempt = false;
        // False is the explicit INVALID_OBJECT_ID tombstone retained by
        // ProjectileStreamUpdate. It prevents later live projectiles from
        // reconnecting across a projectile that disappeared in between.
        bool observedThisFrame = false;
    };

    struct StreamState final {
        ProjectileRenderSnapshot descriptor;
        container::Vector<StreamPoint> points;
    };

    static constexpr size_t kPipelineCount = 12;

    bool createRootSignature();
    bool createPipelineStates();
    void observeProjectiles(
        container::Span<const PreparedProjectileRenderSnapshot> projectiles,
        uint64_t simulationFrame);
    [[nodiscard]] uint32_t textureSrv(container::StringView textureName);

    d3d12::D3D12Device* m_device = nullptr;
    container::SharedPtr<WorldTextureCache> m_textures;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    container::Array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, kPipelineCount>
        m_pipelineStates;
    container::HashMap<container::String, StreamState> m_streams;
    container::HashMap<container::String, uint32_t> m_textureSrvs;
    ProjectileTrailRenderDrawList m_retainedDrawList;
    container::Vector<std::pair<container::StringView, const StreamState*>>
        m_orderedStreamsScratch;
    container::Vector<const StreamPoint*> m_pointScratch;
    container::Vector<ProjectileStreamJoinPoint> m_runScratch;
    ProjectileStreamJoinMesh m_joinMeshScratch;
    uint64_t m_textureBindingGeneration = 1;
    ProjectileTrailRenderStats m_stats;
    uint64_t m_lastSimulationFrame = 0;
    uint32_t m_trailHighWater = 0;
    uint32_t m_textureFilter = 2;
    uint32_t m_anisotropyLevel = 2;
    uint32_t m_sampleCount = 1;
    bool m_initialized = false;
};

} // namespace engine::render
