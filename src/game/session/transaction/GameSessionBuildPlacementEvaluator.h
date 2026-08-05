#pragma once

#include "game/object/contracts/ObjectFixedGeometryTypes.h"
#include "game/session/transaction/GameSessionBuildPlacementContracts.h"

namespace game
{
struct ObjectArchetype;
}

namespace engine
{

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Deterministic build-placement authority shared by local preview,
// confirmed commands and AI construction. It owns no UI state and exposes
// only a value result containing legality and detached obstruction facts.
class GameSessionBuildPlacementEvaluator final
{
public:
    GameSessionBuildPlacementEvaluator(GameSessionContentStartState& content,
                                       GameSessionAIState& ai,
                                       GameSessionScriptPresentationState& presentation,
                                       GameSessionWorldState& world) noexcept
        : m_content(content)
        , m_ai(ai)
        , m_presentation(presentation)
        , m_world(world)
    {
    }

    [[nodiscard]] GameSessionBuildPlacementLegalityEvaluation evaluateFixed(ObjectId sourceObject,
                                                                            const LogicFixedVec3& placementPosition,
                                                                            math::q32_32 placementYaw,
                                                                            PlayerId player,
                                                                            const game::ObjectArchetype& product,
                                                                            bool finalConfirmation,
                                                                            bool requireBuilderReachability = true);

private:
    [[nodiscard]] bool objectAIOwnsMoveStop(ObjectId object) const noexcept;

    GameSessionContentStartState& m_content;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionWorldState& m_world;
};

} // namespace engine
