#include "engine/renderer/world/pipeline/WorldRenderer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::render {
namespace {

constexpr container::Array<float, static_cast<size_t>(ScreenShakeRenderIntensity::Count)>
    kLegacyScriptShakeStrengths = {{0.5f, 1.0f, 2.5f, 5.0f, 8.0f, 12.0f}};
constexpr float kLegacyScriptShakeConfiguredMaximum = 10.0f;
constexpr float kLegacyScriptShakeHistoricalClamp = 3.0f;
constexpr float kLegacyScriptShakeMinimum = 0.01f;
constexpr float kLegacyScriptShakeDamping = 0.75f;
constexpr uint64_t kMaximumScriptShakeCatchUpSteps = 64;

[[nodiscard]] uint64_t mixScriptPresentationSeed(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] math::vec2 scriptShakeDirection(
    const ScreenShakeRenderImpulse& impulse) noexcept {
    // RefCode consumes GameClientRandomValueReal(0, 2PI), deliberately not
    // the deterministic simulation RNG.  A stamp-derived hash preserves that
    // client-only separation while making repeated snapshot consumption
    // stable and independent of unrelated render work.
    uint64_t seed = mixScriptPresentationSeed(impulse.presentationEpoch);
    seed = mixScriptPresentationSeed(seed ^ impulse.sequence);
    seed = mixScriptPresentationSeed(seed ^ impulse.confirmedTick);
    seed = mixScriptPresentationSeed(seed ^ static_cast<uint64_t>(impulse.sourceScriptId));
    seed = mixScriptPresentationSeed(seed ^ static_cast<uint64_t>(impulse.ordinal));
    constexpr double kInvTwoTo53 = 1.0 / 9007199254740992.0;
    constexpr float kTwoPi = 6.28318530717958647692f;
    const float angle = static_cast<float>(static_cast<double>(seed >> 11u) * kInvTwoTo53) * kTwoPi;
    return {std::cos(angle), std::sin(angle)};
}

// CameraShakeSystemClass is a renderer/client effect, not the older
// View::shake XY spring used by SCREEN_SHAKE.  It rotates the camera around
// its source point.  Keep its random-looking waveform presentation-local,
// but derive it solely from the confirmed stamp so consuming a retained
// snapshot twice or skipping an intermediate render frame cannot advance a
// hidden RNG stream.
constexpr float kLegacyLocalizedShakeMinimumOmega = math::deg_to_rad(12.5f * 360.0f);
constexpr float kLegacyLocalizedShakeMaximumOmega = math::deg_to_rad(15.0f * 360.0f);
constexpr float kLegacyLocalizedShakeEndOmega = math::TWO_PI;
constexpr container::Array<float, 3> kLegacyLocalizedShakeAxisRotation = {{
    math::deg_to_rad(7.5f), math::deg_to_rad(15.0f), math::deg_to_rad(5.0f),
}};

[[nodiscard]] uint64_t localizedShakeSeed(const LocalizedCameraShakeRenderImpulse& impulse,
                                          uint64_t salt) noexcept {
    uint64_t seed = mixScriptPresentationSeed(impulse.presentationEpoch);
    seed = mixScriptPresentationSeed(seed ^ impulse.sequence);
    seed = mixScriptPresentationSeed(seed ^ impulse.confirmedTick);
    seed = mixScriptPresentationSeed(seed ^ static_cast<uint64_t>(impulse.sourceScriptId));
    seed = mixScriptPresentationSeed(seed ^ static_cast<uint64_t>(impulse.ordinal));
    return mixScriptPresentationSeed(seed ^ salt);
}

[[nodiscard]] float localizedShakeUnit(uint64_t seed) noexcept {
    constexpr double kInvTwoTo53 = 1.0 / 9007199254740992.0;
    return static_cast<float>(static_cast<double>(seed >> 11u) * kInvTwoTo53);
}

[[nodiscard]] math::vec3 localizedShakeAngles(const LocalizedCameraShakeRenderImpulse& impulse,
                                               uint64_t age, float intensity,
                                               float logicFrameSeconds) noexcept {
    // RefCode creates a three-axis CameraShakerClass with independent omega
    // and phase values.  It also layers three small random secondary vectors
    // during every update.  Recreate that visual character with stamp/tick
    // hashes rather than GameClientRandomValueReal, which would make modern
    // replay/recovery depend on the number of rendered frames.
    const float elapsedSeconds = static_cast<float>(age + 1u) * logicFrameSeconds;
    math::vec3 angles{};
    for (size_t axis = 0; axis < kLegacyLocalizedShakeAxisRotation.size(); ++axis) {
        const uint64_t baseSeed = localizedShakeSeed(impulse, 0x100u + axis);
        const float omega = kLegacyLocalizedShakeMinimumOmega +
            (kLegacyLocalizedShakeMaximumOmega - kLegacyLocalizedShakeMinimumOmega) *
                localizedShakeUnit(baseSeed);
        const float phi = math::TWO_PI * localizedShakeUnit(
            localizedShakeSeed(impulse, 0x200u + axis));
        // Preserve CameraShakeSystem's historical formula, including its
        // elapsed-seconds interpolation rather than normalizing by duration.
        const float currentOmega = omega +
            (kLegacyLocalizedShakeEndOmega - omega) * elapsedSeconds;
        angles[axis] += kLegacyLocalizedShakeAxisRotation[axis] * intensity *
            std::sin(currentOmega * elapsedSeconds + phi);

        const float minorIntensity = intensity * 0.5f;
        for (size_t component = 0; component < 3; ++component) {
            const uint64_t noiseSeed = localizedShakeSeed(
                impulse, 0x400u + axis * 3u + component + age * 11u);
            angles[component] +=
                (localizedShakeUnit(noiseSeed) * 2.0f - 1.0f) * minorIntensity;
        }
    }
    return angles;
}

[[nodiscard]] RenderCameraSnapshot applyLocalizedScriptCameraShakes(
    RenderCameraSnapshot output, uint64_t simulationFrame,
    const ScreenShakeRenderState& screenShake, uint64_t presentationEpoch) noexcept {
    // CAMERA_ADD_SHAKER_AT is spatial and finite, but it never translates a
    // tactical camera.  RefCode's CameraShakeSystem measures the source
    // camera position in 3D, sums roll/pitch/yaw impulses, then rotates the
    // completed view transform in place.  The retained journal lets this be
    // evaluated statelessly from a sealed frame and naturally expire after
    // its authored duration.
    const uint32_t tickRate = std::clamp(screenShake.logicFramesPerSecond, 1u, 1000u);
    const float logicFrameSeconds = 1.0f / static_cast<float>(tickRate);
    math::vec3 accumulatedAngles{};
    for (const LocalizedCameraShakeRenderImpulse& impulse : screenShake.localizedImpulses) {
        if (impulse.presentationEpoch != presentationEpoch ||
            impulse.durationTicks == 0 || impulse.confirmedTick > simulationFrame ||
            !std::isfinite(impulse.amplitude) || !std::isfinite(impulse.radius) ||
            !std::isfinite(impulse.position.x()) || !std::isfinite(impulse.position.y()) ||
            !std::isfinite(impulse.position.z()) || impulse.radius <= math::EPSILON) {
            continue;
        }
        const uint64_t age = simulationFrame - impulse.confirmedTick;
        if (age >= impulse.durationTicks) continue;

        const math::vec3 delta = output.position - impulse.position;
        const float distanceSquared = delta.length_sq();
        if (!std::isfinite(distanceSquared) || distanceSquared >= impulse.radius * impulse.radius) {
            continue;
        }
        const float distance = std::sqrt(std::max(distanceSquared, 0.0f));
        const float spatial = 1.0f - distance / impulse.radius;
        const float temporal = 1.0f - static_cast<float>(age + 1u) /
            static_cast<float>(impulse.durationTicks);
        if (spatial <= 0.0f || temporal <= 0.0f) continue;

        // CameraShakeSystem converts the authored power from degrees to
        // radians before applying its per-axis rotation factors.
        const float intensity = math::deg_to_rad(impulse.amplitude) * spatial * temporal;
        accumulatedAngles += localizedShakeAngles(impulse, age, intensity, logicFrameSeconds);
    }

    if (accumulatedAngles.length_sq() <= math::EPSILON * math::EPSILON ||
        !std::isfinite(accumulatedAngles.x()) || !std::isfinite(accumulatedAngles.y()) ||
        !std::isfinite(accumulatedAngles.z())) {
        return output;
    }

    const math::vec3 sourceToTarget = output.target - output.position;
    const float targetDistance = sourceToTarget.length();
    if (!std::isfinite(targetDistance) || targetDistance <= math::EPSILON) return output;
    const math::vec3 forward = sourceToTarget / targetDistance;
    const float upLength = output.up.length();
    if (!std::isfinite(upLength) || upLength <= math::EPSILON) return output;
    math::vec3 up = output.up / upLength;
    math::vec3 right = forward.cross(up);
    const float rightLength = right.length();
    if (!std::isfinite(rightLength) || rightLength <= math::EPSILON) return output;
    right = right / rightLength;
    // Re-orthogonalize the supplied snapshot basis before applying the local
    // X/Y/Z rotations. It prevents a malformed/near-parallel `up` from
    // turning a harmless presentation effect into an invalid D3D camera.
    up = right.cross(forward);
    const float correctedUpLength = up.length();
    if (!std::isfinite(correctedUpLength) || correctedUpLength <= math::EPSILON) return output;
    up = up / correctedUpLength;

    const math::quat pitch = math::quat::from_axis_angle(right, accumulatedAngles.x());
    const math::quat yaw = math::quat::from_axis_angle(up, accumulatedAngles.y());
    const math::quat roll = math::quat::from_axis_angle(forward, accumulatedAngles.z());
    math::quat rotation = (roll * yaw * pitch).normalized();
    const math::vec3 rotatedForward = rotation.rotate_vec(forward).normalized();
    const math::vec3 rotatedUp = rotation.rotate_vec(up).normalized();
    if (!std::isfinite(rotatedForward.x()) || !std::isfinite(rotatedForward.y()) ||
        !std::isfinite(rotatedForward.z()) || !std::isfinite(rotatedUp.x()) ||
        !std::isfinite(rotatedUp.y()) || !std::isfinite(rotatedUp.z())) {
        return output;
    }
    output.target = output.position + rotatedForward * targetDistance;
    output.up = rotatedUp;
    return output;
}

[[nodiscard]] bool isFiniteVector(const math::vec3& value) noexcept {
    return std::isfinite(value.x()) && std::isfinite(value.y()) &&
        std::isfinite(value.z());
}

} // namespace

