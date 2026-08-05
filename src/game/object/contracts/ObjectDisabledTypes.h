#pragma once

#include "core/container/container_types.h"

#include <cstdint>
#include <optional>

namespace engine {

// Stable disabled identity shared by authored compilers and simulation.
// String vocabulary is consumed only by the loading-side parser below.
enum class ObjectDisabledReason : uint8_t {
    Default,
    Hacked,
    Emp,
    Held,
    Paralyzed,
    Unmanned,
    Underpowered,
    Freefall,
    Awestruck,
    Brainwashed,
    Subdued,
    ScriptDisabled,
    ScriptUnderpowered,
    Count,
};

using ObjectDisabledMask = uint32_t;

[[nodiscard]] constexpr ObjectDisabledMask objectDisabledBit(
    ObjectDisabledReason reason) noexcept {
    return ObjectDisabledMask{1} << static_cast<uint8_t>(reason);
}

[[nodiscard]] constexpr ObjectDisabledMask objectDisabledKnownMask() noexcept {
    static_assert(static_cast<uint8_t>(ObjectDisabledReason::Count) < 32);
    return (ObjectDisabledMask{1} <<
            static_cast<uint8_t>(ObjectDisabledReason::Count)) - 1u;
}

[[nodiscard]] std::optional<ObjectDisabledReason>
objectDisabledReasonFromLegacyToken(container::StringView token) noexcept;

} // namespace engine
