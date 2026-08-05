#include "core/container/container_types.h"
#include "engine/renderer/world/resource/WorldTextureCache.h"
#include "engine/renderer/world/resource/WorldTextureDecodeService.h"
#include "engine/renderer/world/resource/WorldTextureGpuUploadQueue.h"
#include "engine/renderer/world/resource/WorldTextureIdentity.h"

#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/runtime/RenderPerformanceSettings.h"
#include "debug/debug.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <utility>
namespace engine::render {
namespace {

[[nodiscard]] uint64_t retirementIdentityHash(
    container::StringView value) noexcept {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash == 0u ? 1u : hash;
}


} // namespace


WorldTextureCache::WorldTextureCache(d3d12::D3D12Device& device)
    : m_device(&device)
    , m_decodeService(std::make_unique<WorldTextureDecodeService>())
    , m_gpuUploadQueue(std::make_unique<WorldTextureGpuUploadQueue>()) {}

WorldTextureCache::~WorldTextureCache() = default;


std::optional<uint32_t> WorldTextureCache::acquire(
    container::StringView textureName, Variant variant,
    RenderAssetPriority priority) {
    const container::String key = ordinaryTextureKey(textureName, variant);
    if (auto it = m_entries.find(key); it != m_entries.end()) {
        ++m_cacheHits;
        if (it->second.referenceCount != std::numeric_limits<uint32_t>::max()) {
            ++it->second.referenceCount;
        }
        return it->second.srvIndex;
    }

    ++m_cacheMisses;
    bool terminalCpuFailure = false;
    if (!textureName.empty() && m_device && m_decodeService) {
        const WorldTextureDecodeService::Lookup decoded =
            m_decodeService->requestOrdinary(
                textureName, sourceIdentity(textureName), key, variant,
                m_textureReductionFactor, priority);
        if (decoded.state == WorldTextureDecodeService::State::Pending) {
            return std::nullopt;
        }
        if (decoded.state == WorldTextureDecodeService::State::Ready &&
            decoded.payload) {
            m_gpuUploadQueue->enqueue(
                key, textureName, decoded.payload, priority);
            return std::nullopt;
        }
        ++m_fallbackResolutions;
        terminalCpuFailure = true;
        TD_LOG_DEBUG(
            "[WorldTextureCache] '{}' CPU preparation failed: {}; using content fallback",
            textureName, decoded.diagnostic);
    }

    Entry fallback;
    fallback.referenceCount = 1;
    fallback.terminalCpuFailure = terminalCpuFailure;
    const auto [entryIt, inserted] = m_entries.emplace(key, fallback);
    if (!inserted &&
        entryIt->second.referenceCount != std::numeric_limits<uint32_t>::max()) {
        ++entryIt->second.referenceCount;
    }
    return entryIt->second.srvIndex;
}

bool WorldTextureCache::prepare(
    container::StringView textureName, Variant variant,
    RenderAssetPriority priority) {
    const container::String key = ordinaryTextureKey(textureName, variant);
    if (m_entries.contains(key) || textureName.empty() || !m_decodeService) {
        return true;
    }
    const WorldTextureDecodeService::Lookup decoded =
        m_decodeService->requestOrdinary(
            textureName, sourceIdentity(textureName), key, variant,
            m_textureReductionFactor, priority);
    if (decoded.state == WorldTextureDecodeService::State::Ready &&
        decoded.payload) {
        m_gpuUploadQueue->enqueue(key, textureName, decoded.payload, priority);
        return false;
    }
    return decoded.state != WorldTextureDecodeService::State::Pending;
}

std::optional<WorldTextureCache::SourceDimensions>
WorldTextureCache::sourceDimensions(
    container::StringView textureName, Variant variant) const {
    const auto found = m_entries.find(ordinaryTextureKey(textureName, variant));
    if (found == m_entries.end() ||
        found->second.sourceDimensions.width == 0u ||
        found->second.sourceDimensions.height == 0u) {
        return std::nullopt;
    }
    return found->second.sourceDimensions;
}

std::optional<uint32_t> WorldTextureCache::acquireTerrainColor(
    container::StringView textureName, uint32_t sourceTileGridWidth,
    RenderAssetPriority priority) {
    const container::String key = terrainColorKey(
        textureName, sourceTileGridWidth);
    if (auto it = m_entries.find(key); it != m_entries.end()) {
        ++m_cacheHits;
        if (it->second.referenceCount != std::numeric_limits<uint32_t>::max()) {
            ++it->second.referenceCount;
        }
        return it->second.srvIndex;
    }

    ++m_cacheMisses;
    bool terminalCpuFailure = false;
    if (!textureName.empty() && m_device && m_decodeService) {
        const WorldTextureDecodeService::Lookup decoded =
            m_decodeService->requestTerrainColor(
                textureName, sourceIdentity(textureName), key,
                sourceTileGridWidth, m_textureReductionFactor, priority);
        if (decoded.state == WorldTextureDecodeService::State::Pending) {
            return std::nullopt;
        }
        if (decoded.state == WorldTextureDecodeService::State::Ready &&
            decoded.payload) {
            m_gpuUploadQueue->enqueue(
                key, textureName, decoded.payload, priority);
            return std::nullopt;
        }
        ++m_fallbackResolutions;
        terminalCpuFailure = true;
        TD_LOG_DEBUG(
            "[WorldTextureCache] '{}' terrain CPU preparation failed: {}; using content fallback",
            textureName, decoded.diagnostic);
    }

    Entry fallback;
    fallback.referenceCount = 1;
    fallback.terminalCpuFailure = terminalCpuFailure;
    const auto [entryIt, inserted] = m_entries.emplace(key, fallback);
    if (!inserted &&
        entryIt->second.referenceCount != std::numeric_limits<uint32_t>::max()) {
        ++entryIt->second.referenceCount;
    }
    return entryIt->second.srvIndex;
}

bool WorldTextureCache::prepareTerrainColor(
    container::StringView textureName, uint32_t sourceTileGridWidth,
    RenderAssetPriority priority) {
    const container::String key = terrainColorKey(
        textureName, sourceTileGridWidth);
    if (m_entries.contains(key) || textureName.empty() || !m_decodeService) {
        return true;
    }
    const WorldTextureDecodeService::Lookup decoded =
        m_decodeService->requestTerrainColor(
            textureName, sourceIdentity(textureName), key,
            sourceTileGridWidth, m_textureReductionFactor, priority);
    if (decoded.state == WorldTextureDecodeService::State::Ready &&
        decoded.payload) {
        m_gpuUploadQueue->enqueue(key, textureName, decoded.payload, priority);
        return false;
    }
    return decoded.state != WorldTextureDecodeService::State::Pending;
}

void WorldTextureCache::releaseTerrainColor(
    container::StringView textureName, uint32_t sourceTileGridWidth) {
    const container::String key = terrainColorKey(
        textureName, sourceTileGridWidth);
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) return;
    ++m_releases;