CameraSlavePresentationCamera WorldRenderer::applyScriptCameraSlave(
    const RenderCameraSnapshot& cameraSnapshot, const CameraSlaveRenderState& request,
    bool targetPresent, const std::optional<RenderMatrix>& boneWorldTransform) noexcept {
    CameraSlavePresentationCamera result{.camera = cameraSnapshot};
    ScriptCameraSlaveConsumer& consumer = m_scriptCameraSlave;

    if (request.presentationEpoch == 0) {
        // Debug/unscoped frames must never inherit a prior match's camera
        // slave merely because the renderer outlived that GameSession.
        consumer = {};
        return result;
    }
    if (consumer.presentationEpoch == 0 ||
        request.presentationEpoch > consumer.presentationEpoch) {
        consumer = {.presentationEpoch = request.presentationEpoch};
    } else if (request.presentationEpoch < consumer.presentationEpoch) {
        return result;
    }

    if (request.presentationSequence > consumer.presentationSequence) {
        consumer.presentationSequence = request.presentationSequence;
        consumer.enabled = request.enabled && request.objectId != 0 && !request.boneName.empty();
    } else if (request.presentationSequence < consumer.presentationSequence) {
        // A stale prepared frame may carry an old bone pose. Do not allow it
        // to revive or move a newer presentation request.
        return result;
    }

    if (!consumer.enabled) return result;
    if (!targetPresent) {
        // W3DView disables slave mode when getUnitNamed()/getDrawable()
        // fails. This latch belongs solely to renderer presentation; no
        // reverse mutation is sent into the confirmed session.
        consumer.enabled = false;
        return result;
    }
    if (!boneWorldTransform) {
        // The live Object may be awaiting asset upload, use an unsupported
        // draw module, or name a missing bone. The old wrapper's failure path
        // was unsafe; modern code keeps the request but falls back to the
        // normal camera without ever substituting a root transform.
        return result;
    }

    const RenderMatrix& bone = *boneWorldTransform;
    const math::vec3 position = bone.translation();
    math::vec3 forward = bone.forward();
    math::vec3 up = bone.up();
    const float forwardLength = forward.length();
    if (!isFiniteVector(position) || !isFiniteVector(forward) || !isFiniteVector(up) ||
        !std::isfinite(forwardLength) || forwardLength <= math::EPSILON) {
        return result;
    }
    forward = forward / forwardLength;
    // Ignore non-uniform object scale and re-orthogonalize the authored
    // camera basis. W3D camera transforms look down local -Z and use local
    // +Y for up; preserve that convention under wwmath's row-vector layout.
    up -= forward * up.dot(forward);
    const float upLength = up.length();
    if (!isFiniteVector(up) || !std::isfinite(upLength) || upLength <= math::EPSILON) {
        return result;
    }
    up = up / upLength;

    result.camera.position = position;
    result.camera.target = position - forward;
    result.camera.up = up;
    result.applied = true;
    return result;
}

