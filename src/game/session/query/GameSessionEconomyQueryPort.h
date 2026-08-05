#pragma once

#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/simulation/economy/ObjectProduction.h"

namespace engine {

// Read-only economy calculation capability. Callers can evaluate authored
// player costs without obtaining the registry or lifecycle owner.
class GameSessionEconomyQueryPort final {
public:
    GameSessionEconomyQueryPort(
        const ecs::registry& registry,
        const ObjectLifecycle& lifecycle) noexcept
        : m_registry(&registry), m_lifecycle(&lifecycle) {}

    [[nodiscard]] int64_t objectBuildCost(
        const game::ObjectArchetype& product,
        const PlayerState& player) const {
        return calculateObjectBuildCost(
            product, player, *m_registry, *m_lifecycle);
    }

private:
    const ecs::registry* m_registry = nullptr;
    const ObjectLifecycle* m_lifecycle = nullptr;
};

} // namespace engine