    if (it->second.referenceCount == 0u) return;
    --it->second.referenceCount;
    if (it->second.referenceCount == 0u) {
        it->second.lastOwnerReleaseFrame = m_residencyFrame;
    }
}

void WorldTextureCache::configureTextureReduction(
    uint32_t reductionFactor) {
    if (m_textureReductionFactor == reductionFactor) return;
    releaseAll();
    if (m_gpuUploadQueue) m_gpuUploadQueue->clear();
    if (m_decodeService) m_decodeService->invalidateVariants();
    m_textureReductionFactor = reductionFactor;
}

void WorldTextureCache::release(container::StringView textureName, Variant variant) {
    const container::String key = ordinaryTextureKey(textureName, variant);
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) return;
    ++m_releases;

    if (it->second.referenceCount == 0u) return;
    --it->second.referenceCount;
    if (it->second.referenceCount == 0u) {
        it->second.lastOwnerReleaseFrame = m_residencyFrame;
    }
}

bool WorldTextureCache::setPinned(
    container::StringView textureName, Variant variant,
    RenderAssetPinScope scope, bool pinned) {
    const auto found = m_entries.find(ordinaryTextureKey(textureName, variant));
    if (found == m_entries.end()) return false;
    const uint8_t bit = renderAssetPinBit(scope);
    if (pinned) found->second.residencyPinMask |= bit;
    else found->second.residencyPinMask &= static_cast<uint8_t>(~bit);
    return true;
}

void WorldTextureCache::beginResidencyFrame(uint64_t frameOrdinal) noexcept {
    m_residencyFrame = frameOrdinal;
}

