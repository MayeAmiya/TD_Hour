#pragma once

#include "game/object/simulation/combat/ObjectProjectileSystem.h"
#include "game/session/transaction/GameSessionObjectLifecycleTransactions.h"

namespace engine {

class GameSessionContentStartState;
class GameSessionWorldState;

// Complete confirmed projectile creation transaction. It owns admission,
// lifecycle spawn/rollback, projectile initialization and launcher/target
// gameplay edges; callers never mutate projectile ECS state directly.
class GameSessionProjectileSpawnTransactions final {
public:
    GameSessionProjectileSpawnTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionObjectLifecycleTransactions lifecycle) noexcept;

    [[nodiscard]] bool spawn(
        const ObjectProjectileSpawnRequest& request);

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionObjectLifecycleTransactions m_lifecycle;
};

} // namespace engine
