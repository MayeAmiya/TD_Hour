#include "game/object/ai/runtime/ObjectAIShadowBatch.h"
#include <algorithm>

namespace engine::ai
{

[[nodiscard]] ObjectAIShadowBatchOutputs ObjectAIShadowBatch::outputs() noexcept
{
    return {
        .facingCommands = m_facingCommands,
        .pathRequests = m_pathRequests,
        .movementCommands = m_movementCommands,
        .moveEvacuateCommands = m_moveEvacuateCommands,
        .teamProgressRequests = m_teamProgressRequests,
        .waypointCompletions = m_waypointCompletions,
        .containmentCommands = m_containmentCommands,
        .hackCommands = m_hackCommands,
        .attackCommands = m_attackCommands,
        .dockRequests = m_dockRequests,
        .insertionMotionCommands = m_insertionMotionCommands,
        .insertionContainmentCommands = m_insertionContainmentCommands,
        .insertionOperationCommands = m_insertionOperationCommands,
        .insertionEffectCommands = m_insertionEffectCommands,
        .guardTacticalCommands = m_guardTacticalCommands,
        .guardInteractionCommands = m_guardInteractionCommands,
        .tacticalQueryCommands = m_tacticalQueryCommands,
        .tacticalChildCommands = m_tacticalChildCommands,
        .opportunityQueryCommands = m_opportunityQueryCommands,
        .opportunityChildCommands = m_opportunityChildCommands,
    };
}

[[nodiscard]] ObjectAIShadowBatchStatus ObjectAIShadowBatch::stageTransitionRequest(
    const AIStateSoATransitionRequest& request)
{
    if (!m_initialized)
        return ObjectAIShadowBatchStatus::NotInitialized;
    if (m_transitionRequests.size() >= m_capacity * TransitionRequestsPerSlot)
        return ObjectAIShadowBatchStatus::CapacityExceeded;
    m_transitionRequests.push_back(request);
    return ObjectAIShadowBatchStatus::Success;
}

[[nodiscard]] AIStateSoAMultiwaveScratch ObjectAIShadowBatch::scratch() noexcept
{
    return {.results = m_results,
            .actionMask = m_actionMask,
            .exitMask = m_exitMask,
            .enterMask = m_enterMask,
            .transitionEntries = m_transitionEntries};
}

bool ObjectAIShadowBatch::hasRunnableWork() const noexcept
{
    return !m_transitionRequests.empty() ||
        std::any_of(m_scheduled.begin(), m_scheduled.end(),
                    [](uint8_t value) { return value != 0; }) ||
        std::any_of(m_exitMask.begin(), m_exitMask.end(),
                    [](uint8_t value) { return value != 0; });
}

void ObjectAIShadowBatch::clearTransientOutputs(bool clearTransitionRequests) noexcept
{
    clearBuffers(m_facingCommands);
    clearBuffers(m_pathRequests);
    clearBuffers(m_movementCommands);
    clearBuffers(m_moveEvacuateCommands);
    clearBuffers(m_teamProgressRequests);
    clearBuffers(m_waypointCompletions);
    clearBuffers(m_containmentCommands);
    clearBuffers(m_hackCommands);
    clearBuffers(m_attackCommands);
    clearBuffers(m_dockRequests);
    clearBuffers(m_insertionMotionCommands);
    clearBuffers(m_insertionContainmentCommands);
    clearBuffers(m_insertionOperationCommands);
    clearBuffers(m_insertionEffectCommands);
    clearBuffers(m_guardTacticalCommands);
    clearBuffers(m_guardInteractionCommands);
    clearBuffers(m_tacticalQueryCommands);
    clearBuffers(m_tacticalChildCommands);
    clearBuffers(m_opportunityQueryCommands);
    clearBuffers(m_opportunityChildCommands);
    if (clearTransitionRequests)
        m_transitionRequests.clear();
    std::fill(m_results.begin(), m_results.end(), AIStateStepResult::continueState());
    std::fill(m_actionMask.begin(), m_actionMask.end(), uint8_t{0});
    std::fill(m_enterMask.begin(), m_enterMask.end(), uint8_t{0});
    std::fill(m_transitionEntries.begin(), m_transitionEntries.end(), AIStateSoATransitionEntry{});
}

void ObjectAIShadowBatch::clearFeedback() noexcept
{
    std::fill(m_facingFeedback.begin(), m_facingFeedback.end(), AIFacingFeedback{});
    std::fill(m_pathFeedback.begin(), m_pathFeedback.end(), PathFeedback{});
    std::fill(m_movementFeedback.begin(), m_movementFeedback.end(), MovementFeedback{});
    clearBuffers(m_containmentFeedback);
    clearBuffers(m_attackFeedback);
    clearBuffers(m_dockFeedback);
    clearBuffers(m_insertionMotionFeedback);
    clearBuffers(m_insertionContainmentFeedback);
    clearBuffers(m_insertionOperationFeedback);
    clearBuffers(m_guardFeedback);
    clearBuffers(m_tacticalQueryFeedback);
    clearBuffers(m_tacticalChildFeedback);
    clearBuffers(m_opportunityQueryFeedback);
    clearBuffers(m_opportunityChildFeedback);
    std::fill(m_opportunityMovementResults.begin(),
              m_opportunityMovementResults.end(),
              AIStateStepResult::continueState());
}

void ObjectAIShadowBatch::clearDeferredExitState() noexcept
{
    std::fill(m_exitMask.begin(), m_exitMask.end(), uint8_t{0});
}

} // namespace engine::ai
