#pragma once

#include "core/container/hash_containers.h"
#include "engine/renderer/world/resource/RenderAssetLifecycle.h"
#include "engine/renderer/world/resource/RenderAssetScheduling.h"
#include "engine/renderer/world/resource/WorldTextureContracts.h"

#include <cstddef>
#include <cstdint>
#include <optional>
namespace engine {
class TextureManager;

namespace d3d12 {
class D3D12Device;
}

namespace render {

class WorldTextureDecodeService;
class WorldTextureGpuUploadQueue;

// Renderer-owner-thread cache for decoded W3D material textures. request,
// query, completion publish, GPU admission and reset are serialized by that
// owner. The shared decode service writes immutable payload completions only;
// it never mutates cache maps or D3D12 state. Missing textures deliberately
// resolve to the device's white descriptor (slot 0), so material diffuse and
// vertex colour remain visible.
class WorldTextureCache final {
public:
    struct SourceDimensions final {
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct TerrainColorMip final {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t byteOffset = 0;
        uint32_t rowPitch = 0;
        uint32_t slicePitch = 0;
    };

    // CPU-owned terrain payload. Classic terrain selectors address one of
    // four 32px quadrants inside each 64px source tile. Generating the three
    // visible mips independently inside every selector region prevents a
    // source table's neighbouring quadrant from entering mip1/mip2.
    struct TerrainColorMipPayload final {
        container::Vector<uint8_t> rgbaPixels;
        container::Vector<TerrainColorMip> mips;
    };

    // Compatibility alias for existing cache callers. The value contract is
    // shared with CPU decode without including this residency owner.
    using Variant = WorldTextureVariant;

    struct Stats final {
        size_t trackedVariants = 0;
        size_t residentTextures = 0;
        size_t fallbackEntries = 0;
        size_t terminalFailureFallbackEntries = 0;
        uint64_t ownerReferences = 0;
        uint64_t residentBytes = 0;
        uint64_t latestUsedFrame = 0;
        uint64_t latestUsedFence = 0;
        uint64_t cacheHits = 0;
        uint64_t cacheMisses = 0;
        uint64_t gpuUploads = 0;
        uint64_t fallbackResolutions = 0;
        uint64_t failedAcquisitions = 0;
        uint64_t releases = 0;
        uint64_t retiredTextures = 0;
        uint64_t resets = 0;
        size_t decodedSources = 0;
        size_t proceduralSources = 0;
        size_t negativeSourceLookups = 0;
        uint64_t sourceCpuBytes = 0;
        uint64_t sourceRequests = 0;
        uint64_t sourceCacheHits = 0;
        uint64_t sourceCacheMisses = 0;
        uint64_t sourceDecodeAttempts = 0;
        uint64_t sourceDecodeSucceeded = 0;
        uint64_t sourceDecodeFailed = 0;
        uint64_t sourceMissing = 0;
        uint64_t sourceUnsupported = 0;
        uint64_t sourceResets = 0;
        size_t sourceQueuedJobs = 0;
        size_t sourceActiveJobs = 0;
        size_t sourcePendingSources = 0;
        size_t sourceActiveSourceJobs = 0;
        size_t sourcePendingVariants = 0;
        size_t sourceActiveVariantJobs = 0;
        size_t sourcePreparedVariants = 0;
        size_t sourceFailedVariants = 0;
        uint64_t sourceStaleCompletions = 0;
        uint64_t sourceCompletedCpuJobs = 0;
        uint64_t sourcePreparedBytes = 0;
        uint64_t sourceWorkerNanoseconds = 0;
        uint64_t sourceCancelledVariants = 0;
        uint64_t sourceCancelledReady = 0;
        uint64_t sourceCancelRequestedActive = 0;
        uint32_t sourceMaximumQueueAge = 0;
        uint64_t sourceRetainedPreparedBytes = 0;
        uint64_t sourceReclaimedPreparedBytes = 0;
        uint64_t sourceReclaimedBytes = 0;
        uint64_t sourceReclaimedCount = 0;
        size_t queuedGpuUploads = 0;
        uint64_t gpuUploadAttempts = 0;
        uint64_t gpuUploadDeferred = 0;
        uint64_t gpuUploadForcedOversized = 0;
        uint64_t gpuUploadAttemptedBytes = 0;
        uint64_t gpuUploadDeferredBytes = 0;
        uint64_t gpuUploadNanoseconds = 0;
        uint64_t gpuUploadCancelled = 0;
        uint32_t gpuUploadMaximumAge = 0;
        uint64_t residencyEvictions = 0;
        uint64_t residencyEvictedBytes = 0;
        uint64_t residencyOwnerRejects = 0;
        uint64_t residencyPinnedRejects = 0;
        uint32_t residencyPins = 0;
    };

