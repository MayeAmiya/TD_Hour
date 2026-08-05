#include "game/session/transaction/GameSessionAINavigationFrameTransactions.h"

#include "game/session/state/GameSessionDomainState.h"
#include "debug/debug.h"

#include <limits>

namespace engine {
namespace {

// The adapter reports capacity synchronously, before NavigationPathService can
// publish its normal Delayed feedback. Keep this boundary on that service's
// deterministic four-tick retry cadence so every retained correlation yields
// and later retries rather than recontending each confirmed tick.
constexpr uint64_t CapacityRetryDelayTicks = 4;

[[nodiscard]] uint64_t capacityRetryEligibleTick(
    uint64_t confirmedTick) noexcept {
    constexpr uint64_t MaxTick = std::numeric_limits<uint64_t>::max();
    return confirmedTick > MaxTick - CapacityRetryDelayTicks
        ? MaxTick
        : confirmedTick + CapacityRetryDelayTicks;
}

} // namespace

void GameSessionAINavigationFrameTransactions::pollFeedback() {
    ai::ObjectAITransientStore& transients = m_ai.m_objectAI.transients();
    size_t index = 0;
    while (true) {
        const auto requests = transients.pathRequests();
        const auto submitted = transients.pathRequestSubmitted();
        if (index >= requests.size()) break;
        if (submitted[index] == 0) {
            ++index;
            continue;
        }
        if (!transients.canStagePathFeedback()) break;

        const ai::PathCorrelation correlation = requests[index].correlation;
        ai::PathFeedback feedback;
        if (!m_navigation.poll(
                correlation, m_presentation.m_confirmedTick, feedback)) {
            ++index;
            continue;
        }
        if (feedback.status == ai::PathFeedbackStatus::NoPath &&
            feedback.blockingBridge) {
            const PlayerId owner = m_world.m_ownership
                .ownerOf(correlation.subject)
                .value_or(INVALID_PLAYER_ID);
            static_cast<void>(m_ai.m_strategicAI.observeBlockingBridge(
                owner, feedback.blockingBridge,
                m_presentation.m_confirmedTick));
        }
        const ai::ObjectAITransientStatus staged = transients.stage(feedback);
        if (staged != ai::ObjectAITransientStatus::Success) {
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::Feedback,
                .code = staged == ai::ObjectAITransientStatus::CapacityExceeded
                    ? SimulationFaultCode::CapacityExceeded
                    : SimulationFaultCode::InvalidEvent,
                .confirmedTick = m_presentation.m_confirmedTick,
                .subject = correlation.subject.value,
            }));
            TD_LOG_ERROR(
                "[GameSession] Object AI rejected polled path feedback: "
                "subject={} tick={} status={}",
                correlation.subject.value, m_presentation.m_confirmedTick,
                static_cast<uint32_t>(staged));
            return;
        }
        if (feedback.status == ai::PathFeedbackStatus::Delayed) {
            if (transients.deferPathRequest(
                    correlation, feedback.nextEligibleTick,
                    m_presentation.m_confirmedTick)) {
                ++index;
                continue;
            }
            TD_LOG_ERROR(
                "[GameSession] Invalid delayed path retry boundary: "
                "subject={} tick={} nextEligibleTick={}",
                correlation.subject.value, m_presentation.m_confirmedTick,
                feedback.nextEligibleTick);
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::Navigation,
                .code = SimulationFaultCode::AcknowledgementLost,
                .confirmedTick = m_presentation.m_confirmedTick,
                .subject = correlation.subject.value,
            }));
            return;
        }
        static_cast<void>(transients.removePathRequest(correlation));
    }
}

