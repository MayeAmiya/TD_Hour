#pragma once

#include "game/session/ai/GameSessionAISnapshot.h"

namespace engine {

class GameSessionAIDomain;

// Read-only AI diagnostics capability. The app receives a detached digest;
// no AI runtime, ECS storage or navigation state crosses the session boundary.
class GameSessionAIQueryPort final {
public:
    explicit GameSessionAIQueryPort(
        const GameSessionAIDomain& domain) noexcept
        : m_domain(&domain) {}

    [[nodiscard]] ObjectAISimulationDigest objectSimulationDigest() const;

private:
    const GameSessionAIDomain* m_domain = nullptr;
};

} // namespace engine