RenderCameraSnapshot WorldRenderer::prepareScriptViewFilters(
    const RenderCameraSnapshot& cameraSnapshot, uint64_t simulationFrame,
    const BlackAndWhiteRenderState& blackAndWhite,
    const MotionBlurRenderState& motionBlur) noexcept {
    return m_postProcessRenderer.prepareScriptViewFilters(
        cameraSnapshot, simulationFrame, blackAndWhite, motionBlur);
}

void WorldRenderer::renderScriptViewFilters() {
    m_postProcessRenderer.renderScriptViewFilters();
}

bool WorldRenderer::configureFxaa(
    bool enabled, float subpixel, float edgeThreshold,
    float edgeThresholdMin) noexcept {
    return m_postProcessRenderer.configureFxaa(
        enabled, subpixel, edgeThreshold, edgeThresholdMin);
}

bool WorldRenderer::renderFxaa(float tacticalViewportHeightScale) {
    return m_postProcessRenderer.renderFxaa(tacticalViewportHeightScale);
}

RenderCameraSnapshot WorldRenderer::applyScriptScreenShake(
    const RenderCameraSnapshot& cameraSnapshot, uint64_t simulationFrame,
    const ScreenShakeRenderState& screenShake) noexcept {
    ScriptScreenShakeConsumer& shake = m_scriptScreenShake;

    // A GameSession start creates a monotonically increasing epoch.  Ignore
    // an old queued snapshot after a newer match/frame has already reached
    // this renderer; accepting it would resurrect a prior session's shake.
    if (screenShake.presentationEpoch != 0 &&
        (shake.presentationEpoch == 0 || screenShake.presentationEpoch > shake.presentationEpoch)) {
        shake = {};
        shake.presentationEpoch = screenShake.presentationEpoch;
    }
    if (screenShake.presentationEpoch == 0 ||
        screenShake.presentationEpoch != shake.presentationEpoch) {
        return cameraSnapshot;
    }
    if (screenShake.impulsesTrimmedThroughSequence >
        shake.lastSequence) {
        // This pulse stream is additive and cannot be reconstructed from a
        // latest value.  Treat an expired tail explicitly as a client-local
        // reset; injecting forgotten impulses late would be more visible and
        // less faithful than allowing their bounded visual lifetime to end.
        shake.lastSequence = screenShake.impulsesTrimmedThroughSequence;
        shake.intensity = 0.0f;
        shake.offset = {};
    }
    if (screenShake.localizedImpulsesTrimmedThroughSequence >
        shake.localizedTrimmedThroughSequence) {
        // Localized shakes are evaluated directly from the sealed current
        // snapshot; unlike the additive screen shake there is no hidden
        // renderer integrator to carry forward.  Record the explicit tail
        // expiry so a later refactor cannot mistake absence for a replayable
        // one-shot event.
        shake.localizedTrimmedThroughSequence =
            screenShake.localizedImpulsesTrimmedThroughSequence;
    }

    uint64_t firstStep = simulationFrame;
    if (shake.hasSimulationFrame) {
        if (simulationFrame <= shake.lastSimulationFrame) {
            RenderCameraSnapshot output = cameraSnapshot;
            const math::vec3 offset{shake.offset.x(), shake.offset.y(), 0.0f};
            output.position += offset;
            output.target += offset;
            return applyLocalizedScriptCameraShakes(
                output, simulationFrame, screenShake, shake.presentationEpoch);
        }
        firstStep = shake.lastSimulationFrame + 1u;
    } else {
        // A renderer joining mid-session can reconstruct the still-visible
        // tail from the retained journal rather than treating every impulse
        // as if it happened on the latest frame.
        for (const ScreenShakeRenderImpulse& impulse : screenShake.impulses) {
            if (impulse.presentationEpoch == shake.presentationEpoch &&
                impulse.sequence > shake.lastSequence &&
                impulse.confirmedTick <= simulationFrame &&
                impulse.confirmedTick < firstStep) {
                firstStep = impulse.confirmedTick;
            }
        }
    }

    // A 0.75 spring decays below meaningful precision long before 64 fixed
    // steps. Avoid a pathological billion-tick catch-up while preserving the
    // full visible tail of every recent impulse.
    if (simulationFrame >= firstStep &&
        simulationFrame - firstStep + 1u > kMaximumScriptShakeCatchUpSteps) {
        firstStep = simulationFrame - kMaximumScriptShakeCatchUpSteps + 1u;
        shake.intensity = 0.0f;
        shake.offset = {};
    }

    const auto consumeImpulse = [&shake](const ScreenShakeRenderImpulse& impulse) noexcept {
        const size_t intensityIndex = static_cast<size_t>(impulse.intensity);
        shake.lastSequence = impulse.sequence;
        if (intensityIndex >= kLegacyScriptShakeStrengths.size()) return;
        shake.direction = scriptShakeDirection(impulse);
        shake.intensity += kLegacyScriptShakeStrengths[intensityIndex];
        if (shake.intensity > kLegacyScriptShakeConfiguredMaximum) {
            shake.intensity = kLegacyScriptShakeHistoricalClamp;
        }
    };
    const auto discardImpulse = [&shake](const ScreenShakeRenderImpulse& impulse) noexcept {
        shake.lastSequence = impulse.sequence;
    };
    const auto stepShake = [&shake]() noexcept {
        if (shake.intensity > kLegacyScriptShakeMinimum) {
            shake.offset = shake.direction * shake.intensity;
            shake.intensity *= kLegacyScriptShakeDamping;
            shake.direction = -shake.direction;
        } else {
            shake.intensity = 0.0f;
            shake.offset = {};
        }
    };

    for (uint64_t step = firstStep; step <= simulationFrame; ++step) {
        for (const ScreenShakeRenderImpulse& impulse : screenShake.impulses) {
            if (impulse.presentationEpoch != shake.presentationEpoch ||
                impulse.sequence <= shake.lastSequence || impulse.sequence == 0 ||
                impulse.confirmedTick > step) {
                continue;
            }
            // If recovery had to skip an old span, a pulse that fully decayed
            // before the retained tail must advance the cursor but cannot be
            // injected late as a fresh full-strength camera jump.
            if (impulse.confirmedTick < firstStep) {
                discardImpulse(impulse);
            } else {
                consumeImpulse(impulse);
            }
        }
        stepShake();
        if (step == std::numeric_limits<uint64_t>::max()) break;
    }
    shake.lastSimulationFrame = simulationFrame;
    shake.hasSimulationFrame = true;

    RenderCameraSnapshot output = cameraSnapshot;
    const math::vec3 offset{shake.offset.x(), shake.offset.y(), 0.0f};
    output.position += offset;
    output.target += offset;

    return applyLocalizedScriptCameraShakes(
        output, simulationFrame, screenShake, shake.presentationEpoch);
}

void WorldRenderer::renderScreenFade(
    const ScreenFadeRenderState& fade, uint64_t simulationFrame) {
    m_postProcessRenderer.renderScreenFade(fade, simulationFrame);
}

} // namespace engine::render
