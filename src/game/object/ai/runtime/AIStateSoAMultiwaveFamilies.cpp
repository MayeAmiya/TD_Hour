#include "game/object/ai/runtime/AIStateSoAMultiwaveFamilyDispatch.h"

#include <algorithm>
#include <limits>

namespace engine::ai::detail
{

[[nodiscard]] AIPickUpCrateStateSoAKernelInput bindPickUpCrateChild(
    const AIAttackChildSoAScratch& scratch,
    const AIPickUpCrateStateSoAKernelInput& base) noexcept;

inline void projectPickUpCrateChild(
    const AIAttackChildSoAScratch& scratch, size_t slot,
    ObjectId target, AIFixedPosition position,
    bool positionValid) noexcept;

[[nodiscard]] constexpr bool linkedGuardState(AIStateId state) noexcept
{
    return state == AIStateId::Guard ||
           state == AIStateId::GuardRetaliate ||
           state == AIStateId::GuardTunnelNetwork;
}

[[nodiscard]] constexpr AIStateId guardAttackChildState(
    AIStateId wrapperState, AIGuardPhase phase) noexcept
{
    // RefCode uses follow=true only for the dedicated AttackAggressor state
    // of ordinary Guard and TunnelNetwork Guard. GuardRetaliate, Inner and
    // Outer attacks all use the ordinary non-following AttackObject policy.
    return phase == AIGuardPhase::AttackAggressor &&
            wrapperState != AIStateId::GuardRetaliate
        ? AIStateId::AttackAndFollowObject : AIStateId::AttackObject;
}

[[nodiscard]] constexpr AIStateRequestId guardAttackChildRequest(
    const AIGuardCorrelation& correlation) noexcept
{
    uint32_t sequence =
        (correlation.stateRequest.sequence * 16777619u) ^
        correlation.operationRevision;
    sequence = (sequence * 16777619u) ^
        static_cast<uint32_t>(correlation.phase);
    if (sequence == 0)
        sequence = 1;
    return {correlation.stateRequest.issuedTick, sequence};
}

[[nodiscard]] constexpr AIGuardFeedbackKind guardAttackChildTerminal(
    const AIStateStepResult& result) noexcept
{
    switch (result.kind)
    {
    case AIStateStepKind::Success:
        return AIGuardFeedbackKind::Succeeded;
    case AIStateStepKind::Failure:
    case AIStateStepKind::Transition:
        return AIGuardFeedbackKind::Failed;
    case AIStateStepKind::Unsupported:
        return AIGuardFeedbackKind::Unsupported;
    default:
        return AIGuardFeedbackKind::None;
    }
}

inline void clearGuardAttackChildScratch(
    const AIStateSoAMultiwaveInput& input) noexcept
{
    std::fill(input.guardAttackChild.scheduled.begin(),
              input.guardAttackChild.scheduled.end(), uint8_t{0});
    std::fill(input.guardAttackChild.states.begin(),
              input.guardAttackChild.states.end(), AIStateId::Invalid);
    std::fill(input.guardAttackChild.goalObjects.begin(),
              input.guardAttackChild.goalObjects.end(), INVALID_OBJECT_ID);
    std::fill(input.guardAttackChild.goalPositions.begin(),
              input.guardAttackChild.goalPositions.end(), AIFixedPosition{});
    std::fill(input.guardAttackChild.hasGoalPositions.begin(),
              input.guardAttackChild.hasGoalPositions.end(), uint8_t{0});
    std::fill(input.guardAttackChild.results.begin(),
              input.guardAttackChild.results.end(),
              AIStateStepResult::continueState());
}

inline void projectGuardAttackChild(
    const AIStateSoAMultiwaveInput& input, size_t slot,
    ObjectId target, AIStateId wrapperState,
    AIGuardPhase phase) noexcept
{
    input.guardAttackChild.scheduled[slot] = 1;
    input.guardAttackChild.states[slot] =
        guardAttackChildState(wrapperState, phase);
    input.guardAttackChild.goalObjects[slot] = target;
}

[[nodiscard]] inline AIGuardCorrelation guardAttackCorrelation(
    const AIGuardSoAColumns& columns,
    const AIGuardStateSoAKernelInput& guard, size_t slot) noexcept
{
    return guard_detail::correlation(
        columns, columns.state[slot], guard.subjects[slot], slot,
        AIGuardOperation::Attack, columns.taskRevision[slot]);
}

[[nodiscard]] bool updateGuardAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.guardAttackChild.aligned(storage.size()))
        return input.guardAttackChild.empty();
    clearGuardAttackChildScratch(input);
    AIGuardSoAColumns& wrapper = storage.guard();
    AIAttackSoAColumns attackColumns = storage.attack().view();
    const auto runtimes = storage.runtimes();
    bool orphaned = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 ||
            !linkedGuardState(runtimes[slot].currentState) ||
            attackColumns.phase[slot] == AIAttackPhase::Inactive)
            continue;
        bool ownsCurrent = wrapper.active[slot] != 0 &&
            linkedGuardState(wrapper.state[slot]) &&
            wrapper.taskAt(slot) == AIGuardOperation::Attack &&
            wrapper.taskDomainAt(slot) ==
                AIGuardOperationDomain::Tactical;
        if (ownsCurrent)
        {
            const AIGuardCorrelation current =
                guardAttackCorrelation(wrapper, guard, slot);
            const AIStateRequestId request =
                guardAttackChildRequest(current);
            ownsCurrent = attackColumns.requestTick[slot] ==
                              request.issuedTick &&
                          attackColumns.requestSequence[slot] ==
                              request.sequence;
        }
        if (ownsCurrent)
            continue;
        projectGuardAttackChild(
            input, slot, attackColumns.trackedTarget[slot],
            runtimes[slot].currentState, wrapper.phaseAt(slot));
        orphaned = true;
    }
    if (orphaned)
    {
        AIAttackStateSoAKernelInput cleanup =
            input.guardAttackChild.bind(input.attack,
                                        storage.executionSlots());
        cleanup.confirmedTick = input.confirmedTick;
        cleanup.subjects = storage.subjects();
        if (!canExitAttackStateSoA(attackColumns, cleanup) ||
            !exitAttackStateSoA(attackColumns, cleanup))
            return false;
        clearGuardAttackChildScratch(input);
    }
    bool any = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            wrapper.taskAt(slot) != AIGuardOperation::Attack ||
            wrapper.taskDomainAt(slot) !=
                AIGuardOperationDomain::Tactical ||
            attackColumns.phase[slot] == AIAttackPhase::Inactive)
            continue;
        const AIGuardCorrelation correlation =
            guardAttackCorrelation(wrapper, guard, slot);
        const AIStateRequestId request =
            guardAttackChildRequest(correlation);
        if (attackColumns.requestTick[slot] != request.issuedTick ||
            attackColumns.requestSequence[slot] != request.sequence)
            continue;
        projectGuardAttackChild(
            input, slot, wrapper.nemesis[slot], wrapper.state[slot],
            wrapper.phaseAt(slot));
        any = true;
    }
    if (!any)
        return true;

    AIAttackStateSoAKernelInput child =
        input.guardAttackChild.bind(input.attack, storage.executionSlots());
    child.confirmedTick = input.confirmedTick;
    child.subjects = storage.subjects();
    if (!updateAttackStateSoA(attackColumns, child))
        return false;

    for (const size_t slot : storage.executionSlots())
    {
        if (input.guardAttackChild.scheduled[slot] == 0 ||
            guard.guardRangeRaw[slot] <= 0)
            continue;
        const AIGuardPhase guardPhase = wrapper.phaseAt(slot);
        if (guardPhase != AIGuardPhase::Inner &&
            guardPhase != AIGuardPhase::Outer)
            continue;
        const AIAttackFeedbackBuffer& attackFeedback =
            input.attack.combatFeedback[slot];
        const AIAttackFeedback* snapshot = nullptr;
        for (size_t index = 0;
             index < std::min(attackFeedback.count,
                              attackFeedback.values.size());
             ++index)
        {
            const AIAttackFeedback& candidate =
                attackFeedback.values[index];
            if (candidate.kind == AIAttackFeedbackKind::Snapshot &&
                candidate.target == wrapper.nemesis[slot] &&
                candidate.targetValid)
                snapshot = &candidate;
        }
        if (!snapshot)
            continue;
        const AIFixedPosition anchor = wrapper.anchorAt(slot);
        const uint64_t dx =
            opportunity_attack_move_detail::unsignedDistance(
                anchor.xRaw, snapshot->targetPosition.xRaw);
        const uint64_t dy =
            opportunity_attack_move_detail::unsignedDistance(
                anchor.yRaw, snapshot->targetPosition.yRaw);
        const uint64_t distanceSquared =
            opportunity_attack_move_detail::saturatingAdd(
                opportunity_attack_move_detail::saturatingSquare(dx),
                opportunity_attack_move_detail::saturatingSquare(dy));
        const uint64_t radiusSquared =
            opportunity_attack_move_detail::saturatingSquare(
                static_cast<uint64_t>(guard.guardRangeRaw[slot]));
        const bool withinInner = distanceSquared <= radiusSquared;
        if ((guardPhase == AIGuardPhase::Inner && withinInner) ||
            (guardPhase == AIGuardPhase::Outer && !withinInner) ||
            !guard.feedback[slot].hasCapacity())
            continue;
        static_cast<void>(guard.feedback[slot].push({
            .correlation = guardAttackCorrelation(wrapper, guard, slot),
            .kind = guardPhase == AIGuardPhase::Outer
                ? AIGuardFeedbackKind::Progress
                : AIGuardFeedbackKind::Failed,
            .target = wrapper.nemesis[slot],
            .targetPosition = snapshot->targetPosition,
            .confirmedTick = input.confirmedTick,
            .targetWithinInnerRange = withinInner,
        }));
    }

    bool terminal = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (input.guardAttackChild.scheduled[slot] == 0 ||
            guardAttackChildTerminal(
                input.guardAttackChild.results[slot]) ==
                AIGuardFeedbackKind::None)
            continue;
        if (!guard.feedback[slot].hasCapacity())
            return false;
        terminal = true;
    }
    if (!terminal)
        return true;
    if (!canExitAttackStateSoA(attackColumns, child) ||
        !exitAttackStateSoA(attackColumns, child))
        return false;
    for (const size_t slot : storage.executionSlots())
    {
        if (input.guardAttackChild.scheduled[slot] == 0)
            continue;
        const AIGuardFeedbackKind kind = guardAttackChildTerminal(
            input.guardAttackChild.results[slot]);
        if (kind == AIGuardFeedbackKind::None)
            continue;
        static_cast<void>(guard.feedback[slot].push({
            .correlation = guardAttackCorrelation(wrapper, guard, slot),
            .kind = kind,
            .target = wrapper.nemesis[slot],
            .confirmedTick = input.confirmedTick,
        }));
        attackColumns.requestTick[slot] = 0;
        attackColumns.requestSequence[slot] = 0;
    }
    return true;
}

