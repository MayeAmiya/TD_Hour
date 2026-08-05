#pragma once

#include <cstdint>

namespace engine::presentation {

// Player-targeted presentation policy is frozen while confirmed session state
// is available.  Renderer, FX and audio can carry this value without reading
// PlayerRegistry or any ECS object later on.
inline constexpr uint8_t kInvalidPlayerAudience = 0xffu;

enum class PlayerAudienceRelation : uint8_t {
    Unknown,
    Self,
    Ally,
    Enemy,
    Neutral,
};

struct PlayerAudience final {
    // Opaque PlayerId retained only for diagnostics/value tracing. Consumers
    // must use relation; it is the observer-relative fact already frozen by
    // the session presentation extractor.
    uint8_t sourcePlayer = kInvalidPlayerAudience;
    PlayerAudienceRelation relation = PlayerAudienceRelation::Unknown;

    [[nodiscard]] constexpr bool hasSourcePlayer() const noexcept {
        return sourcePlayer != kInvalidPlayerAudience;
    }
};

} // namespace engine::presentation
