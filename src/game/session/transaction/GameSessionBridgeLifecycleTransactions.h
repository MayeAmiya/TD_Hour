#pragma once

#include "game/session/transaction/GameSessionObjectDamageTransactions.h"

#include <cstdint>

namespace engine {

class GameSessionContentStartState;
class GameSessionWorldState;
class GameSessionScriptPresentationState;

class GameSessionBridgeLifecycleTransactions final {
public:
    GameSessionBridgeLifecycleTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionLifecycleTransactionPort lifecycle,
        GameSessionObjectDamageTransactions damage) noexcept;

    [[nodiscard]] bool collapseTerrainSurface(
        uint64_t sourceRecordIndex, uint64_t confirmedTick,
        bool permanentlyRemove = true);
    [[nodiscard]] bool createScaffolding(
        ObjectId bridge, uint64_t confirmedTick);
    [[nodiscard]] bool removeScaffolding(
        ObjectId bridge, uint64_t confirmedTick);
    [[nodiscard]] bool scaffoldingPresent(ObjectId bridge) const noexcept;
    [[nodiscard]] bool scaffoldingInMotion(ObjectId bridge) const noexcept;

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionLifecycleTransactionPort m_lifecycle;
    GameSessionObjectDamageTransactions m_damage;
};

} // namespace engine