size_t WorldTextureCache::trimResidency(
    size_t maximumTextures, uint64_t maximumBytes,
    uint64_t graceFrames) {
    size_t residentTextures = residentTextureCount();
    uint64_t residentBytes = residentTextureBytes();
    if (residentTextures <= maximumTextures && residentBytes <= maximumBytes) {
        return 0u;
    }

    struct ResidencyCandidate {
        container::String key;
        uint64_t lastUse = 0;
    };
    container::Vector<ResidencyCandidate> candidates;
    candidates.reserve(m_entries.size());
    // Collect eligible entries once. The previous implementation repeated
    // this full hash-table scan after every eviction, turning a pressure
    // frame with many evictions into an avoidable O(evictions * entries)
    // burst. Candidate keys are copied because erasing an entry invalidates
    // only that entry's iterator; the remaining candidates are revalidated
    // below before their fence/owner-sensitive destruction.
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        const Entry& entry = it->second;
        if (entry.srvIndex == 0u || entry.srvIndex == UINT32_MAX) continue;
        if (entry.referenceCount != 0u) {
            ++m_residencyOwnerRejects;
            continue;
        }
        if (entry.residencyPinMask != 0u) {
            ++m_residencyPinnedRejects;
            continue;
        }
        const uint64_t deviceUse = m_device
            ? m_device->srvLastUsedFrame(entry.srvIndex) : 0u;
        const uint64_t lastUse = std::max({
            entry.residentSinceFrame,
            entry.lastOwnerReleaseFrame,
            deviceUse,
        });
        if (m_residencyFrame < lastUse ||
            m_residencyFrame - lastUse < graceFrames) {
            continue;
        }
        candidates.push_back({it->first, lastUse});
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const ResidencyCandidate& left,
           const ResidencyCandidate& right) {
            return std::tie(left.lastUse, left.key) <
                std::tie(right.lastUse, right.key);
        });

    size_t evicted = 0;
    for (const ResidencyCandidate& candidate : candidates) {
        if ((residentTextures <= maximumTextures &&
             residentBytes <= maximumBytes) ||
            evicted >= performance_limits::kWorldTextureResidencyEvictionsPerFrame) {
            break;
        }
        auto found = m_entries.find(candidate.key);
        if (found == m_entries.end()) continue;
        Entry& entry = found->second;
        // Owner/pin state and device last-use can change after collection;
        // preserve the original safety contract by rechecking before free.
        if (entry.srvIndex == 0u || entry.srvIndex == UINT32_MAX ||
            entry.referenceCount != 0u || entry.residencyPinMask != 0u) {
            continue;
        }
        const uint64_t deviceUse = m_device
            ? m_device->srvLastUsedFrame(entry.srvIndex) : 0u;
        const uint64_t lastUse = std::max({
            entry.residentSinceFrame,
            entry.lastOwnerReleaseFrame,
            deviceUse,
        });
        if (m_residencyFrame < lastUse ||
            m_residencyFrame - lastUse < graceFrames) {
            continue;
        }

        const uint64_t bytes = entry.byteSize;
        if (m_device) m_device->freeTexture(entry.srvIndex);
        ++m_retiredTextures;
        ++m_residencyEvictions;
        m_residencyEvictedBytes += bytes;
        m_entries.erase(found);
        --residentTextures;
        residentBytes = residentBytes >= bytes ? residentBytes - bytes : 0u;
        ++evicted;
    }
    return evicted;
}

std::optional<uint32_t> WorldTextureCache::acquireTerrainAlphaEdge(
    container::StringView textureName, RenderAssetPriority priority) {
    const container::String key = terrainAlphaEdgeKey(textureName);
    if (auto it = m_entries.find(key); it != m_entries.end()) {
        ++m_cacheHits;
        if (it->second.referenceCount != std::numeric_limits<uint32_t>::max()) {
            ++it->second.referenceCount;
        }
        return it->second.srvIndex;
    }

    ++m_cacheMisses;
    bool terminalCpuFailure = false;
    if (!textureName.empty() && m_device && m_decodeService) {
        const WorldTextureDecodeService::Lookup decoded =
            m_decodeService->requestTerrainAlphaEdge(
                textureName, sourceIdentity(textureName), key, priority);
        if (decoded.state == WorldTextureDecodeService::State::Pending) {
            return std::nullopt;
        }
        if (decoded.state == WorldTextureDecodeService::State::Ready &&
            decoded.payload) {
            m_gpuUploadQueue->enqueue(
                key, textureName, decoded.payload, priority);
            return std::nullopt;
        }
        ++m_fallbackResolutions;
        terminalCpuFailure = true;
        TD_LOG_DEBUG(
            "[WorldTextureCache] '{}' alpha-edge CPU preparation failed: {}; using content fallback",
            textureName, decoded.diagnostic);
    }

    Entry fallback;
    fallback.referenceCount = 1;
    fallback.terminalCpuFailure = terminalCpuFailure;
    const auto [entryIt, inserted] = m_entries.emplace(key, fallback);
    if (!inserted &&
        entryIt->second.referenceCount != std::numeric_limits<uint32_t>::max()) {
        ++entryIt->second.referenceCount;
    }
    return entryIt->second.srvIndex;
}

