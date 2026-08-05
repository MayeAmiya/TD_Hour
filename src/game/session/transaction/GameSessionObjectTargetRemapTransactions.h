#pragma once

#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"

namespace engine {

class GameSessionWorldState;

// Retargets every confirmed object-to-object attack reference as one atomic
// simulation operation. Rebuild/replacement producers do not mutate three
// unrelated component families themselves.
class GameSessionObjectTargetRemapTransactions final {
public:
    explicit GameSessionObjectTargetRemapTransactions(
        ecs::registry& registry) noexcept;
    explicit GameSessionObjectTargetRemapTransactions(
        GameSessionWorldState& world) noexcept;

    void remapAttackTargets(ObjectId from, ObjectId to);

private:
    ecs::registry& m_registry;
};

} // namespace engine
