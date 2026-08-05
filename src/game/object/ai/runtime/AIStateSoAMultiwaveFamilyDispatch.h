#pragma once

#include "game/object/ai/runtime/AIStateSoAMultiwaveExecutor.h"
#include "game/object/ai/runtime/AIStateSoAParity.h"

namespace engine::ai::detail
{

// Family kernels, family dispatch/alignment and wave orchestration share only
// this internal contract. Production callers continue to use
// runAIStateSoAMultiwave().
[[nodiscard]] bool updateGuardAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool beginGuardAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool canExitGuardAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool exitGuardAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool updateGuardMoveChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool beginGuardMoveChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool canExitGuardMoveChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool exitGuardMoveChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool updateGuardPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool beginGuardPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool canExitGuardPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool exitGuardPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept;

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

[[nodiscard]] bool updateTacticalPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AITacticalAttackStateSoAKernelInput& tactical,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool beginTacticalPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AITacticalAttackStateSoAKernelInput& tactical,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool canExitTacticalPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool exitTacticalPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool updateOpportunityAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIOpportunityAttackMoveStateSoAKernelInput& opportunity,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool beginOpportunityAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIOpportunityAttackMoveStateSoAKernelInput& opportunity,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool canExitOpportunityAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool exitOpportunityAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool updateOpportunityPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIOpportunityAttackMoveStateSoAKernelInput& opportunity,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool beginOpportunityPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIOpportunityAttackMoveStateSoAKernelInput& opportunity,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool canExitOpportunityPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] bool exitOpportunityPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept;

[[nodiscard]] AIContainmentStateSoAColumns containmentColumns(
    AIStateFamilySoAStorage& storage) noexcept;

[[nodiscard]] bool isImplementedStateSoAState(AIStateId state) noexcept;

void mergeSoAWaveReport(AIStateSoAMultiwaveReport& target,
                               const AIStateSoALifecycleWaveReport& source) noexcept;

[[nodiscard]] bool dispatchSoAUpdates(AIStateFamilySoAStorage& storage,
                                             const AIStateSoAMultiwaveInput& input,
                                             container::Span<const uint8_t> mask,
                                             container::Span<AIStateStepResult> results) noexcept;

[[nodiscard]] bool dispatchSoAEnters(AIStateFamilySoAStorage& storage,
                                            const AIStateSoAMultiwaveInput& input,
                                            container::Span<const uint8_t> mask,
                                            container::Span<AIStateStepResult> results) noexcept;

[[nodiscard]] bool dispatchSoAExits(AIStateFamilySoAStorage& storage,
                                           const AIStateSoAMultiwaveInput& input,
                                           container::Span<const uint8_t> mask,
                                           container::Span<AIStateStepResult> results) noexcept;

[[nodiscard]] bool hasAlignedSoAMultiwaveSpans(AIStateFamilySoAStorage& storage,
                                                       const AIStateSoAMultiwaveInput& input,
                                                       const AIStateSoAMultiwaveScratch& scratch) noexcept;

} // namespace engine::ai::detail

