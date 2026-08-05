#pragma once

#include <algorithm>
#include <cstdint>

namespace engine {

// A malformed legacy map can author any signed INT. Keep a deterministic,
// bounded amount of confirmed single-player work per outer/UI update so one
// bad value cannot monopolize the event loop. This is a pacing cap, not a
// change to a fixed simulation delta or to the stored W3DView-compatible
// visual multiplier.
inline constexpr uint32_t kMaximumSinglePlayerVisualSpeedFramesPerUpdate = 16;

[[nodiscard]] constexpr uint32_t visualSpeedConfirmedFrameBudget(
    int32_t multiplier, bool networkSession) noexcept {
    // Lockstep owns its confirmed-frame availability. It must never locally
    // turn one ready network frame into N frames merely because a presentation
    // script changed the tactical-view speed multiplier.
    if (networkSession || multiplier <= 1) return 1;
    return std::min<uint32_t>(static_cast<uint32_t>(multiplier),
                              kMaximumSinglePlayerVisualSpeedFramesPerUpdate);
}

} // namespace engine