    explicit WorldTextureCache(d3d12::D3D12Device& device);
    ~WorldTextureCache();

    WorldTextureCache(const WorldTextureCache&) = delete;
    WorldTextureCache& operator=(const WorldTextureCache&) = delete;
    WorldTextureCache(WorldTextureCache&&) = delete;
    WorldTextureCache& operator=(WorldTextureCache&&) = delete;

    // Must run between D3D12Device::beginFrame() and endFrame(). Repeated
    // acquisitions reuse one upload/SRV but intentionally add owner
    // references; every successful acquire must be paired with release.
    // A value of 0 is a deliberate white fallback for a genuinely missing or
    // undecodable source texture. nullopt means CPU preparation is pending or
    // GPU upload failed transiently; callers retry and never cache white for
    // either condition.
    [[nodiscard]] std::optional<uint32_t> acquire(
        container::StringView textureName,
        Variant variant = Variant::ColorLegacyGamma,
        RenderAssetPriority priority = RenderAssetPriority::Normal);
    // Idempotently starts/observes preparation without recording GPU work or
    // taking an owner reference. Parallel producers submit only value
    // requests; the renderer owner merges them before calling this API.
    // false means the texture is still CPU-pending or queued for bounded
    // begin-frame GPU admission.
    [[nodiscard]] bool prepare(
        container::StringView textureName,
        Variant variant = Variant::ColorLegacyGamma,
        RenderAssetPriority priority = RenderAssetPriority::Normal);
    void release(container::StringView textureName,
                 Variant variant = Variant::ColorLegacyGamma);
    bool setPinned(
        container::StringView textureName, Variant variant,
        RenderAssetPinScope scope, bool pinned);
    // Returns level-zero decoded source dimensions for a resident texture.
    // This intentionally ignores GameLOD mip reduction: legacy presentation
    // formulas such as WaterSet::SkyTexelsPerUnit used the authored level-zero
    // width even when the GPU selected a reduced mip range.
    [[nodiscard]] std::optional<SourceDimensions> sourceDimensions(
        container::StringView textureName,
        Variant variant = Variant::ColorLegacyGamma) const;

    // RefCode's base/extra/blend TerrainTextureClass atlas exposes only the
    // first three authored source mips. Keep this in a terrain-only cache
    // namespace: ordinary W3D, road, bridge, bib and water textures retain
    // acquire()'s complete reduced mip chain.
    [[nodiscard]] std::optional<uint32_t> acquireTerrainColor(
        container::StringView textureName,
        uint32_t sourceTileGridWidth,
        RenderAssetPriority priority = RenderAssetPriority::Normal);
    [[nodiscard]] bool prepareTerrainColor(
        container::StringView textureName,
        uint32_t sourceTileGridWidth,
        RenderAssetPriority priority = RenderAssetPriority::Normal);
    void releaseTerrainColor(container::StringView textureName,
                             uint32_t sourceTileGridWidth);

    // Value-only mip builder shared with focused probes. The source must be
    // a classic square terrain table whose axes contain
    // sourceTileGridWidth * 2 independently sampled selector regions.
    [[nodiscard]] static std::optional<TerrainColorMipPayload>
    buildTerrainColorMipPayload(
        container::Span<const uint8_t> rgbaPixels,
        uint32_t width,
        uint32_t height,
        uint32_t sourceTileGridWidth);

    // Session-frozen GameLOD/Options texture quality. Changing sessions
    // retires every previous SRV before changing the upload interpretation,
    // so one canonical texture identity can never alias two mip ranges.
    void configureTextureReduction(uint32_t reductionFactor);
    [[nodiscard]] uint32_t textureReductionFactor() const noexcept {
        return m_textureReductionFactor;
    }

    // RefCode's AlphaEdgeTextureClass does not use an authored source alpha.
    // It derives a renderer-owned alpha channel from edge RGB data: black is
    // the reserved 0x80 gap, white is transparent, and every other texel is
    // opaque.  Keep this a separate cache namespace so an edge map never
    // aliases an ordinary W3D/terrain texture with the same VFS name.
    [[nodiscard]] std::optional<uint32_t> acquireTerrainAlphaEdge(
        container::StringView textureName,
        RenderAssetPriority priority = RenderAssetPriority::Normal);
    [[nodiscard]] bool prepareTerrainAlphaEdge(
        container::StringView textureName,
        RenderAssetPriority priority = RenderAssetPriority::Normal);

