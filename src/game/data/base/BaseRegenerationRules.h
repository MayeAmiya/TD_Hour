#pragma once

#include "core/container/container_types.h"

#include "math/fixed/q32_32.h"

#include <cstdint>
namespace engine {

// Immutable-at-session-start projection of the two GlobalData values used by
// BaseRegenerateUpdate.  The percentage is converted to Q32.32 at the
// content boundary: no confirmed-frame health update depends on a mutable
// GlobalData singleton or a floating-point authoring value.
struct BaseRegenerationRules final {
    using Scalar = math::q32_32;

    // `BaseRegenHealthPercentPerSecond` is authored in percent units, so
    // `5` and `5%` both become the unit value 0.05.
    Scalar healthPercentPerSecond{};
    // `BaseRegenDelay` uses RefCode's parseDurationUnsignedInt convention:
    // authoring is milliseconds. ObjectBaseRegenerateSystem converts it to
    // confirmed ticks using the session's frozen logic rate.
    uint32_t damageDelayMilliseconds = 0;

    [[nodiscard]] bool enabled() const noexcept {
        return healthPercentPerSecond > Scalar{};
    }

    // Kept for the common rule-freeze boundary. Authored domain values are
    // preserved; only representation-equivalent normalization belongs here.
    void canonicalize() noexcept;

    [[nodiscard]] bool applyLegacyGameDataOverrides(
        container::StringView content, container::StringView sourceName,
        container::String* error = nullptr);

    // Reads the winning VFS instance of a legacy GameData source.
    // Only BaseRegenerateUpdate's global values are consumed here; physics
    // and unrelated GlobalData settings retain their own typed compilers.
    [[nodiscard]] static bool loadFromLegacyGameData(container::StringView path,
                                                      BaseRegenerationRules& rules,
                                                      container::String* error = nullptr);
};

} // namespace engine
