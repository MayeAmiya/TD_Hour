#pragma once

#include "OrderContracts.h"

#include "game/object/contracts/ObjectLifecycle.h"
#include "game/player/PlayerRegistry.h"

namespace engine {

class ObjectTeamRegistry;
namespace ai {
struct ObjectAIOrderCapabilitySnapshot;
}

// Authoritative order adapter/executor. Conversion canonicalizes actor IDs
// before any ECS lookup; execution then validates every actor before mutating
// queues, preventing half-applied group orders or iteration-order drift.
class OrderExecutor final {
public:
    [[nodiscard]] static std::optional<PlayerOrder> fromGameCommand(
        const GameCommand& command, container::String* error = nullptr);
    [[nodiscard]] static OrderExecutionResult executePlayer(
        ecs::registry& registry, const PlayerRegistry& players,
        const ObjectLifecycle& objects, const PlayerOrder& order,
        bool allowHackInternetCommand, bool allowCombatDropCommand,
        int64_t groupMoveClickToGatherFactorRaw,
        const ai::ObjectAIOrderCapabilitySnapshot& capabilities);
    [[nodiscard]] static OrderExecutionResult executeScript(
        ecs::registry& registry, const PlayerRegistry& players,
        const ObjectLifecycle& objects, const ObjectTeamRegistry* teams,
        const ScriptOrderIntent& order,
        bool allowHackInternetCommand, bool allowCombatDropCommand,
        const ai::ObjectAIOrderCapabilitySnapshot& capabilities);
    [[nodiscard]] static OrderExecutionResult executeStrategicAttack(
        ecs::registry& registry, const PlayerRegistry& players,
        const ObjectLifecycle& objects, PlayerId player, GameTick tick,
        uint32_t sequence, const container::Vector<ObjectId>& actors,
        ObjectId target,
        const ai::ObjectAIOrderCapabilitySnapshot& capabilities);
    [[nodiscard]] static OrderExecutionResult executeScatter(
        ecs::registry& registry, const PlayerRegistry& players,
        const ObjectLifecycle& objects, PlayerId player, GameTick tick,
        uint32_t sequence, container::Span<const ObjectId> actors);
};

} // namespace engine
