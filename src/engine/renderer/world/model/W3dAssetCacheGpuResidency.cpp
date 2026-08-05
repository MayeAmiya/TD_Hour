#include "engine/renderer/world/model/W3dAssetCache.h"
#include "engine/renderer/runtime/RenderPerformanceSettings.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iterator>
#include <tuple>
#include <utility>

namespace engine::render {
namespace {

uint64_t saturatedAdd(uint64_t lhs, uint64_t rhs) noexcept {
    return rhs > std::numeric_limits<uint64_t>::max() - lhs
        ? std::numeric_limits<uint64_t>::max()
        : lhs + rhs;
}

size_t priorityIndex(W3dGpuUploadPriority priority) noexcept {
    return std::min(static_cast<size_t>(priority),
                    kW3dGpuUploadPriorityCount - 1u);
}

W3dGpuUploadPriority sanitizedPriority(
    W3dGpuUploadPriority priority) noexcept {
    return priority < W3dGpuUploadPriority::Count
        ? priority : W3dGpuUploadPriority::Normal;
}

void accumulateUploadStats(W3dGpuUploadBatchStats& total,
                           const W3dGpuUploadBatchStats& batch) noexcept {
    total.attemptedUploads += batch.attemptedUploads;
    total.succeededUploads += batch.succeededUploads;
    total.failedUploads += batch.failedUploads;
    total.deferredUploads += batch.deferredUploads;
    total.discardedStaleUploads += batch.discardedStaleUploads;
    total.forcedOversizedUploads += batch.forcedOversizedUploads;
    total.attemptedEstimatedBytes = saturatedAdd(
        total.attemptedEstimatedBytes, batch.attemptedEstimatedBytes);
    total.deferredEstimatedBytes = saturatedAdd(
        total.deferredEstimatedBytes, batch.deferredEstimatedBytes);
    total.attemptedElapsedNanoseconds = saturatedAdd(
        total.attemptedElapsedNanoseconds,
        batch.attemptedElapsedNanoseconds);
    total.elapsedNanoseconds = saturatedAdd(
        total.elapsedNanoseconds, batch.elapsedNanoseconds);
    for (size_t index = 0; index < kW3dGpuUploadPriorityCount; ++index) {
        total.attemptedByPriority[index] += batch.attemptedByPriority[index];
        total.deferredByPriority[index] += batch.deferredByPriority[index];
    }
}

} // namespace

bool W3dAssetCache::queueGpuUpload(
    W3dModelHandle handle, W3dGpuUploadPriority priority) {
    Slot* slot = findSlot(handle);
    if (!slot || !slot->cpuModel) return false;
    slot->lastReachableFrame = m_residencyFrame;
    priority = sanitizedPriority(priority);
    if (slot->state == W3dAssetState::UploadQueued) {
        for (PendingUpload& pending : m_uploadQueue) {
            if (pending.handle == handle && pending.revision == slot->revision) {
                pending.priority = std::max(pending.priority, priority);
                break;
            }
        }
        return false;
    }
    if (slot->state == W3dAssetState::Uploading ||
        slot->state == W3dAssetState::GpuReady) {
        return false;
    }

    slot->state = W3dAssetState::UploadQueued;
    slot->error.clear();
    slot->errorKind = RenderAssetErrorKind::None;
    const uint64_t sequence = m_nextUploadSequence++;
    if (m_nextUploadSequence == 0) m_nextUploadSequence = 1;
    m_uploadQueue.push_back({
        .handle = handle,
        .revision = slot->revision,
        .estimatedBytes = estimateW3dGpuUploadBytes(*slot->cpuModel),
        .enqueueSequence = sequence,
        .priority = priority,
    });
    return true;
}

void W3dAssetCache::notifyBeginFrameComplete() noexcept {
    m_uploadWindowOpen = true;
}

void W3dAssetCache::notifyFrameSubmitted() noexcept {
    m_uploadWindowOpen = false;
}

bool W3dAssetCache::gpuUploadWindowOpen() const noexcept {
    return m_uploadWindowOpen;
}

size_t W3dAssetCache::processGpuUploads(
    const UploadFunction& upload, size_t maxUploads) {
    return processGpuUploads(upload, W3dGpuUploadBudget{
        .maxUploads = maxUploads,
    });
}

size_t W3dAssetCache::processGpuUploads(
    const UploadFunction& upload, const W3dGpuUploadBudget& budget) {
    m_lastUploadBatch = {};
    container::Vector<PendingUpload> deferredThisPass;
    if (!m_uploadWindowOpen || !upload || budget.maxUploads == 0 ||
        budget.maxEstimatedBytes == 0 || budget.maxElapsedMicroseconds == 0) {
        return 0;
    }

    for (size_t index = 0; index < m_uploadQueue.size();) {
        const PendingUpload& pending = m_uploadQueue[index];
        const Slot* slot = findSlot(pending.handle);
        if (!slot || slot->revision != pending.revision ||
            slot->state != W3dAssetState::UploadQueued || !slot->cpuModel) {
            m_uploadQueue.erase(m_uploadQueue.begin() +
                                static_cast<std::ptrdiff_t>(index));
            ++m_lastUploadBatch.discardedStaleUploads;
        } else {
            ++index;
        }
    }

    const auto started = std::chrono::steady_clock::now();
    uint64_t attemptedBytes = 0;
    const auto betterCandidate = [](const PendingUpload& candidate,
                                    const PendingUpload& current) noexcept {
        const uint32_t candidateEffective = effectiveRenderAssetPriority(
            candidate.priority, candidate.deferredPasses);
        const uint32_t currentEffective = effectiveRenderAssetPriority(
            current.priority, current.deferredPasses);
        if (candidateEffective != currentEffective)
            return candidateEffective > currentEffective;
        if (candidate.deferredPasses != current.deferredPasses)
            return candidate.deferredPasses > current.deferredPasses;
        return candidate.enqueueSequence < current.enqueueSequence;
    };

    while (m_lastUploadBatch.attemptedUploads < budget.maxUploads &&
           !m_uploadQueue.empty()) {
        if (m_lastUploadBatch.attemptedUploads != 0) {
            const uint64_t wallMicroseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count());
            if (wallMicroseconds >= budget.maxElapsedMicroseconds) break;
        }

        const uint64_t remainingBytes = attemptedBytes <
                budget.maxEstimatedBytes
            ? budget.maxEstimatedBytes - attemptedBytes : 0;
        size_t fittingIndex = m_uploadQueue.size();
        size_t starvedOversizedIndex = m_uploadQueue.size();
        size_t anyIndex = m_uploadQueue.size();
        for (size_t index = 0; index < m_uploadQueue.size(); ++index) {
            const PendingUpload& pending = m_uploadQueue[index];
            if (anyIndex == m_uploadQueue.size() ||
                betterCandidate(pending, m_uploadQueue[anyIndex])) {
                anyIndex = index;
            }
            if (pending.estimatedBytes <= remainingBytes &&
                (fittingIndex == m_uploadQueue.size() ||
                 betterCandidate(pending, m_uploadQueue[fittingIndex]))) {
                fittingIndex = index;
            }
            if (pending.estimatedBytes > budget.maxEstimatedBytes &&
                pending.deferredPasses >=
                    kRenderAssetOversizedProgressPasses &&
                (starvedOversizedIndex == m_uploadQueue.size() ||
                 betterCandidate(
                     pending, m_uploadQueue[starvedOversizedIndex]))) {
                starvedOversizedIndex = index;
            }
        }

        size_t selectedIndex = fittingIndex;
        bool forcedOversized = false;
        if (m_lastUploadBatch.attemptedUploads == 0 &&
            starvedOversizedIndex != m_uploadQueue.size()) {
            selectedIndex = starvedOversizedIndex;
            forcedOversized = true;
        } else if (selectedIndex == m_uploadQueue.size() &&
                   m_lastUploadBatch.attemptedUploads == 0) {
            selectedIndex = anyIndex;
            forcedOversized = selectedIndex != m_uploadQueue.size();
        }
        if (selectedIndex == m_uploadQueue.size()) break;

        PendingUpload pending = m_uploadQueue[selectedIndex];
        m_uploadQueue.erase(m_uploadQueue.begin() +
                            static_cast<std::ptrdiff_t>(selectedIndex));
        Slot* slot = findSlot(pending.handle);
        if (!slot || slot->revision != pending.revision ||
            slot->state != W3dAssetState::UploadQueued || !slot->cpuModel) {
            ++m_lastUploadBatch.discardedStaleUploads;
            continue;
        }

        slot->state = W3dAssetState::Uploading;
        W3dGpuUploadRequest uploadRequest{
            .handle = pending.handle,
            .revision = pending.revision,
            .cpuModel = slot->cpuModel,
            .estimatedBytes = pending.estimatedBytes,
            .priority = pending.priority,
        };
        W3dGpuUploadResult result;
        const auto attemptStarted = std::chrono::steady_clock::now();
        try {
            result = upload(uploadRequest);
        } catch (const std::exception& exception) {
            result.error = container::String("W3D GPU upload threw: ") +
                exception.what();
        } catch (...) {
            result.error = "W3D GPU upload threw an unknown exception";
        }
        m_lastUploadBatch.attemptedElapsedNanoseconds = saturatedAdd(
            m_lastUploadBatch.attemptedElapsedNanoseconds,
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - attemptStarted)
                    .count()));

        ++m_lastUploadBatch.attemptedUploads;
        m_lastUploadBatch.attemptedEstimatedBytes = saturatedAdd(
            m_lastUploadBatch.attemptedEstimatedBytes,
            pending.estimatedBytes);
        attemptedBytes = saturatedAdd(attemptedBytes, pending.estimatedBytes);
        ++m_lastUploadBatch.attemptedByPriority[
            priorityIndex(pending.priority)];
        if (forcedOversized) ++m_lastUploadBatch.forcedOversizedUploads;

        slot = findSlot(pending.handle);
        if (!slot || slot->revision != pending.revision) {
            retireGpuModel(std::move(result.model));
            continue;
        }
        if (result.model) {
            retireGpuModel(std::move(slot->gpuModel));
            slot->gpuModel = std::move(result.model);
            slot->state = W3dAssetState::GpuReady;
            slot->residentSinceFrame = m_residencyFrame;
            slot->error.clear();
            slot->errorKind = RenderAssetErrorKind::None;
            ++m_lastUploadBatch.succeededUploads;
        } else if (result.deferred) {
            slot->state = W3dAssetState::UploadQueued;
            slot->error.clear();
            slot->errorKind = RenderAssetErrorKind::None;
            deferredThisPass.push_back(std::move(pending));
            continue;
        } else {
            slot->state = W3dAssetState::GpuUploadFailed;
            slot->error = result.error.empty()
                ? "W3D GPU upload returned no model" : std::move(result.error);
            slot->errorKind = RenderAssetErrorKind::Upload;
            ++m_lastUploadBatch.failedUploads;
        }
    }

    m_uploadQueue.insert(m_uploadQueue.end(),
                         std::make_move_iterator(deferredThisPass.begin()),
                         std::make_move_iterator(deferredThisPass.end()));
    for (PendingUpload& pending : m_uploadQueue) {
        if (pending.deferredPasses != std::numeric_limits<uint32_t>::max())
            ++pending.deferredPasses;
        ++m_lastUploadBatch.deferredUploads;
        m_lastUploadBatch.deferredEstimatedBytes = saturatedAdd(
            m_lastUploadBatch.deferredEstimatedBytes,
            pending.estimatedBytes);
        ++m_lastUploadBatch.deferredByPriority[
            priorityIndex(pending.priority)];
    }
    m_lastUploadBatch.elapsedNanoseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    accumulateUploadStats(m_lifetimeUploadTotals, m_lastUploadBatch);
    return m_lastUploadBatch.attemptedUploads;
}