[[nodiscard]] bool beginGuardAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.guardAttackChild.aligned(storage.size()))
        return input.guardAttackChild.empty();
    clearGuardAttackChildScratch(input);
    AIGuardSoAColumns& wrapper = storage.guard();
    AIAttackSoAColumns attackColumns = storage.attack().view();
    bool any = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            wrapper.taskAt(slot) != AIGuardOperation::Attack ||
            wrapper.taskDomainAt(slot) !=
                AIGuardOperationDomain::Tactical ||
            attackColumns.phase[slot] != AIAttackPhase::Inactive)
            continue;
        const AIGuardCorrelation expected =
            guardAttackCorrelation(wrapper, guard, slot);
        if (!wrapper.nemesis[slot])
            continue;
        const AIStateRequestId request =
            guardAttackChildRequest(expected);
        attackColumns.requestTick[slot] = request.issuedTick;
        attackColumns.requestSequence[slot] = request.sequence;
        projectGuardAttackChild(
            input, slot, wrapper.nemesis[slot], wrapper.state[slot],
            wrapper.phaseAt(slot));
        any = true;
    }
    if (!any)
        return true;
    AIAttackStateSoAKernelInput child =
        input.guardAttackChild.bind(input.attack, storage.executionSlots());
    child.confirmedTick = input.confirmedTick;
    child.subjects = storage.subjects();
    return enterAttackStateSoA(attackColumns, child);
}

[[nodiscard]] AIStateRequestId guardMoveChildRequest(
    const AIGuardCorrelation& correlation) noexcept
{
    uint32_t sequence =
        (correlation.stateRequest.sequence * 2166136261u) ^
        correlation.operationRevision;
    sequence = (sequence * 16777619u) ^
        static_cast<uint32_t>(correlation.phase);
    if (sequence == 0)
        sequence = 1;
    return {correlation.stateRequest.issuedTick, sequence};
}

[[nodiscard]] constexpr AIGuardFeedbackKind guardMoveChildTerminal(
    const AIStateStepResult& result) noexcept
{
    switch (result.kind)
    {
    case AIStateStepKind::Success:
        return AIGuardFeedbackKind::Succeeded;
    case AIStateStepKind::Failure:
    case AIStateStepKind::Transition:
        return AIGuardFeedbackKind::Failed;
    case AIStateStepKind::Unsupported:
        return AIGuardFeedbackKind::Unsupported;
    default:
        return AIGuardFeedbackKind::None;
    }
}

inline void clearGuardMoveChildScratch(
    const AIStateSoAMultiwaveInput& input) noexcept
{
    std::fill(input.guardMoveChild.scheduled.begin(),
              input.guardMoveChild.scheduled.end(), uint8_t{0});
    std::fill(input.guardMoveChild.moveTargetValid.begin(),
              input.guardMoveChild.moveTargetValid.end(), uint8_t{0});
    std::fill(input.guardMoveChild.resolvedMoveTarget.begin(),
              input.guardMoveChild.resolvedMoveTarget.end(),
              AIFixedPosition{});
    std::fill(input.guardMoveChild.results.begin(),
              input.guardMoveChild.results.end(),
              AIStateStepResult::continueState());
}

inline void projectGuardMoveChild(
    const AIStateSoAMultiwaveInput& input, size_t slot,
    const AIFixedPosition& destination) noexcept
{
    input.guardMoveChild.scheduled[slot] = 1;
    input.guardMoveChild.moveTargetValid[slot] = 1;
    input.guardMoveChild.resolvedMoveTarget[slot] = destination;
}

[[nodiscard]] inline AIGuardCorrelation guardMoveCorrelation(
    const AIGuardSoAColumns& columns,
    const AIGuardStateSoAKernelInput& guard, size_t slot) noexcept
{
    return guard_detail::correlation(
        columns, columns.state[slot], guard.subjects[slot], slot,
        AIGuardOperation::Move, columns.taskRevision[slot]);
}

[[nodiscard]] bool updateGuardMoveChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.guardMoveChild.aligned(storage.size()))
        return input.guardMoveChild.empty();
    clearGuardMoveChildScratch(input);
    AIGuardSoAColumns& wrapper = storage.guard();
    AIMoveToSoAColumns& moveColumns = storage.moveTo();
    const auto runtimes = storage.runtimes();
    bool orphaned = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 ||
            !linkedGuardState(runtimes[slot].currentState))
            continue;
        const AIMoveToStatePayload payload = moveColumns.load(slot);
        if (!payload.request.isValid())
            continue;
        bool ownsCurrent = wrapper.active[slot] != 0 &&
            linkedGuardState(wrapper.state[slot]) &&
            wrapper.taskAt(slot) == AIGuardOperation::Move &&
            wrapper.taskDomainAt(slot) ==
                AIGuardOperationDomain::Tactical;
        if (ownsCurrent)
        {
            const AIGuardCorrelation current =
                guardMoveCorrelation(wrapper, guard, slot);
            ownsCurrent = payload.request == guardMoveChildRequest(current);
        }
        if (ownsCurrent)
            continue;
        projectGuardMoveChild(input, slot, payload.resolvedGoal);
        orphaned = true;
    }
    if (orphaned)
    {
        AIMoveStateSoAKernelInput cleanup =
            input.guardMoveChild.bind(input.move);
        cleanup.confirmedTick = input.confirmedTick;
        if (!canExitMoveToSoA(storage, cleanup) ||
            !exitMoveToSoA(storage, cleanup))
            return false;
        for (const size_t slot : storage.executionSlots())
        {
            if (input.guardMoveChild.scheduled[slot] == 0)
                continue;
            AIMoveToStatePayload cleared = moveColumns.load(slot);
            cleared.request = {};
            cleared.sourceOrderRevision = 0;
            moveColumns.store(slot, cleared);
        }
        clearGuardMoveChildScratch(input);
    }
    bool any = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            wrapper.taskAt(slot) != AIGuardOperation::Move ||
            wrapper.taskDomainAt(slot) !=
                AIGuardOperationDomain::Tactical)
            continue;
        const AIGuardCorrelation correlation =
            guardMoveCorrelation(wrapper, guard, slot);
        const AIStateRequestId request = guardMoveChildRequest(correlation);
        const AIMoveToStatePayload payload = moveColumns.load(slot);
        if (payload.request != request)
            continue;
        projectGuardMoveChild(input, slot, wrapper.anchorAt(slot));
        any = true;
    }
    if (!any)
        return true;

    AIMoveStateSoAKernelInput child = input.guardMoveChild.bind(input.move);
    child.confirmedTick = input.confirmedTick;
    if (!updateMoveToSoA(storage, child))
        return false;

    bool terminal = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (input.guardMoveChild.scheduled[slot] == 0 ||
            guardMoveChildTerminal(input.guardMoveChild.results[slot]) ==
                AIGuardFeedbackKind::None)
            continue;
        if (!guard.feedback[slot].hasCapacity())
            return false;
        terminal = true;
    }
    if (!terminal)
        return true;
    if (!canExitMoveToSoA(storage, child) ||
        !exitMoveToSoA(storage, child))
        return false;
    for (const size_t slot : storage.executionSlots())
    {
        if (input.guardMoveChild.scheduled[slot] == 0)
            continue;
        const AIGuardFeedbackKind kind = guardMoveChildTerminal(
            input.guardMoveChild.results[slot]);
        if (kind == AIGuardFeedbackKind::None)
            continue;
        static_cast<void>(guard.feedback[slot].push({
            .correlation = guardMoveCorrelation(wrapper, guard, slot),
            .kind = kind,
            .confirmedTick = input.confirmedTick,
        }));
        AIMoveToStatePayload cleared = moveColumns.load(slot);
        cleared.request = {};
        cleared.sourceOrderRevision = 0;
        moveColumns.store(slot, cleared);
    }
    return true;
}

[[nodiscard]] bool beginGuardMoveChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.guardMoveChild.aligned(storage.size()))
        return input.guardMoveChild.empty();
    clearGuardMoveChildScratch(input);
    AIGuardSoAColumns& wrapper = storage.guard();
    AIMoveToSoAColumns& moveColumns = storage.moveTo();
    auto parameters = storage.parameters();
    bool any = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            wrapper.taskAt(slot) != AIGuardOperation::Move ||
            wrapper.taskDomainAt(slot) !=
                AIGuardOperationDomain::Tactical)
            continue;
        const AIGuardCorrelation expected =
            guardMoveCorrelation(wrapper, guard, slot);
        const AIStateRequestId request = guardMoveChildRequest(expected);
        if (moveColumns.load(slot).request == request)
            continue;
        AIStateParameters& parameter = parameters[slot];
        parameter.goalObject = {};
        parameter.goalPosition = wrapper.anchorAt(slot);
        parameter.hasGoalPosition = true;
        parameter.sourceOrderRevision =
            expected.sourceOrderRevision;
        AIMoveToStatePayload payload;
        payload.request = request;
        payload.generation = 1;
        payload.adjustDestinations = parameter.adjustDestinations;
        moveColumns.store(slot, payload);
        projectGuardMoveChild(input, slot, wrapper.anchorAt(slot));
        any = true;
    }
    if (!any)
        return true;
    AIMoveStateSoAKernelInput child = input.guardMoveChild.bind(input.move);
    child.confirmedTick = input.confirmedTick;
    return enterMoveToSoA(storage, child);
}

