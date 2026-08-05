#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/player/PlayerTypes.h"

namespace engine {

class GameSessionScriptPresentationState;
class GameSessionWorldState;

struct GameSessionReadyPlayerEvacuation final {
    ObjectId container = INVALID_OBJECT_ID;
    PlayerId player = INVALID_PLAYER_ID;
};

// Resolves pending aircraft landing orders and returns only the evacuations
// that became ready. The caller submits those values through the normal
// confirmed containment command path.
class GameSessionPendingEvacuationTransactions final {
public:
    GameSessionPendingEvacuationTransactions(
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation) noexcept
        : m_world(world), m_presentation(presentation) {}

    [[nodiscard]] container::Vector<GameSessionReadyPlayerEvacuation>
    advance();

private:
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
};

} // namespace engine
