#pragma once

#include "core/container/container_types.h"
#include "engine/renderer/world/resource/RenderAssetScheduling.h"
#include "engine/renderer/world/resource/WorldTextureDecodeService.h"

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <optional>

namespace engine::d3d12 {
class D3D12Device;
}

namespace engine::render {

// Render-owner-thread scheduler between immutable CPU decode products and
// D3D12 residency. It owns candidate lifetime, bounded admission, retries,
// ageing and upload execution; WorldTextureCache owns only the resulting
// resident-entry transaction and reference/pin policy.
class WorldTextureGpuUploadQueue final {
public:
    using Payload = WorldTextureDecodeService::Lookup::Payload;

    struct Pending final {
        container::String key;
        container::String logicalName;
        container::SharedPtr<const Payload> payload;
        uint64_t enqueueSequence = 0;
        uint32_t deferredPasses = 0;
        uint32_t attempts = 0;
        RenderAssetPriority priority = RenderAssetPriority::Normal;
    };

    struct Totals final {
        uint64_t attempts = 0;
        uint64_t deferred = 0;
        uint64_t forcedOversized = 0;
        uint64_t attemptedBytes = 0;
        uint64_t deferredBytes = 0;
        uint64_t elapsedNanoseconds = 0;
        uint32_t maximumAge = 0;
    };

    void enqueue(container::String key,
                 container::StringView logicalName,
                 container::SharedPtr<const Payload> payload,
                 RenderAssetPriority priority);
    void clear();

    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] bool contains(const container::String& key) const noexcept;
    [[nodiscard]] const Totals& totals() const noexcept;
    [[nodiscard]] uint64_t cancelled() const noexcept;

    // One render-frame admission pass. The queue owns priority selection,
    // byte/time budgets, oversized progress, retry staging, ageing and all
    // related totals. The cache receives one value candidate at a time and
    // never mutates queue storage or counters directly.
    [[nodiscard]] bool beginPass(const RenderAssetReadyBudget& budget) noexcept;
    [[nodiscard]] Pending* takeNext();
    [[nodiscard]] uint32_t uploadCurrent(d3d12::D3D12Device& device) const;
    void complete() noexcept;
    void defer();
    void finishPass();

private:
    [[nodiscard]] static uint32_t upload(
        d3d12::D3D12Device& device, const Payload& payload);
    [[nodiscard]] static bool better(
        const Pending& candidate, const Pending& current) noexcept;

    container::Deque<Pending> m_pending;
    container::Vector<Pending> m_deferredThisPass;
    std::optional<Pending> m_inFlight;
    Totals m_totals;
    uint64_t m_nextSequence = 1;
    uint64_t m_cancelled = 0;
    RenderAssetReadyBudget m_passBudget{};
    std::chrono::steady_clock::time_point m_passStarted{};
    uint64_t m_passAttemptedBytes = 0;
    size_t m_passAttempts = 0;
    bool m_passActive = false;
};

} // namespace engine::render