[[nodiscard]] bool canExitGuardMoveChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.guardMoveChild.aligned(storage.size()))
        return input.guardMoveChild.empty();
    clearGuardMoveChildScratch(input);
    const AIGuardSoAColumns& wrapper = storage.guard();
    const AIMoveToSoAColumns& moveColumns = storage.moveTo();
    bool any = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            !linkedGuardState(wrapper.state[slot]))
            continue;
        const AIMoveToStatePayload payload = moveColumns.load(slot);
        if (!payload.request.isValid())
            continue;
        projectGuardMoveChild(input, slot, payload.resolvedGoal);
        any = true;
    }
    if (!any)
        return true;
    AIMoveStateSoAKernelInput child = input.guardMoveChild.bind(input.move);
    child.confirmedTick = input.confirmedTick;
    return canExitMoveToSoA(storage, child);
}

[[nodiscard]] bool exitGuardMoveChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept
{
    if (!canExitGuardMoveChildren(storage, input, mask))
        return false;
    if (input.guardMoveChild.empty())
        return true;
    bool any = false;
    for (const size_t slot : storage.executionSlots())
        any = any || input.guardMoveChild.scheduled[slot] != 0;
    if (!any)
        return true;
    AIMoveStateSoAKernelInput child = input.guardMoveChild.bind(input.move);
    child.confirmedTick = input.confirmedTick;
    if (!exitMoveToSoA(storage, child))
        return false;
    AIMoveToSoAColumns& moveColumns = storage.moveTo();
    for (const size_t slot : storage.executionSlots())
    {
        if (input.guardMoveChild.scheduled[slot] == 0)
            continue;
        AIMoveToStatePayload payload = moveColumns.load(slot);
        payload.request = {};
        payload.sourceOrderRevision = 0;
        moveColumns.store(slot, payload);
    }
    return true;
}

[[nodiscard]] bool canExitGuardAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.guardAttackChild.aligned(storage.size()))
        return input.guardAttackChild.empty();
    clearGuardAttackChildScratch(input);
    const AIGuardSoAColumns& wrapper = storage.guard();
    const AIAttackSoAColumns attackColumns = storage.attack().view();
    bool any = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            !linkedGuardState(wrapper.state[slot]) ||
            attackColumns.phase[slot] == AIAttackPhase::Inactive)
            continue;
        projectGuardAttackChild(
            input, slot, attackColumns.trackedTarget[slot],
            wrapper.state[slot], wrapper.phaseAt(slot));
        any = true;
    }
    if (!any)
        return true;
    AIAttackStateSoAKernelInput child =
        input.guardAttackChild.bind(input.attack, storage.executionSlots());
    child.confirmedTick = input.confirmedTick;
    child.subjects = storage.subjects();
    return canExitAttackStateSoA(attackColumns, child);
}

[[nodiscard]] bool exitGuardAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept
{
    if (!canExitGuardAttackObjectChildren(storage, input, mask))
        return false;
    if (input.guardAttackChild.empty())
        return true;
    bool any = false;
    for (const size_t slot : storage.executionSlots())
        any = any || input.guardAttackChild.scheduled[slot] != 0;
    if (!any)
        return true;
    AIAttackSoAColumns attackColumns = storage.attack().view();
    AIAttackStateSoAKernelInput child =
        input.guardAttackChild.bind(input.attack, storage.executionSlots());
    child.confirmedTick = input.confirmedTick;
    child.subjects = storage.subjects();
    if (!exitAttackStateSoA(attackColumns, child))
        return false;
    for (const size_t slot : storage.executionSlots())
    {
        if (input.guardAttackChild.scheduled[slot] == 0)
            continue;
        attackColumns.requestTick[slot] = 0;
        attackColumns.requestSequence[slot] = 0;
    }
    return true;
}

[[nodiscard]] inline AIGuardCorrelation guardPickUpCrateCorrelation(
    const AIGuardSoAColumns& wrapper,
    const AIGuardStateSoAKernelInput& guard, size_t slot) noexcept
{
    return guard_detail::correlation(
        wrapper, wrapper.state[slot], guard.subjects[slot], slot,
        AIGuardOperation::PickUpCrate, wrapper.taskRevision[slot]);
}

[[nodiscard]] bool updateGuardPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.guardAttackChild.aligned(storage.size()))
        return input.guardAttackChild.empty();
    clearGuardAttackChildScratch(input);
    AIGuardSoAColumns& wrapper = storage.guard();
    AIMoveToSoAColumns& moveColumns = storage.moveTo();
    bool any = false;
    for (const size_t slot : storage.executionSlots()) {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            wrapper.taskAt(slot) != AIGuardOperation::PickUpCrate ||
            wrapper.taskDomainAt(slot) != AIGuardOperationDomain::Interaction)
            continue;
        const AIMoveToStatePayload payload = moveColumns.load(slot);
        if (payload.request != guardAttackChildRequest(
                guardPickUpCrateCorrelation(wrapper, guard, slot)))
            continue;
        projectPickUpCrateChild(input.guardAttackChild, slot,
                                guard.crate[slot], payload.resolvedGoal, true);
        any = true;
    }
    if (!any) return true;
    AIPickUpCrateStateSoAKernelInput child = bindPickUpCrateChild(
        input.guardAttackChild, input.pickUpCrate);
    child.confirmedTick = input.confirmedTick;
    if (!updatePickUpCrateSoA(storage, child)) return false;
    bool terminal = false;
    for (const size_t slot : storage.executionSlots()) {
        if (input.guardAttackChild.scheduled[slot] == 0 ||
            guardAttackChildTerminal(input.guardAttackChild.results[slot]) ==
                AIGuardFeedbackKind::None) {
            input.guardAttackChild.scheduled[slot] = 0;
            continue;
        }
        if (!guard.feedback[slot].hasCapacity()) return false;
        terminal = true;
    }
    if (!terminal) return true;
    if (!canExitPickUpCrateSoA(storage, child) ||
        !exitPickUpCrateSoA(storage, child))
        return false;
    for (const size_t slot : storage.executionSlots()) {
        if (input.guardAttackChild.scheduled[slot] == 0) continue;
        static_cast<void>(guard.feedback[slot].push({
            .correlation = guardPickUpCrateCorrelation(wrapper, guard, slot),
            .kind = guardAttackChildTerminal(
                input.guardAttackChild.results[slot]),
            .target = guard.crate[slot],
            .confirmedTick = input.confirmedTick,
        }));
        AIMoveToStatePayload cleared = moveColumns.load(slot);
        cleared.request = {};
        cleared.sourceOrderRevision = 0;
        moveColumns.store(slot, cleared);
    }
    return true;
}

[[nodiscard]] bool beginGuardPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.guardAttackChild.aligned(storage.size()))
        return input.guardAttackChild.empty();
    clearGuardAttackChildScratch(input);
    AIGuardSoAColumns& wrapper = storage.guard();
    AIMoveToSoAColumns& moveColumns = storage.moveTo();
    auto parameters = storage.parameters();
    bool any = false;
    for (const size_t slot : storage.executionSlots()) {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            wrapper.taskAt(slot) != AIGuardOperation::PickUpCrate ||
            wrapper.taskDomainAt(slot) != AIGuardOperationDomain::Interaction)
            continue;
        const AIGuardCorrelation expected =
            guardPickUpCrateCorrelation(wrapper, guard, slot);
        if (moveColumns.load(slot).request ==
            guardAttackChildRequest(expected))
            continue;
        const AIGuardInteractionCommand* begin = nullptr;
        const auto& commands = guard.interactionCommands[slot];
        const size_t count = std::min(commands.count, commands.values.size());
        for (size_t index = 0; index < count; ++index) {
            const auto& command = commands.values[index];
            if (command.kind == AIGuardInteractionCommandKind::BeginPickUpCrate &&
                command.correlation == expected &&
                command.targetPositionValid &&
                command.confirmedTick <= input.confirmedTick)
                begin = &command;
        }
        if (!begin) continue;
        AIStateParameters& parameter = parameters[slot];
        parameter.goalObject = begin->target;
        parameter.goalPosition = begin->targetPosition;
        parameter.hasGoalPosition = true;
        parameter.sourceOrderRevision = expected.sourceOrderRevision;
        AIMoveToStatePayload payload{guardAttackChildRequest(expected)};
        payload.sourceOrderRevision = expected.sourceOrderRevision;
        payload.adjustDestinations = true;
        moveColumns.store(slot, payload);
        projectPickUpCrateChild(input.guardAttackChild, slot,
                                begin->target, begin->targetPosition, true);
        any = true;
    }
    if (!any) return true;
    AIPickUpCrateStateSoAKernelInput child = bindPickUpCrateChild(
        input.guardAttackChild, input.pickUpCrate);
    child.confirmedTick = input.confirmedTick;
    return enterPickUpCrateSoA(storage, child);
}

[[nodiscard]] bool canExitGuardPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.guardAttackChild.aligned(storage.size()))
        return input.guardAttackChild.empty();
    clearGuardAttackChildScratch(input);
    const AIGuardSoAColumns& wrapper = storage.guard();
    const AIMoveToSoAColumns& moveColumns = storage.moveTo();
    bool any = false;
    for (const size_t slot : storage.executionSlots()) {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            wrapper.taskAt(slot) != AIGuardOperation::PickUpCrate)
            continue;
        const AIMoveToStatePayload payload = moveColumns.load(slot);
        if (!payload.request.isValid()) continue;
        projectPickUpCrateChild(input.guardAttackChild, slot,
                                guard.crate[slot], payload.resolvedGoal, true);
        any = true;
    }
    if (!any) return true;
    AIPickUpCrateStateSoAKernelInput child = bindPickUpCrateChild(
        input.guardAttackChild, input.pickUpCrate);
    child.confirmedTick = input.confirmedTick;
    return canExitPickUpCrateSoA(storage, child);
}

