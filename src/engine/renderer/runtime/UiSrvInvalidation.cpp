#include "core/container/container_types.h"
#include "UiSrvInvalidation.h"

#include <atomic>
#include <mutex>

namespace engine::render {

namespace {

struct UiSrvInvalidationState final {
    std::mutex mutex;
    container::Vector<UiSrvInvalidation> pending;
    std::atomic<uint64_t> dropped{0};
};

[[nodiscard]] UiSrvInvalidationState& invalidationState() {
    // See the header contract: this deliberately has process lifetime.
    static UiSrvInvalidationState* state = new UiSrvInvalidationState();
    return *state;
}

} // namespace

void publishUiSrvInvalidation(
    UiSrvResourceKind kind, uint64_t identity) noexcept {
    if (identity == 0) return;
    try {
        UiSrvInvalidationState& state = invalidationState();
        const std::scoped_lock lock(state.mutex);
        state.pending.push_back({kind, identity});
    } catch (...) {
        try {
            invalidationState().dropped.fetch_add(
                1, std::memory_order_relaxed);
        } catch (...) {
            // No state exists to report the allocation failure. Monotonic
            // identity and the renderer's idle fallback still prevent alias.
        }
    }
}

container::Vector<UiSrvInvalidation> takeUiSrvInvalidations() noexcept {
    container::Vector<UiSrvInvalidation> output;
    try {
        UiSrvInvalidationState& state = invalidationState();
        const std::scoped_lock lock(state.mutex);
        output.swap(state.pending);
    } catch (...) {
        // A lock failure is not recoverable here, but retaining the queue lets
        // a later frame retry while identity uniqueness prevents aliasing.
    }
    return output;
}

uint64_t takeDroppedUiSrvInvalidationCount() noexcept {
    try {
        return invalidationState().dropped.exchange(
            0, std::memory_order_relaxed);
    } catch (...) {
        return 1;
    }
}

} // namespace engine::render
