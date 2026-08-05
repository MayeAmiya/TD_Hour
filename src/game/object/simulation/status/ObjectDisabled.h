#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/contracts/ObjectDisabledTypes.h"
#include <cstdint>
#include <limits>
namespace engine {

inline constexpr uint64_t OBJECT_DISABLED_FOREVER_TICK =
    std::numeric_limits<uint64_t>::max();

struct ObjectDisabledComponent final {
    container::Array<uint64_t, static_cast<size_t>(ObjectDisabledReason::Count)>
        untilTicks{};
    container::Array<uint64_t, static_cast<size_t>(ObjectDisabledReason::Count)>
        startedTicks{};
    uint64_t revision = 0;
    uint64_t lastChangedTick = 0;
};

struct ObjectDisabledTransition final {
    ObjectDisabledMask previous = 0;
    ObjectDisabledMask current = 0;
    ObjectDisabledMask newlySet = 0;
    ObjectDisabledMask newlyCleared = 0;

    [[nodiscard]] bool changed() const noexcept {
        return newlySet != 0 || newlyCleared != 0;
    }
};

[[nodiscard]] ObjectDisabledMask objectDisabledMask(
    const ecs::registry& registry, ecs::entity entity,
    uint64_t confirmedTick) noexcept;

[[nodiscard]] bool isObjectDisabled(
    const ecs::registry& registry, ecs::entity entity,
    uint64_t confirmedTick,
    ObjectDisabledMask reasonsAllowedToProcess = 0) noexcept;

[[nodiscard]] bool isObjectDisabledBy(
    const ecs::registry& registry, ecs::entity entity,
    ObjectDisabledReason reason, uint64_t confirmedTick) noexcept;

[[nodiscard]] uint64_t objectDisabledUntil(
    const ecs::registry& registry, ecs::entity entity,
    ObjectDisabledReason reason) noexcept;

[[nodiscard]] uint64_t objectDisabledStartedAt(
    const ecs::registry& registry, ecs::entity entity,
    ObjectDisabledReason reason) noexcept;

// One central writer for reason-specific deadlines. A replacement deadline
// may shorten as well as extend a status, matching Object::setDisabledUntil.
// A deadline equal to the current tick clears immediately.
class ObjectDisabledSystem final {
public:
    [[nodiscard]] static ObjectDisabledTransition setUntil(
        ecs::registry& registry, ecs::entity entity,
        ObjectDisabledReason reason, uint64_t untilTick,
        uint64_t confirmedTick);

    [[nodiscard]] static ObjectDisabledTransition clear(
        ecs::registry& registry, ecs::entity entity,
        ObjectDisabledReason reason, uint64_t confirmedTick);
};

} // namespace engine