[[nodiscard]] bool exitGuardPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIGuardStateSoAKernelInput& guard,
    container::Span<const uint8_t> mask) noexcept
{
    if (!canExitGuardPickUpCrateChildren(storage, input, guard, mask))
        return false;
    bool any = false;
    for (const size_t slot : storage.executionSlots())
        any = any || input.guardAttackChild.scheduled[slot] != 0;
    if (!any) return true;
    AIPickUpCrateStateSoAKernelInput child = bindPickUpCrateChild(
        input.guardAttackChild, input.pickUpCrate);
    child.confirmedTick = input.confirmedTick;
    if (!exitPickUpCrateSoA(storage, child)) return false;
    AIMoveToSoAColumns& moveColumns = storage.moveTo();
    for (const size_t slot : storage.executionSlots()) {
        if (input.guardAttackChild.scheduled[slot] == 0) continue;
        AIMoveToStatePayload cleared = moveColumns.load(slot);
        cleared.request = {};
        cleared.sourceOrderRevision = 0;
        moveColumns.store(slot, cleared);
    }
    return true;
}

[[nodiscard]] AIContainmentStateSoAColumns containmentColumns(
    AIStateFamilySoAStorage& storage) noexcept
{
    return {.requestTick = storage.containmentRequestTick(),
            .requestSequence = storage.containmentRequestSequence(),
            .trackedGoal = storage.containmentTrackedGoal(),
            .entryToClear = storage.containmentEntryToClear(),
            .phase = storage.containmentPhase()};
}

[[nodiscard]] bool isImplementedStateSoAState(AIStateId state) noexcept
{
    switch (state)
    {
    case AIStateId::Idle:
    case AIStateId::Wait:
    case AIStateId::Busy:
    case AIStateId::Dead:
    case AIStateId::FaceObject:
    case AIStateId::FacePosition:
    case AIStateId::MoveTo:
    case AIStateId::FollowPath:
    case AIStateId::FollowExitProductionPath:
    case AIStateId::FollowWaypointPathAsTeam:
    case AIStateId::FollowWaypointPathAsIndividuals:
    case AIStateId::FollowWaypointPathAsTeamExact:
    case AIStateId::FollowWaypointPathAsIndividualsExact:
    case AIStateId::MoveOutOfTheWay:
    case AIStateId::MoveAndTighten:
    case AIStateId::Wander:
    case AIStateId::Panic:
    case AIStateId::PickUpCrate:
    case AIStateId::MoveAndEvacuate:
    case AIStateId::MoveAndEvacuateAndExit:
    case AIStateId::MoveAndDelete:
    case AIStateId::Enter:
    case AIStateId::Exit:
    case AIStateId::ExitInstantly:
    case AIStateId::HackInternet:
    case AIStateId::AttackPosition:
    case AIStateId::AttackObject:
    case AIStateId::ForceAttackObject:
    case AIStateId::AttackAndFollowObject:
    case AIStateId::Dock:
    case AIStateId::GetRepaired:
    case AIStateId::RappelInto:
    case AIStateId::CombatDrop:
    case AIStateId::Guard:
    case AIStateId::GuardRetaliate:
    case AIStateId::GuardTunnelNetwork:
    case AIStateId::Hunt:
    case AIStateId::AttackSquad:
    case AIStateId::AttackArea:
    case AIStateId::AttackMoveTo:
    case AIStateId::AttackFollowWaypointPathAsIndividuals:
    case AIStateId::AttackFollowWaypointPathAsTeam:
    case AIStateId::MoveAwayFromRepulsors:
    case AIStateId::WanderInPlace:
        return true;
    default:
        return false;
    }
}

void mergeSoAWaveReport(AIStateSoAMultiwaveReport& target,
                               const AIStateSoALifecycleWaveReport& source) noexcept
{
    target.stepsProcessed += source.stepsProcessed;
    target.sleeping += source.sleeping;
    target.unsupported += source.unsupported;
    target.transitionsRequested += source.transitionsRequested;
    target.transitionsCommitted += source.transitionsCommitted;
    target.transitionsRejected += source.transitionsRejected;
    target.transitionConflicts += source.transitionConflicts;
    target.transitionBudgetExceeded += source.transitionBudgetExceeded;
    target.spansRejected = target.spansRejected || source.spansRejected;
    target.transitionCapacityExceeded =
        target.transitionCapacityExceeded || source.transitionCapacityExceeded;
}

[[nodiscard]] AIStateRequestId tacticalAttackChildRequest(
    const AITacticalAttackChildCorrelation& correlation) noexcept
{
    uint32_t sequence = (correlation.stateRequest.sequence * 16777619u) ^ correlation.generation;
    sequence = (sequence * 16777619u) ^ static_cast<uint32_t>(correlation.targetRevision);
    if (sequence == 0)
        sequence = 1;
    return {correlation.stateRequest.issuedTick, sequence};
}

[[nodiscard]] AITacticalAttackChildStatus tacticalAttackChildTerminal(
    const AIStateStepResult& result) noexcept
{
    switch (result.kind)
    {
    case AIStateStepKind::Success:
        return AITacticalAttackChildStatus::Succeeded;
    case AIStateStepKind::Failure:
    case AIStateStepKind::Transition:
        return AITacticalAttackChildStatus::Failed;
    case AIStateStepKind::Unsupported:
        return AITacticalAttackChildStatus::Unsupported;
    default:
        return AITacticalAttackChildStatus::None;
    }
}

[[nodiscard]] AIPickUpCrateStateSoAKernelInput bindPickUpCrateChild(
    const AIAttackChildSoAScratch& scratch,
    const AIPickUpCrateStateSoAKernelInput& base) noexcept
{
    AIPickUpCrateStateSoAKernelInput child = base;
    child.childMode = true;
    child.scheduled = scratch.scheduled;
    child.moveTargetValid = scratch.hasGoalPositions;
    child.resolvedMoveTarget = scratch.goalPositions;
    child.results = scratch.results;
    return child;
}

inline void projectPickUpCrateChild(
    const AIAttackChildSoAScratch& scratch, size_t slot,
    ObjectId target, AIFixedPosition position,
    bool positionValid) noexcept
{
    scratch.scheduled[slot] = 1;
    scratch.states[slot] = AIStateId::PickUpCrate;
    scratch.goalObjects[slot] = target;
    scratch.goalPositions[slot] = position;
    scratch.hasGoalPositions[slot] = positionValid ? uint8_t{1} : uint8_t{0};
}

inline void clearTacticalAttackChildScratch(const AIStateSoAMultiwaveInput& input) noexcept
{
    std::fill(input.tacticalAttackChild.scheduled.begin(),
              input.tacticalAttackChild.scheduled.end(),
              uint8_t{0});
    std::fill(input.tacticalAttackChild.states.begin(),
              input.tacticalAttackChild.states.end(),
              AIStateId::Invalid);
    std::fill(input.tacticalAttackChild.goalObjects.begin(),
              input.tacticalAttackChild.goalObjects.end(),
              INVALID_OBJECT_ID);
    std::fill(input.tacticalAttackChild.goalPositions.begin(),
              input.tacticalAttackChild.goalPositions.end(),
              AIFixedPosition{});
    std::fill(input.tacticalAttackChild.hasGoalPositions.begin(),
              input.tacticalAttackChild.hasGoalPositions.end(),
              uint8_t{0});
    std::fill(input.tacticalAttackChild.results.begin(),
              input.tacticalAttackChild.results.end(),
              AIStateStepResult::continueState());
}

inline void projectTacticalAttackChild(const AIStateSoAMultiwaveInput& input,
                                       size_t slot,
                                       ObjectId target) noexcept
{
    input.tacticalAttackChild.scheduled[slot] = 1;
    input.tacticalAttackChild.states[slot] = AIStateId::AttackObject;
    input.tacticalAttackChild.goalObjects[slot] = target;
}

[[nodiscard]] inline AITacticalAttackChildCorrelation tacticalAttackChildCorrelation(
    const AITacticalAttackSoAColumns& columns,
    const AITacticalAttackStateSoAKernelInput& tactical,
    size_t slot) noexcept
{
    return tactical_attack_detail::childCorrelation(columns, tactical, slot);
}

[[nodiscard]] inline bool hasTacticalAttackChildScratch(
    const AIStateSoAMultiwaveInput& input,
    size_t count) noexcept
{
    return input.tacticalAttackChild.aligned(count) &&
           input.tacticalAttackChildFeedback.size() == count;
}

