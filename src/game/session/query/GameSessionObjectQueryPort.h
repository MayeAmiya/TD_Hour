#pragma once

#include "game/object/contracts/ObjectLifecycle.h"

#include <optional>

namespace engine {

// Read-only object identity/liveness capability. It deliberately does not
// expose the lifecycle owner, allocator, retirement queues or registry.
class GameSessionObjectQueryPort final {
public:
    explicit GameSessionObjectQueryPort(
        const ObjectLifecycle& lifecycle) noexcept
        : m_lifecycle(&lifecycle) {}

    [[nodiscard]] std::optional<ecs::entity> entity(
        ObjectId object) const noexcept {
        return m_lifecycle->entityFromId(object);
    }
    [[nodiscard]] std::optional<ecs::entity> entityIncludingPending(
        ObjectId object) const noexcept {
        return m_lifecycle->entityFromIdIncludingPending(object);
    }
    [[nodiscard]] bool pendingDestroy(ObjectId object) const noexcept {
        return m_lifecycle->isPendingDestroy(object);
    }
    [[nodiscard]] bool live(ObjectId object) const noexcept {
        return object && entity(object).has_value() &&
            !pendingDestroy(object);
    }

private:
    const ObjectLifecycle* m_lifecycle = nullptr;
};

} // namespace engine
