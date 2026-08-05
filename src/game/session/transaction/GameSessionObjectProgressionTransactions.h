#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/player/PlayerTypes.h"
#include "game/session/transaction/GameSessionTransactionPorts.h"

#include <cstdint>

namespace engine {

class GameSessionContentStartState;
class GameSessionWorldState;
class GameSessionScriptPresentationState;

// Confirmed progression mutations: object/player upgrades and sticky
// booby-trap attach. Bomb create/destroy still runs through Session so spawn
// membership cascades stay singular.
class GameSessionObjectProgressionTransactions final {
public:
    GameSessionObjectProgressionTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionLifecycleTransactionPort lifecyclePublisher) noexcept;

    [[nodiscard]] bool completeObjectUpgrade(
        ObjectId object, container::StringView upgrade);
    [[nodiscard]] bool completePlayerUpgrade(
        PlayerId player, container::String upgrade);
    [[nodiscard]] bool commitQueuedPlayerUpgrade(
        PlayerId player, UpgradeContentId upgrade);
    void fanOutPlayerUpgradeCompletion(PlayerId player);
    [[nodiscard]] bool attachScriptBoobyTrap(
        ObjectId target, container::StringView templateName,
        uint64_t confirmedTick);

private:
    void refreshDerivedAggregates(uint64_t confirmedTick);

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionLifecycleTransactionPort m_lifecyclePublisher;
};

} // namespace engine
