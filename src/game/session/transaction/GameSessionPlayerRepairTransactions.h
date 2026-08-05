#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/player/PlayerTypes.h"
#include "game/session/command/OrderContracts.h"

#include <cstdint>

namespace engine {

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Owns the complete confirmed player Repair transaction: target visibility,
// builder repair admission, repair-dock AI activation and order revision.
class GameSessionPlayerRepairTransactions final {
public:
    GameSessionPlayerRepairTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation) noexcept
        : m_content(content), m_world(world), m_ai(ai),
          m_presentation(presentation) {}

    [[nodiscard]] OrderExecutionResult execute(
        PlayerId player, container::Span<const ObjectId> actors,
        ObjectId structure, uint32_t sourceSequence,
        uint64_t confirmedTick);

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
};

} // namespace engine