void W3dAssetCache::setRetireFunction(RetireFunction retire) {
    m_retire = std::move(retire);
}

bool W3dAssetCache::setGpuResidencyPinned(
    W3dModelHandle handle, RenderAssetPinScope scope,
    bool pinned) noexcept {
    Slot* slot = findSlot(handle);
    if (!slot) return false;
    const uint8_t bit = renderAssetPinBit(scope);
    if (pinned) slot->residencyPinMask |= bit;
    else slot->residencyPinMask &= static_cast<uint8_t>(~bit);
    return true;
}

void W3dAssetCache::beginResidencyFrame(uint64_t frameOrdinal) noexcept {
    m_residencyFrame = frameOrdinal;
}

size_t W3dAssetCache::trimGpuResidency(
    size_t maximumModels, uint64_t maximumBytes,
    uint64_t graceFrames) {
    size_t residentModels = 0;
    uint64_t residentBytes = 0;
    for (const Slot& slot : m_slots) {
        if (!slot.occupied || !slot.gpuModel) continue;
        ++residentModels;
        residentBytes += slot.gpuModel->residentBytes();
    }

    size_t evicted = 0;
    while ((residentModels > maximumModels || residentBytes > maximumBytes) &&
           evicted < performance_limits::kW3dResidencyEvictionsPerFrame) {
        size_t candidate = m_slots.size();
        uint64_t candidateLastUse = std::numeric_limits<uint64_t>::max();
        uint64_t candidateBytes = 0;
        for (size_t index = 0; index < m_slots.size(); ++index) {
            const Slot& slot = m_slots[index];
            if (!slot.occupied || slot.state != W3dAssetState::GpuReady ||
                !slot.gpuModel) {
                continue;
            }
            if (slot.residencyPinMask != 0u) {
                ++m_gpuResidencyPinnedRejects;
                continue;
            }
            if (slot.gpuModel.use_count() != 1) {
                ++m_gpuResidencyReferencedRejects;
                continue;
            }
            const uint64_t lastUse = std::max({
                slot.residentSinceFrame,
                slot.lastReachableFrame,
                slot.gpuModel->lastUsedFrame(),
            });
            if (m_residencyFrame < lastUse ||
                m_residencyFrame - lastUse < graceFrames) {
                continue;
            }
            const uint64_t bytes = slot.gpuModel->residentBytes();
            if (candidate == m_slots.size() ||
                std::tie(lastUse, slot.key.sourcePath, slot.key.prototype) <
                    std::tie(candidateLastUse,
                             m_slots[candidate].key.sourcePath,
                             m_slots[candidate].key.prototype)) {
                candidate = index;
                candidateLastUse = lastUse;
                candidateBytes = bytes;
            }
        }
        if (candidate == m_slots.size()) break;

        Slot& slot = m_slots[candidate];
        retireGpuModel(std::move(slot.gpuModel));
        slot.state = slot.cpuModel
            ? W3dAssetState::CpuReady : W3dAssetState::Failed;
        slot.residentSinceFrame = 0;
        ++m_gpuResidencyEvictions;
        m_gpuResidencyEvictedBytes += candidateBytes;
        --residentModels;
        residentBytes = residentBytes >= candidateBytes
            ? residentBytes - candidateBytes : 0u;
        ++evicted;
    }
    return evicted;
}

} // namespace engine::render