[[nodiscard]] bool updateTacticalAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AITacticalAttackStateSoAKernelInput& tactical,
    container::Span<const uint8_t> mask) noexcept
{
    if (!hasTacticalAttackChildScratch(input, storage.size()))
        return input.tacticalAttackChild.empty() && input.tacticalAttackChildFeedback.empty();

    clearTacticalAttackChildScratch(input);
    AITacticalAttackSoAColumns& wrapper = storage.tacticalAttack();
    AIAttackSoAColumns attackColumns = storage.attack().view();
    bool any = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            !tacticalAttackPolicyFor(wrapper.state[slot]).valid ||
            wrapper.childState[slot] != AIStateId::AttackObject ||
            attackColumns.phase[slot] == AIAttackPhase::Inactive)
        {
            continue;
        }
        const auto correlation = tacticalAttackChildCorrelation(wrapper, tactical, slot);
        if (attackColumns.requestTick[slot] != tacticalAttackChildRequest(correlation).issuedTick ||
            attackColumns.requestSequence[slot] != tacticalAttackChildRequest(correlation).sequence)
        {
            continue;
        }
        projectTacticalAttackChild(input, slot, wrapper.target[slot]);
        any = true;
    }
    if (!any)
        return true;

    AIAttackStateSoAKernelInput child = input.tacticalAttackChild.bind(
        input.attack, storage.executionSlots());
    child.confirmedTick = input.confirmedTick;
    child.subjects = storage.subjects();
    if (!updateAttackStateSoA(attackColumns, child))
        return false;

    bool terminal = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (input.tacticalAttackChild.scheduled[slot] == 0 ||
            tacticalAttackChildTerminal(input.tacticalAttackChild.results[slot]) ==
                AITacticalAttackChildStatus::None)
        {
            input.tacticalAttackChild.scheduled[slot] = 0;
            continue;
        }
        if (!input.tacticalAttackChildFeedback[slot].hasCapacity())
            return false;
        terminal = true;
    }
    if (!terminal)
        return true;
    if (!canExitAttackStateSoA(attackColumns, child) || !exitAttackStateSoA(attackColumns, child))
        return false;

    for (const size_t slot : storage.executionSlots())
    {
        if (input.tacticalAttackChild.scheduled[slot] == 0)
            continue;
        static_cast<void>(input.tacticalAttackChildFeedback[slot].push({
            .correlation = tacticalAttackChildCorrelation(wrapper, tactical, slot),
            .status = tacticalAttackChildTerminal(input.tacticalAttackChild.results[slot]),
            .confirmedTick = input.confirmedTick,
        }));
        attackColumns.requestTick[slot] = 0;
        attackColumns.requestSequence[slot] = 0;
    }
    return true;
}

[[nodiscard]] bool beginTacticalAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AITacticalAttackStateSoAKernelInput& tactical,
    container::Span<const uint8_t> mask) noexcept
{
    if (!hasTacticalAttackChildScratch(input, storage.size()))
        return input.tacticalAttackChild.empty() && input.tacticalAttackChildFeedback.empty();

    clearTacticalAttackChildScratch(input);
    AITacticalAttackSoAColumns& wrapper = storage.tacticalAttack();
    AIAttackSoAColumns attackColumns = storage.attack().view();

    // StartOrReplace also means Stop for a previously owned AttackObject when
    // another child kind (currently PickUpCrate) becomes authoritative.
    bool stopAny = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            !tacticalAttackPolicyFor(wrapper.state[slot]).valid ||
            attackColumns.phase[slot] == AIAttackPhase::Inactive)
            continue;
        const auto expected = tacticalAttackChildCorrelation(wrapper, tactical, slot);
        const auto expectedRequest = tacticalAttackChildRequest(expected);
        bool replace = false;
        const auto& commands = tactical.childCommands[slot];
        const size_t count = std::min(commands.count, commands.values.size());
        for (size_t index = 0; index < count; ++index)
        {
            const auto& command = commands.values[index];
            replace = replace ||
                      (command.kind == AITacticalAttackChildCommandKind::StartOrReplace &&
                       command.correlation == expected && command.confirmedTick <= input.confirmedTick);
        }
        if (!replace || (expected.childState == AIStateId::AttackObject &&
                         attackColumns.requestTick[slot] == expectedRequest.issuedTick &&
                         attackColumns.requestSequence[slot] == expectedRequest.sequence))
            continue;
        projectTacticalAttackChild(input, slot, attackColumns.trackedTarget[slot]);
        stopAny = true;
    }
    if (stopAny)
    {
        AIAttackStateSoAKernelInput child = input.tacticalAttackChild.bind(
            input.attack, storage.executionSlots());
        child.confirmedTick = input.confirmedTick;
        child.subjects = storage.subjects();
        if (!canExitAttackStateSoA(attackColumns, child) || !exitAttackStateSoA(attackColumns, child))
            return false;
    }

    clearTacticalAttackChildScratch(input);
    bool beginAny = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            !tacticalAttackPolicyFor(wrapper.state[slot]).valid ||
            wrapper.childState[slot] != AIStateId::AttackObject ||
            attackColumns.phase[slot] != AIAttackPhase::Inactive)
            continue;
        const auto expected = tacticalAttackChildCorrelation(wrapper, tactical, slot);
        bool begin = false;
        const auto& commands = tactical.childCommands[slot];
        const size_t count = std::min(commands.count, commands.values.size());
        for (size_t index = 0; index < count; ++index)
        {
            const auto& command = commands.values[index];
            begin = begin ||
                    (command.kind == AITacticalAttackChildCommandKind::StartOrReplace &&
                     command.correlation == expected && command.confirmedTick <= input.confirmedTick);
        }
        if (!begin)
            continue;
        const AIStateRequestId request = tacticalAttackChildRequest(expected);
        attackColumns.requestTick[slot] = request.issuedTick;
        attackColumns.requestSequence[slot] = request.sequence;
        projectTacticalAttackChild(input, slot, wrapper.target[slot]);
        beginAny = true;
    }
    if (!beginAny)
        return true;

    AIAttackStateSoAKernelInput child = input.tacticalAttackChild.bind(
        input.attack, storage.executionSlots());
    child.confirmedTick = input.confirmedTick;
    child.subjects = storage.subjects();
    if (!enterAttackStateSoA(attackColumns, child))
        return false;

    bool immediateTerminal = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (input.tacticalAttackChild.scheduled[slot] == 0)
            continue;
        const auto status = tacticalAttackChildTerminal(input.tacticalAttackChild.results[slot]);
        if (status == AITacticalAttackChildStatus::None)
        {
            input.tacticalAttackChild.scheduled[slot] = 0;
            continue;
        }
        if (!input.tacticalAttackChildFeedback[slot].hasCapacity())
            return false;
        static_cast<void>(input.tacticalAttackChildFeedback[slot].push({
            .correlation = tacticalAttackChildCorrelation(wrapper, tactical, slot),
            .status = status,
            .confirmedTick = input.confirmedTick,
        }));
        attackColumns.requestTick[slot] = 0;
        attackColumns.requestSequence[slot] = 0;
        immediateTerminal = true;
    }
    if (!immediateTerminal)
        return true;

    AITacticalAttackStateSoAKernelInput consume = tactical;
    consume.scheduled = input.tacticalAttackChild.scheduled;
    return updateTacticalAttackStateSoA(
               wrapper, AIStateId::Hunt, consume) &&
           updateTacticalAttackStateSoA(
               wrapper, AIStateId::AttackSquad, consume) &&
           updateTacticalAttackStateSoA(
               wrapper, AIStateId::AttackArea, consume);
}

[[nodiscard]] bool canExitTacticalAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept
{
    if (!hasTacticalAttackChildScratch(input, storage.size()))
        return input.tacticalAttackChild.empty() && input.tacticalAttackChildFeedback.empty();
    clearTacticalAttackChildScratch(input);
    const AITacticalAttackSoAColumns& wrapper = storage.tacticalAttack();
    const AIAttackSoAColumns attackColumns = storage.attack().view();
    bool any = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            !tacticalAttackPolicyFor(wrapper.state[slot]).valid ||
            attackColumns.phase[slot] == AIAttackPhase::Inactive)
            continue;
        projectTacticalAttackChild(input, slot, wrapper.target[slot]);
        any = true;
    }
    if (!any)
        return true;
    AIAttackStateSoAKernelInput child = input.tacticalAttackChild.bind(
        input.attack, storage.executionSlots());
    child.confirmedTick = input.confirmedTick;
    child.subjects = storage.subjects();
    return canExitAttackStateSoA(attackColumns, child);
}

[[nodiscard]] bool exitTacticalAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept
{
    if (!hasTacticalAttackChildScratch(input, storage.size()))
        return input.tacticalAttackChild.empty() && input.tacticalAttackChildFeedback.empty();
    if (!canExitTacticalAttackObjectChildren(storage, input, mask))
        return false;
    AIAttackSoAColumns attackColumns = storage.attack().view();
    bool any = false;
    for (const size_t slot : storage.executionSlots())
        any = any || input.tacticalAttackChild.scheduled[slot] != 0;
    if (!any)
        return true;
    AIAttackStateSoAKernelInput child = input.tacticalAttackChild.bind(
        input.attack, storage.executionSlots());
    child.confirmedTick = input.confirmedTick;
    child.subjects = storage.subjects();
    if (!exitAttackStateSoA(attackColumns, child))
        return false;
    for (const size_t slot : storage.executionSlots())
    {
        if (input.tacticalAttackChild.scheduled[slot] != 0)
        {
            attackColumns.requestTick[slot] = 0;
            attackColumns.requestSequence[slot] = 0;
        }
    }
    return true;
}

