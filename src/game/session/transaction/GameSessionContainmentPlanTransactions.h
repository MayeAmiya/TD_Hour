#pragma once

#include "core/ecs/ObjectId.h"
#include "game/object/contracts/ObjectTeamRegistry.h"
#include "game/player/PlayerTypes.h"
#include "game/session/transaction/GameSessionContainmentTransactions.h"

#include <cstdint>

namespace engine {

class GameSessionContentStartState;
class GameSessionWorldState;
class GameSessionAIState;
class GameSessionScriptPresentationState;
class GameSessionObjectDamageTransactions;

// Higher-level containment planners that select participants then commit
// through GameSessionContainmentTransactions (or damage admission for kill).
// Not a Session forwarding facade: selection and capacity reservation live here.
class GameSessionContainmentPlanTransactions final {
public:
    GameSessionContainmentPlanTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionObjectDamageTransactions* damage) noexcept;

    [[nodiscard]] size_t requestTeamCaptureNearestUnmanned(
        ObjectTeamId team, uint32_t sourceSequence, uint64_t confirmedTick);
    [[nodiscard]] size_t requestTeamGarrisonNearest(
        ObjectTeamId team, uint32_t sourceSequence, uint64_t confirmedTick);
    [[nodiscard]] size_t requestPlayerGarrisonAll(
        PlayerId player, uint32_t sourceSequence, uint64_t confirmedTick);
    [[nodiscard]] size_t requestTeamLoadTransports(
        ObjectTeamId team, uint32_t sourceSequence, uint64_t confirmedTick);
    [[nodiscard]] size_t killContainedObjects(
        ObjectId host, uint32_t sourceSequence, uint64_t confirmedTick);

private:
    [[nodiscard]] bool acceptsConfirmedTick(uint64_t confirmedTick) const noexcept;
    [[nodiscard]] GameSessionContainmentTransactions atomic() noexcept;

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionObjectDamageTransactions* m_damage = nullptr;
};

} // namespace engine