    // Render-thread frame boundary. Publishes completed scheduler-owned CPU
    // work and advances local retries even when no material retries in the
    // current frame. GPU admission must follow pumpCpuCompletions() and
    // precede model admission so queued dependencies can become resident.
    void pumpCpuCompletions();
    size_t processGpuUploads(const RenderAssetReadyBudget& budget);
    void beginResidencyFrame(uint64_t frameOrdinal) noexcept;
    size_t trimResidency(
        size_t maximumTextures, uint64_t maximumBytes,
        uint64_t graceFrames);
    void releaseTerrainAlphaEdge(container::StringView textureName);

    // Value-only AlphaEdgeTextureClass conversion used by the renderer and
    // offline probes. `rgbaPixels` is TextureManager-normalized top-to-bottom
    // RGBA data; the original per-tile source-row inversion has already been
    // accounted for by that decoder convention. Returns nullopt for malformed
    // dimensions/data rather than issuing a partial GPU upload.
    [[nodiscard]] static std::optional<container::Vector<uint8_t>> buildTerrainAlphaEdgePixels(
        container::Span<const uint8_t> rgbaPixels,
        uint32_t width,
        uint32_t height);

    // Retires every non-fallback texture through D3D12Device's fence-aware
    // texture lifetime. Call while the device is alive.
    void releaseAll();

    // Safe-boundary reset for a map/session or content-mount generation.
    // This retires every GPU entry and also drops decoded sources, aliases,
    // the VFS index and negative lookups. Genuine missing/undecodable content
    // is deliberately cached as white during a generation; resetSourceCache()
    // is the explicit point at which it becomes eligible for another IO/decode
    // attempt. Owners must discard their SRV-bearing models/materials first.
    void resetSourceCache();

    [[nodiscard]] size_t residentTextureCount() const noexcept;
    [[nodiscard]] uint64_t residentTextureBytes() const noexcept;
    [[nodiscard]] Stats stats() const noexcept;
    [[nodiscard]] std::optional<RenderAssetLifecycleRecord>
    describeLifecycle(
        container::StringView textureName,
        Variant variant = Variant::ColorLegacyGamma) const;
    [[nodiscard]] std::optional<RenderAssetLifecycleRecord>
    describeTerrainColorLifecycle(
        container::StringView textureName,
        uint32_t sourceTileGridWidth) const;
    [[nodiscard]] std::optional<RenderAssetLifecycleRecord>
    describeTerrainAlphaEdgeLifecycle(
        container::StringView textureName) const;

private:
    struct Entry {
        uint32_t srvIndex = 0;
        uint32_t referenceCount = 0;
        uint64_t byteSize = 0;
        uint64_t residentSinceFrame = 0;
        uint64_t lastOwnerReleaseFrame = 0;
        uint8_t residencyPinMask = 0;
        SourceDimensions sourceDimensions{};
        bool terminalCpuFailure = false;
    };

    [[nodiscard]] container::String sourceIdentity(
        container::StringView textureName) const;
    [[nodiscard]] container::String ordinaryTextureKey(
        container::StringView textureName,
        Variant variant) const;
    [[nodiscard]] container::String terrainColorKey(
        container::StringView textureName,
        uint32_t sourceTileGridWidth) const;
    [[nodiscard]] container::String terrainAlphaEdgeKey(container::StringView textureName) const;
    [[nodiscard]] std::optional<RenderAssetLifecycleRecord>
    describeEntryLifecycle(
        const container::String& key,
        container::StringView logicalName,
        container::String variant) const;

    d3d12::D3D12Device* m_device = nullptr;
    container::UniquePtr<WorldTextureDecodeService> m_decodeService;
    container::UniquePtr<WorldTextureGpuUploadQueue> m_gpuUploadQueue;
    container::HashMap<container::String, Entry> m_entries;
    uint32_t m_textureReductionFactor = 0;
    uint64_t m_cacheHits = 0;
    uint64_t m_cacheMisses = 0;
    uint64_t m_gpuUploads = 0;
    uint64_t m_fallbackResolutions = 0;
    uint64_t m_failedAcquisitions = 0;
    uint64_t m_releases = 0;
    uint64_t m_retiredTextures = 0;
    uint64_t m_resets = 0;
    uint64_t m_sourceGeneration = 1;
    uint64_t m_residencyFrame = 0;
    uint64_t m_residencyEvictions = 0;
    uint64_t m_residencyEvictedBytes = 0;
    uint64_t m_residencyOwnerRejects = 0;
    uint64_t m_residencyPinnedRejects = 0;
};

} // namespace render
} // namespace engine
