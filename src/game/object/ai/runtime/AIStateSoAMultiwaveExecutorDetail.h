#pragma once

#include "game/object/ai/runtime/AIStateSoAMultiwaveExecutor.h"
#include "game/object/ai/runtime/AIStateSoAMultiwaveFamilyDispatch.h"
#include "game/object/ai/runtime/AIStateSoAParity.h"

namespace engine::ai::detail
{

// Narrow white-box contract surface. Production callers must use
// runAIStateSoAMultiwave() from AIStateSoAMultiwaveExecutor.h.
[[nodiscard]] bool isImplementedStateSoAState(AIStateId state) noexcept;

[[nodiscard]] AIStateRequestId guardMoveChildRequest(
    const AIGuardCorrelation& correlation) noexcept;

[[nodiscard]] AIStateRequestId tacticalAttackChildRequest(
    const AITacticalAttackChildCorrelation& correlation) noexcept;

[[nodiscard]] AITacticalAttackChildStatus tacticalAttackChildTerminal(
    const AIStateStepResult& result) noexcept;

[[nodiscard]] bool updateTacticalAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AITacticalAttackStateSoAKernelInput& tactical,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool beginTacticalAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AITacticalAttackStateSoAKernelInput& tactical,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool canExitTacticalAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool exitTacticalAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept;

} // namespace engine::ai::detail
