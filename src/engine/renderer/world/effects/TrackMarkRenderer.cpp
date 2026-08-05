#include "core/container/hash_containers.h"
#include "engine/renderer/world/effects/TrackMarkRenderer.h"

#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/world/resource/WorldTextureCache.h"
#include "debug/debug.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>
namespace engine::render {
namespace {

constexpr float kTrackGroundBias = 0.055f;
constexpr float kMinimumVisibleAlpha = 1.0f / 255.0f;

[[nodiscard]] bool finiteVector(const RenderVector& value) noexcept {
    return std::isfinite(value.x()) && std::isfinite(value.y()) &&
           std::isfinite(value.z());
}

[[nodiscard]] std::optional<float> terrainHeightAt(
    const TerrainRenderSnapshot* terrain, float worldX, float worldY) noexcept {
    if (!terrain) return std::nullopt;
    if (!terrain->isValid() || !std::isfinite(worldX) ||
        !std::isfinite(worldY)) {
        return std::nullopt;
    }
    const float sampleX = worldX / terrain->cellWorldSize +
        static_cast<float>(terrain->borderSize);
    const float sampleY = worldY / terrain->cellWorldSize +
        static_cast<float>(terrain->borderSize);
    if (sampleX < 0.0f || sampleY < 0.0f ||
        sampleX > static_cast<float>(terrain->width - 1) ||
        sampleY > static_cast<float>(terrain->height - 1)) {
        return std::nullopt;
    }

    const int32_t x0 = static_cast<int32_t>(std::floor(sampleX));
    const int32_t y0 = static_cast<int32_t>(std::floor(sampleY));
    const int32_t x1 = std::min(x0 + 1, terrain->width - 1);
    const int32_t y1 = std::min(y0 + 1, terrain->height - 1);
    const float tx = sampleX - static_cast<float>(x0);
    const float ty = sampleY - static_cast<float>(y0);
    const float h00 = terrain->heightWorld(x0, y0);
    const float h10 = terrain->heightWorld(x1, y0);
    const float h01 = terrain->heightWorld(x0, y1);
    const float h11 = terrain->heightWorld(x1, y1);
    return (h00 + (h10 - h00) * tx) * (1.0f - ty) +
           (h01 + (h11 - h01) * tx) * ty;
}

[[nodiscard]] std::optional<RenderVector> trackSide(
    RenderVector forward) noexcept {
    if (!finiteVector(forward)) return std::nullopt;
    forward = {forward.x(), forward.y(), 0.0f};
    const float length = forward.length();
    if (!std::isfinite(length) || length <= math::EPSILON) return std::nullopt;
    forward = forward / length;
    return RenderVector{-forward.y(), forward.x(), 0.0f};
}

[[nodiscard]] RenderVector interpolatedSide(
    const RenderVector& from, const RenderVector& to, float amount) noexcept {
    RenderVector value = from * (1.0f - amount) + to * amount;
    value = {value.x(), value.y(), 0.0f};
    const float length = value.length();
    if (!std::isfinite(length) || length <= math::EPSILON) return to;
    return value / length;
}

[[nodiscard]] StaticMeshVertex trackVertex(
    const RenderVector& position, float u, float v, float alpha) noexcept {
    StaticMeshVertex output;
    output.position = position;
    output.normal = {0.0f, 0.0f, 1.0f};
    output.texcoord = {u, v};
    output.color = {1.0f, 1.0f, 1.0f, std::clamp(alpha, 0.0f, 1.0f)};
    return output;
}

} // namespace

TrackMarkRenderer::TrackMarkRenderer(
    d3d12::D3D12Device& device,
    container::SharedPtr<WorldTextureCache> textures) {
    static_cast<void>(init(device, std::move(textures)));
}

TrackMarkRenderer::~TrackMarkRenderer() {
    shutdown();
}

bool TrackMarkRenderer::init(
    d3d12::D3D12Device& device,
    container::SharedPtr<WorldTextureCache> textures) {
    shutdown();
    m_device = &device;
    m_textures = std::move(textures);
    if (!m_textures) {
        shutdown();
        return false;
    }
    m_initialized = true;
    TD_LOG_INFO(
        "[TrackMarkRenderer] Initialized (streams={}, edges/stream={}, total edges={})",
        track_marks::performance_limits::kHardMaximumStreams,
        track_marks::performance_limits::kHardMaximumEdgesPerStream,
        track_marks::performance_limits::kHardMaximumTotalEdges);
    return true;
}

void TrackMarkRenderer::shutdown() {
    reset();
    resetTextureCache();
    m_textures.reset();
    m_device = nullptr;
    m_initialized = false;
}

void TrackMarkRenderer::resetHistory(bool timelineReset) {
    m_streams.clear();
    m_lastSimulationFrame = 0;
    m_presentationEpoch = 0;
    m_streamHighWater = 0;
    m_edgeHighWater = 0;
    m_rejectedSources = 0;
    m_trimmedEdges = 0;
    m_hasTimeline = false;
    if (timelineReset) {
        ++m_timelineResets;
    } else {
        m_timelineResets = 0;
    }
    m_stats = {
        .timelineResets = m_timelineResets,
        .cachedTextures = static_cast<uint32_t>(m_textureSrvs.size()),
    };
}

void TrackMarkRenderer::reset() {
    resetHistory(false);
}

void TrackMarkRenderer::resetTextureCache() {
    if (m_textures) {
        for (const auto& [name, ignored] : m_textureSrvs) {
            static_cast<void>(ignored);
            m_textures->release(name);
        }
    }
    m_textureSrvs.clear();
    ++m_textureBindingGeneration;
    if (m_textureBindingGeneration == 0) ++m_textureBindingGeneration;
    m_stats.cachedTextures = 0;
}

TrackMarkRenderDrawList TrackMarkRenderer::buildDrawList(
    container::Span<const TrackMarkRenderSource> sources,
    const TerrainRenderSnapshot* terrain,
    uint64_t simulationFrame,
    uint64_t presentationEpoch) {
    TrackMarkRenderDrawList output;
    buildDrawListIntoRetained(
        output, sources, terrain, simulationFrame, presentationEpoch);
    return output;
}

void TrackMarkRenderer::buildDrawListIntoRetained(
    TrackMarkRenderDrawList& output,
    container::Span<const TrackMarkRenderSource> sources,
    const TerrainRenderSnapshot* terrain,
    uint64_t simulationFrame,
    uint64_t presentationEpoch) {
    output.textureBindingGeneration = 0;
    output.vertices.clear();
    output.indices.clear();
    output.batches.clear();
    output.streamCount = 0;
    output.edgeCount = 0;
    output.segmentCount = 0;

    if (m_hasTimeline &&
        (presentationEpoch != m_presentationEpoch ||
         simulationFrame < m_lastSimulationFrame)) {
        resetHistory(true);
    }
    m_hasTimeline = true;
    m_presentationEpoch = presentationEpoch;
    m_lastSimulationFrame = simulationFrame;

    auto& seenSources = m_seenSourcesScratch;
    seenSources.clear();
    seenSources.reserve(sources.size());
    auto& publishedSourceIds = m_publishedSourceIdsScratch;
    publishedSourceIds.clear();
    publishedSourceIds.reserve(sources.size());
    for (const TrackMarkRenderSource& source : sources) {
        if (source.objectId != 0) publishedSourceIds.insert(source.objectId);
    }

    for (const TrackMarkRenderSource& source : sources) {
        if (source.objectId == 0 || !seenSources.insert(source.objectId).second) {
            ++m_rejectedSources;
            continue;
        }

        auto found = m_streams.find(source.objectId);
        const bool validDescriptor = finiteVector(source.position) &&
            !source.textureName.empty() && std::isfinite(source.trackWidth) &&
            source.trackWidth > 0.0f && std::isfinite(source.edgeSpacing) &&
            source.edgeSpacing > 0.0f;
        const std::optional<RenderVector> sourceSide = trackSide(source.forward);
        if (!validDescriptor || !sourceSide) {
            if (found != m_streams.end()) {
                found->second.lastSeenFrame = simulationFrame;
                found->second.anchorValid = false;
                found->second.nextTextureV = 0.0f;
            }
            ++m_rejectedSources;
            continue;
        }

        if (found == m_streams.end()) {
            if (!source.visible || !source.moving) continue;
            if (m_streams.size() >=
                track_marks::performance_limits::kHardMaximumStreams) {
                // Missing objects retain fading geometry, but must not starve
                // a newly published vehicle for the entire lifetime window.
                // Reuse the oldest absent stream first; never evict another
                // source that is merely later in this same snapshot.
                auto eviction = m_streams.end();
                for (auto candidate = m_streams.begin();
                     candidate != m_streams.end(); ++candidate) {
                    if (publishedSourceIds.contains(candidate->first)) continue;
                    if (eviction == m_streams.end() ||
                        std::tie(candidate->second.lastSeenFrame,
                                 candidate->first) <
                            std::tie(eviction->second.lastSeenFrame,
                                     eviction->first)) {
                        eviction = candidate;
                    }
                }
                if (eviction == m_streams.end()) {
                    ++m_rejectedSources;
                    continue;
                }
                m_trimmedEdges += static_cast<uint32_t>(std::min<size_t>(
                    eviction->second.edges.size(),
                    std::numeric_limits<uint32_t>::max() - m_trimmedEdges));
                m_streams.erase(eviction);
            }
            found = m_streams.emplace(source.objectId, StreamState{}).first;
            m_streamHighWater = std::max<uint32_t>(
                m_streamHighWater, static_cast<uint32_t>(m_streams.size()));
        }

        StreamState& stream = found->second;
        stream.lastSeenFrame = simulationFrame;
        const uint32_t maximumEdges = std::clamp(
            source.maximumEdges,
            track_marks::performance_limits::kMinimumEdgesPerStream,
            track_marks::performance_limits::kHardMaximumEdgesPerStream);
        if (!stream.textureName.empty() &&
            stream.textureName != source.textureName) {
            stream.edges.clear();
            stream.anchorValid = false;
            stream.nextTextureV = 0.0f;
        }
        stream.textureName = source.textureName;
        stream.width = std::clamp(
            source.trackWidth,
            track_marks::performance_limits::kMinimumTrackWidth,
            track_marks::performance_limits::kMaximumTrackWidth);
        stream.spacing = std::clamp(
            source.edgeSpacing,
            track_marks::performance_limits::kMinimumEdgeSpacing,
            track_marks::performance_limits::kMaximumEdgeSpacing);
        stream.maximumEdges = maximumEdges;
        stream.opaqueEdges = std::min(source.opaqueEdges, maximumEdges);
        // The immutable extraction contract already clamps authored
        // milliseconds and converts them with this session's actual logic
        // rate.  Reapplying a 30 Hz-derived upper bound here truncates a
        // valid 45/60 Hz lifetime (ten minutes at 60 Hz becomes five). Keep
        // renderer-side protection numeric only; this boundary must not
        // perform a second, clockless time-unit conversion.
        stream.fadeLifetimeFrames = std::clamp(
            source.fadeLifetimeFrames, 1u,
            std::numeric_limits<uint32_t>::max());

        while (stream.edges.size() > stream.maximumEdges) {
            stream.edges.pop_front();
            ++m_trimmedEdges;
        }
        if (!stream.edges.empty()) stream.edges.front().startsStrip = true;

        if (!source.visible) {
            stream.anchorValid = false;
            stream.nextTextureV = 0.0f;
            continue;
        }
        // RefCode keeps the current TerrainTracks anchor while a vehicle is
        // stopped. No edge is sampled until motion resumes, but the strip is
        // not split merely because forward speed reached zero.
        if (!source.moving) continue;

        const auto makeEdge = [&](const RenderVector& center,
                                  const RenderVector& side,
                                  bool startsStrip) -> std::optional<Edge> {
            const float halfWidth = stream.width * 0.5f;
            RenderVector left = center - side * halfWidth;
            RenderVector right = center + side * halfWidth;
            if (terrain) {
                const std::optional<float> leftHeight = terrainHeightAt(
                    terrain, left.x(), left.y());
                const std::optional<float> rightHeight = terrainHeightAt(
                    terrain, right.x(), right.y());
                if (!leftHeight || !rightHeight) return std::nullopt;
                left = {left.x(), left.y(), *leftHeight + kTrackGroundBias};
                right = {right.x(), right.y(), *rightHeight + kTrackGroundBias};
            } else {
                left = {left.x(), left.y(), center.z() + kTrackGroundBias};
                right = {right.x(), right.y(), center.z() + kTrackGroundBias};
            }
            return Edge{
                .left = left,
                .right = right,
                .bornFrame = simulationFrame,
                .textureV = stream.nextTextureV,
                .startsStrip = startsStrip || stream.edges.empty(),
            };
        };

        const auto appendEdge = [&](Edge edge) {
            while (stream.edges.size() >= stream.maximumEdges) {
                stream.edges.pop_front();
                ++m_trimmedEdges;
            }
            if (!stream.edges.empty()) stream.edges.front().startsStrip = true;
            if (stream.edges.empty()) edge.startsStrip = true;
            stream.edges.push_back(std::move(edge));
            stream.nextTextureV = stream.nextTextureV < 0.5f ? 1.0f : 0.0f;
        };

        if (!stream.anchorValid) {
            stream.nextTextureV = 0.0f;
            if (std::optional<Edge> edge = makeEdge(
                    source.position, *sourceSide, true)) {
                appendEdge(std::move(*edge));
                stream.anchor = source.position;
                stream.anchorSide = *sourceSide;
                stream.anchorValid = true;
            }
            continue;
        }

        RenderVector displacement = source.position - stream.anchor;
        displacement = {displacement.x(), displacement.y(), 0.0f};
        const float distance = displacement.length();
        if (!std::isfinite(distance) || distance < stream.spacing) continue;

        const uint64_t stepCount = static_cast<uint64_t>(
            std::floor(distance / stream.spacing));
        // A discontinuity larger than the complete retained history is a
        // teleport/seek, not a road-length strip. Start a new island instead
        // of spending the entire bounded edge budget bridging empty space.
        if (stepCount > stream.maximumEdges) {
            stream.anchorValid = false;
            stream.nextTextureV = 0.0f;
            if (std::optional<Edge> edge = makeEdge(
                    source.position, *sourceSide, true)) {
                appendEdge(std::move(*edge));
                stream.anchor = source.position;
                stream.anchorSide = *sourceSide;
                stream.anchorValid = true;
            }
            ++m_rejectedSources;
            continue;
        }

        const RenderVector startAnchor = stream.anchor;
        const RenderVector startSide = stream.anchorSide;
        const RenderVector travelDirection = displacement / distance;
        bool complete = true;
        for (uint64_t step = 1; step <= stepCount; ++step) {
            const float travelled = stream.spacing * static_cast<float>(step);
            const float amount = std::clamp(travelled / distance, 0.0f, 1.0f);
            const RenderVector center = startAnchor + travelDirection * travelled;
            const RenderVector side = interpolatedSide(
                startSide, *sourceSide, amount);
            std::optional<Edge> edge = makeEdge(center, side, false);
            if (!edge) {
                stream.anchorValid = false;
                stream.nextTextureV = 0.0f;
                complete = false;
                break;
            }
            appendEdge(std::move(*edge));
            stream.anchor = center;
            stream.anchorSide = side;
        }
        if (!complete) continue;
    }

    size_t totalEdges = 0;
    for (auto stream = m_streams.begin(); stream != m_streams.end();) {
        StreamState& state = stream->second;
        if (!seenSources.contains(stream->first)) {
            state.anchorValid = false;
            state.nextTextureV = 0.0f;
        }
        while (!state.edges.empty()) {
            const Edge& edge = state.edges.front();
            const uint64_t age = simulationFrame >= edge.bornFrame
                ? simulationFrame - edge.bornFrame
                : std::numeric_limits<uint64_t>::max();
            if (age < state.fadeLifetimeFrames) break;
            state.edges.pop_front();
        }
        if (!state.edges.empty()) state.edges.front().startsStrip = true;
        if (state.edges.empty() && !state.anchorValid) {
            stream = m_streams.erase(stream);
            continue;
        }
        totalEdges += state.edges.size();
        ++stream;
    }
    totalEdges = std::min<size_t>(
        totalEdges,
        track_marks::performance_limits::kHardMaximumTotalEdges);
    m_edgeHighWater = std::max<uint32_t>(
        m_edgeHighWater, static_cast<uint32_t>(totalEdges));

    output.streamCount = static_cast<uint32_t>(std::min<size_t>(
        m_streams.size(), std::numeric_limits<uint32_t>::max()));
    output.edgeCount = static_cast<uint32_t>(totalEdges);
    output.vertices.reserve(totalEdges > m_streams.size()
        ? (totalEdges - m_streams.size()) * 4u : 0u);
    output.indices.reserve(totalEdges > m_streams.size()
        ? (totalEdges - m_streams.size()) * 6u : 0u);

    auto& orderedStreams = m_orderedStreamsScratch;
    orderedStreams.clear();
    orderedStreams.reserve(m_streams.size());
    for (const auto& [objectId, stream] : m_streams) {
        orderedStreams.emplace_back(objectId, &stream);
    }
    std::sort(orderedStreams.begin(), orderedStreams.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.second->textureName, left.first) <
                         std::tie(right.second->textureName, right.first);
              });

    auto& batchSegmentCounts = m_batchSegmentCountsScratch;
    batchSegmentCounts.clear();
    batchSegmentCounts.reserve(m_streams.size());
    for (const auto& [ignoredObjectId, streamPointer] : orderedStreams) {
        static_cast<void>(ignoredObjectId);
        const StreamState& stream = *streamPointer;
        if (stream.edges.size() < 2) continue;

        const auto edgeAlpha = [&](size_t edgeIndex) noexcept {
            const Edge& edge = stream.edges[edgeIndex];
            if (edge.startsStrip) return 0.0f;
            const uint64_t age = simulationFrame >= edge.bornFrame
                ? simulationFrame - edge.bornFrame
                : std::numeric_limits<uint64_t>::max();
            if (age >= stream.fadeLifetimeFrames) return 0.0f;
            const float lifetimeAlpha = 1.0f -
                static_cast<float>(age) /
                    static_cast<float>(stream.fadeLifetimeFrames);

            const size_t distanceFromNewest =
                stream.edges.size() - 1u - edgeIndex;
            float lengthAlpha = 1.0f;
            if (distanceFromNewest >= stream.opaqueEdges &&
                stream.maximumEdges > stream.opaqueEdges) {
                const float fadeSpan = static_cast<float>(
                    stream.maximumEdges - stream.opaqueEdges);
                lengthAlpha = std::clamp(
                    (static_cast<float>(stream.maximumEdges) -
                     static_cast<float>(distanceFromNewest)) /
                        fadeSpan,
                    0.0f, 1.0f);
            }
            return std::clamp(lifetimeAlpha * lengthAlpha, 0.0f, 1.0f);
        };

        for (size_t edgeIndex = 1; edgeIndex < stream.edges.size(); ++edgeIndex) {
            const Edge& previous = stream.edges[edgeIndex - 1u];
            const Edge& current = stream.edges[edgeIndex];
            if (current.startsStrip) continue;
            const float previousAlpha = edgeAlpha(edgeIndex - 1u);
            const float currentAlpha = edgeAlpha(edgeIndex);
            if (previousAlpha < kMinimumVisibleAlpha &&
                currentAlpha < kMinimumVisibleAlpha) {
                continue;
            }

            if (output.batches.empty() ||
                output.batches.back().textureName != stream.textureName) {
                output.batches.push_back({
                    .textureName = stream.textureName,
                    .firstIndex = static_cast<uint32_t>(output.indices.size()),
                });
                batchSegmentCounts.push_back(0);
            }
            TrackMarkRenderBatch& batch = output.batches.back();
            const RenderVector segmentCenter =
                (previous.left + previous.right + current.left + current.right) *
                0.25f;
            batch.sortCenter = batch.sortCenter + segmentCenter;
            ++batchSegmentCounts.back();

            const uint32_t baseVertex = static_cast<uint32_t>(
                output.vertices.size());
            output.vertices.push_back(trackVertex(
                previous.left, 0.0f, previous.textureV, previousAlpha));
            output.vertices.push_back(trackVertex(
                previous.right, 1.0f, previous.textureV, previousAlpha));
            output.vertices.push_back(trackVertex(
                current.right, 1.0f, current.textureV, currentAlpha));
            output.vertices.push_back(trackVertex(
                current.left, 0.0f, current.textureV, currentAlpha));
            const container::Array<uint32_t, 6> indices{
                baseVertex, baseVertex + 2u, baseVertex + 1u,
                baseVertex, baseVertex + 3u, baseVertex + 2u,
            };
            output.indices.insert(
                output.indices.end(), indices.begin(), indices.end());
            batch.indexCount += static_cast<uint32_t>(indices.size());
            ++output.segmentCount;
        }
    }
    for (size_t batchIndex = 0; batchIndex < output.batches.size(); ++batchIndex) {
        if (batchSegmentCounts[batchIndex] != 0) {
            output.batches[batchIndex].sortCenter =
                output.batches[batchIndex].sortCenter /
                static_cast<float>(batchSegmentCounts[batchIndex]);
        }
    }
    m_stats = {
        .activeStreams = output.streamCount,
        .activeEdges = output.edgeCount,
        .renderedSegments = output.segmentCount,
        .rejectedSources = m_rejectedSources,
        .trimmedEdges = m_trimmedEdges,
        .streamHighWater = m_streamHighWater,
        .edgeHighWater = m_edgeHighWater,
        .timelineResets = m_timelineResets,
        .cachedTextures = static_cast<uint32_t>(m_textureSrvs.size()),
    };

    // Retain backing capacity, but never retain references into m_streams or
    // stale membership state beyond this build.
    orderedStreams.clear();
    batchSegmentCounts.clear();
    seenSources.clear();
    publishedSourceIds.clear();
}

