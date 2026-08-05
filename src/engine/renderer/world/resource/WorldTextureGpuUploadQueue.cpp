#include "engine/renderer/world/resource/WorldTextureGpuUploadQueue.h"

#include "engine/renderer/d3d12/runtime/D3D12Device.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace engine::render {

void WorldTextureGpuUploadQueue::enqueue(
    container::String key,
    container::StringView logicalName,
    container::SharedPtr<const Payload> payload,
    RenderAssetPriority priority) {
    if (!payload) return;
    priority = sanitizeRenderAssetPriority(priority);
    for (Pending& pending : m_pending) {
        if (pending.key != key) continue;
        pending.priority = std::max(pending.priority, priority);
        pending.payload = std::move(payload);
        return;
    }
    m_pending.push_back({
        .key = std::move(key),
        .logicalName = container::String(logicalName),
        .payload = std::move(payload),
        .enqueueSequence = m_nextSequence++,
        .priority = priority,
    });
}

void WorldTextureGpuUploadQueue::clear() {
    m_cancelled += m_pending.size() + m_deferredThisPass.size() +
        (m_inFlight ? 1u : 0u);
    m_pending.clear();
    m_deferredThisPass.clear();
    m_inFlight.reset();
    m_passActive = false;
    m_passBudget = {};
    m_passAttemptedBytes = 0;
    m_passAttempts = 0;
}

size_t WorldTextureGpuUploadQueue::size() const noexcept {
    return m_pending.size() + m_deferredThisPass.size() +
        (m_inFlight ? 1u : 0u);
}

bool WorldTextureGpuUploadQueue::contains(
    const container::String& key) const noexcept {
    if (m_inFlight && m_inFlight->key == key) return true;
    if (std::any_of(
        m_pending.begin(), m_pending.end(),
        [&key](const Pending& pending) { return pending.key == key; })) {
        return true;
    }
    return std::any_of(
        m_deferredThisPass.begin(), m_deferredThisPass.end(),
        [&key](const Pending& pending) { return pending.key == key; });
}

const WorldTextureGpuUploadQueue::Totals&
WorldTextureGpuUploadQueue::totals() const noexcept {
    return m_totals;
}

uint64_t WorldTextureGpuUploadQueue::cancelled() const noexcept {
    return m_cancelled;
}

bool WorldTextureGpuUploadQueue::beginPass(
    const RenderAssetReadyBudget& budget) noexcept {
    if (m_passActive || budget.maxItems == 0u || budget.maxBytes == 0u ||
        budget.maxElapsedMicroseconds == 0u) {
        return false;
    }
    m_passBudget = budget;
    m_passStarted = std::chrono::steady_clock::now();
    m_passAttemptedBytes = 0;
    m_passAttempts = 0;
    m_deferredThisPass.clear();
    m_passActive = true;
    return true;
}

bool WorldTextureGpuUploadQueue::better(
    const Pending& candidate, const Pending& current) noexcept {
    const uint32_t candidatePriority = effectiveRenderAssetPriority(
        candidate.priority, candidate.deferredPasses);
    const uint32_t currentPriority = effectiveRenderAssetPriority(
        current.priority, current.deferredPasses);
    if (candidatePriority != currentPriority) {
        return candidatePriority > currentPriority;
    }
    if (candidate.deferredPasses != current.deferredPasses) {
        return candidate.deferredPasses > current.deferredPasses;
    }
    if (candidate.payload->byteSize != current.payload->byteSize) {
        return candidate.payload->byteSize < current.payload->byteSize;
    }
    return candidate.enqueueSequence < current.enqueueSequence;
}

