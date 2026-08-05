#include "game/session/frame/GameSessionFramePort.h"

#include "game/object/component/ObjectDirty.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/render/VisualAnimationState.h"
#include "game/session/presentation/GameSessionPresentationDetail.h"
#include "game/session/script/GameSessionScriptFrameTransactions.h"
#include "game/session/state/GameSessionDomainState.h"
#include "presentation/render/RenderWorldDescriptorContracts.h"

#include <algorithm>
#include <utility>

namespace engine {

bool GameSessionFramePort::simulationTimeFrozen() const noexcept {
    if (!m_content.m_active || m_content.m_startInfo.network.enabled) {
        return false;
    }
    return m_presentation.m_scriptTimeFrozen ||
        (m_presentation.m_scriptCamera.timeFreezeArmed() &&
         m_presentation.m_scriptCameraCompletedRevision <
             m_presentation.m_scriptCameraMovementRevision);
}

bool GameSessionFramePort::begin(
    uint64_t confirmedFrame,
    bool worldFrozen) noexcept {
    const uint32_t pendingMask = m_frame.m_pendingDegradationMask;
    const uint32_t pendingCount = m_frame.m_pendingDegradationCount;
    const SimulationFault pendingFault = m_frame.m_pendingFault;
    const uint32_t pendingAdditionalFaultCount =
        m_frame.m_pendingAdditionalFaultCount;
    m_frame.m_pendingDegradationMask = 0;
    m_frame.m_pendingDegradationCount = 0;
    m_frame.m_pendingFault = {};
    m_frame.m_pendingAdditionalFaultCount = 0;

    if (m_frame.m_open) {
        m_frame.m_result = {
            .state = FrameCommitState::Faulted,
            .confirmedTick = confirmedFrame,
            .degradationMask = pendingMask,
            .degradationCount = pendingCount,
            .fault = {
                .domain = SimulationFaultDomain::FrameIngress,
                .code = SimulationFaultCode::UnfinishedPriorFrame,
                .confirmedTick = confirmedFrame,
            },
        };
        m_frame.m_open = false;
        return false;
    }

    m_frame.m_result = {
        .state = FrameCommitState::Open,
        .confirmedTick = confirmedFrame,
        .degradationMask = pendingMask,
        .degradationCount = pendingCount,
        .additionalFaultCount = pendingAdditionalFaultCount,
        .fault = pendingFault,
    };
    m_frame.m_open = true;
    if (pendingFault) {
        static_cast<void>(complete());
        return false;
    }
    const bool invalidWorldFrame = worldFrozen
        ? (!m_presentation.m_hasConfirmedFrame ||
           confirmedFrame != m_presentation.m_confirmedTick)
        : (m_presentation.m_hasConfirmedFrame &&
           confirmedFrame <= m_presentation.m_confirmedTick);
    if (!m_content.m_active || confirmedFrame == 0 || invalidWorldFrame) {
        static_cast<void>(m_publication.raiseSimulationFault({
            .domain = SimulationFaultDomain::FrameIngress,
            .code = SimulationFaultCode::InvalidConfirmedTick,
            .confirmedTick = confirmedFrame,
        }));
        static_cast<void>(complete());
        return false;
    }
    if (!m_presentation.m_fxInvocations.beginConfirmedFrame(
            confirmedFrame, worldFrozen)) {
        static_cast<void>(m_publication.raiseSimulationFault({
            .domain = SimulationFaultDomain::PresentationClock,
            .code = SimulationFaultCode::PresentationFrameRejected,
            .confirmedTick = confirmedFrame,
        }));
        static_cast<void>(complete());
        return false;
    }
    m_presentation.m_confirmedTick = confirmedFrame;
    m_presentation.m_hasConfirmedFrame = true;
    m_objectEvents.m_frameLifecycleEvents.clear();
    m_objectEvents.m_frameHealthEvents.clear();
    drainVisualAnimationCompletions();
    game_session_presentation_detail::advanceScreenFadeState(
        m_presentation.m_scriptScreenFadePresentation);
    static_cast<void>(
        m_presentation.m_scriptMapPresentation.advanceRadarEvents(
            confirmedFrame));
    m_presentation.m_scriptObjectPresentation.advance(confirmedFrame);
    if (!worldFrozen) {
        m_world.m_objectTeams.beginConfirmedTick(confirmedFrame);
    }
    m_presentation.m_audioJournal.beginConfirmedFrame(
        confirmedFrame, !worldFrozen);
    return true;
}

const FrameCommitResult& GameSessionFramePort::result() const noexcept {
    return m_frame.m_result;
}

void GameSessionFramePort::noteCommandOutcome(
    bool accepted,
    uint64_t count) noexcept {
    if (count == 0 || !m_frame.m_open) return;
    uint32_t& destination = accepted
        ? m_frame.m_result.acceptedCommandCount
        : m_frame.m_result.rejectedCommandCount;
    destination = game_session_presentation_detail::saturatingFrameCount(
        destination, count);
}

void GameSessionFramePort::noteDeferredCommands(uint64_t count) noexcept {
    if (count == 0 || !m_frame.m_open) return;
    m_frame.m_result.deferredCommandCount =
        game_session_presentation_detail::saturatingFrameCount(
            m_frame.m_result.deferredCommandCount, count);
}

void GameSessionFramePort::noteDegradation(
    FrameDegradation degradation, uint64_t count) noexcept {
    if (degradation == FrameDegradation::None || count == 0) return;
    if (m_frame.m_open) {
        m_frame.m_result.degradationMask |= frameDegradationBit(degradation);
        m_frame.m_result.degradationCount =
            game_session_presentation_detail::saturatingFrameCount(
                m_frame.m_result.degradationCount, count);
        return;
    }
    m_frame.m_pendingDegradationMask |= frameDegradationBit(degradation);
    m_frame.m_pendingDegradationCount =
        game_session_presentation_detail::saturatingFrameCount(
            m_frame.m_pendingDegradationCount, count);
}

script::ScriptRuntimeStepResult GameSessionFramePort::advanceScripts(
    uint64_t confirmedInputTick,
    container::Span<const ObjectId> localSelection,
    uint64_t worldConfirmedTick) {
    if (!m_scriptRuntime) return {};
    return m_scriptRuntime->advance(
        confirmedInputTick, localSelection, worldConfirmedTick);
}

container::Vector<script::ScriptForceObjectSelectionPresentation>
GameSessionFramePort::takeForceSelectionPresentations() {
    auto output = std::move(m_presentation.m_scriptForceObjectSelectionRequests);
    m_presentation.m_scriptForceObjectSelectionRequests.clear();
    return output;
}

container::Vector<script::ScriptMoveCameraToSelectionPresentation>
GameSessionFramePort::takeSelectionCameraPresentations() {
    auto output = std::move(m_presentation.m_scriptMoveCameraToSelectionRequests);
    m_presentation.m_scriptMoveCameraToSelectionRequests.clear();
    return output;
}

container::Vector<CommandBackendOutcome>
GameSessionFramePort::takeBackendOutcomes() {
    auto output = std::move(m_presentation.m_commandBackendOutcomes);
    m_presentation.m_commandBackendOutcomes.clear();
    return output;
}

container::Vector<ObjectLifecycleEvent>
GameSessionFramePort::lifecyclePresentationEvents(size_t offset) const {
    const auto& events = m_objectEvents.m_frameLifecycleEvents;
    if (offset >= events.size()) return {};
    container::Vector<ObjectLifecycleEvent> result;
    result.assign(
        events.begin() + static_cast<std::ptrdiff_t>(offset), events.end());
    return result;
}

FrameCommitResult GameSessionFramePort::complete(bool worldFrozen) noexcept {
    if (!m_frame.m_open) return m_frame.m_result;
    m_frame.m_result.state = m_frame.m_result.fault
        ? FrameCommitState::Faulted
        : worldFrozen ? FrameCommitState::Frozen
                      : FrameCommitState::Committed;
    if (m_frame.m_result.state == FrameCommitState::Faulted) {
        m_objectEvents.m_frameLifecycleEvents.clear();
        m_objectEvents.m_frameHealthEvents.clear();
    }
    m_frame.m_open = false;
    return m_frame.m_result;
}

void GameSessionFramePort::drainVisualAnimationCompletions() {
    if (m_presentation.m_pendingVisualAnimationAdmissions.empty() &&
        m_presentation.m_pendingVisualAnimationCompletions.empty()) {
        return;
    }
    auto admissions =
        std::move(m_presentation.m_pendingVisualAnimationAdmissions);
    m_presentation.m_pendingVisualAnimationAdmissions.clear();
    for (const auto& [key, admission] : admissions) {
        static_cast<void>(key);
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(admission.object);
        if (!entity) continue;
        RenderModelComponent* visual = ecs::try_get<RenderModelComponent>(
            m_world.m_registry, *entity);
        if (!visual) continue;
        if (applyVisualAnimationEndpointAdmission(
                *visual, admission.channelIndex, admission.generation)) {
            markObjectDirty(
                m_world.m_registry, *entity,
                ObjectDirtyDomain::RenderExtraction);
        }
    }
    if (m_presentation.m_pendingVisualAnimationCompletions.empty()) return;

    using PendingCompletion =
        GameSessionScriptPresentationState::PendingVisualAnimationCompletion;
    container::Vector<PendingCompletion> pending =
        std::move(m_presentation.m_pendingVisualAnimationCompletions);
    m_presentation.m_pendingVisualAnimationCompletions.clear();
    std::stable_sort(
        pending.begin(), pending.end(),
        [](const PendingCompletion& left, const PendingCompletion& right) {
            if (left.object != right.object)
                return left.object.value < right.object.value;
            if (left.channelIndex != right.channelIndex)
                return left.channelIndex < right.channelIndex;
            if (left.generation != right.generation)
                return left.generation < right.generation;
            if (left.phase != right.phase) return left.phase < right.phase;
            return left.kind < right.kind;
        });
    for (const PendingCompletion& completion : pending) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(completion.object);
        if (!entity) continue;
        RenderModelComponent* visual = ecs::try_get<RenderModelComponent>(
            m_world.m_registry, *entity);
        if (!visual) continue;
        if (applyVisualAnimationEndpointAdmission(
                *visual, completion.channelIndex, completion.generation)) {
            markObjectDirty(
                m_world.m_registry, *entity,
                ObjectDirtyDomain::RenderExtraction);
        }
        const ThingTemplateComponent* source =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        const game::ThingTemplate* templateData = source && source->archetype
            ? &source->archetype->templateData
            : nullptr;
        VisualAnimationCompletionPhase phase;
        switch (static_cast<render::RenderAnimationCompletionPhase>(
            completion.phase)) {
        case render::RenderAnimationCompletionPhase::PresentedSource:
            phase = VisualAnimationCompletionPhase::PresentedSource;
            break;
        case render::RenderAnimationCompletionPhase::Transition:
            phase = VisualAnimationCompletionPhase::Transition;
            break;
        case render::RenderAnimationCompletionPhase::ActiveState:
            phase = VisualAnimationCompletionPhase::ActiveState;
            break;
        default:
            continue;
        }
        const auto feedbackKind =
            static_cast<render::RenderAnimationFeedbackKind>(completion.kind);
        if (feedbackKind ==
                render::RenderAnimationFeedbackKind::ResourcePending ||
            feedbackKind ==
                render::RenderAnimationFeedbackKind::ResourceReady) {
            if (applyVisualAnimationResourceGate(
                    *visual, completion.channelIndex, completion.generation,
                    phase,
                    feedbackKind ==
                        render::RenderAnimationFeedbackKind::ResourcePending)) {
                markObjectDirty(
                    m_world.m_registry, *entity,
                    ObjectDirtyDomain::RenderExtraction);
            }
            continue;
        }
        if (applyVisualAnimationCompletion(
                *visual,
                {
                    .channelIndex = completion.channelIndex,
                    .generation = completion.generation,
                    .phase = phase,
                    .completedDurationSeconds =
                        completion.completedDurationSeconds,
                },
                m_presentation.m_confirmedTick, templateData,
                completion.object.value)) {
            markObjectDirty(
                m_world.m_registry, *entity,
                ObjectDirtyDomain::RenderExtraction);
        }
    }
}

} // namespace engine