bool WorldTextureCache::prepareTerrainAlphaEdge(
    container::StringView textureName, RenderAssetPriority priority) {
    const container::String key = terrainAlphaEdgeKey(textureName);
    if (m_entries.contains(key) || textureName.empty() || !m_decodeService) {
        return true;
    }
    const WorldTextureDecodeService::Lookup decoded =
        m_decodeService->requestTerrainAlphaEdge(
            textureName, sourceIdentity(textureName), key, priority);
    if (decoded.state == WorldTextureDecodeService::State::Ready &&
        decoded.payload) {
        m_gpuUploadQueue->enqueue(key, textureName, decoded.payload, priority);
        return false;
    }
    return decoded.state != WorldTextureDecodeService::State::Pending;
}

void WorldTextureCache::pumpCpuCompletions() {
    if (m_decodeService) m_decodeService->pumpCompletions();
}

size_t WorldTextureCache::processGpuUploads(
    const RenderAssetReadyBudget& budget) {
    if (!m_device || !m_gpuUploadQueue || budget.maxItems == 0u ||
        budget.maxBytes == 0u || budget.maxElapsedMicroseconds == 0u) {
        return 0;
    }

    if (!m_gpuUploadQueue->beginPass(budget)) return 0;
    size_t completed = 0;
    while (m_gpuUploadQueue->size() != 0u) {
        // World uploads share the device SRV heap with UI and fixed renderer
        // descriptors. Stop admission before consuming the reserved
        // headroom; the pending item remains queued for a later frame after
        // fence retirement/eviction has made space again.
        const SrvDescriptorRenderStats srvStats =
            m_device->srvDescriptorStats();
        if (srvStats.available <=
            d3d12::performance_limits::kSrvFixedDescriptorReserve) {
            if ((m_residencyFrame % 60u) == 0u) {
                TD_LOG_WARN(
                    "[WorldTextureCache] SRV headroom exhausted; deferring world texture uploads (available={} reserve={})",
                    srvStats.available,
                    d3d12::performance_limits::kSrvFixedDescriptorReserve);
            }
            break;
        }

        WorldTextureGpuUploadQueue::Pending* next =
            m_gpuUploadQueue->takeNext();
        if (!next) break;
        const WorldTextureGpuUploadQueue::Pending& pending = *next;
        auto [entryIt, inserted] = m_entries.emplace(pending.key, Entry{});
        if (!inserted) {
            if (m_decodeService) {
                m_decodeService->discardPreparedVariant(pending.key);
            }
            m_gpuUploadQueue->complete();
            continue;
        }

        uint32_t uploaded = UINT32_MAX;
        bool exceptionLogged = false;
        try {
            uploaded = m_gpuUploadQueue->uploadCurrent(*m_device);
        } catch (const std::exception& exception) {
            static_cast<void>(exception);
            exceptionLogged = true;
            if (pending.attempts == 1u || pending.attempts % 60u == 0u) {
                TD_LOG_WARN(
                    "[WorldTextureCache] GPU upload threw for '{}': {}; queued for retry",
                    pending.logicalName, exception.what());
            }
        } catch (...) {
            exceptionLogged = true;
            if (pending.attempts == 1u || pending.attempts % 60u == 0u) {
                TD_LOG_WARN(
                    "[WorldTextureCache] GPU upload threw for '{}'; queued for retry",
                    pending.logicalName);
            }
        }
        if (uploaded == UINT32_MAX) {
            m_entries.erase(entryIt);
            ++m_failedAcquisitions;
            if (!exceptionLogged &&
                (pending.attempts == 1u || pending.attempts % 60u == 0u)) {
                TD_LOG_WARN(
                    "[WorldTextureCache] Transient GPU upload failure for '{}' attempt={}; queued for retry",
                    pending.logicalName, pending.attempts);
            }
            m_gpuUploadQueue->defer();
            continue;
        }

        entryIt->second.srvIndex = uploaded;
        entryIt->second.referenceCount = 0;
        entryIt->second.byteSize = pending.payload->byteSize;
        entryIt->second.residentSinceFrame = m_residencyFrame;
        entryIt->second.sourceDimensions = {
            pending.payload->sourceWidth, pending.payload->sourceHeight};
        m_device->setSrvRetirementIdentity(uploaded, {
            .identityHash = retirementIdentityHash(pending.key),
            .generation = m_sourceGeneration,
            .revision = 1u,
        });
        if (m_decodeService) {
            m_decodeService->discardPreparedVariant(pending.key);
        }
        m_gpuUploadQueue->complete();
        ++m_gpuUploads;
        ++completed;
    }

    m_gpuUploadQueue->finishPass();
    if (m_decodeService) {
        m_decodeService->collectSourceGarbage(
            performance_limits::kWorldTextureDecodedCpuBytes,
            performance_limits::kWorldTextureCpuGcItemsPerFrame);
    }
    return completed;
}

