#include "engine/renderer/world/pipeline/WorldRenderPipeline.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::render {
namespace {

// Bounds for the distanceCovered-derived playback multiplier. RefCode leaves
// the ratio unclamped, but the published duration is authored content divided
// by a live speed, so both ends are fenced: the low end keeps a crawling unit
// from freezing its clip into an effectively infinite completion time, and the
// high end stops a mistyped one-unit distanceCovered from spinning a clip at
// thousands of times its authored rate. Anything outside the bound is content
// error, not gameplay.
constexpr float kMinimumMovementSyncAnimationRate = 0.01f;
constexpr float kMaximumMovementSyncAnimationRate = 100.0f;
// Mirrors the extraction-side cap on the published duration. Re-checked here
// because the renderer must never trust a snapshot field's magnitude.
constexpr float kMaximumMovementSyncDurationSeconds = 60.0f;

[[nodiscard]] double recoilDrivenDistance(
    const RenderWeaponImpulse& recoil, uint64_t drivenSteps) noexcept {
    if (drivenSteps == 0u || recoil.initialSpeed <= 0.0f) return 0.0;

    const double initialSpeed = static_cast<double>(recoil.initialSpeed);
    const double damping = static_cast<double>(recoil.damping);
    if (damping <= 0.0) return initialSpeed;
    if (damping >= 1.0) {
        return initialSpeed * static_cast<double>(drivenSteps);
    }
    return initialSpeed *
        (1.0 - std::pow(damping, static_cast<double>(drivenSteps))) /
        (1.0 - damping);
}

[[nodiscard]] uint64_t recoilDistanceTransitionStep(
    const RenderWeaponImpulse& recoil) noexcept {
    constexpr uint64_t kNoTransition = UINT64_MAX;
    if (recoil.maximumDistance <= 0.0f) return 1u;
    if (recoil.initialSpeed <= 0.0f) return kNoTransition;
    if (recoil.initialSpeed >= recoil.maximumDistance) return 1u;

    const double initialSpeed = static_cast<double>(recoil.initialSpeed);
    const double maximumDistance = static_cast<double>(recoil.maximumDistance);
    const double damping = static_cast<double>(recoil.damping);
    if (damping <= 0.0) return kNoTransition;
    if (damping >= 1.0) {
        return static_cast<uint64_t>(std::ceil(maximumDistance / initialSpeed));
    }

    const double asymptoticDistance = initialSpeed / (1.0 - damping);
    if (maximumDistance >= asymptoticDistance) return kNoTransition;
    const double remainingFraction =
        1.0 - maximumDistance / asymptoticDistance;
    uint64_t step = static_cast<uint64_t>(std::max(
        1.0, std::ceil(std::log(remainingFraction) / std::log(damping))));
    while (step > 1u &&
           recoilDrivenDistance(recoil, step - 1u) >= maximumDistance) {
        --step;
    }
    while (recoilDrivenDistance(recoil, step) < maximumDistance) ++step;
    return step;
}

[[nodiscard]] uint64_t recoilRateTransitionStep(
    const RenderWeaponImpulse& recoil) noexcept {
    constexpr uint64_t kNoTransition = UINT64_MAX;
    constexpr double kMinimumDrivenRate = 0.01;
    const double initialSpeed = static_cast<double>(recoil.initialSpeed);
    const double damping = static_cast<double>(recoil.damping);
    if (initialSpeed * damping < kMinimumDrivenRate) return 1u;
    if (damping >= 1.0) return kNoTransition;
    if (damping <= 0.0) return 1u;

    const double realStep =
        std::log(kMinimumDrivenRate / initialSpeed) / std::log(damping);
    uint64_t step = static_cast<uint64_t>(std::max(
        1.0, std::floor(realStep) + 1.0));
    const auto rateAfter = [initialSpeed, damping](uint64_t drivenSteps) {
        return initialSpeed *
            std::pow(damping, static_cast<double>(drivenSteps));
    };
    while (step > 1u && rateAfter(step - 1u) < kMinimumDrivenRate) --step;
    while (rateAfter(step) >= kMinimumDrivenRate) ++step;
    return step;
}

[[nodiscard]] float weaponRecoilShift(
    const RenderWeaponImpulse& recoil, uint64_t ageFrames) noexcept {
    constexpr uint64_t kMaximumRecoilSteps = 512u;
    const uint64_t steps = ageFrames >= kMaximumRecoilSteps - 1u
        ? kMaximumRecoilSteps
        : ageFrames + 1u;
    const uint64_t transitionStep = std::min(
        recoilDistanceTransitionStep(recoil), recoilRateTransitionStep(recoil));
    if (transitionStep > steps) {
        return static_cast<float>(recoilDrivenDistance(recoil, steps));
    }

    const double peak = std::min(
        recoilDrivenDistance(recoil, transitionStep),
        static_cast<double>(recoil.maximumDistance));
    const double settled = peak -
        static_cast<double>(steps - transitionStep) *
            static_cast<double>(recoil.settleSpeed);
    return static_cast<float>(std::max(0.0, settled));
}

} // namespace

