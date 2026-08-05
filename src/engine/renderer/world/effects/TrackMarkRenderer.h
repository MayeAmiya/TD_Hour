#pragma once

#include "core/container/hash_containers.h"

#include "presentation/render/TrackMarksPerformanceSettings.h"
#include "presentation/render/TrackMarksVisualSettings.h"
#include "engine/renderer/world/model/W3dStaticModel.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"

#include <cstddef>
#include <cstdint>
#include <utility>
namespace engine::d3d12 {
class D3D12Device;
}

namespace engine::render {

class WorldTextureCache;

// Detached, current-frame input for one vehicle's authored TrackMarks draw
// module. The game/ECS side publishes only this value; the renderer owns all
// cross-frame anchors and edge history.
struct TrackMarkRenderSource final {
    RenderEntityId objectId = 0;
    RenderVector position{};
    RenderVector forward{1.0f, 0.0f, 0.0f};
    container::String textureName;
    float trackWidth = ::game::track_marks::visual_defaults::kFallbackWidth;
    float edgeSpacing = ::game::track_marks::visual_defaults::kSegmentLength;
    uint32_t maximumEdges = ::engine::kHighTrackMarksBudget.maximumEdges;
    uint32_t opaqueEdges = ::engine::kHighTrackMarksBudget.opaqueEdges;
    uint32_t fadeLifetimeFrames = static_cast<uint32_t>(
        (static_cast<uint64_t>(
             ::engine::kHighTrackMarksBudget.fadeDelayMilliseconds) *
             30u +
         999u) /
        1000u);
    bool moving = false;
    bool visible = true;
};

struct TrackMarkRenderStats final {
    uint32_t activeStreams = 0;
    uint32_t activeEdges = 0;
    uint32_t renderedSegments = 0;
    uint32_t rejectedSources = 0;
    uint32_t trimmedEdges = 0;
    uint32_t streamHighWater = 0;
    uint32_t edgeHighWater = 0;
    uint32_t timelineResets = 0;
    uint32_t drawCalls = 0;
    uint32_t cachedTextures = 0;
};

struct TrackMarkRenderBatch final {
    container::String textureName;
    uint32_t textureSrvIndex = 0;
    RenderVector sortCenter{};
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
};

struct TrackMarkRenderDrawList final {
    uint64_t textureBindingGeneration = 0;
    container::Vector<StaticMeshVertex> vertices;
    container::Vector<uint32_t> indices;
    container::Vector<TrackMarkRenderBatch> batches;
    uint32_t streamCount = 0;
    uint32_t edgeCount = 0;
    uint32_t segmentCount = 0;
};

// Bounded renderer-local vehicle track history. CPU preparation is usable
// without a D3D12 device so focused probes can validate pause/replay, caps,
// terrain conformance, strip breaks and deterministic fading.
class TrackMarkRenderer final {
public:
    TrackMarkRenderer() = default;
    TrackMarkRenderer(d3d12::D3D12Device& device,
                      container::SharedPtr<WorldTextureCache> textures);
    ~TrackMarkRenderer();

    TrackMarkRenderer(const TrackMarkRenderer&) = delete;
    TrackMarkRenderer& operator=(const TrackMarkRenderer&) = delete;

    bool init(d3d12::D3D12Device& device,
              container::SharedPtr<WorldTextureCache> textures);
    void shutdown();
    void reset();
    void resetTextureCache();

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }
    [[nodiscard]] const TrackMarkRenderStats& stats() const noexcept {
        return m_stats;
    }
    [[nodiscard]] size_t cachedTextureCount() const noexcept {
        return m_textureSrvs.size();
    }

    [[nodiscard]] TrackMarkRenderDrawList buildDrawList(
        container::Span<const TrackMarkRenderSource> sources,
        const TerrainRenderSnapshot* terrain,
        uint64_t simulationFrame,
        uint64_t presentationEpoch);
    // Normal frame path: the caller retains `output` across frames so its
    // vertex/index/batch storage can grow to the observed high-water mark
    // without being allocated again on every build. The value-returning
    // overload above remains for probes and compatibility callers.
    void buildDrawListIntoRetained(
        TrackMarkRenderDrawList& output,
        container::Span<const TrackMarkRenderSource> sources,
        const TerrainRenderSnapshot* terrain,
        uint64_t simulationFrame,
        uint64_t presentationEpoch);
    void prepareTextureBindings(TrackMarkRenderDrawList& drawList);

    // Uploads the value-only draw list into this frame's upload arena and
    // appends ordinary WorldRenderer packets. Those packets deliberately use
    // alpha blend, depth test/no-write, no shadows, fog and local visibility.
    [[nodiscard]] size_t appendDrawPackets(
        const TrackMarkRenderDrawList& drawList,
        container::Vector<StaticMeshDrawPacket>& output);

private:
    struct Edge final {
        RenderVector left{};
        RenderVector right{};
        uint64_t bornFrame = 0;
        float textureV = 0.0f;
        bool startsStrip = true;
    };

    struct StreamState final {
        container::String textureName;
        container::Deque<Edge> edges;
        RenderVector anchor{};
        RenderVector anchorSide{0.0f, 1.0f, 0.0f};
        float width = ::game::track_marks::visual_defaults::kFallbackWidth;
        float spacing = ::game::track_marks::visual_defaults::kSegmentLength;
        uint32_t maximumEdges = ::engine::kHighTrackMarksBudget.maximumEdges;
        uint32_t opaqueEdges = ::engine::kHighTrackMarksBudget.opaqueEdges;
        uint32_t fadeLifetimeFrames = static_cast<uint32_t>(
            (static_cast<uint64_t>(
                 ::engine::kHighTrackMarksBudget.fadeDelayMilliseconds) *
                 30u +
             999u) /
            1000u);
        uint64_t lastSeenFrame = 0;
        float nextTextureV = 0.0f;
        bool anchorValid = false;
    };

    void resetHistory(bool timelineReset);
    [[nodiscard]] uint32_t textureSrv(container::StringView textureName);

    d3d12::D3D12Device* m_device = nullptr;
    container::SharedPtr<WorldTextureCache> m_textures;
    container::HashMap<RenderEntityId, StreamState> m_streams;
    container::HashMap<container::String, uint32_t> m_textureSrvs;
    // Per-frame construction scratch. Capacities/buckets are intentionally
    // retained; pointer-bearing storage is emptied before returning.
    container::HashSet<RenderEntityId> m_seenSourcesScratch;
    container::HashSet<RenderEntityId> m_publishedSourceIdsScratch;
    container::Vector<std::pair<RenderEntityId, const StreamState*>>
        m_orderedStreamsScratch;
    container::Vector<uint32_t> m_batchSegmentCountsScratch;
    uint64_t m_textureBindingGeneration = 1;
    TrackMarkRenderStats m_stats;
    uint64_t m_lastSimulationFrame = 0;
    uint64_t m_presentationEpoch = 0;
    uint32_t m_streamHighWater = 0;
    uint32_t m_edgeHighWater = 0;
    uint32_t m_rejectedSources = 0;
    uint32_t m_trimmedEdges = 0;
    uint32_t m_timelineResets = 0;
    bool m_hasTimeline = false;
    bool m_initialized = false;
};

} // namespace engine::render