[[nodiscard]] bool updateTacticalPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AITacticalAttackStateSoAKernelInput& tactical,
    container::Span<const uint8_t> mask) noexcept
{
    if (!hasTacticalAttackChildScratch(input, storage.size()))
        return input.tacticalAttackChild.empty() &&
               input.tacticalAttackChildFeedback.empty();
    clearTacticalAttackChildScratch(input);
    AITacticalAttackSoAColumns& wrapper = storage.tacticalAttack();
    AIMoveToSoAColumns& moveColumns = storage.moveTo();
    bool any = false;
    for (const size_t slot : storage.executionSlots()) {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            wrapper.childState[slot] != AIStateId::PickUpCrate)
            continue;
        const auto correlation =
            tacticalAttackChildCorrelation(wrapper, tactical, slot);
        const AIMoveToStatePayload payload = moveColumns.load(slot);
        if (payload.request != tacticalAttackChildRequest(correlation))
            continue;
        projectPickUpCrateChild(
            input.tacticalAttackChild, slot, wrapper.target[slot],
            payload.resolvedGoal, true);
        any = true;
    }
    if (!any) return true;

    AIPickUpCrateStateSoAKernelInput child = bindPickUpCrateChild(
        input.tacticalAttackChild, input.pickUpCrate);
    child.confirmedTick = input.confirmedTick;
    if (!updatePickUpCrateSoA(storage, child)) return false;

    bool terminal = false;
    for (const size_t slot : storage.executionSlots()) {
        if (input.tacticalAttackChild.scheduled[slot] == 0 ||
            tacticalAttackChildTerminal(
                input.tacticalAttackChild.results[slot]) ==
                AITacticalAttackChildStatus::None) {
            input.tacticalAttackChild.scheduled[slot] = 0;
            continue;
        }
        if (!input.tacticalAttackChildFeedback[slot].hasCapacity())
            return false;
        terminal = true;
    }
    if (!terminal) return true;
    if (!canExitPickUpCrateSoA(storage, child) ||
        !exitPickUpCrateSoA(storage, child))
        return false;
    for (const size_t slot : storage.executionSlots()) {
        if (input.tacticalAttackChild.scheduled[slot] == 0) continue;
        static_cast<void>(input.tacticalAttackChildFeedback[slot].push({
            .correlation =
                tacticalAttackChildCorrelation(wrapper, tactical, slot),
            .status = tacticalAttackChildTerminal(
                input.tacticalAttackChild.results[slot]),
            .confirmedTick = input.confirmedTick,
        }));
        AIMoveToStatePayload cleared = moveColumns.load(slot);
        cleared.request = {};
        cleared.sourceOrderRevision = 0;
        moveColumns.store(slot, cleared);
    }
    return true;
}

[[nodiscard]] bool beginTacticalPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AITacticalAttackStateSoAKernelInput& tactical,
    container::Span<const uint8_t> mask) noexcept
{
    if (!hasTacticalAttackChildScratch(input, storage.size()))
        return input.tacticalAttackChild.empty() &&
               input.tacticalAttackChildFeedback.empty();
    clearTacticalAttackChildScratch(input);
    AITacticalAttackSoAColumns& wrapper = storage.tacticalAttack();
    AIMoveToSoAColumns& moveColumns = storage.moveTo();
    auto parameters = storage.parameters();
    bool any = false;
    for (const size_t slot : storage.executionSlots()) {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            wrapper.childState[slot] != AIStateId::PickUpCrate)
            continue;
        const auto expected =
            tacticalAttackChildCorrelation(wrapper, tactical, slot);
        if (moveColumns.load(slot).request ==
            tacticalAttackChildRequest(expected))
            continue;
        const AITacticalAttackChildCommand* begin = nullptr;
        const auto& commands = tactical.childCommands[slot];
        const size_t count = std::min(commands.count, commands.values.size());
        for (size_t index = 0; index < count; ++index) {
            const auto& command = commands.values[index];
            if (command.kind ==
                    AITacticalAttackChildCommandKind::StartOrReplace &&
                command.correlation == expected &&
                command.confirmedTick <= input.confirmedTick) {
                begin = &command;
            }
        }
        if (!begin) continue;
        if (!begin->targetPositionValid) {
            if (!input.tacticalAttackChildFeedback[slot].hasCapacity())
                return false;
            input.tacticalAttackChild.scheduled[slot] = 1;
            input.tacticalAttackChild.states[slot] = AIStateId::Invalid;
            input.tacticalAttackChild.results[slot] =
                AIStateStepResult::failure();
            any = true;
            continue;
        }
        AIStateParameters& parameter = parameters[slot];
        parameter.goalObject = wrapper.target[slot];
        parameter.goalPosition = begin->targetPosition;
        parameter.hasGoalPosition = true;
        parameter.sourceOrderRevision = expected.sourceOrderRevision;
        AIMoveToStatePayload payload{
            tacticalAttackChildRequest(expected)};
        payload.sourceOrderRevision = expected.sourceOrderRevision;
        payload.adjustDestinations = true;
        moveColumns.store(slot, payload);
        projectPickUpCrateChild(input.tacticalAttackChild, slot,
                                wrapper.target[slot],
                                begin->targetPosition, true);
        any = true;
    }
    if (!any) return true;

    for (const size_t slot : storage.executionSlots()) {
        if (input.tacticalAttackChild.states[slot] == AIStateId::Invalid)
            input.tacticalAttackChild.scheduled[slot] = 0;
    }
    AIPickUpCrateStateSoAKernelInput child = bindPickUpCrateChild(
        input.tacticalAttackChild, input.pickUpCrate);
    child.confirmedTick = input.confirmedTick;
    if (!enterPickUpCrateSoA(storage, child)) return false;
    for (const size_t slot : storage.executionSlots()) {
        if (input.tacticalAttackChild.states[slot] == AIStateId::Invalid)
            input.tacticalAttackChild.scheduled[slot] = 1;
    }
    bool immediateTerminal = false;
    for (const size_t slot : storage.executionSlots()) {
        if (input.tacticalAttackChild.scheduled[slot] == 0) continue;
        const auto status = tacticalAttackChildTerminal(
            input.tacticalAttackChild.results[slot]);
        if (status == AITacticalAttackChildStatus::None) {
            input.tacticalAttackChild.scheduled[slot] = 0;
            continue;
        }
        if (!input.tacticalAttackChildFeedback[slot].hasCapacity())
            return false;
        static_cast<void>(input.tacticalAttackChildFeedback[slot].push({
            .correlation =
                tacticalAttackChildCorrelation(wrapper, tactical, slot),
            .status = status,
            .confirmedTick = input.confirmedTick,
        }));
        AIMoveToStatePayload cleared = moveColumns.load(slot);
        cleared.request = {};
        cleared.sourceOrderRevision = 0;
        moveColumns.store(slot, cleared);
        immediateTerminal = true;
    }
    if (!immediateTerminal) return true;
    AITacticalAttackStateSoAKernelInput consume = tactical;
    consume.scheduled = input.tacticalAttackChild.scheduled;
    return updateTacticalAttackStateSoA(
               wrapper, AIStateId::Hunt, consume) &&
           updateTacticalAttackStateSoA(
               wrapper, AIStateId::AttackSquad, consume) &&
           updateTacticalAttackStateSoA(
               wrapper, AIStateId::AttackArea, consume);
}

[[nodiscard]] bool canExitTacticalPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept
{
    if (!hasTacticalAttackChildScratch(input, storage.size()))
        return input.tacticalAttackChild.empty() &&
               input.tacticalAttackChildFeedback.empty();
    clearTacticalAttackChildScratch(input);
    const AITacticalAttackSoAColumns& wrapper = storage.tacticalAttack();
    const AIMoveToSoAColumns& moveColumns = storage.moveTo();
    bool any = false;
    for (const size_t slot : storage.executionSlots()) {
        if (mask[slot] == 0 || wrapper.active[slot] == 0 ||
            wrapper.childState[slot] != AIStateId::PickUpCrate)
            continue;
        const AIMoveToStatePayload payload = moveColumns.load(slot);
        if (!payload.request.isValid()) continue;
        projectPickUpCrateChild(input.tacticalAttackChild, slot,
                                wrapper.target[slot],
                                payload.resolvedGoal, true);
        any = true;
    }
    if (!any) return true;
    AIPickUpCrateStateSoAKernelInput child = bindPickUpCrateChild(
        input.tacticalAttackChild, input.pickUpCrate);
    child.confirmedTick = input.confirmedTick;
    return canExitPickUpCrateSoA(storage, child);
}

[[nodiscard]] bool exitTacticalPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept
{
    if (!canExitTacticalPickUpCrateChildren(storage, input, mask))
        return false;
    bool any = false;
    for (const size_t slot : storage.executionSlots())
        any = any || input.tacticalAttackChild.scheduled[slot] != 0;
    if (!any) return true;
    AIPickUpCrateStateSoAKernelInput child = bindPickUpCrateChild(
        input.tacticalAttackChild, input.pickUpCrate);
    child.confirmedTick = input.confirmedTick;
    if (!exitPickUpCrateSoA(storage, child)) return false;
    AIMoveToSoAColumns& moveColumns = storage.moveTo();
    for (const size_t slot : storage.executionSlots()) {
        if (input.tacticalAttackChild.scheduled[slot] == 0) continue;
        AIMoveToStatePayload cleared = moveColumns.load(slot);
        cleared.request = {};
        cleared.sourceOrderRevision = 0;
        moveColumns.store(slot, cleared);
    }
    return true;
}

[[nodiscard]] constexpr AIStateRequestId opportunityAttackChildRequest(
    const AIOpportunityAttackMoveCorrelation& correlation) noexcept
{
    uint32_t sequence = (correlation.stateRequest.sequence * 16777619u) ^ correlation.operationRevision;
    if (sequence == 0)
        sequence = 1;
    return {correlation.stateRequest.issuedTick, sequence};
}

[[nodiscard]] constexpr AIOpportunityAttackMoveChildFeedbackKind opportunityAttackChildTerminal(
    const AIStateStepResult& result) noexcept
{
    switch (result.kind)
    {
    case AIStateStepKind::Success:
        return AIOpportunityAttackMoveChildFeedbackKind::Succeeded;
    case AIStateStepKind::Failure:
    case AIStateStepKind::Transition:
        return AIOpportunityAttackMoveChildFeedbackKind::Failed;
    case AIStateStepKind::Unsupported:
        return AIOpportunityAttackMoveChildFeedbackKind::Unsupported;
    default:
        return AIOpportunityAttackMoveChildFeedbackKind::None;
    }
}

inline void clearOpportunityAttackChildScratch(const AIStateSoAMultiwaveInput& input) noexcept
{
    std::fill(input.opportunityAttackChild.scheduled.begin(),
              input.opportunityAttackChild.scheduled.end(),
              uint8_t{0});
    std::fill(input.opportunityAttackChild.states.begin(),
              input.opportunityAttackChild.states.end(),
              AIStateId::Invalid);
    std::fill(input.opportunityAttackChild.goalObjects.begin(),
              input.opportunityAttackChild.goalObjects.end(),
              INVALID_OBJECT_ID);
    std::fill(input.opportunityAttackChild.goalPositions.begin(),
              input.opportunityAttackChild.goalPositions.end(),
              AIFixedPosition{});
    std::fill(input.opportunityAttackChild.hasGoalPositions.begin(),
              input.opportunityAttackChild.hasGoalPositions.end(),
              uint8_t{0});
    std::fill(input.opportunityAttackChild.results.begin(),
              input.opportunityAttackChild.results.end(),
              AIStateStepResult::continueState());
}

