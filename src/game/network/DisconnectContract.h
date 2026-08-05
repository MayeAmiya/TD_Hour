#pragma once

#include "core/container/container_types.h"

#include <cstdint>

namespace engine {

// Stable logic/presentation boundary for the in-game disconnect overlay.
// Resume tickets and missing-frame ranges deliberately belong to the later
// recovery protocol, not to this product-facing state contract.
enum class DisconnectState : uint8_t {
    Connected,
    Lost,
    Reconnecting,
    Resync,
    Ready,
    Terminal,
};

enum class DisconnectAction : uint8_t {
    None,
    Reconnect,
    Cancel,
    Exit,
};

enum class DisconnectActionResult : uint8_t {
    None,
    Accepted,
    Rejected,
    Unsupported,
};

struct DisconnectStatus final {
    DisconnectState state = DisconnectState::Connected;
    container::String reason;
    // Zero means the backend did not issue a retry deadline.
    uint64_t deadlineRemainingMilliseconds = 0;
    bool retryable = false;
    uint32_t attempt = 0;

    // Action acknowledgement is revisioned independently from the outer UI
    // projection so a newest-value consumer can distinguish a fresh failure.
    uint64_t actionRevision = 0;
    DisconnectAction lastAction = DisconnectAction::None;
    DisconnectActionResult lastActionResult =
        DisconnectActionResult::None;
    container::String actionError;
};

} // namespace engine