void TrackMarkRenderer::prepareTextureBindings(TrackMarkRenderDrawList& drawList) {
    for (TrackMarkRenderBatch& batch : drawList.batches) {
        batch.textureSrvIndex = textureSrv(batch.textureName);
    }
    drawList.textureBindingGeneration = m_textureBindingGeneration;
}

size_t TrackMarkRenderer::appendDrawPackets(
    const TrackMarkRenderDrawList& drawList,
    container::Vector<StaticMeshDrawPacket>& output) {
    m_stats.drawCalls = 0;
    if (!m_initialized || !m_device || !m_device->commandList() ||
        drawList.vertices.empty() || drawList.indices.empty()) {
        return 0;
    }

    const size_t vertexBytes64 =
        drawList.vertices.size() * sizeof(StaticMeshVertex);
    const size_t indexBytes64 = drawList.indices.size() * sizeof(uint32_t);
    if (vertexBytes64 > std::numeric_limits<uint32_t>::max() ||
        indexBytes64 > std::numeric_limits<uint32_t>::max()) {
        return 0;
    }
    const uint32_t vertexBytes = static_cast<uint32_t>(vertexBytes64);
    const uint32_t indexBytes = static_cast<uint32_t>(indexBytes64);
    const d3d12::FrameUploadAllocation vertexAllocation =
        m_device->allocateFrameUpload(
            drawList.vertices.data(), vertexBytes, alignof(StaticMeshVertex));
    const d3d12::FrameUploadAllocation indexAllocation =
        m_device->allocateFrameUpload(
            drawList.indices.data(), indexBytes, alignof(uint32_t));
    if (!vertexAllocation || !indexAllocation) return 0;

    const D3D12_VERTEX_BUFFER_VIEW vertexView{
        .BufferLocation = vertexAllocation.gpuAddress,
        .SizeInBytes = vertexBytes,
        .StrideInBytes = sizeof(StaticMeshVertex),
    };
    const D3D12_INDEX_BUFFER_VIEW indexView{
        .BufferLocation = indexAllocation.gpuAddress,
        .SizeInBytes = indexBytes,
        .Format = DXGI_FORMAT_R32_UINT,
    };

    const size_t initialPacketCount = output.size();
    output.reserve(output.size() + drawList.batches.size());
    for (const TrackMarkRenderBatch& batch : drawList.batches) {
        const uint64_t indexEnd =
            static_cast<uint64_t>(batch.firstIndex) + batch.indexCount;
        if (batch.textureName.empty() || batch.indexCount == 0 ||
            indexEnd > drawList.indices.size()) {
            continue;
        }
        StaticMeshDrawPacket packet;
        packet.vertexBuffer = vertexView;
        packet.indexBuffer = indexView;
        const uint32_t textureSrvIndex =
            drawList.textureBindingGeneration == m_textureBindingGeneration
            ? batch.textureSrvIndex : 0;
        packet.textureSrv = m_device->getSrvGpuHandle(
            textureSrvIndex);
        packet.worldTransform = math::transform::identity();
        packet.sortCenter = batch.sortCenter;
        packet.hasExplicitSortCenter = true;
        packet.diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
        packet.lightingEnabled = false;
        packet.castsShadow = false;
        packet.receivesShadow = false;
        packet.receivesVisibility = true;
        packet.receivesMapBorder = true;
        packet.twoSided = true;
        packet.fogFunc = 1; // W3DFOG_ENABLE
        packet.depthWrite = false;
        packet.depthCompare = StaticMeshDepthCompare::LessEqual;
        packet.blendMode = StaticMeshBlendMode::Alpha;
        packet.worldLayer = StaticMeshWorldLayer::Tracks;
        packet.firstIndex = batch.firstIndex;
        packet.indexCount = batch.indexCount;
        output.push_back(packet);
    }
    m_stats.drawCalls = static_cast<uint32_t>(std::min<size_t>(
        output.size() - initialPacketCount,
        std::numeric_limits<uint32_t>::max()));
    m_stats.cachedTextures = static_cast<uint32_t>(m_textureSrvs.size());
    return output.size() - initialPacketCount;
}

uint32_t TrackMarkRenderer::textureSrv(container::StringView textureName) {
    if (!m_device || !m_textures || textureName.empty()) return 0;
    const container::String key(textureName);
    if (const auto found = m_textureSrvs.find(key);
        found != m_textureSrvs.end()) {
        return found->second;
    }
    const std::optional<uint32_t> acquired = m_textures->acquire(textureName);
    if (!acquired) return 0;
    m_textureSrvs.emplace(key, *acquired);
    return *acquired;
}

} // namespace engine::render
