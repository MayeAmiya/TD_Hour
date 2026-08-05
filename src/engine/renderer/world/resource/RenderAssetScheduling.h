#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace engine::render {

// Shared scheduling vocabulary only. Type-specific caches continue to own
// their queues, payloads, errors and resource lifetime.
enum class RenderAssetPriority : uint8_t {
    Background = 0,
    Preload,
    Normal,
    Visible,
    Count,
};

enum class RenderAssetPinScope : uint8_t {
    Map = 1u << 0u,
    Session = 1u << 1u,
    Debug = 1u << 2u,
};

[[nodiscard]] inline constexpr uint8_t renderAssetPinBit(
    RenderAssetPinScope scope) noexcept {
    return static_cast<uint8_t>(scope);
}

inline constexpr size_t kRenderAssetPriorityCount =
    static_cast<size_t>(RenderAssetPriority::Count);
inline constexpr uint32_t kRenderAssetAgingPassesPerPromotion = 4u;
inline constexpr uint32_t kRenderAssetOversizedProgressPasses = 8u;

[[nodiscard]] inline RenderAssetPriority sanitizeRenderAssetPriority(
    RenderAssetPriority priority) noexcept {
    return priority < RenderAssetPriority::Count
        ? priority : RenderAssetPriority::Normal;
}

[[nodiscard]] inline uint32_t effectiveRenderAssetPriority(
    RenderAssetPriority priority, uint32_t deferredPasses) noexcept {
    return std::min(
        static_cast<uint32_t>(RenderAssetPriority::Visible),
        static_cast<uint32_t>(sanitizeRenderAssetPriority(priority)) +
            deferredPasses / kRenderAssetAgingPassesPerPromotion);
}

struct RenderAssetReadyBudget final {
    size_t maxItems = std::numeric_limits<size_t>::max();
    uint64_t maxBytes = std::numeric_limits<uint64_t>::max();
    uint64_t maxElapsedMicroseconds = std::numeric_limits<uint64_t>::max();
};

} // namespace engine::render
