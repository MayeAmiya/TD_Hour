#pragma once

#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"
#include "game/object/ai/states/combat/AITacticalAttackStateData.h"

#include <cstdint>
#include <optional>

namespace engine {

class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Shared read-only ObjectAI policy projection used by order admission and
// frame resolution. It owns no ECS mutation and publishes no events.
class GameSessionAIOrderPolicy final {
public:
    GameSessionAIOrderPolicy(
        const GameSessionContentStartState& content,
        const GameSessionWorldState& world,
        const GameSessionScriptPresentationState& presentation) noexcept
        : m_content(content), m_world(world), m_presentation(presentation) {}

    [[nodiscard]] ai::AISquadTargetSelection squadTargetSelection(
        ObjectId subject, bool playerIssued = false) const noexcept;
    [[nodiscard]] int32_t attackPriorityForTarget(
        ObjectId subject, ecs::entity target) const noexcept;
    [[nodiscard]] bool hasExplicitAttackPrioritySet(
        ObjectId subject) const noexcept;
    [[nodiscard]] bool rejectsOrdersWhileSleeping(
        ObjectId subject) const noexcept;
    [[nodiscard]] std::optional<ObjectId> passiveRetaliationTarget(
        ObjectId subject, bool& passive) const noexcept;
    // AttackSquad has a distinct RefCode path: unlike ordinary mood-target
    // acquisition it returns the latest DamageInfo source without excluding
    // DAMAGE_HEALING. Keep that compatibility quirk out of the shared mood
    // policy above.
    [[nodiscard]] std::optional<ObjectId> attackSquadPassiveTarget(
        ObjectId subject) const noexcept;
    [[nodiscard]] bool attitudePromotesMove(
        ObjectId subject) const noexcept;

private:
    const GameSessionContentStartState& m_content;
    const GameSessionWorldState& m_world;
    const GameSessionScriptPresentationState& m_presentation;
};

} // namespace engine