WorldTextureGpuUploadQueue::Pending*
WorldTextureGpuUploadQueue::takeNext() {
    if (!m_passActive || m_inFlight || m_pending.empty() ||
        m_passAttempts >= m_passBudget.maxItems) {
        return nullptr;
    }
    const uint64_t elapsedMicroseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - m_passStarted).count());
    if (m_passAttempts != 0u &&
        elapsedMicroseconds >= m_passBudget.maxElapsedMicroseconds) {
        return nullptr;
    }

    const uint64_t remainingBytes =
        m_passAttemptedBytes < m_passBudget.maxBytes
        ? m_passBudget.maxBytes - m_passAttemptedBytes : 0u;
    size_t selected = m_pending.size();
    size_t oversized = m_pending.size();
    for (size_t index = 0; index < m_pending.size(); ++index) {
        const Pending& pending = m_pending[index];
        if (!pending.payload) continue;
        const uint64_t bytes = pending.payload->byteSize;
        if (bytes <= remainingBytes &&
            (selected == m_pending.size() ||
             better(pending, m_pending[selected]))) {
            selected = index;
        }
        if (bytes > m_passBudget.maxBytes &&
            pending.deferredPasses >= kRenderAssetOversizedProgressPasses &&
            (oversized == m_pending.size() ||
             better(pending, m_pending[oversized]))) {
            oversized = index;
        }
    }
    bool forcedOversized = false;
    if (m_passAttempts == 0u && oversized != m_pending.size()) {
        selected = oversized;
        forcedOversized = true;
    }
    if (selected == m_pending.size()) return nullptr;

    m_inFlight.emplace(std::move(m_pending[selected]));
    m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(selected));
    ++m_inFlight->attempts;
    ++m_passAttempts;
    ++m_totals.attempts;
    m_passAttemptedBytes += m_inFlight->payload->byteSize;
    m_totals.attemptedBytes += m_inFlight->payload->byteSize;
    if (forcedOversized) ++m_totals.forcedOversized;
    return &*m_inFlight;
}

void WorldTextureGpuUploadQueue::complete() noexcept {
    m_inFlight.reset();
}

uint32_t WorldTextureGpuUploadQueue::uploadCurrent(
    d3d12::D3D12Device& device) const {
    return m_inFlight && m_inFlight->payload
        ? upload(device, *m_inFlight->payload) : UINT32_MAX;
}

void WorldTextureGpuUploadQueue::defer() {
    if (!m_inFlight) return;
    m_deferredThisPass.push_back(std::move(*m_inFlight));
    m_inFlight.reset();
}

void WorldTextureGpuUploadQueue::finishPass() {
    if (!m_passActive) return;
    if (m_inFlight) defer();
    for (Pending& pending : m_deferredThisPass) {
        m_pending.push_back(std::move(pending));
    }
    m_deferredThisPass.clear();
    for (Pending& pending : m_pending) {
        if (pending.deferredPasses != std::numeric_limits<uint32_t>::max()) {
            ++pending.deferredPasses;
        }
        ++m_totals.deferred;
        m_totals.deferredBytes += pending.payload
            ? pending.payload->byteSize : 0u;
        m_totals.maximumAge = std::max(
            m_totals.maximumAge, pending.deferredPasses);
    }
    m_totals.elapsedNanoseconds += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - m_passStarted).count());
    m_passBudget = {};
    m_passAttemptedBytes = 0;
    m_passAttempts = 0;
    m_passActive = false;
}

uint32_t WorldTextureGpuUploadQueue::upload(
    d3d12::D3D12Device& device, const Payload& payload) {
    if (!payload.pixels || payload.mips.empty() || payload.width == 0u ||
        payload.height == 0u || payload.format == DXGI_FORMAT_UNKNOWN) {
        return UINT32_MAX;
    }
    container::Vector<d3d12::TextureSubresourceUpload> subresources;
    subresources.reserve(payload.mips.size());
    for (const Payload::Mip& mip : payload.mips) {
        if (mip.byteOffset > payload.pixels->size() ||
            mip.slicePitch > payload.pixels->size() - mip.byteOffset) {
            return UINT32_MAX;
        }
        subresources.push_back({
            .data = payload.pixels->data() + mip.byteOffset,
            .rowPitch = mip.rowPitch,
            .slicePitch = mip.slicePitch,
        });
    }
    return device.uploadTexture2D(
        payload.width, payload.height, payload.format, subresources);
}

} // namespace engine::render