void GameSessionAINavigationFrameTransactions::submitRequests() {
    ai::ObjectAITransientStore& transients = m_ai.m_objectAI.transients();
    size_t index = 0;
    while (true) {
        const auto requests = transients.pathRequests();
        const auto submitted = transients.pathRequestSubmitted();
        const auto nextEligibleTicks = transients.pathRequestNextEligibleTicks();
        if (index >= requests.size()) break;
        if (submitted[index] != 0) {
            ++index;
            continue;
        }
        if (nextEligibleTicks[index] != 0 &&
            m_presentation.m_confirmedTick < nextEligibleTicks[index]) {
            ++index;
            continue;
        }

        const ai::PathRequest request = requests[index];
        const navigation::NavigationAdapterSubmitResult result =
            m_navigation.submit(
                request, m_presentation.m_confirmedTick);
        switch (result) {
            case navigation::NavigationAdapterSubmitResult::Accepted:
            case navigation::NavigationAdapterSubmitResult::Replaced:
            case navigation::NavigationAdapterSubmitResult::Cancelled:
                if (!transients.markPathRequestSubmitted(request.correlation)) {
                    static_cast<void>(m_publication.raiseSimulationFault({
                        .domain = SimulationFaultDomain::Navigation,
                        .code = SimulationFaultCode::AcknowledgementLost,
                        .confirmedTick = m_presentation.m_confirmedTick,
                        .subject = request.correlation.subject.value,
                    }));
                    TD_LOG_ERROR(
                        "[GameSession] Navigation accepted a path request whose "
                        "AI ownership marker was lost: subject={} tick={}",
                        request.correlation.subject.value,
                        m_presentation.m_confirmedTick);
                    return;
                }
                ++index;
                break;
            case navigation::NavigationAdapterSubmitResult::CapacityExceeded: {
                const uint64_t nextEligibleTick = capacityRetryEligibleTick(
                    m_presentation.m_confirmedTick);
                // Retain both parts of the normal delayed protocol: the AI
                // state observes Delayed, while the correlation itself stays
                // in the transient store until this exact retry boundary.
                if (!transients.deferPathRequest(
                        request.correlation, nextEligibleTick,
                        m_presentation.m_confirmedTick)) {
                    static_cast<void>(m_publication.raiseSimulationFault({
                        .domain = SimulationFaultDomain::Navigation,
                        .code = SimulationFaultCode::AcknowledgementLost,
                        .confirmedTick = m_presentation.m_confirmedTick,
                        .subject = request.correlation.subject.value,
                    }));
                    TD_LOG_ERROR(
                        "[GameSession] Navigation capacity retry lost its "
                        "Object AI correlation: subject={} tick={}",
                        request.correlation.subject.value,
                        m_presentation.m_confirmedTick);
                    return;
                }
                ai::PathFeedback feedback;
                feedback.correlation = request.correlation;
                feedback.status = ai::PathFeedbackStatus::Delayed;
                feedback.confirmedTick = m_presentation.m_confirmedTick;
                feedback.nextEligibleTick = nextEligibleTick;
                const ai::ObjectAITransientStatus staged =
                    transients.stage(feedback);
                if (staged != ai::ObjectAITransientStatus::Success) {
                    static_cast<void>(m_publication.raiseSimulationFault({
                        .domain = SimulationFaultDomain::Feedback,
                        .code = staged ==
                                ai::ObjectAITransientStatus::CapacityExceeded
                            ? SimulationFaultCode::CapacityExceeded
                            : SimulationFaultCode::InvalidEvent,
                        .confirmedTick = m_presentation.m_confirmedTick,
                        .subject = request.correlation.subject.value,
                    }));
                    return;
                }
                ++index;
                break;
            }
            case navigation::NavigationAdapterSubmitResult::InvalidRequest:
            case navigation::NavigationAdapterSubmitResult::StaleCorrelation:
            case navigation::NavigationAdapterSubmitResult::NotFound: {
                if (!transients.canStagePathFeedback()) {
                    ++index;
                    break;
                }
                ai::PathFeedback feedback;
                feedback.correlation = request.correlation;
                feedback.status = result ==
                        navigation::NavigationAdapterSubmitResult::InvalidRequest
                    ? ai::PathFeedbackStatus::Unsupported
                    : ai::PathFeedbackStatus::Cancelled;
                feedback.confirmedTick = m_presentation.m_confirmedTick;
                const ai::ObjectAITransientStatus staged =
                    transients.stage(feedback);
                if (staged != ai::ObjectAITransientStatus::Success) {
                    static_cast<void>(m_publication.raiseSimulationFault({
                        .domain = SimulationFaultDomain::Feedback,
                        .code = staged ==
                                ai::ObjectAITransientStatus::CapacityExceeded
                            ? SimulationFaultCode::CapacityExceeded
                            : SimulationFaultCode::InvalidEvent,
                        .confirmedTick = m_presentation.m_confirmedTick,
                        .subject = request.correlation.subject.value,
                    }));
                    ++index;
                    return;
                }
                static_cast<void>(
                    transients.removePathRequest(request.correlation));
                break;
            }
        }
    }
}

} // namespace engine
