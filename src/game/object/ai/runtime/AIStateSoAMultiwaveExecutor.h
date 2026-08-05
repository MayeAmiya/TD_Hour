#pragma once

#include <cstddef>
#include <cstdint>

#include "core/container/container_types.h"
#include "game/object/ai/states/core/AICoreStateSoAKernels.h"
#include "game/object/ai/states/core/AIFacingStateSoAKernels.h"
#include "game/object/ai/states/move/AIFollowPathStateSoAKernels.h"
#include "game/object/ai/states/move/AIMoveStateSoAKernels.h"
#include "game/object/ai/states/move/AIMoveOutOfWayStateSoAKernels.h"
#include "game/object/ai/states/move/AIMoveAndTightenStateSoAKernels.h"
#include "game/object/ai/states/move/AIRepulsorStateSoAKernels.h"
#include "game/object/ai/states/move/AIWanderPanicStateSoAKernels.h"
#include "game/object/ai/states/move/AIPickUpCrateStateSoAKernels.h"
#include "game/object/ai/states/move/AIMoveEvacuateStateSoAKernels.h"
#include "game/object/ai/states/special/AIContainmentStateSoAKernels.h"
#include "game/object/ai/states/special/AIHackInternetStateSoAKernels.h"
#include "game/object/ai/states/combat/AIAttackStateSoAKernels.h"
#include "game/object/ai/states/special/AIDockStateSoAKernels.h"
#include "game/object/ai/states/special/AIInsertionStateSoAKernels.h"
#include "game/object/ai/states/combat/AIGuardStateSoAKernels.h"
#include "game/object/ai/states/combat/AITacticalAttackStateSoAKernels.h"
#include "game/object/ai/states/combat/AIOpportunityAttackMoveStateSoAKernels.h"
#include "game/object/ai/states/move/AIWaypointStateSoAKernels.h"
#include "game/object/ai/runtime/AIStateSoALifecycle.h"

namespace engine::ai
{

// Global production-shaped runner for the currently implemented SoA states.
// Kernel inputs contain facts and caller-owned output sinks; this runner
// replaces their scheduled/results spans with its own stable-slot scratch.
struct AIStateSoAMultiwaveInput final
{
    struct MoveChildScratch final
    {
        container::Span<uint8_t> scheduled;
        container::Span<uint8_t> moveTargetValid;
        container::Span<AIFixedPosition> resolvedMoveTarget;
        container::Span<AIStateStepResult> results;

        [[nodiscard]] bool aligned(size_t count) const noexcept;
        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] AIMoveStateSoAKernelInput bind(
            const AIMoveStateSoAKernelInput& base) const noexcept;
    };

    uint64_t confirmedTick = 0;
    container::Span<const uint8_t> scheduled;
    container::Span<const AIStateSoATransitionRequest> requests{};
    AICoreStateSoAKernelInput core;
    AIFacingStateSoAKernelInput facing;
    AIMoveStateSoAKernelInput move;
    AIFollowPathStateSoAKernelInput followPath;
    AIWaypointStateSoAKernelInput waypoint;
    AIMoveOutOfWayStateSoAKernelInput moveOutOfWay;
    AIMoveAndTightenStateSoAKernelInput tighten;
    AIRepulsorStateSoAKernelInput repulsor;
    AIWanderPanicStateSoAKernelInput wanderPanic;
    AIPickUpCrateStateSoAKernelInput pickUpCrate;
    AIMoveEvacuateStateSoAKernelInput moveEvacuate;
    AIContainmentStateSoAKernelInput containment;
    AIHackInternetStateSoAKernelInput hackInternet;
    AIAttackStateSoAKernelInput attack;
    AIDockStateSoAKernelInput dock;
    AIInsertionStateSoAKernelInput insertion;
    AIGuardStateSoAKernelInput guard;
    MoveChildScratch guardMoveChild;
    AIAttackChildSoAScratch guardAttackChild;
    AITacticalAttackStateSoAKernelInput tacticalAttack;
    AIAttackChildSoAScratch tacticalAttackChild;
    container::Span<AITacticalAttackChildFeedbackBuffer> tacticalAttackChildFeedback;
    AIOpportunityAttackMoveStateSoAKernelInput opportunityAttackMove;
    AIAttackChildSoAScratch opportunityAttackChild;
    container::Span<AIOpportunityAttackMoveChildFeedbackBuffer> opportunityAttackChildFeedback;
};

struct AIStateSoAMultiwaveScratch final
{
    container::Span<AIStateStepResult> results;
    container::Span<uint8_t> actionMask;
    container::Span<uint8_t> exitMask;
    container::Span<uint8_t> enterMask;
    container::Span<AIStateSoATransitionEntry> transitionEntries;
};

struct AIStateSoAMultiwaveReport final
{
    size_t waves = 0;
    size_t stepsProcessed = 0;
    size_t sleeping = 0;
    size_t unsupported = 0;
    size_t transitionsRequested = 0;
    size_t transitionsCommitted = 0;
    size_t transitionsRejected = 0;
    size_t transitionConflicts = 0;
    size_t transitionBudgetExceeded = 0;
    size_t deferredRetries = 0;
    bool spansRejected = false;
    bool transitionCapacityExceeded = false;
    bool exitBlocked = false;
    bool currentTickWorkDeferred = false;
};

// Executes updates and enter-generated transition chains until quiescent.
// Every wave follows collect -> arbitrate/commit runtime -> exit old payload ->
// activate target payload -> enter target. No allocation or AoS payload walk is
// hidden inside the runner.
[[nodiscard]] AIStateSoAMultiwaveReport runAIStateSoAMultiwave(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIStateSoAMultiwaveScratch& scratch) noexcept;

} // namespace engine::ai