void WorldRenderPipeline::resolveAnimationPresentationInto(
    ResolvedAnimationPresentation& resolved,
    const RenderEntitySnapshot& instance) const {
    resolved.modelAsset = instance.modelAsset;
    resolved.visual = instance.visual;
    // World snapshots are complete states and may be coalesced twice before
    // CPU preparation. Weapon impulses deliberately remain in the newest
    // object descriptor; detect an impulse newer than the last endpoint that
    // actually completed preparation and begin its visible envelope now.
    // This preserves the latest fire edge without replaying ancient shots on
    // objects that merely re-entered the view.
    if (m_hasPreviousFrame &&
        m_previousFrame.presentationEpoch == m_snapshot.presentationEpoch &&
        m_previousFrame.simulationFrame < m_snapshot.simulationFrame) {
        for (RenderWeaponImpulse& impulse :
             resolved.visual.weaponImpulses) {
            if (impulse.fireTick > m_previousFrame.simulationFrame &&
                impulse.fireTick < m_snapshot.simulationFrame) {
                impulse.presentationTick = m_snapshot.simulationFrame;
            }
        }
    }
    if (m_snapshot.simulationFrame >
        resolved.visual.animationSampleTick) {
        const uint32_t logicFramesPerSecond = std::max<uint32_t>(
            1u, m_snapshot.objectUi.logicFramesPerSecond);
        const uint64_t elapsedTicks =
            m_snapshot.simulationFrame - resolved.visual.animationSampleTick;
        const auto advanceTick = [elapsedTicks](uint64_t value) noexcept {
            return elapsedTicks >
                    std::numeric_limits<uint64_t>::max() - value
                ? std::numeric_limits<uint64_t>::max()
                : value + elapsedTicks;
        };
        if (resolved.visual.policeCar.enabled) {
            resolved.visual.policeCar.simulationFrame = advanceTick(
                resolved.visual.policeCar.simulationFrame);
        }
        if (resolved.visual.debris.enabled) {
            resolved.visual.debris.ageFrames = advanceTick(
                resolved.visual.debris.ageFrames);
            if (resolved.visual.debris.finalState) {
                resolved.visual.debris.finalAgeFrames = advanceTick(
                    resolved.visual.debris.finalAgeFrames);
            }
        }
        if (resolved.visual.floatSwayEnabled &&
            resolved.visual.floatSwayRunning) {
            resolved.visual.floatSwaySampleTick = advanceTick(
                resolved.visual.floatSwaySampleTick);
        }
        if (resolved.visual.opacityFadeMode !=
            RenderOpacityFadeMode::None) {
            const uint64_t fadeElapsed = m_snapshot.simulationFrame >=
                    resolved.visual.opacityFadeStartTick
                ? m_snapshot.simulationFrame -
                    resolved.visual.opacityFadeStartTick
                : 0u;
            const uint64_t duration =
                resolved.visual.opacityFadeDurationFrames;
            const float progress = duration == 0
                ? 1.0f
                : std::clamp(
                    static_cast<float>(std::min(fadeElapsed, duration)) /
                        static_cast<float>(duration),
                    0.0f, 1.0f);
            resolved.visual.objectOpacity =
                resolved.visual.opacityFadeMode ==
                    RenderOpacityFadeMode::In
                ? progress : 1.0f - progress;
            if (resolved.visual.friendlyStealthPulseEnabled) {
                resolved.visual.friendlyStealthBaseOpacity =
                    resolved.visual.objectOpacity;
            }
        }
        if (resolved.visual.friendlyStealthPulseEnabled &&
            resolved.visual.friendlyStealthPulseRunning) {
            constexpr float kPulseRadiansPerTick = 0.2f;
            constexpr float kTwoPi = 6.28318530717958647692f;
            resolved.visual.friendlyStealthPulsePhaseRadians = std::fmod(
                resolved.visual.friendlyStealthPulsePhaseRadians +
                    static_cast<float>(elapsedTicks) *
                        kPulseRadiansPerTick,
                kTwoPi);
            resolved.visual.objectOpacity =
                resolveFriendlyStealthOpacity(
                    resolved.visual.friendlyStealthBaseOpacity,
                    resolved.visual.friendlyStealthMinimumOpacity,
                    resolved.visual.friendlyStealthPulsePhaseRadians,
                    false);
        }
        float currentDisabledTintScale =
            resolved.visual.disabledTintSampleScale;
        if (resolved.visual.disabledTintMode !=
            RenderTintEnvelopeMode::None) {
            constexpr float kTintEnvelopeFrames = 30.0f;
            if (resolved.visual.disabledTintMode ==
                    RenderTintEnvelopeMode::Attack) {
                const uint64_t age = m_snapshot.simulationFrame >=
                        resolved.visual.disabledTintStartTick
                    ? m_snapshot.simulationFrame -
                        resolved.visual.disabledTintStartTick
                    : 0u;
                currentDisabledTintScale = std::min(
                    1.0f,
                    (static_cast<float>(age) + 1.0f) /
                        kTintEnvelopeFrames);
            } else if (resolved.visual.disabledTintMode ==
                       RenderTintEnvelopeMode::Release) {
                const uint64_t age = m_snapshot.simulationFrame >=
                        resolved.visual.disabledTintStartTick
                    ? m_snapshot.simulationFrame -
                        resolved.visual.disabledTintStartTick
                    : 0u;
                currentDisabledTintScale =
                    resolved.visual.disabledTintReleaseStartScale *
                    std::max(
                        0.0f,
                        1.0f - static_cast<float>(age) /
                            kTintEnvelopeFrames);
            } else if (resolved.visual.disabledTintMode ==
                       RenderTintEnvelopeMode::Constant) {
                currentDisabledTintScale = 1.0f;
            }
            const float delta = currentDisabledTintScale -
                resolved.visual.disabledTintSampleScale;
            resolved.visual.scriptFlashBaseTint += RenderVector{
                -0.5f * delta, -0.5f * delta, -0.5f * delta};
            resolved.visual.disabledTintSampleScale =
                currentDisabledTintScale;
        }
        if (resolved.visual.temporaryBonusTintMode !=
            RenderTintEnvelopeMode::None) {
            constexpr float kTintEnvelopeFrames = 30.0f;
            const uint64_t age = m_snapshot.simulationFrame >=
                    resolved.visual.temporaryBonusTintStartTick
                ? m_snapshot.simulationFrame -
                    resolved.visual.temporaryBonusTintStartTick
                : 0u;
            float scale = 0.0f;
            if (resolved.visual.temporaryBonusTintMode ==
                    RenderTintEnvelopeMode::Attack) {
                scale = std::min(
                    1.0f,
                    (static_cast<float>(age) + 1.0f) /
                        kTintEnvelopeFrames);
            } else if (resolved.visual.temporaryBonusTintMode ==
                       RenderTintEnvelopeMode::Release) {
                scale = std::max(
                    0.0f,
                    resolved.visual.temporaryBonusTintReleaseStartScale -
                        (static_cast<float>(age) + 1.0f) /
                            kTintEnvelopeFrames);
            }
            if (currentDisabledTintScale > 0.0f) scale = 0.0f;
            const float delta = scale -
                resolved.visual.temporaryBonusTintSampleAppliedScale;
            if (resolved.visual.temporaryBonusTintInfantry) {
                resolved.visual.scriptFlashBaseTint += RenderVector{
                    0.0f, -0.7f * delta, -0.7f * delta};
            } else {
                resolved.visual.scriptFlashBaseTint += RenderVector{
                    0.2f * delta, -0.2f * delta, -0.2f * delta};
            }
            resolved.visual.temporaryBonusTintSampleAppliedScale = scale;
        }
        resolved.visual.scriptFlashTint =
            resolved.visual.scriptFlashBaseTint;
        if (resolved.visual.scriptFlashEnabled) {
            if (m_snapshot.simulationFrame >=
                    resolved.visual.scriptFlashFirstPulseTick &&
                m_snapshot.simulationFrame <
                    resolved.visual.scriptFlashEndTick &&
                resolved.visual.scriptFlashPulseIntervalTicks != 0 &&
                resolved.visual.scriptFlashDecayTicks != 0) {
                const uint64_t phase =
                    (m_snapshot.simulationFrame -
                     resolved.visual.scriptFlashFirstPulseTick) %
                    resolved.visual.scriptFlashPulseIntervalTicks;
                if (phase < resolved.visual.scriptFlashDecayTicks) {
                    const float intensity = static_cast<float>(
                        resolved.visual.scriptFlashDecayTicks - phase) /
                        static_cast<float>(
                            resolved.visual.scriptFlashDecayTicks);
                    resolved.visual.scriptFlashTint +=
                        resolved.visual.scriptFlashColor * intensity;
                }
            }
        }
        if (!resolved.visual.animationPaused &&
            resolved.visual.animationMode != RenderAnimationMode::Manual) {
            resolved.visual.animationTimeSeconds +=
                static_cast<float>(elapsedTicks) /
                static_cast<float>(logicFramesPerSecond);
        }
        resolved.visual.animationSampleTick = m_snapshot.simulationFrame;
    }
    for (const RenderWeaponImpulse& impulse :
         resolved.visual.weaponImpulses) {
        const uint64_t sampleStartTick = impulse.presentationTick != 0
            ? impulse.presentationTick : impulse.fireTick;
        if (m_snapshot.simulationFrame < sampleStartTick) continue;
        if (!impulse.recoilBone.empty()) {
            const float shift = weaponRecoilShift(
                impulse, m_snapshot.simulationFrame - sampleStartTick);
            if (shift > 0.0f) {
                resolved.visual.boneControls.push_back({
                    .boneName = impulse.recoilBone,
                    .translation = {-shift, 0.0f, 0.0f},
                    .boneNameIsPrefix = impulse.recoilBoneIsPrefix,
                    .boneNameSequenceOrdinal = impulse.sequenceOrdinal,
                    .boneNamePrefixFallsBackToBare =
                        impulse.recoilBoneIsPrefix,
                });
            }
        }
        if (m_snapshot.simulationFrame == sampleStartTick &&
            !impulse.muzzleFlash.empty()) {
            resolved.visual.subObjectVisibility.push_back({
                .name = impulse.muzzleFlash,
                .visible = true,
                .nameIsPrefix = impulse.muzzleFlashIsPrefix,
                .nameSequenceOrdinal = impulse.sequenceOrdinal,
                .namePrefixFallsBackToBare =
                    impulse.muzzleFlashIsPrefix,
            });
        }
    }
    resolved.weaponLaunchBones = instance.weaponLaunchBones;
    resolved.weaponLaunchBoneSequenceOrdinals =
        instance.weaponLaunchBoneSequenceOrdinals;
    resolved.completions.clear();
    resolved.terminalFallbackCompletions = 0;
    enum class ClipResolutionKind : uint8_t {
        Pending,
        Ready,
        Failed,
    };
    struct ClipResolution final {
        const AnimationClip* animation = nullptr;
        ClipResolutionKind kind = ClipResolutionKind::Pending;
    };
    const auto clipFor = [this](container::StringView modelAsset,
                                container::StringView animationState)
        -> ClipResolution {
        const auto model = m_models.find(container::String{modelAsset});
        if (model == m_models.end()) return {};
        if (!model->second.resolutionError.empty()) {
            return {.kind = ClipResolutionKind::Failed};
        }
        if (!model->second.skeleton || model->second.skeleton->empty()) {
            return {};
        }
        const auto clip = model->second.animations.find(
            container::String{animationState});
        if (clip != model->second.animations.end() && clip->second) {
            return {
                .animation = clip->second.get(),
                .kind = ClipResolutionKind::Ready,
            };
        }
        return model->second.animationErrors.contains(
                   container::String{animationState})
            ? ClipResolution{.kind = ClipResolutionKind::Failed}
            : ClipResolution{};
    };
    const auto frameFraction = [](float timeSeconds, float durationSeconds,
                                  RenderAnimationMode mode) noexcept {
        if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0f) {
            return 0.0f;
        }
        float progress = std::max(0.0f, timeSeconds) / durationSeconds;
        switch (mode) {
        case RenderAnimationMode::Once:
        case RenderAnimationMode::Manual:
            return std::clamp(progress, 0.0f, 1.0f);
        case RenderAnimationMode::OnceBackwards:
            return 1.0f - std::clamp(progress, 0.0f, 1.0f);
        case RenderAnimationMode::LoopBackwards:
            return 1.0f - std::fmod(progress, 1.0f);
        case RenderAnimationMode::LoopPingPong: {
            const float phase = std::fmod(progress, 2.0f);
            return phase <= 1.0f ? phase : 2.0f - phase;
        }
        case RenderAnimationMode::Loop:
            return std::fmod(progress, 1.0f);
        }
        return 0.0f;
    };
    // RefCode W3DModelDraw::adjustAnimSpeedToMovementSpeed() runs once per
    // client frame and overwrites the HLod frame-rate multiplier whenever the
    // clip currently selected by m_curState has an authored distanceCovered.
    // setCurAnimDurationInMsec() converts the requested duration into
    // naturalDurationInMsec / desiredDurationInMsec, so the same ratio is
    // rebuilt here from the immutable clip the game side cannot see. Doing it
    // before completion is evaluated matches RefCode, where the multiplier is
    // already live on the render object when Is_Animation_Complete() is asked.
    const auto applyMovementSpeedSync = [&resolved, &clipFor]() {
        const float requestedDuration =
            resolved.visual.animationSpeedSyncDurationSeconds;
        if (!std::isfinite(requestedDuration) || requestedDuration <= 0.0f ||
            requestedDuration > kMaximumMovementSyncDurationSeconds ||
            resolved.visual.animationMode == RenderAnimationMode::Manual) {
            return;
        }
        const ClipResolution clip = clipFor(
            resolved.modelAsset, resolved.visual.animationState);
        if (!clip.animation) return;
        const float authoredDuration =
            clip.animation->completionTimeSeconds(1.0f);
        if (!std::isfinite(authoredDuration) || authoredDuration <= 0.0f) {
            return;
        }
        const float rate = authoredDuration / requestedDuration;
        if (!std::isfinite(rate)) return;
        resolved.visual.animationRate = std::clamp(
            rate, kMinimumMovementSyncAnimationRate,
            kMaximumMovementSyncAnimationRate);
    };
    const auto reportCompletion = [this, &instance, &resolved](
            RenderAnimationCompletionPhase phase,
            float durationSeconds) {
        const uint8_t phaseBit = static_cast<uint8_t>(
            1u << static_cast<uint8_t>(phase));
        if (!instance.animationCompletionFeedbackEnabled ||
            resolved.visual.animationStart.generation == 0 ||
            (resolved.visual.animationCompletionMask & phaseBit) != 0) {
            return;
        }
        resolved.completions.push_back({
            .presentationEpoch = m_snapshot.presentationEpoch,
            .simulationFrame = m_snapshot.simulationFrame,
            .objectId = instance.objectId != 0
                ? instance.objectId : instance.id,
            .channelIndex = instance.channelIndex,
            .generation = resolved.visual.animationStart.generation,
            .phase = phase,
            .kind = RenderAnimationFeedbackKind::Completed,
            .completedDurationSeconds =
                std::isfinite(durationSeconds)
                    ? std::max(0.0f, durationSeconds) : 0.0f,
        });
    };
    const auto reportResourceGate = [this, &instance, &resolved](
            RenderAnimationCompletionPhase phase,
            ClipResolutionKind resolution) {
        if (!instance.animationCompletionFeedbackEnabled ||
            resolved.visual.animationStart.generation == 0 ||
            resolved.visual.animationState.empty()) {
            return;
        }
        RenderAnimationFeedbackKind kind;
        if (resolution == ClipResolutionKind::Pending) {
            kind = RenderAnimationFeedbackKind::ResourcePending;
        } else if (resolution == ClipResolutionKind::Ready &&
                   resolved.visual.animationResourcePendingGeneration ==
                       resolved.visual.animationStart.generation &&
                   resolved.visual.animationResourcePendingPhase ==
                       static_cast<uint8_t>(phase)) {
            kind = RenderAnimationFeedbackKind::ResourceReady;
        } else {
            return;
        }
        const bool duplicate = std::any_of(
            resolved.completions.begin(), resolved.completions.end(),
            [phase, kind](const RenderAnimationCompletionFeedback& current) {
                return current.phase == phase && current.kind == kind;
            });
        if (duplicate) return;
        resolved.completions.push_back({
            .presentationEpoch = m_snapshot.presentationEpoch,
            .simulationFrame = m_snapshot.simulationFrame,
            .objectId = instance.objectId != 0
                ? instance.objectId : instance.id,
            .channelIndex = instance.channelIndex,
            .generation = resolved.visual.animationStart.generation,
            .phase = phase,
            .kind = kind,
            .completedDurationSeconds = 0.0f,
        });
    };
    const auto applyAnimationStart = [
            &resolved, &clipFor, &frameFraction, &reportResourceGate](
            RenderAnimationCompletionPhase phase) {
        const ClipResolution clip = clipFor(
            resolved.modelAsset, resolved.visual.animationState);
        reportResourceGate(phase, clip.kind);
        if (!clip.animation) return clip;
        const AnimationClip* animation = clip.animation;
        RenderAnimationStartDescriptor& start =
            resolved.visual.animationStart;
        const float duration = animation->completionTimeSeconds(
            resolved.visual.animationRate);
        float requestedFraction = 0.0f;
        bool hasRequestedFraction = false;
        switch (start.kind) {
        case RenderAnimationStartKind::FirstFrame:
            requestedFraction = 0.0f;
            hasRequestedFraction = true;
            break;
        case RenderAnimationStartKind::LastFrame:
            requestedFraction = 1.0f;
            hasRequestedFraction = true;
            break;
        case RenderAnimationStartKind::RandomFrame:
            requestedFraction = std::clamp(
                start.randomFraction, 0.0f, 1.0f);
            hasRequestedFraction = true;
            break;
        case RenderAnimationStartKind::MaintainFraction: {
            const ClipResolution source = clipFor(
                start.sourceModelAsset, start.sourceAnimationState);
            if (source.animation) {
                const float sourceDuration =
                    source.animation->completionTimeSeconds(
                    start.sourceRate);
                requestedFraction = frameFraction(
                    start.sourceTimeSeconds, sourceDuration,
                    start.sourceMode);
                hasRequestedFraction = true;
            }
            break;
        }
        case RenderAnimationStartKind::Default:
            break;
        }
        if (hasRequestedFraction &&
            resolved.visual.animationMode == RenderAnimationMode::Manual) {
            // W3DModelDraw applies START_FRAME_FIRST/LAST/RANDOMSTART and
            // MAINTAIN_FRAME before freezing a MANUAL animation.  The game
            // contract carries an explicit frame index for later
            // setAnimationFrame() calls, so resolve the state-entry fraction
            // to that same literal frame here.  Otherwise compilePoseSample()
            // correctly honours animationManualFrame but its default zero
            // silently overwrites START_FRAME_LAST (notably launch-bay final
            // poses and their ParticleSysBone anchors).
            const uint32_t lastFrame = animation->frameCount() > 0u
                ? animation->frameCount() - 1u : 0u;
            const float frame = std::clamp(
                requestedFraction, 0.0f, 1.0f) *
                static_cast<float>(lastFrame);
            resolved.visual.animationManualFrame =
                static_cast<uint32_t>(std::clamp(
                    frame + 0.5f, 0.0f,
                    static_cast<float>(lastFrame)));
        }
        if (hasRequestedFraction && std::isfinite(duration)) {
            const bool backwards =
                resolved.visual.animationMode ==
                    RenderAnimationMode::OnceBackwards ||
                resolved.visual.animationMode ==
                    RenderAnimationMode::LoopBackwards;
            const float timelineFraction = backwards
                ? 1.0f - requestedFraction : requestedFraction;
            resolved.visual.animationTimeSeconds = std::max(
                0.0f, resolved.visual.animationTimeSeconds) +
                std::max(0.0f, duration) * timelineFraction;
        }
        start.kind = RenderAnimationStartKind::Default;
        start.sourceModelAsset.clear();
        start.sourceAnimationState.clear();
        return clip;
    };
    const auto advanceTo = [
            &resolved, &applyAnimationStart, &applyMovementSpeedSync,
            &reportCompletion](
            const std::optional<RenderAnimationCompletionTarget>& target,
            RenderAnimationCompletionPhase phase) {
        if (!target) return false;
        const float inputTime = std::max(
            0.0f, resolved.visual.animationTimeSeconds);
        applyMovementSpeedSync();
        const ClipResolution clip = applyAnimationStart(phase);
        const AnimationClip* animation = clip.animation;
        const bool complete = resolved.visual.animationState.empty() ||
            clip.kind == ClipResolutionKind::Failed ||
            (animation && animation->isComplete(
                resolved.visual.animationTimeSeconds,
                resolved.visual.animationMode,
                resolved.visual.animationRate));
        if (!complete) return false;
        const float duration = animation
            ? animation->completionTimeSeconds(resolved.visual.animationRate)
            : 0.0f;
        const float remainder = std::max(
            0.0f, resolved.visual.animationTimeSeconds - duration);
        // State-start offsets (RANDOMSTART/endpoints/maintain-fraction) are
        // renderer metadata. Return only the portion consumed from the
        // logic-owned clock so game-side phase retirement preserves any
        // overshoot without subtracting an offset it never stored.
        const size_t completionCountBefore = resolved.completions.size();
        reportCompletion(phase, std::max(0.0f, inputTime - remainder));
        if (clip.kind == ClipResolutionKind::Failed &&
            resolved.completions.size() != completionCountBefore) {
            ++resolved.terminalFallbackCompletions;
        }
        resolved.modelAsset = target->modelAsset;
        resolved.visual.animationState = target->animationState;
        resolved.visual.animationTimeSeconds = remainder;
        resolved.visual.animationRate = target->animationRate;
        resolved.visual.animationSpeedSyncDurationSeconds =
            target->animationSpeedSyncDurationSeconds;
        resolved.visual.animationMode = target->animationMode;
        resolved.visual.animationManualFrame =
            target->animationManualFrame;
        resolved.visual.animationStart = target->animationStart;
        resolved.visual.animationCompleted = false;
        resolved.visual.subObjectVisibility = target->subObjectVisibility;
        resolved.visual.boneControls = target->boneControls;
        resolved.visual.particleSystemBones = target->particleSystemBones;
        resolved.weaponLaunchBones = target->weaponLaunchBones;
        resolved.weaponLaunchBoneSequenceOrdinals =
            target->weaponLaunchBoneSequenceOrdinals;
        return true;
    };
    if (advanceTo(instance.animationCompletionTarget,
                  instance.animationCompletionPhase)) {
        static_cast<void>(advanceTo(
            instance.animationFinalTarget,
            RenderAnimationCompletionPhase::Transition));
    }
    applyMovementSpeedSync();
    const ClipResolution activeClip = applyAnimationStart(
        RenderAnimationCompletionPhase::ActiveState);
    const AnimationClip* activeAnimation = activeClip.animation;
    if (!resolved.visual.animationState.empty() &&
        activeClip.kind == ClipResolutionKind::Failed) {
        const size_t completionCountBefore = resolved.completions.size();
        reportCompletion(RenderAnimationCompletionPhase::ActiveState, 0.0f);
        if (resolved.completions.size() != completionCountBefore) {
            ++resolved.terminalFallbackCompletions;
        }
    }
    if (activeAnimation && activeAnimation->isComplete(
            resolved.visual.animationTimeSeconds,
            resolved.visual.animationMode,
            resolved.visual.animationRate)) {
        const float duration = activeAnimation->completionTimeSeconds(
            resolved.visual.animationRate);
        reportCompletion(
            RenderAnimationCompletionPhase::ActiveState, duration);
        if (resolved.visual.animationStart.restartWhenComplete &&
            std::isfinite(duration) && duration > 0.0f) {
            // Keep the current renderer frame seamless while the detached
            // completion waits for confirmed-tick admission. The game will
            // advance the generation and select the next candidate.
            resolved.visual.animationTimeSeconds = std::fmod(
                resolved.visual.animationTimeSeconds, duration);
            resolved.visual.animationCompleted = false;
        }
    }
}

} // namespace engine::render