void WorldTextureCache::releaseTerrainAlphaEdge(container::StringView textureName) {
    const container::String key = terrainAlphaEdgeKey(textureName);
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) return;
    ++m_releases;

    if (it->second.referenceCount == 0u) return;
    --it->second.referenceCount;
    if (it->second.referenceCount == 0u) {
        it->second.lastOwnerReleaseFrame = m_residencyFrame;
    }
}

void WorldTextureCache::releaseAll() {
    if (m_device) {
        for (const auto& [key, entry] : m_entries) {
            (void)key;
            if (entry.srvIndex != 0 && entry.srvIndex != UINT32_MAX) {
                m_device->freeTexture(entry.srvIndex);
                ++m_retiredTextures;
            }
        }
    }
    m_entries.clear();
}

void WorldTextureCache::resetSourceCache() {
    ++m_resets;
    ++m_sourceGeneration;
    if (m_sourceGeneration == 0u) ++m_sourceGeneration;
    releaseAll();
    if (m_gpuUploadQueue) m_gpuUploadQueue->clear();
    if (m_decodeService) m_decodeService->reset();
    m_residencyFrame = 0;
}

size_t WorldTextureCache::residentTextureCount() const noexcept {
    return static_cast<size_t>(std::count_if(
        m_entries.begin(), m_entries.end(), [](const auto& pair) {
            return pair.second.srvIndex != 0 && pair.second.srvIndex != UINT32_MAX;
        }));
}

uint64_t WorldTextureCache::residentTextureBytes() const noexcept {
    uint64_t result = 0;
    for (const auto& [key, entry] : m_entries) {
        (void)key;
        result += entry.byteSize;
    }
    return result;
}