inline void projectOpportunityAttackChild(const AIStateSoAMultiwaveInput& input,
                                          size_t slot,
                                          ObjectId target) noexcept
{
    input.opportunityAttackChild.scheduled[slot] = 1;
    input.opportunityAttackChild.states[slot] = AIStateId::AttackObject;
    input.opportunityAttackChild.goalObjects[slot] = target;
}

[[nodiscard]] inline AIOpportunityAttackMoveCorrelation opportunityAttackChildCorrelation(
    const AIOpportunityAttackMoveSoAColumns& columns,
    const AIOpportunityAttackMoveStateSoAKernelInput& opportunity,
    size_t slot) noexcept
{
    return opportunity_attack_move_detail::correlation(
        columns,
        opportunity,
        slot,
        AIOpportunityAttackMoveOperation::Attack,
        columns.childRevision[slot]);
}

[[nodiscard]] bool updateOpportunityAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIOpportunityAttackMoveStateSoAKernelInput& opportunity,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.opportunityAttackChild.aligned(storage.size()))
        return input.opportunityAttackChild.empty() && input.opportunityAttackChildFeedback.empty();
    clearOpportunityAttackChildScratch(input);
    AIOpportunityAttackMoveSoAColumns& wrapper = storage.opportunityAttackMove();
    AIAttackSoAColumns attackColumns = storage.attack().view();
    bool any = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 || wrapper.childActive[slot] == 0 ||
            static_cast<AIOpportunityAttackMoveOperation>(wrapper.childOperation[slot]) !=
                AIOpportunityAttackMoveOperation::Attack ||
            attackColumns.phase[slot] == AIAttackPhase::Inactive)
        {
            continue;
        }
        projectOpportunityAttackChild(input, slot, wrapper.childTarget[slot]);
        any = true;
    }
    if (!any)
        return true;

    AIAttackStateSoAKernelInput child = input.opportunityAttackChild.bind(
        input.attack, storage.executionSlots());
    child.confirmedTick = input.confirmedTick;
    child.subjects = storage.subjects();
    if (!updateAttackStateSoA(attackColumns, child))
        return false;

    bool terminal = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (input.opportunityAttackChild.scheduled[slot] == 0)
            continue;
        const auto kind = opportunityAttackChildTerminal(input.opportunityAttackChild.results[slot]);
        if (kind == AIOpportunityAttackMoveChildFeedbackKind::None)
            continue;
        if (!input.opportunityAttackChildFeedback[slot].hasCapacity())
            return false;
        terminal = true;
    }
    if (!terminal)
        return true;
    for (const size_t slot : storage.executionSlots())
    {
        if (input.opportunityAttackChild.scheduled[slot] != 0 &&
            opportunityAttackChildTerminal(input.opportunityAttackChild.results[slot]) ==
                AIOpportunityAttackMoveChildFeedbackKind::None)
        {
            input.opportunityAttackChild.scheduled[slot] = 0;
        }
    }
    if (!canExitAttackStateSoA(attackColumns, child) || !exitAttackStateSoA(attackColumns, child))
        return false;
    for (const size_t slot : storage.executionSlots())
    {
        if (input.opportunityAttackChild.scheduled[slot] == 0)
            continue;
        const auto kind = opportunityAttackChildTerminal(input.opportunityAttackChild.results[slot]);
        if (kind == AIOpportunityAttackMoveChildFeedbackKind::None)
            continue;
        static_cast<void>(input.opportunityAttackChildFeedback[slot].push(
            {.correlation = opportunityAttackChildCorrelation(wrapper, opportunity, slot),
             .kind = kind,
             .confirmedTick = input.confirmedTick}));
        attackColumns.requestTick[slot] = 0;
        attackColumns.requestSequence[slot] = 0;
    }
    return true;
}

[[nodiscard]] bool beginOpportunityAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIOpportunityAttackMoveStateSoAKernelInput& opportunity,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.opportunityAttackChild.aligned(storage.size()))
        return input.opportunityAttackChild.empty() && input.opportunityAttackChildFeedback.empty();
    clearOpportunityAttackChildScratch(input);
    AIOpportunityAttackMoveSoAColumns& wrapper = storage.opportunityAttackMove();
    AIAttackSoAColumns attackColumns = storage.attack().view();
    bool any = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 || wrapper.childActive[slot] == 0 ||
            static_cast<AIOpportunityAttackMoveOperation>(wrapper.childOperation[slot]) !=
                AIOpportunityAttackMoveOperation::Attack ||
            attackColumns.phase[slot] != AIAttackPhase::Inactive)
        {
            continue;
        }
        const auto expected = opportunityAttackChildCorrelation(wrapper, opportunity, slot);
        const auto& commands = opportunity.childCommands[slot];
        const size_t count = std::min(commands.count, commands.values.size());
        bool begin = false;
        for (size_t index = 0; index < count; ++index)
        {
            const auto& command = commands.values[index];
            begin = begin || (command.kind == AIOpportunityAttackMoveChildCommandKind::BeginAttack &&
                              command.correlation == expected && command.target == wrapper.childTarget[slot] &&
                              command.commandSourceIsAI && command.confirmedTick <= input.confirmedTick);
        }
        if (!begin)
            continue;
        const AIStateRequestId request = opportunityAttackChildRequest(expected);
        attackColumns.requestTick[slot] = request.issuedTick;
        attackColumns.requestSequence[slot] = request.sequence;
        projectOpportunityAttackChild(input, slot, wrapper.childTarget[slot]);
        any = true;
    }
    if (!any)
        return true;

    AIAttackStateSoAKernelInput child = input.opportunityAttackChild.bind(
        input.attack, storage.executionSlots());
    child.confirmedTick = input.confirmedTick;
    child.subjects = storage.subjects();
    if (!enterAttackStateSoA(attackColumns, child))
        return false;

    bool immediateTerminal = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (input.opportunityAttackChild.scheduled[slot] == 0)
            continue;
        const auto kind = opportunityAttackChildTerminal(input.opportunityAttackChild.results[slot]);
        if (kind == AIOpportunityAttackMoveChildFeedbackKind::None)
            continue;
        if (!input.opportunityAttackChildFeedback[slot].hasCapacity())
            return false;
        static_cast<void>(input.opportunityAttackChildFeedback[slot].push(
            {.correlation = opportunityAttackChildCorrelation(wrapper, opportunity, slot),
             .kind = kind,
             .confirmedTick = input.confirmedTick}));
        attackColumns.requestTick[slot] = 0;
        attackColumns.requestSequence[slot] = 0;
        immediateTerminal = true;
    }
    if (!immediateTerminal)
        return true;

    AIOpportunityAttackMoveStateSoAKernelInput consume = opportunity;
    consume.scheduled = input.opportunityAttackChild.scheduled;
    return updateOpportunityAttackMoveSoA(wrapper, consume);
}

[[nodiscard]] bool exitOpportunityAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.opportunityAttackChild.aligned(storage.size()))
        return input.opportunityAttackChild.empty() && input.opportunityAttackChildFeedback.empty();
    clearOpportunityAttackChildScratch(input);
    AIOpportunityAttackMoveSoAColumns& wrapper = storage.opportunityAttackMove();
    AIAttackSoAColumns attackColumns = storage.attack().view();
    bool any = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 || wrapper.childActive[slot] == 0 ||
            static_cast<AIOpportunityAttackMoveOperation>(wrapper.childOperation[slot]) !=
                AIOpportunityAttackMoveOperation::Attack ||
            attackColumns.phase[slot] == AIAttackPhase::Inactive)
            continue;
        projectOpportunityAttackChild(input, slot, wrapper.childTarget[slot]);
        any = true;
    }
    if (!any)
        return true;
    AIAttackStateSoAKernelInput child = input.opportunityAttackChild.bind(
        input.attack, storage.executionSlots());
    child.confirmedTick = input.confirmedTick;
    child.subjects = storage.subjects();
    if (!canExitAttackStateSoA(attackColumns, child) || !exitAttackStateSoA(attackColumns, child))
        return false;
    for (const size_t slot : storage.executionSlots())
    {
        if (input.opportunityAttackChild.scheduled[slot] != 0)
        {
            attackColumns.requestTick[slot] = 0;
            attackColumns.requestSequence[slot] = 0;
        }
    }
    return true;
}

[[nodiscard]] bool canExitOpportunityAttackObjectChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.opportunityAttackChild.aligned(storage.size()))
        return input.opportunityAttackChild.empty() && input.opportunityAttackChildFeedback.empty();
    clearOpportunityAttackChildScratch(input);
    const AIOpportunityAttackMoveSoAColumns& wrapper = storage.opportunityAttackMove();
    const AIAttackSoAColumns attackColumns = storage.attack().view();
    bool any = false;
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] == 0 || wrapper.childActive[slot] == 0 ||
            static_cast<AIOpportunityAttackMoveOperation>(wrapper.childOperation[slot]) !=
                AIOpportunityAttackMoveOperation::Attack ||
            attackColumns.phase[slot] == AIAttackPhase::Inactive)
            continue;
        projectOpportunityAttackChild(input, slot, wrapper.childTarget[slot]);
        any = true;
    }
    if (!any)
        return true;
    AIAttackStateSoAKernelInput child = input.opportunityAttackChild.bind(
        input.attack, storage.executionSlots());
    child.confirmedTick = input.confirmedTick;
    child.subjects = storage.subjects();
    return canExitAttackStateSoA(attackColumns, child);
}

