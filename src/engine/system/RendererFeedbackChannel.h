#pragma once

#include "core/container/container_types.h"
#include "engine/fx/runtime/FxPresentationCommands.h"
#include "engine/renderer/runtime/RendererStats.h"
#include "presentation/render/RenderSceneSnapshot.h"

#include <atomic>
#include <compare>
#include <cstdint>
#include <mutex>
#include <optional>

namespace engine {

// Single owner for render-thread feedback and world-retirement tombstones.
// Publication, retirement and app-side drains share one lock so a retired
// world can never be resurrected by an in-flight render result.
class RendererFeedbackChannel final {
public:
    struct RetirementFloor final {
        uint64_t presentationEpoch = 0;
        uint64_t sessionRevision = 0;
    };

    struct CameraLookup final {
        bool sampled = false;
        std::optional<render::RenderCameraSnapshot> camera;
    };

    void reset() noexcept;
    [[nodiscard]] RetirementFloor retire(
        render::WorldPreparationStamp world) noexcept;

    [[nodiscard]] uint64_t retiredPresentationEpoch() const noexcept;
    [[nodiscard]] uint64_t retiredSessionRevision() const noexcept;
    [[nodiscard]] bool isRetired(
        uint64_t presentationEpoch,
        uint64_t sessionRevision = 0) const noexcept;

    void publish(
        uint64_t presentationEpoch,
        container::Vector<fx::FxSoundCommand> sounds,
        container::Vector<render::RenderAnimationCompletionFeedback>
            animations,
        std::optional<render::WorldFrameRenderStats> stats,
        std::optional<render::RenderViewState> renderView);

    [[nodiscard]] container::Vector<fx::FxSoundCommand> takeFxSounds();
    [[nodiscard]] container::Vector<
        render::RenderAnimationCompletionFeedback>
    takeAnimationCompletions();
    [[nodiscard]] std::optional<render::WorldFrameRenderStats>
    lastWorldFrameStats() const noexcept;
    [[nodiscard]] std::optional<render::RenderViewState>
    lastPresentedRenderView() const noexcept;

    [[nodiscard]] CameraLookup camera(
        uint64_t presentationEpoch) const noexcept;
    [[nodiscard]] bool claimCameraQuery(
        uint64_t presentationEpoch) noexcept;
    void abandonCameraQuery(uint64_t presentationEpoch) noexcept;
    void publishCamera(
        uint64_t presentationEpoch,
        std::optional<render::RenderCameraSnapshot> camera) noexcept;

private:
    struct AnimationAdmissionKey final {
        uint64_t presentationEpoch = 0;
        uint64_t objectId = 0;
        uint32_t channelIndex = 0;

        constexpr auto operator<=>(
            const AnimationAdmissionKey&) const noexcept = default;
    };

    void clearLocked() noexcept;

    mutable std::mutex m_mutex;
    std::atomic<uint64_t> m_retiredPresentationEpoch{0};
    std::atomic<uint64_t> m_retiredSessionRevision{0};
    container::Vector<fx::FxSoundCommand> m_fxSounds;
    container::Vector<render::RenderAnimationCompletionFeedback>
        m_animationCompletions;
    container::TreeMap<
        AnimationAdmissionKey,
        render::RenderAnimationCompletionFeedback>
        m_animationAdmissions;
    std::optional<render::WorldFrameRenderStats> m_worldStats;
    std::optional<render::RenderViewState> m_renderView;
    std::optional<render::RenderCameraSnapshot> m_camera;
    uint64_t m_cameraEpoch = 0;
    uint64_t m_cameraQueryEpoch = 0;
};

} // namespace engine
