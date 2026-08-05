#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/player/PlayerTypes.h"
#include "game/command/GameCommand.h"

#include <cstdint>
#include <optional>

namespace engine {

namespace ai {
enum class ObjectAIOrderCapability : uint8_t;
}

class GameSessionContentStartState;
class GameSessionAIState;
class GameSessionWorldState;

enum class PlayerRepairTargetAction : uint8_t {
    None,
    DoRepair,
    ResumeConstruction,
    GetRepaired,
};

// Read-only command UI/targeting capability. It deliberately does not expose
// containment storage, AI ownership sets, visibility, or ECS state.
class GameSessionCommandQueryPort final {
public:
    explicit GameSessionCommandQueryPort(
        const GameSessionContentStartState& content,
        const GameSessionWorldState& world,
        const GameSessionAIState& ai) noexcept
        : m_content(&content), m_world(&world), m_ai(&ai) {}

    [[nodiscard]] bool canRepairSelectionTarget(
        PlayerId player, container::Span<const ObjectId> actors,
        ObjectId structure) const;
    [[nodiscard]] PlayerRepairTargetAction repairSelectionTargetAction(
        PlayerId player, container::Span<const ObjectId> actors,
        ObjectId structure) const;
    [[nodiscard]] bool canExitPassengerThrough(
        ObjectId container, ObjectId passenger) const;
    [[nodiscard]] container::Vector<ObjectId>
    containmentPassengers(ObjectId container) const;
    [[nodiscard]] bool isCommandPlayer(PlayerId player) const noexcept;
    [[nodiscard]] bool isControllableBeacon(
        PlayerId player, ObjectId object) const noexcept;
    [[nodiscard]] bool hasOrderCapability(
        ObjectId object, ai::ObjectAIOrderCapability capability) const noexcept;
    struct SciencePurchaseRequest final {
        GameTick tick = 0;
        uint64_t requestSequence = 0;
        uint64_t buttonStableId = 0;
        container::StringView commandButtonName;
        container::StringView science;
    };
    [[nodiscard]] std::optional<GameCommand> composeSciencePurchase(
        SciencePurchaseRequest request) const;

private:
    [[nodiscard]] bool objectForbidsPlayerCommands(
        ObjectId object) const noexcept;

    const GameSessionContentStartState* m_content = nullptr;
    const GameSessionWorldState* m_world = nullptr;
    const GameSessionAIState* m_ai = nullptr;
};

} // namespace engine