WorldTextureCache::Stats WorldTextureCache::stats() const noexcept {
    Stats result{
        .trackedVariants = m_entries.size(),
        .residentTextures = residentTextureCount(),
        .residentBytes = residentTextureBytes(),
        .cacheHits = m_cacheHits,
        .cacheMisses = m_cacheMisses,
        .gpuUploads = m_gpuUploads,
        .fallbackResolutions = m_fallbackResolutions,
        .failedAcquisitions = m_failedAcquisitions,
        .releases = m_releases,
        .retiredTextures = m_retiredTextures,
        .resets = m_resets,
    };
    result.residencyEvictions = m_residencyEvictions;
    result.residencyEvictedBytes = m_residencyEvictedBytes;
    result.residencyOwnerRejects = m_residencyOwnerRejects;
    result.residencyPinnedRejects = m_residencyPinnedRejects;
    for (const auto& [key, entry] : m_entries) {
        static_cast<void>(key);
        result.ownerReferences += entry.referenceCount;
        if (entry.residencyPinMask != 0u) ++result.residencyPins;
        if (m_device && entry.srvIndex != 0u &&
            entry.srvIndex != UINT32_MAX) {
            result.latestUsedFrame = std::max(
                result.latestUsedFrame,
                m_device->srvLastUsedFrame(entry.srvIndex));
            result.latestUsedFence = std::max(
                result.latestUsedFence,
                m_device->srvLastUsedFence(entry.srvIndex));
        }
        if (entry.srvIndex == 0u) {
            ++result.fallbackEntries;
            if (entry.terminalCpuFailure) {
                ++result.terminalFailureFallbackEntries;
            }
        }
    }
    if (m_decodeService) {
        const TextureManagerStats source = m_decodeService->stats();
        const WorldTextureDecodeService::Diagnostics diagnostics =
            m_decodeService->diagnostics();
        result.decodedSources = source.decodedSources;
        result.proceduralSources = source.proceduralSources;
        result.negativeSourceLookups = source.negativeLookups;
        result.sourceCpuBytes = source.cpuBytes;
        result.sourceRequests = source.requests;
        result.sourceCacheHits = source.cacheHits;
        result.sourceCacheMisses = source.cacheMisses;
        result.sourceDecodeAttempts = source.decodeAttempts;
        result.sourceDecodeSucceeded = source.decodeSucceeded;
        result.sourceDecodeFailed = source.decodeFailed;
        result.sourceMissing = source.missingSources;
        result.sourceUnsupported = source.unsupportedSources;
        result.sourceResets = source.resets;
        result.sourceQueuedJobs = diagnostics.queuedJobs;
        result.sourceActiveJobs = diagnostics.activeJobs;
        result.sourcePendingSources = diagnostics.pendingSources;
        result.sourceActiveSourceJobs = diagnostics.activeSourceJobs;
        result.sourcePendingVariants = diagnostics.pendingVariants;
        result.sourceActiveVariantJobs = diagnostics.activeVariantJobs;
        result.sourcePreparedVariants = diagnostics.preparedVariants;
        result.sourceFailedVariants = diagnostics.failedVariants;
        result.sourceStaleCompletions = diagnostics.staleCompletions;
        result.sourceCompletedCpuJobs = diagnostics.completedCpuJobs;
        result.sourcePreparedBytes = diagnostics.preparedBytes;
        result.sourceWorkerNanoseconds = diagnostics.workerNanoseconds;
        result.sourceCancelledVariants = diagnostics.cancelledVariants;
        result.sourceCancelledReady = diagnostics.cancelledReady;
        result.sourceCancelRequestedActive =
            diagnostics.cancelRequestedActive;
        result.sourceMaximumQueueAge = diagnostics.maximumQueueAge;
        result.sourceRetainedPreparedBytes =
            diagnostics.retainedPreparedBytes;
        result.sourceReclaimedPreparedBytes =
            diagnostics.reclaimedPreparedBytes;
        result.sourceReclaimedBytes = diagnostics.reclaimedSourceBytes;
        result.sourceReclaimedCount = diagnostics.reclaimedSources;
    }
    if (m_gpuUploadQueue) {
        const WorldTextureGpuUploadQueue::Totals& totals =
            m_gpuUploadQueue->totals();
        result.queuedGpuUploads = m_gpuUploadQueue->size();
        result.gpuUploadAttempts = totals.attempts;
        result.gpuUploadDeferred = totals.deferred;
        result.gpuUploadForcedOversized = totals.forcedOversized;
        result.gpuUploadAttemptedBytes = totals.attemptedBytes;
        result.gpuUploadDeferredBytes = totals.deferredBytes;
        result.gpuUploadNanoseconds = totals.elapsedNanoseconds;
        result.gpuUploadCancelled = m_gpuUploadQueue->cancelled();
        result.gpuUploadMaximumAge = totals.maximumAge;
    }
    return result;
}

std::optional<RenderAssetLifecycleRecord>
WorldTextureCache::describeLifecycle(
    container::StringView textureName, Variant variant) const {
    return describeEntryLifecycle(
        ordinaryTextureKey(textureName, variant), textureName,
        variant == Variant::ColorLegacyGamma
            ? "color-legacy-gamma" : "data-linear");
}

std::optional<RenderAssetLifecycleRecord>
WorldTextureCache::describeTerrainColorLifecycle(
    container::StringView textureName,
    uint32_t sourceTileGridWidth) const {
    return describeEntryLifecycle(
        terrainColorKey(textureName, sourceTileGridWidth), textureName,
        "terrain-color:grid=" + std::to_string(sourceTileGridWidth));
}

std::optional<RenderAssetLifecycleRecord>
WorldTextureCache::describeTerrainAlphaEdgeLifecycle(
    container::StringView textureName) const {
    return describeEntryLifecycle(
        terrainAlphaEdgeKey(textureName), textureName,
        "terrain-alpha-edge");
}

