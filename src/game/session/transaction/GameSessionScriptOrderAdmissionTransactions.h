#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/session/command/OrderContracts.h"
#include "game/session/script/GameSessionScriptContracts.h"
#include "game/script/runtime/ScriptRuntime.h"
#include "game/session/transaction/GameSessionTransactionPorts.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <optional>

namespace engine {

class GameSessionContentStartState;
class GameSessionWorldState;
class GameSessionAIState;
class GameSessionScriptPresentationState;

// Script order admission and CommandButton routing. Selection, sleep/AI
// filtering and OrderExecutor batch commit live here; Session remains the
// barrier sink for production/sale/upgrade and AI policy helpers that still
// own multi-caller surfaces.
class GameSessionScriptOrderAdmissionTransactions final {
public:
    GameSessionScriptOrderAdmissionTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionOrderAdmissionPolicyPort policy) noexcept;

    [[nodiscard]] OrderExecutionResult executeScriptOrder(
        const ScriptOrderIntent& order);
    [[nodiscard]] container::Vector<ObjectId> selectScriptMoveOrderActors(
        container::Span<const ObjectId> actors,
        ScriptOrderAuthority authority) const;
    [[nodiscard]] OrderExecutionResult executeScriptCommandButton(
        const ScriptOrderIntent& envelope,
        bool requireButtonInCommandSet);
    [[nodiscard]] std::optional<ScriptCommandButtonSelectionResult>
    selectScriptCommandButtonExecution(
        container::Span<const ObjectId> teamActors,
        container::StringView buttonName,
        script::ScriptCommandButtonActorPolicy actorPolicy,
        math::q32_32 actorPercentage,
        script::ScriptCommandButtonTargetKind targetKind,
        ObjectId explicitTarget,
        container::StringView targetFilter,
        container::Span<const container::String> targetObjectTypes,
        std::optional<math::q32_32> maximumRange) const;

    [[nodiscard]] OrderExecutionResult executeScriptOrderInternal(
        const ScriptOrderIntent& order,
        bool allowHackInternetCommand,
        bool allowCombatDropCommand);

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionOrderAdmissionPolicyPort m_policy;
};

} // namespace engine
