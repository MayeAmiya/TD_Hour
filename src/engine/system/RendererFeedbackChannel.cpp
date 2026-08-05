#include "engine/system/RendererFeedbackChannel.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

namespace engine {
namespace {

[[nodiscard]] bool retiredWorldDomain(
    uint64_t presentationEpoch, uint64_t sessionRevision,
    uint64_t retiredPresentationEpoch,
    uint64_t retiredSessionRevision) noexcept {
    return (retiredPresentationEpoch != 0u &&
            presentationEpoch <= retiredPresentationEpoch) ||
        (retiredSessionRevision != 0u && sessionRevision != 0u &&
         sessionRevision <= retiredSessionRevision);
}

void advanceMonotonic(
    std::atomic<uint64_t>& value, uint64_t candidate) noexcept {
    uint64_t current = value.load(std::memory_order_acquire);
    while (candidate > current &&
           !value.compare_exchange_weak(
               current, candidate, std::memory_order_acq_rel,
               std::memory_order_acquire)) {
    }
}

} // namespace

void RendererFeedbackChannel::clearLocked() noexcept {
    m_fxSounds.clear();
    m_animationCompletions.clear();
    m_animationAdmissions.clear();
    m_worldStats.reset();
    m_renderView.reset();
    m_camera.reset();
    m_cameraEpoch = 0;
}

void RendererFeedbackChannel::reset() noexcept {
    std::lock_guard lock(m_mutex);
    m_retiredPresentationEpoch.store(0, std::memory_order_release);
    m_retiredSessionRevision.store(0, std::memory_order_release);
    m_cameraQueryEpoch = 0;
    clearLocked();
}

RendererFeedbackChannel::RetirementFloor RendererFeedbackChannel::retire(
    render::WorldPreparationStamp world) noexcept {
    std::lock_guard lock(m_mutex);
    advanceMonotonic(
        m_retiredPresentationEpoch, world.presentationEpoch);
    advanceMonotonic(
        m_retiredSessionRevision, world.sessionRevision);
    m_cameraQueryEpoch = 0;
    clearLocked();
    return {
        .presentationEpoch = retiredPresentationEpoch(),
        .sessionRevision = retiredSessionRevision(),
    };
}

uint64_t RendererFeedbackChannel::retiredPresentationEpoch() const noexcept {
    return m_retiredPresentationEpoch.load(std::memory_order_acquire);
}

uint64_t RendererFeedbackChannel::retiredSessionRevision() const noexcept {
    return m_retiredSessionRevision.load(std::memory_order_acquire);
}

bool RendererFeedbackChannel::isRetired(
    uint64_t presentationEpoch, uint64_t sessionRevision) const noexcept {
    return retiredWorldDomain(
        presentationEpoch, sessionRevision,
        retiredPresentationEpoch(), retiredSessionRevision());
}

void RendererFeedbackChannel::publish(
    uint64_t presentationEpoch,
    container::Vector<fx::FxSoundCommand> sounds,
    container::Vector<render::RenderAnimationCompletionFeedback> animations,
    std::optional<render::WorldFrameRenderStats> stats,
    std::optional<render::RenderViewState> renderView) {
    std::lock_guard lock(m_mutex);
    const uint64_t retiredEpoch = retiredPresentationEpoch();
    const uint64_t retiredSession = retiredSessionRevision();
    if (presentationEpoch == 0u ||
        (retiredEpoch != 0u && presentationEpoch <= retiredEpoch)) {
        sounds.clear();
    }
    std::erase_if(
        animations,
        [retiredEpoch](const auto& completion) {
            return retiredEpoch != 0u &&
                completion.presentationEpoch <= retiredEpoch;
        });
    if (stats && retiredWorldDomain(
            stats->presentationEpoch, stats->sessionRevision,
            retiredEpoch, retiredSession)) {
        stats.reset();
    }
    if (renderView && retiredWorldDomain(
            renderView->sourceWorld.presentationEpoch,
            renderView->sourceWorld.sessionRevision,
            retiredEpoch, retiredSession)) {
        renderView.reset();
    }

    m_fxSounds.insert(
        m_fxSounds.end(),
        std::make_move_iterator(sounds.begin()),
        std::make_move_iterator(sounds.end()));
    for (auto& feedback : animations) {
        if (feedback.kind !=
            render::RenderAnimationFeedbackKind::EndpointPublished) {
            m_animationCompletions.push_back(std::move(feedback));
            continue;
        }
        const AnimationAdmissionKey key{
            .presentationEpoch = feedback.presentationEpoch,
            .objectId = feedback.objectId,
            .channelIndex = feedback.channelIndex,
        };
        auto [entry, inserted] =
            m_animationAdmissions.try_emplace(key, feedback);
        if (!inserted &&
            (feedback.generation > entry->second.generation ||
             (feedback.generation == entry->second.generation &&
              feedback.simulationFrame >
                  entry->second.simulationFrame))) {
            entry->second = std::move(feedback);
        }
    }
    constexpr size_t kMaximumAnimationCompletionFeedback = 4096;
    if (m_animationCompletions.size() >
        kMaximumAnimationCompletionFeedback) {
        m_animationCompletions.erase(
            m_animationCompletions.begin(),
            m_animationCompletions.end() -
                static_cast<std::ptrdiff_t>(
                    kMaximumAnimationCompletionFeedback));
    }
    m_worldStats = std::move(stats);
    m_renderView = std::move(renderView);
}

container::Vector<fx::FxSoundCommand>
RendererFeedbackChannel::takeFxSounds() {
    std::lock_guard lock(m_mutex);
    container::Vector<fx::FxSoundCommand> result;
    result.swap(m_fxSounds);
    return result;
}

container::Vector<render::RenderAnimationCompletionFeedback>
RendererFeedbackChannel::takeAnimationCompletions() {
    std::lock_guard lock(m_mutex);
    container::Vector<render::RenderAnimationCompletionFeedback> result;
    result.reserve(
        m_animationAdmissions.size() + m_animationCompletions.size());
    for (auto& [key, admission] : m_animationAdmissions) {
        static_cast<void>(key);
        result.push_back(std::move(admission));
    }
    m_animationAdmissions.clear();
    result.insert(
        result.end(),
        std::make_move_iterator(m_animationCompletions.begin()),
        std::make_move_iterator(m_animationCompletions.end()));
    m_animationCompletions.clear();
    return result;
}

std::optional<render::WorldFrameRenderStats>
RendererFeedbackChannel::lastWorldFrameStats() const noexcept {
    std::lock_guard lock(m_mutex);
    if (!m_worldStats || isRetired(
            m_worldStats->presentationEpoch,
            m_worldStats->sessionRevision)) {
        return std::nullopt;
    }
    return m_worldStats;
}

std::optional<render::RenderViewState>
RendererFeedbackChannel::lastPresentedRenderView() const noexcept {
    std::lock_guard lock(m_mutex);
    if (!m_renderView || isRetired(
            m_renderView->sourceWorld.presentationEpoch,
            m_renderView->sourceWorld.sessionRevision)) {
        return std::nullopt;
    }
    return m_renderView;
}

RendererFeedbackChannel::CameraLookup RendererFeedbackChannel::camera(
    uint64_t presentationEpoch) const noexcept {
    std::lock_guard lock(m_mutex);
    if (presentationEpoch == 0u || isRetired(presentationEpoch)) return {};
    return m_cameraEpoch == presentationEpoch
        ? CameraLookup{.sampled = true, .camera = m_camera}
        : CameraLookup{};
}

bool RendererFeedbackChannel::claimCameraQuery(
    uint64_t presentationEpoch) noexcept {
    std::lock_guard lock(m_mutex);
    if (presentationEpoch == 0u || isRetired(presentationEpoch) ||
        m_cameraEpoch == presentationEpoch ||
        m_cameraQueryEpoch >= presentationEpoch) {
        return false;
    }
    m_cameraQueryEpoch = presentationEpoch;
    return true;
}

void RendererFeedbackChannel::abandonCameraQuery(
    uint64_t presentationEpoch) noexcept {
    std::lock_guard lock(m_mutex);
    if (m_cameraQueryEpoch == presentationEpoch &&
        m_cameraEpoch != presentationEpoch) {
        m_cameraQueryEpoch = 0;
    }
}

void RendererFeedbackChannel::publishCamera(
    uint64_t presentationEpoch,
    std::optional<render::RenderCameraSnapshot> cameraValue) noexcept {
    std::lock_guard lock(m_mutex);
    if (presentationEpoch == 0u || isRetired(presentationEpoch)) return;
    m_cameraEpoch = presentationEpoch;
    m_camera = std::move(cameraValue);
}

} // namespace engine