std::optional<RenderAssetLifecycleRecord>
WorldTextureCache::describeEntryLifecycle(
    const container::String& key,
    container::StringView logicalName,
    container::String variant) const {
    const auto found = m_entries.find(key);
    const std::optional<WorldTextureDecodeService::Phase> cpuPhase =
        found == m_entries.end() && m_decodeService
        ? m_decodeService->phase(key)
        : std::nullopt;
    if (found == m_entries.end() && !cpuPhase) return std::nullopt;

    RenderAssetLifecycleRecord result;
    result.identity.kind = RenderAssetKind::TextureVariant;
    result.identity.logicalName = container::String(logicalName);
    result.identity.canonicalSource = sourceIdentity(logicalName);
    result.identity.variant = std::move(variant);
    result.identity.generation = m_sourceGeneration;
    result.identity.revision = 1u;
    if (found == m_entries.end() && m_gpuUploadQueue &&
        m_gpuUploadQueue->contains(key)) {
        result.state = RenderAssetLifecycleState::GpuQueued;
    } else if (found == m_entries.end()) {
        switch (*cpuPhase) {
        case WorldTextureDecodeService::Phase::Queued:
            result.state = RenderAssetLifecycleState::IoQueued;
            break;
        case WorldTextureDecodeService::Phase::Active:
            result.state = RenderAssetLifecycleState::IoInFlight;
            break;
        case WorldTextureDecodeService::Phase::Ready:
            result.state = RenderAssetLifecycleState::CpuReady;
            break;
        case WorldTextureDecodeService::Phase::Failed:
            result.state = RenderAssetLifecycleState::Failed;
            result.errorKind = RenderAssetErrorKind::Decode;
            result.diagnostic = "texture CPU preparation failed";
            break;
        }
    } else if (found->second.srvIndex == 0u) {
        result.ownerReferences = found->second.referenceCount;
        result.state = RenderAssetLifecycleState::Fallback;
        result.errorKind = RenderAssetErrorKind::Resolve;
        result.diagnostic = "white content fallback";
    } else if (found->second.srvIndex == UINT32_MAX) {
        result.ownerReferences = found->second.referenceCount;
        result.state = RenderAssetLifecycleState::Failed;
        result.errorKind = RenderAssetErrorKind::Upload;
        result.diagnostic = "invalid GPU descriptor";
    } else {
        result.ownerReferences = found->second.referenceCount;
        result.state = RenderAssetLifecycleState::GpuResident;
    }
    return result;
}

container::String WorldTextureCache::sourceIdentity(
    container::StringView textureName) const {
    // Canonical VFS resolution performs IO/index construction and belongs to
    // the async decoder. The render thread only needs a stable lexical key for
    // same-generation request de-duplication.
    container::String identity =
        detail::canonicalWorldTextureIdentity(textureName);
    if (identity.empty()) identity = "__white";
    return identity;
}

container::String WorldTextureCache::ordinaryTextureKey(
    container::StringView textureName, Variant variant) const {
    const container::String identity = sourceIdentity(textureName);
    // Length-prefix the source instead of relying on a delimiter that can be
    // present in a legal VFS path. Keep mip selection in the GPU variant: a
    // source decoded once may legitimately own several immutable GPU ranges.
    return "world_texture:v2;kind=ordinary;transfer=" +
        container::String(variant == Variant::ColorLegacyGamma
                              ? "legacy-gamma" : "linear") +
        ";mip_reduction=" + std::to_string(m_textureReductionFactor) +
        ";source=" + std::to_string(identity.size()) + ":" + identity;
}

container::String WorldTextureCache::terrainAlphaEdgeKey(container::StringView textureName) const {
    const container::String identity = sourceIdentity(textureName);
    // Alpha-edge generation ignores authored mips and texture reduction, so
    // those policies intentionally do not participate in this variant.
    return "world_texture:v2;kind=terrain_alpha_edge;transfer=linear;source=" +
        std::to_string(identity.size()) + ":" + identity;
}

container::String WorldTextureCache::terrainColorKey(
    container::StringView textureName,
    uint32_t sourceTileGridWidth) const {
    const container::String identity = sourceIdentity(textureName);
    return "world_texture:v2;kind=terrain_color;transfer=legacy-gamma;mips=3;grid=" +
        std::to_string(sourceTileGridWidth) + ";mip_reduction=" +
        std::to_string(m_textureReductionFactor) + ";source=" +
        std::to_string(identity.size()) + ":" + identity;
}

} // namespace engine::render
