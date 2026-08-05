#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace engine::render {

// Renderer identities are unique for one process lifetime. Exhausting the
// 64-bit space permanently saturates the allocator at zero; zero is the
// invalid identity and is never inserted into a renderer cache. This keeps a
// theoretical wrap from aliasing a still-live descriptor without adding a
// generation field to the UI hot path.
[[nodiscard]] inline uint64_t allocateMonotonicRendererIdentity(
    std::atomic<uint64_t>& nextIdentity) noexcept {
    uint64_t current = nextIdentity.load(std::memory_order_relaxed);
    while (current != 0) {
        const uint64_t next = current == std::numeric_limits<uint64_t>::max()
            ? 0u
            : current + 1u;
        if (nextIdentity.compare_exchange_weak(
                current, next, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return current;
        }
    }
    return 0;
}

} // namespace engine::render