[[nodiscard]] bool updateOpportunityPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIOpportunityAttackMoveStateSoAKernelInput& opportunity,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.opportunityAttackChild.aligned(storage.size()))
        return input.opportunityAttackChild.empty() &&
               input.opportunityAttackChildFeedback.empty();
    clearOpportunityAttackChildScratch(input);
    AIOpportunityAttackMoveSoAColumns& wrapper =
        storage.opportunityAttackMove();
    AIMoveToSoAColumns& moveColumns = storage.moveTo();
    bool any = false;
    for (const size_t slot : storage.executionSlots()) {
        if (mask[slot] == 0 || wrapper.childActive[slot] == 0 ||
            static_cast<AIOpportunityAttackMoveOperation>(
                wrapper.childOperation[slot]) !=
                AIOpportunityAttackMoveOperation::PickUpCrate)
            continue;
        const auto correlation = opportunityAttackChildCorrelation(
            wrapper, opportunity, slot);
        const AIMoveToStatePayload payload = moveColumns.load(slot);
        if (payload.request != opportunityAttackChildRequest(correlation))
            continue;
        projectPickUpCrateChild(
            input.opportunityAttackChild, slot,
            wrapper.childTarget[slot], payload.resolvedGoal, true);
        any = true;
    }
    if (!any) return true;
    AIPickUpCrateStateSoAKernelInput child = bindPickUpCrateChild(
        input.opportunityAttackChild, input.pickUpCrate);
    child.confirmedTick = input.confirmedTick;
    if (!updatePickUpCrateSoA(storage, child)) return false;

    bool terminal = false;
    for (const size_t slot : storage.executionSlots()) {
        if (input.opportunityAttackChild.scheduled[slot] == 0 ||
            opportunityAttackChildTerminal(
                input.opportunityAttackChild.results[slot]) ==
                AIOpportunityAttackMoveChildFeedbackKind::None) {
            input.opportunityAttackChild.scheduled[slot] = 0;
            continue;
        }
        if (!input.opportunityAttackChildFeedback[slot].hasCapacity())
            return false;
        terminal = true;
    }
    if (!terminal) return true;
    if (!canExitPickUpCrateSoA(storage, child) ||
        !exitPickUpCrateSoA(storage, child))
        return false;
    for (const size_t slot : storage.executionSlots()) {
        if (input.opportunityAttackChild.scheduled[slot] == 0) continue;
        static_cast<void>(input.opportunityAttackChildFeedback[slot].push({
            .correlation = opportunityAttackChildCorrelation(
                wrapper, opportunity, slot),
            .kind = opportunityAttackChildTerminal(
                input.opportunityAttackChild.results[slot]),
            .confirmedTick = input.confirmedTick,
        }));
        AIMoveToStatePayload cleared = moveColumns.load(slot);
        cleared.request = {};
        cleared.sourceOrderRevision = 0;
        moveColumns.store(slot, cleared);
    }
    return true;
}

[[nodiscard]] bool beginOpportunityPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIOpportunityAttackMoveStateSoAKernelInput& opportunity,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.opportunityAttackChild.aligned(storage.size()))
        return input.opportunityAttackChild.empty() &&
               input.opportunityAttackChildFeedback.empty();
    clearOpportunityAttackChildScratch(input);
    AIOpportunityAttackMoveSoAColumns& wrapper =
        storage.opportunityAttackMove();
    AIMoveToSoAColumns& moveColumns = storage.moveTo();
    auto parameters = storage.parameters();
    bool any = false;
    for (const size_t slot : storage.executionSlots()) {
        if (mask[slot] == 0 || wrapper.childActive[slot] == 0 ||
            static_cast<AIOpportunityAttackMoveOperation>(
                wrapper.childOperation[slot]) !=
                AIOpportunityAttackMoveOperation::PickUpCrate)
            continue;
        const auto expected = opportunityAttackChildCorrelation(
            wrapper, opportunity, slot);
        if (moveColumns.load(slot).request ==
            opportunityAttackChildRequest(expected))
            continue;
        const AIOpportunityAttackMoveChildCommand* begin = nullptr;
        const auto& commands = opportunity.childCommands[slot];
        const size_t count = std::min(commands.count, commands.values.size());
        for (size_t index = 0; index < count; ++index) {
            const auto& command = commands.values[index];
            if (command.kind ==
                    AIOpportunityAttackMoveChildCommandKind::BeginPickUpCrate &&
                command.correlation == expected &&
                command.target == wrapper.childTarget[slot] &&
                command.confirmedTick <= input.confirmedTick) {
                begin = &command;
            }
        }
        if (!begin) continue;
        if (!begin->targetPositionValid) {
            if (!input.opportunityAttackChildFeedback[slot].hasCapacity())
                return false;
            input.opportunityAttackChild.scheduled[slot] = 1;
            input.opportunityAttackChild.states[slot] = AIStateId::Invalid;
            input.opportunityAttackChild.results[slot] =
                AIStateStepResult::failure();
            any = true;
            continue;
        }
        AIStateParameters& parameter = parameters[slot];
        parameter.goalObject = wrapper.childTarget[slot];
        parameter.goalPosition = begin->targetPosition;
        parameter.hasGoalPosition = true;
        parameter.sourceOrderRevision = expected.sourceOrderRevision;
        AIMoveToStatePayload payload{
            opportunityAttackChildRequest(expected)};
        payload.sourceOrderRevision = expected.sourceOrderRevision;
        payload.adjustDestinations = true;
        moveColumns.store(slot, payload);
        projectPickUpCrateChild(input.opportunityAttackChild, slot,
                                wrapper.childTarget[slot],
                                begin->targetPosition, true);
        any = true;
    }
    if (!any) return true;
    for (const size_t slot : storage.executionSlots()) {
        if (input.opportunityAttackChild.states[slot] == AIStateId::Invalid)
            input.opportunityAttackChild.scheduled[slot] = 0;
    }
    AIPickUpCrateStateSoAKernelInput child = bindPickUpCrateChild(
        input.opportunityAttackChild, input.pickUpCrate);
    child.confirmedTick = input.confirmedTick;
    if (!enterPickUpCrateSoA(storage, child)) return false;
    for (const size_t slot : storage.executionSlots()) {
        if (input.opportunityAttackChild.states[slot] == AIStateId::Invalid)
            input.opportunityAttackChild.scheduled[slot] = 1;
    }
    bool immediateTerminal = false;
    for (const size_t slot : storage.executionSlots()) {
        if (input.opportunityAttackChild.scheduled[slot] == 0) continue;
        const auto kind = opportunityAttackChildTerminal(
            input.opportunityAttackChild.results[slot]);
        if (kind == AIOpportunityAttackMoveChildFeedbackKind::None) {
            input.opportunityAttackChild.scheduled[slot] = 0;
            continue;
        }
        if (!input.opportunityAttackChildFeedback[slot].hasCapacity())
            return false;
        static_cast<void>(input.opportunityAttackChildFeedback[slot].push({
            .correlation = opportunityAttackChildCorrelation(
                wrapper, opportunity, slot),
            .kind = kind,
            .confirmedTick = input.confirmedTick,
        }));
        AIMoveToStatePayload cleared = moveColumns.load(slot);
        cleared.request = {};
        cleared.sourceOrderRevision = 0;
        moveColumns.store(slot, cleared);
        immediateTerminal = true;
    }
    if (!immediateTerminal) return true;
    AIOpportunityAttackMoveStateSoAKernelInput consume = opportunity;
    consume.scheduled = input.opportunityAttackChild.scheduled;
    return updateOpportunityAttackMoveSoA(wrapper, consume);
}

[[nodiscard]] bool canExitOpportunityPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept
{
    if (!input.opportunityAttackChild.aligned(storage.size()))
        return input.opportunityAttackChild.empty() &&
               input.opportunityAttackChildFeedback.empty();
    clearOpportunityAttackChildScratch(input);
    const AIOpportunityAttackMoveSoAColumns& wrapper =
        storage.opportunityAttackMove();
    const AIMoveToSoAColumns& moveColumns = storage.moveTo();
    bool any = false;
    for (const size_t slot : storage.executionSlots()) {
        if (mask[slot] == 0 || wrapper.childActive[slot] == 0 ||
            static_cast<AIOpportunityAttackMoveOperation>(
                wrapper.childOperation[slot]) !=
                AIOpportunityAttackMoveOperation::PickUpCrate)
            continue;
        const AIMoveToStatePayload payload = moveColumns.load(slot);
        if (!payload.request.isValid()) continue;
        projectPickUpCrateChild(input.opportunityAttackChild, slot,
                                wrapper.childTarget[slot],
                                payload.resolvedGoal, true);
        any = true;
    }
    if (!any) return true;
    AIPickUpCrateStateSoAKernelInput child = bindPickUpCrateChild(
        input.opportunityAttackChild, input.pickUpCrate);
    child.confirmedTick = input.confirmedTick;
    return canExitPickUpCrateSoA(storage, child);
}

[[nodiscard]] bool exitOpportunityPickUpCrateChildren(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    container::Span<const uint8_t> mask) noexcept
{
    if (!canExitOpportunityPickUpCrateChildren(storage, input, mask))
        return false;
    bool any = false;
    for (const size_t slot : storage.executionSlots())
        any = any || input.opportunityAttackChild.scheduled[slot] != 0;
    if (!any) return true;
    AIPickUpCrateStateSoAKernelInput child = bindPickUpCrateChild(
        input.opportunityAttackChild, input.pickUpCrate);
    child.confirmedTick = input.confirmedTick;
    if (!exitPickUpCrateSoA(storage, child)) return false;
    AIMoveToSoAColumns& moveColumns = storage.moveTo();
    for (const size_t slot : storage.executionSlots()) {
        if (input.opportunityAttackChild.scheduled[slot] == 0) continue;
        AIMoveToStatePayload cleared = moveColumns.load(slot);
        cleared.request = {};
        cleared.sourceOrderRevision = 0;
        moveColumns.store(slot, cleared);
    }
    return true;
}

} // namespace engine::ai::detail
