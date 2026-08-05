#pragma once

#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/session/transaction/GameSessionTransactionPorts.h"

#include <cstdint>

namespace engine {

class GameSessionContentStartState;
class GameSessionWorldState;
class GameSessionScriptPresentationState;

// Sole Body/Health admission for script, water, containment-kill and other
// Session producers. queueObjectDamage owns the session-level liveness contract;
// resolveQueuedObjectDamage remains the singular Die/DeleteWalk barrier and
// still drains FX/tech/lifecycle through the Session sink so handler children
// stay depth-first and ordered.
class GameSessionObjectDamageTransactions final {
public:
    GameSessionObjectDamageTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionLifecycleTransactionPort barrier) noexcept;

    [[nodiscard]] bool queueObjectDamage(ObjectDamageRequest request);
    void resolveQueuedObjectDamage();
    void resolveTerrainWaterDamage();

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionLifecycleTransactionPort m_barrier;
};

} // namespace engine
