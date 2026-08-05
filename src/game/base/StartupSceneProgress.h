#pragma once

#include <cstdint>

namespace engine {

enum class StartupSceneProgressState : uint8_t {
    Inactive,
    Pending,
    Ready,
    Degraded,
    Failed,
};

// Renderer-independent value admitted back to the logic-owned Loading state.
// Counts describe one exact loading/session generation, never global caches.
struct StartupSceneProgress final {
    StartupSceneProgressState state = StartupSceneProgressState::Inactive;
    uint32_t requiredTotal = 0;
    uint32_t requiredReady = 0;
    uint32_t requiredPending = 0;
    uint32_t requiredFailed = 0;
    uint32_t optionalTotal = 0;
    uint32_t optionalReady = 0;
    uint32_t optionalPending = 0;
    uint32_t optionalDegraded = 0;

    friend bool operator==(
        const StartupSceneProgress&, const StartupSceneProgress&) = default;
};

} // namespace engine
