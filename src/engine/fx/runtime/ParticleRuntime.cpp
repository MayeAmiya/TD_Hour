#include "core/container/container_types.h"
#include "ParticleRuntime.h"
#include "FxRuntimeMath.h"
#include "presentation/fx/runtime/FxPresentationSnapshot.h"
#include "engine/renderer/runtime/RenderParallelExecutor.h"
#include <taskflow/taskflow.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace engine::fx {
namespace {

using runtime_detail::rotateEuler;
using runtime_detail::sampleGroundHeight;

constexpr float kTau = 6.28318530717958647692f;
constexpr float kDirectionEpsilon = 1.0e-12f;
constexpr float kMaximumAccumulatedSizeBonus = 50.0f;
constexpr uint64_t kStreamStride = 0x9e3779b97f4a7c15ull;
constexpr float kWindFullStrengthRadius = 75.0f;
constexpr float kWindOuterRadius = 200.0f;
constexpr float kWindFalloffDistance = kWindOuterRadius - kWindFullStrengthRadius;
static_assert(kWindFalloffDistance > 0.0f);

[[nodiscard]] constexpr uint32_t normalizedLogicFramesPerSecond(
    uint32_t value) noexcept {
    return value != 0 ? value : kParticleAuthoredFramesPerSecond;
}

[[nodiscard]] constexpr uint32_t nextNonzeroCounter(uint32_t value) noexcept {
    ++value;
    return value != 0 ? value : 1;
}

[[nodiscard]] constexpr uint64_t authoredFrameForSimulationFrame(
    uint64_t simulationFrame, uint32_t logicFramesPerSecond) noexcept {
    const uint64_t rate = normalizedLogicFramesPerSecond(
        logicFramesPerSecond);
    const uint64_t wholeSeconds = simulationFrame / rate;
    const uint64_t partialTicks = simulationFrame % rate;
    if (wholeSeconds >
        std::numeric_limits<uint64_t>::max() /
            kParticleAuthoredFramesPerSecond) {
        return std::numeric_limits<uint64_t>::max();
    }
    // partialTicks < uint32_t rate, so this product cannot overflow uint64_t.
    const uint64_t wholeFrames =
        wholeSeconds * kParticleAuthoredFramesPerSecond;
    const uint64_t partialFrames =
        (partialTicks * kParticleAuthoredFramesPerSecond) / rate;
    return partialFrames >
            std::numeric_limits<uint64_t>::max() - wholeFrames
        ? std::numeric_limits<uint64_t>::max()
        : wholeFrames + partialFrames;
}

enum RandomStream : uint64_t {
    StreamInitialDelay = 1,
    StreamBurstCount = 2,
    StreamBurstDelay = 3,
    StreamVolumeBase = 16,
    StreamVelocityBase = 32,
    StreamVelocityDamping = 48,
    StreamAngularDamping = 49,
    StreamAngle = 50,
    StreamAngularRate = 51,
    StreamLifetime = 52,
    StreamStartSize = 53,
    StreamStartSizeRate = 54,
    StreamSizeRate = 55,
    StreamSizeRateDamping = 56,
    StreamColorScale = 57,
    // Keep the established Z streams above stable: adding X/Y must never
    // perturb existing billboard particles or replay fingerprints.
    StreamAngleX = 58,
    StreamAngleY = 59,
    StreamAngularRateX = 60,
    StreamAngularRateY = 61,
    StreamAlphaBase = 64,
    StreamWindAngleChange = 80,
    StreamWindStartAngle = 81,
    StreamWindEndAngle = 82,
    StreamWindRandomness = 83,
    StreamRelatedSystem = 96,
};

[[nodiscard]] uint64_t splitMix64(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] uint64_t streamBits(uint64_t seed, uint64_t stream) noexcept {
    return splitMix64(seed + stream * kStreamStride);
}

[[nodiscard]] float unitRandom(uint64_t seed, uint64_t stream) noexcept {
    constexpr float inverseTwentyFourBits = 1.0f / 16777216.0f;
    return static_cast<float>(streamBits(seed, stream) >> 40u) * inverseTwentyFourBits;
}

[[nodiscard]] uint32_t randomIndex(uint64_t seed, uint64_t stream, uint32_t count) noexcept {
    return count == 0 ? 0 : static_cast<uint32_t>(streamBits(seed, stream) % count);
}

[[nodiscard]] float sampleRange(const ParticleRange& range, uint64_t seed,
                                uint64_t stream) noexcept {
    return range.minimum + (range.maximum - range.minimum) * unitRandom(seed, stream);
}

[[nodiscard]] float sampledWholeFrames(const ParticleRange& range, uint64_t seed,
                                        uint64_t stream) noexcept {
    return std::floor(std::max(0.0f, sampleRange(range, seed, stream)));
}

[[nodiscard]] ParticleVector3 add(ParticleVector3 left, ParticleVector3 right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] ParticleVector3 subtract(ParticleVector3 left, ParticleVector3 right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] ParticleVector3 multiply(ParticleVector3 value, float scalar) noexcept {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

[[nodiscard]] float dot(ParticleVector3 left, ParticleVector3 right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] ParticleVector3 cross(ParticleVector3 left, ParticleVector3 right) noexcept {
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

[[nodiscard]] ParticleVector3 normalized(ParticleVector3 value,
                                         ParticleVector3 fallback = {1.0f, 0.0f, 0.0f}) noexcept {
    const float lengthSquared = dot(value, value);
    if (lengthSquared <= kDirectionEpsilon) return fallback;
    return multiply(value, 1.0f / std::sqrt(lengthSquared));
}

[[nodiscard]] ParticleVector3 randomSphereDirection(uint64_t seed, uint64_t stream) noexcept {
    const ParticleVector3 direction = {
        unitRandom(seed, stream) * 2.0f - 1.0f,
        unitRandom(seed, stream + 1) * 2.0f - 1.0f,
        unitRandom(seed, stream + 2) * 2.0f - 1.0f,
    };
    return normalized(direction);
}

[[nodiscard]] ParticleVector3 randomHemisphereDirection(uint64_t seed,
                                                         uint64_t stream) noexcept {
    const ParticleVector3 direction = {
        unitRandom(seed, stream) * 2.0f - 1.0f,
        unitRandom(seed, stream + 1) * 2.0f - 1.0f,
        unitRandom(seed, stream + 2),
    };
    return normalized(direction, {0.0f, 0.0f, 1.0f});
}

[[nodiscard]] ParticleVector3 sampleVolumePosition(const ParticleSystemTemplate& definition,
                                                    float radiusOverride,
                                                    uint64_t seed) noexcept {
    switch (definition.volumeKind) {
    case ParticleVolumeKind::Line: {
        const float amount = unitRandom(seed, StreamVolumeBase);
        return add(definition.volumeLineStart,
                   multiply(subtract(definition.volumeLineEnd, definition.volumeLineStart), amount));
    }
    case ParticleVolumeKind::Box: {
        ParticleVector3 result = {
            (unitRandom(seed, StreamVolumeBase) * 2.0f - 1.0f) * definition.volumeBoxHalfSize.x,
            (unitRandom(seed, StreamVolumeBase + 1) * 2.0f - 1.0f) * definition.volumeBoxHalfSize.y,
            (unitRandom(seed, StreamVolumeBase + 2) * 2.0f - 1.0f) * definition.volumeBoxHalfSize.z,
        };
        if (!definition.hollow) return result;

        const uint32_t face = randomIndex(seed, StreamVolumeBase + 3, 6);
        if (face == 0 || face == 3) {
            result.z = face == 0 ? -definition.volumeBoxHalfSize.z
                                 : definition.volumeBoxHalfSize.z;
        } else if (face == 1 || face == 4) {
            result.x = face == 1 ? -definition.volumeBoxHalfSize.x
                                 : definition.volumeBoxHalfSize.x;
        } else {
            result.y = face == 2 ? -definition.volumeBoxHalfSize.y
                                 : definition.volumeBoxHalfSize.y;
        }
        return result;
    }
    case ParticleVolumeKind::Sphere: {
        const float authoredRadius = radiusOverride > 0.0f
            ? radiusOverride : definition.volumeSphereRadius;
        const float radius = definition.hollow
            ? authoredRadius
            : unitRandom(seed, StreamVolumeBase + 3) * authoredRadius;
        return multiply(randomSphereDirection(seed, StreamVolumeBase), radius);
    }
    case ParticleVolumeKind::Cylinder: {
        const float authoredRadius = radiusOverride > 0.0f
            ? radiusOverride : definition.volumeCylinderRadius;
        const float angle = unitRandom(seed, StreamVolumeBase) * kTau;
        const float radius = definition.hollow
            ? authoredRadius
            : unitRandom(seed, StreamVolumeBase + 1) * authoredRadius;
        return {
            radius * std::cos(angle),
            radius * std::sin(angle),
            (unitRandom(seed, StreamVolumeBase + 2) - 0.5f) * definition.volumeCylinderLength,
        };
    }
    case ParticleVolumeKind::None:
    case ParticleVolumeKind::Point:
    case ParticleVolumeKind::Count:
        return {};
    }
    return {};
}

[[nodiscard]] ParticleVector3 lineOutwardVelocity(const ParticleSystemTemplate& definition,
                                                   float speed, float otherSpeed) noexcept {
    const ParticleVector3 along = normalized(
        subtract(definition.volumeLineEnd, definition.volumeLineStart), {1.0f, 0.0f, 0.0f});
    ParticleVector3 perpendicular = cross({0.0f, 0.0f, 1.0f}, along);
    if (dot(perpendicular, perpendicular) <= kDirectionEpsilon) {
        perpendicular = cross({0.0f, 1.0f, 0.0f}, along);
    }
    perpendicular = normalized(perpendicular, {1.0f, 0.0f, 0.0f});
    const ParticleVector3 localUp = normalized(cross(along, perpendicular), {0.0f, 0.0f, 1.0f});
    return add(multiply(perpendicular, speed), multiply(localUp, otherSpeed));
}

[[nodiscard]] ParticleVector3 sampleVelocity(const ParticleSystemTemplate& definition,
                                             ParticleVector3 localPosition,
                                             uint64_t seed) noexcept {
    switch (definition.velocityKind) {
    case ParticleVelocityKind::Ortho:
        return {
            sampleRange(definition.velocityOrthoX, seed, StreamVelocityBase),
            sampleRange(definition.velocityOrthoY, seed, StreamVelocityBase + 1),
            sampleRange(definition.velocityOrthoZ, seed, StreamVelocityBase + 2),
        };
    case ParticleVelocityKind::Spherical:
        return multiply(randomSphereDirection(seed, StreamVelocityBase),
                        sampleRange(definition.velocitySpherical, seed, StreamVelocityBase + 3));
    case ParticleVelocityKind::Hemispherical:
        return multiply(randomHemisphereDirection(seed, StreamVelocityBase),
                        sampleRange(definition.velocityHemispherical, seed, StreamVelocityBase + 3));
    case ParticleVelocityKind::Cylindrical: {
        const float angle = unitRandom(seed, StreamVelocityBase) * kTau;
        const float radial = sampleRange(definition.velocityCylindricalRadial, seed,
                                         StreamVelocityBase + 1);
        return {
            radial * std::cos(angle),
            radial * std::sin(angle),
            sampleRange(definition.velocityCylindricalNormal, seed, StreamVelocityBase + 2),
        };
    }
    case ParticleVelocityKind::Outward: {
        const float speed = sampleRange(definition.velocityOutward, seed, StreamVelocityBase);
        const float otherSpeed = sampleRange(definition.velocityOutwardOther, seed,
                                              StreamVelocityBase + 1);
        switch (definition.volumeKind) {
        case ParticleVolumeKind::Cylinder: {
            ParticleVector3 radial = normalized({localPosition.x, localPosition.y, 0.0f},
                                                randomSphereDirection(seed, StreamVelocityBase + 2));
            radial.z = 0.0f;
            radial = normalized(radial, {1.0f, 0.0f, 0.0f});
            return {radial.x * speed, radial.y * speed, otherSpeed};
        }
        case ParticleVolumeKind::Line:
            return lineOutwardVelocity(definition, speed, otherSpeed);
        case ParticleVolumeKind::Box:
        case ParticleVolumeKind::Sphere:
            return multiply(normalized(localPosition,
                                       randomSphereDirection(seed, StreamVelocityBase + 2)), speed);
        case ParticleVolumeKind::None:
        case ParticleVolumeKind::Point:
        case ParticleVolumeKind::Count:
            return multiply(randomSphereDirection(seed, StreamVelocityBase + 2), speed);
        }
        break;
    }
    case ParticleVelocityKind::None:
    case ParticleVelocityKind::Count:
        return {};
    }
    return {};
}

[[nodiscard]] float sampleAlphaKey(const ParticleSystemTemplate& definition, size_t key,
                                   uint64_t seed) noexcept {
    return sampleRange(definition.alphaKeys[key].value, seed, StreamAlphaBase + key);
}

[[nodiscard]] float evaluateAlpha(const ParticleSystemTemplate& definition, uint64_t seed,
                                  float ageFrames) noexcept {
    float previousValue = sampleAlphaKey(definition, 0, seed);
    uint32_t previousFrame = definition.alphaKeys[0].frame;
    for (size_t key = 1; key < kParticleKeyframeCount; ++key) {
        const ParticleAlphaKeyframe& target = definition.alphaKeys[key];
        if (target.frame == 0) break;
        const float targetValue = sampleAlphaKey(definition, key, seed);
        if (ageFrames < static_cast<float>(target.frame)) {
            const uint32_t frameSpan = target.frame > previousFrame
                ? target.frame - previousFrame
                : 0;
            if (frameSpan == 0) return targetValue;
            const float amount = std::clamp(
                (ageFrames - static_cast<float>(previousFrame)) / static_cast<float>(frameSpan),
                0.0f, 1.0f);
            return std::lerp(previousValue, targetValue, amount);
        }
        previousValue = targetValue;
        previousFrame = target.frame;
    }
    return previousValue;
}

[[nodiscard]] ParticleVector3 colorValue(ParticleColor color) noexcept {
    constexpr float inverseByte = 1.0f / 255.0f;
    return {
        static_cast<float>(color.red) * inverseByte,
        static_cast<float>(color.green) * inverseByte,
        static_cast<float>(color.blue) * inverseByte,
    };
}

[[nodiscard]] ParticleVector3 evaluateColor(const ParticleSystemTemplate& definition,
                                            float ageFrames,
                                            ParticleVector3 laterKeyTint) noexcept {
    ParticleVector3 previousValue = colorValue(definition.colorKeys[0].color);
    uint32_t previousFrame = definition.colorKeys[0].frame;
    for (size_t key = 1; key < kParticleKeyframeCount; ++key) {
        const ParticleColorKeyframe& target = definition.colorKeys[key];
        if (target.frame == 0) break;
        ParticleVector3 targetValue = colorValue(target.color);
        targetValue.x *= laterKeyTint.x;
        targetValue.y *= laterKeyTint.y;
        targetValue.z *= laterKeyTint.z;
        if (ageFrames < static_cast<float>(target.frame)) {
            const uint32_t frameSpan = target.frame > previousFrame
                ? target.frame - previousFrame
                : 0;
            if (frameSpan == 0) return targetValue;
            const float amount = std::clamp(
                (ageFrames - static_cast<float>(previousFrame)) / static_cast<float>(frameSpan),
                0.0f, 1.0f);
            return {
                std::lerp(previousValue.x, targetValue.x, amount),
                std::lerp(previousValue.y, targetValue.y, amount),
                std::lerp(previousValue.z, targetValue.z, amount),
            };
        }
        previousValue = targetValue;
        previousFrame = target.frame;
    }
    return previousValue;
}

[[nodiscard]] bool hasFutureColorKey(
    const ParticleSystemTemplate& definition, float ageFrames) noexcept {
    for (size_t key = 1; key < kParticleKeyframeCount; ++key) {
        const uint32_t frame = definition.colorKeys[key].frame;
        if (frame == 0) break;
        if (static_cast<float>(frame) > ageFrames) return true;
    }
    return false;
}

[[nodiscard]] bool hasFutureAlphaKey(
    const ParticleSystemTemplate& definition, float ageFrames) noexcept {
    for (size_t key = 1; key < kParticleKeyframeCount; ++key) {
        const uint32_t frame = definition.alphaKeys[key].frame;
        if (frame == 0) break;
        if (static_cast<float>(frame) > ageFrames) return true;
    }
    return false;
}

[[nodiscard]] bool shaderMakesParticleInvisible(
    const ParticleSystemTemplate& definition, float ageFrames, float alpha,
    float red, float green, float blue) noexcept {
    constexpr float invisibleThreshold = 0.01f;
    switch (definition.shader) {
    case ParticleShader::Additive:
        return !hasFutureColorKey(definition, ageFrames) &&
            red < invisibleThreshold && green < invisibleThreshold &&
            blue < invisibleThreshold;
    case ParticleShader::None:
    case ParticleShader::Alpha:
        return !hasFutureAlphaKey(definition, ageFrames) &&
            alpha < invisibleThreshold;
    case ParticleShader::AlphaTest:
        return false;
    case ParticleShader::Multiply:
        return !hasFutureColorKey(definition, ageFrames) &&
            red > 1.0f - invisibleThreshold &&
            green > 1.0f - invisibleThreshold &&
            blue > 1.0f - invisibleThreshold;
    case ParticleShader::Count:
        return true;
    }
    return true;
}

[[nodiscard]] uint32_t maximumBlockCount(size_t maximumParticles) noexcept {
    if (maximumParticles == 0) return 0;
    const size_t blocks = (maximumParticles + kParticleAoSoAWidth - 1) / kParticleAoSoAWidth;
    return static_cast<uint32_t>(std::min<size_t>(blocks,
        std::numeric_limits<uint32_t>::max()));
}

[[nodiscard]] size_t priorityIndex(ParticlePriority priority) noexcept {
    const size_t index = static_cast<size_t>(priority);
    return index < static_cast<size_t>(ParticlePriority::Count)
        ? index
        : static_cast<size_t>(ParticlePriority::Invalid);
}

[[nodiscard]] bool priorityAtLeast(ParticlePriority value,
                                   ParticlePriority minimum) noexcept {
    return priorityIndex(value) >= priorityIndex(minimum);
}

[[nodiscard]] bool supportedParticleKind(ParticleKind kind) noexcept {
    return kind == ParticleKind::Billboard || kind == ParticleKind::Streak ||
        kind == ParticleKind::Volume || kind == ParticleKind::Drawable ||
        kind == ParticleKind::Smudge;
}

[[nodiscard]] bool sameTemplate(ParticleTemplateId left,
                                ParticleTemplateId right) noexcept {
    return left.value == right.value;
}

[[nodiscard]] float particleUpAngle(float directionX,
                                    float directionY) noexcept {
    const float length = std::sqrt(
        directionX * directionX + directionY * directionY);
    if (!(length > 0.0f)) return 0.0f;
    // Preserve ParticleSys.cpp::angleBetween(up, direction) + PI,
    // including its authored orthogonal special case.
    const float dot = directionY;
    if (dot == 0.0f) {
        return directionX > 0.0f ? kTau : 0.5f * kTau;
    }
    const float theta = std::acos(std::clamp(dot / length, -1.0f, 1.0f));
    return (directionX > 0.0f ? theta : -theta) + 0.5f * kTau;
}

} // namespace

ParticleRuntime::ParticleRuntime(container::SharedPtr<const ParticleSystemCatalog> catalog,
                                 size_t maximumParticles,
                                 size_t initialEmitterCapacity,
                                 size_t maximumEmitters,
                                 ParticleAdmissionSettings admission,
                                 ParticleUpdateSettings updateSettings)
    : m_catalog(std::move(catalog)),
      m_blocks(maximumBlockCount(maximumParticles)),
      m_maximumParticles(std::min<size_t>(maximumParticles,
          std::numeric_limits<uint32_t>::max())),
      m_maximumEmitters(std::min<size_t>(maximumEmitters,
          std::numeric_limits<uint32_t>::max())),
      m_updateSettings(updateSettings) {
    setAdmissionSettings(admission);
    m_blocks.reserveBlocks(maximumBlockCount(m_maximumParticles));
    m_activeBlocks.reserve(maximumBlockCount(m_maximumParticles));
    m_emitters.reserve(std::min(initialEmitterCapacity, m_maximumEmitters));
    m_particleLocations.reserve(m_maximumParticles);
    m_slaveEmitters.reserve(std::min<size_t>(m_maximumEmitters, 256));
    m_controlledEmitters.reserve(std::min<size_t>(m_maximumEmitters, 256));
    m_updateSettings.blocksPerTask = std::clamp<size_t>(
        m_updateSettings.blocksPerTask, 1,
        std::max<size_t>(1, maximumBlockCount(m_maximumParticles)));
}

void ParticleRuntime::setAdmissionSettings(
    ParticleAdmissionSettings admission) noexcept {
    if (admission.ordinaryParticleLimit ==
        std::numeric_limits<size_t>::max()) {
        admission.ordinaryParticleLimit = m_maximumParticles;
    } else {
        admission.ordinaryParticleLimit = std::min(
            admission.ordinaryParticleLimit, m_maximumParticles);
    }
    admission.fieldParticleLimit = std::min(
        admission.fieldParticleLimit, m_maximumParticles);
    if (admission.minimumPriority >= ParticlePriority::Count) {
        admission.minimumPriority = ParticlePriority::Invalid;
    }
    if (admission.minimumSkipPriority >= ParticlePriority::Count) {
        admission.minimumSkipPriority = ParticlePriority::Invalid;
    }
    m_admission = admission;
}

ParticleEmitterHandle ParticleRuntime::createEmitter(const ParticleEmitterSpawn& spawn) {
    container::Vector<ParticleTemplateId>& recursionStack =
        m_relatedSystemRecursionScratch[0];
    recursionStack.clear();
    recursionStack.reserve(kMaximumRelatedSystemDepth);
    const ParticleEmitterHandle emitter =
        createEmitterInternal(spawn, 0, false, recursionStack);
    primeEmitter(emitter);
    return emitter;
}

void ParticleRuntime::primeEmitter(ParticleEmitterHandle emitter) {
    ParticleEmitterState* state = m_emitters.get(emitter);
    const ParticleSystemTemplate* definition = state && m_catalog
        ? m_catalog->find(state->templateId)
        : nullptr;
    if (!state || !definition || state->externallyDriven ||
        !state->spawning || state->ageFrames < 0.0f ||
        state->ageFrames < state->nextBurstFrame ||
        (definition->oneShot && state->emittedBurst)) {
        return;
    }
    emitBurst(emitter, *state, *definition);
    state = m_emitters.get(emitter);
    if (!state) return;
    state->emittedBurst = true;
    if (definition->oneShot) state->spawning = false;
    // The primed burst belongs to the current authored frame. The next
    // synchronize step must observe the following frame, so zero-delay
    // continuous systems may emit again there instead of slipping a frame.
    state->ageFrames += 1.0f;
}

ParticleEmitterHandle ParticleRuntime::createEmitterInternal(
    const ParticleEmitterSpawn& spawn, uint8_t relationDepth,
    bool externallyDriven, container::Vector<ParticleTemplateId>& recursionStack) {
    const ParticleSystemTemplate* definition = m_catalog ? m_catalog->find(spawn.templateId) : nullptr;
    if (!definition || !supportedParticleKind(definition->kind) ||
        !std::isfinite(spawn.position.x) || !std::isfinite(spawn.position.y) ||
        !std::isfinite(spawn.position.z) || !std::isfinite(spawn.rollRadians) ||
        !std::isfinite(spawn.pitchRadians) || !std::isfinite(spawn.yawRadians) ||
        !std::isfinite(spawn.emissionRadiusOverride) ||
        !std::isfinite(spawn.velocityMultiplier.x) ||
        !std::isfinite(spawn.velocityMultiplier.y) ||
        !std::isfinite(spawn.velocityMultiplier.z) ||
        !std::isfinite(spawn.burstCountMultiplier) ||
        !std::isfinite(spawn.sizeMultiplier) ||
        !std::isfinite(spawn.laterColorKeyTint.x) ||
        !std::isfinite(spawn.laterColorKeyTint.y) ||
        !std::isfinite(spawn.laterColorKeyTint.z) ||
        relationDepth > kMaximumRelatedSystemDepth ||
        m_emitters.size() >= m_maximumEmitters ||
        std::find_if(recursionStack.begin(), recursionStack.end(),
            [definition](ParticleTemplateId value) {
                return sameTemplate(value, definition->id);
            }) != recursionStack.end()) {
        ++m_stats.rejectedEmitters;
        return {};
    }

    ParticleEmitterState state;
    state.templateId = spawn.templateId;
    state.position = spawn.position;
    state.previousAuthoredPosition = spawn.position;
    state.rollRadians = spawn.rollRadians;
    state.pitchRadians = spawn.pitchRadians;
    state.yawRadians = spawn.yawRadians;
    state.emissionRadiusOverride = std::max(0.0f, spawn.emissionRadiusOverride);
    state.velocityMultiplier = {
        std::max(0.0f, spawn.velocityMultiplier.x),
        std::max(0.0f, spawn.velocityMultiplier.y),
        std::max(0.0f, spawn.velocityMultiplier.z),
    };
    state.burstCountMultiplier = std::max(0.0f, spawn.burstCountMultiplier);
    state.sizeMultiplier = std::max(0.0f, spawn.sizeMultiplier);
    state.laterColorKeyTint = {
        std::clamp(spawn.laterColorKeyTint.x, 0.0f, 1.0f),
        std::clamp(spawn.laterColorKeyTint.y, 0.0f, 1.0f),
        std::clamp(spawn.laterColorKeyTint.z, 0.0f, 1.0f),
    };
    state.systemLifetimeOverrideFrames =
        spawn.systemLifetimeOverrideFrames;
    state.seed = spawn.seed;
    state.relationDepth = relationDepth;
    state.externallyDriven = externallyDriven;
    state.spawning = spawn.spawning;
    state.retainedWhenStopped = spawn.retainedWhenStopped;
    state.ageFrames = -static_cast<float>(spawn.initialDelayFrames.value_or(
        static_cast<uint32_t>(sampledWholeFrames(definition->initialDelay, spawn.seed,
                                                 StreamInitialDelay))));
    state.windAngleChange = sampleRange(
        {definition->windAngleChangeMinimum,
         definition->windAngleChangeMaximum},
        spawn.seed, StreamWindAngleChange);
    state.windStartAngle = sampleRange(
        {definition->windPingPongStartAngleMinimum,
         definition->windPingPongStartAngleMaximum},
        spawn.seed, StreamWindStartAngle);
    state.windEndAngle = sampleRange(
        {definition->windPingPongEndAngleMinimum,
         definition->windPingPongEndAngleMaximum},
        spawn.seed, StreamWindEndAngle);
    if (state.windStartAngle > state.windEndAngle) {
        std::swap(state.windStartAngle, state.windEndAngle);
    }
    state.windAngle = std::lerp(
        state.windStartAngle, state.windEndAngle,
        unitRandom(spawn.seed, StreamWindEndAngle + 1));
    const ParticleEmitterHandle handle = m_emitters.emplace(std::move(state));
    m_stats.emitterHighWater = std::max(m_stats.emitterHighWater, m_emitters.size());

    recursionStack.push_back(definition->id);
    if (definition->slaveSystem) {
        ParticleEmitterSpawn slaveSpawn = spawn;
        slaveSpawn.templateId = definition->slaveSystem;
        slaveSpawn.seed = splitMix64(spawn.seed ^ StreamRelatedSystem);
        slaveSpawn.initialDelayFrames = 0;
        // ParticleSystemInfo::tintAllColors() mutates only the concrete
        // ParticleSystem on which it is called. A separately constructed
        // SlaveSystem retains its own authored color keys.
        slaveSpawn.laterColorKeyTint = {1.0f, 1.0f, 1.0f};
        const ParticleEmitterHandle slave = createEmitterInternal(
            slaveSpawn, static_cast<uint8_t>(relationDepth + 1), true,
            recursionStack);
        if (slave) {
            m_slaveEmitters.push_back({
                .master = handle,
                .slave = slave,
                .offset = definition->slavePositionOffset,
            });
        } else {
            ++m_stats.rejectedRelatedSystems;
        }
    } else if (!definition->slaveSystemName.empty()) {
        ++m_stats.rejectedRelatedSystems;
    }
    if (!definition->perParticleAttachedSystem &&
        !definition->perParticleAttachedSystemName.empty()) {
        ++m_stats.rejectedRelatedSystems;
    }
    recursionStack.pop_back();
    return handle;
}

bool ParticleRuntime::setEmitterPosition(ParticleEmitterHandle emitter,
                                         ParticleVector3 position) noexcept {
    ParticleEmitterState* state = m_emitters.get(emitter);
    if (!state || !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) {
        return false;
    }
    state->position = position;
    return true;
}

bool ParticleRuntime::setEmitterTransform(ParticleEmitterHandle emitter,
                                          ParticleVector3 position,
                                          float rollRadians,
                                          float pitchRadians,
                                          float yawRadians) noexcept {
    ParticleEmitterState* state = m_emitters.get(emitter);
    if (!state || !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || !std::isfinite(rollRadians) ||
        !std::isfinite(pitchRadians) || !std::isfinite(yawRadians)) {
        return false;
    }
    state->position = position;
    state->rollRadians = rollRadians;
    state->pitchRadians = pitchRadians;
    state->yawRadians = yawRadians;
    return true;
}

bool ParticleRuntime::setEmitterMultipliers(
    ParticleEmitterHandle emitter, ParticleVector3 velocityMultiplier,
    float burstCountMultiplier, float sizeMultiplier) noexcept {
    ParticleEmitterState* state = m_emitters.get(emitter);
    if (!state || !std::isfinite(velocityMultiplier.x) ||
        !std::isfinite(velocityMultiplier.y) ||
        !std::isfinite(velocityMultiplier.z) ||
        !std::isfinite(burstCountMultiplier) ||
        !std::isfinite(sizeMultiplier)) {
        return false;
    }
    state->velocityMultiplier = {
        std::max(0.0f, velocityMultiplier.x),
        std::max(0.0f, velocityMultiplier.y),
        std::max(0.0f, velocityMultiplier.z),
    };
    state->burstCountMultiplier = std::max(0.0f, burstCountMultiplier);
    state->sizeMultiplier = std::max(0.0f, sizeMultiplier);
    return true;
}

bool ParticleRuntime::setEmitterSpawning(
    ParticleEmitterHandle emitter, bool spawning) noexcept {
    ParticleEmitterState* state = m_emitters.get(emitter);
    if (!state) return false;
    state->spawning = spawning;
    for (const SlaveEmitterLink& link : m_slaveEmitters) {
        if (link.master != emitter) continue;
        if (ParticleEmitterState* slave = m_emitters.get(link.slave)) {
            slave->spawning = spawning;
        }
    }
    return true;
}

bool ParticleRuntime::triggerEmitter(ParticleEmitterHandle emitter) {
    ParticleEmitterState* state = m_emitters.get(emitter);
    const ParticleSystemTemplate* definition = state && m_catalog
        ? m_catalog->find(state->templateId) : nullptr;
    if (!state || !definition || !state->spawning) return false;
    emitBurst(emitter, *state, *definition);
    return true;
}

bool ParticleRuntime::stopEmitter(ParticleEmitterHandle emitter) noexcept {
    ParticleEmitterState* state = m_emitters.get(emitter);
    if (!state) return false;
    state->spawning = false;
    state->retainedWhenStopped = false;
    for (const SlaveEmitterLink& link : m_slaveEmitters) {
        if (link.master != emitter) continue;
        if (ParticleEmitterState* slave = m_emitters.get(link.slave)) {
            slave->spawning = false;
            slave->retainedWhenStopped = false;
        }
    }
    if (state->particleCount == 0) (void)m_emitters.erase(emitter);
    return true;
}

void ParticleRuntime::setGroundHeightField(
    container::SharedPtr<const FxGroundHeightFieldSnapshot> groundHeights) noexcept {
    m_groundHeights = std::move(groundHeights);
}

void ParticleRuntime::updateSeconds(float deltaSeconds) {
    if (!(deltaSeconds > 0.0f) || !std::isfinite(deltaSeconds)) return;
    // Wall time is presentation-only. Authored burst/lifetime/keyframe state
    // is advanced exclusively by synchronizeAuthoredFrame().
    m_renderInterpolationAlpha = std::clamp(
        m_renderInterpolationAlpha +
            deltaSeconds * static_cast<float>(
                kParticleAuthoredFramesPerSecond),
        0.0f, 1.0f);
}

void ParticleRuntime::updateAuthoredFrames(uint32_t frames) {
    ParticleRuntimePhaseProfile aggregate;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        stepOneAuthoredFrame();
        ++aggregate.authoredFrames;
        aggregate.emitterUpdateNanoseconds +=
            m_lastPhaseProfile.emitterUpdateNanoseconds;
        aggregate.integrationNanoseconds +=
            m_lastPhaseProfile.integrationNanoseconds;
        aggregate.serialCompactNanoseconds +=
            m_lastPhaseProfile.serialCompactNanoseconds;
        aggregate.integratedParticles +=
            m_lastPhaseProfile.integratedParticles;
        aggregate.compactedDeadParticles +=
            m_lastPhaseProfile.compactedDeadParticles;
        aggregate.activeBlocks = std::max(
            aggregate.activeBlocks, m_lastPhaseProfile.activeBlocks);
        aggregate.integrationTasks +=
            m_lastPhaseProfile.integrationTasks;
        aggregate.parallelIntegration =
            aggregate.parallelIntegration ||
            m_lastPhaseProfile.parallelIntegration;
    }
    if (frames == 0) return;
    if (m_phaseProfileOrdinal != std::numeric_limits<uint64_t>::max()) {
        ++m_phaseProfileOrdinal;
    }
    aggregate.sampleOrdinal = m_phaseProfileOrdinal;
    m_lastPhaseProfile = aggregate;
    if (m_authoredCursorInitialized) {
        const uint64_t remaining =
            std::numeric_limits<uint64_t>::max() -
            m_authoredFrame;
        m_authoredFrame += std::min<uint64_t>(frames, remaining);
    }
    m_renderInterpolationAlpha = 0.0f;
}

void ParticleRuntime::synchronizeAuthoredFrame(
    uint64_t epoch, uint64_t simulationFrame,
    uint32_t logicFramesPerSecond) {
    if (epoch == 0) return;
    logicFramesPerSecond = normalizedLogicFramesPerSecond(
        logicFramesPerSecond);
    const uint64_t authoredFrame = authoredFrameForSimulationFrame(
        simulationFrame, logicFramesPerSecond);
    if (!m_authoredCursorInitialized) {
        m_authoredEpoch = epoch;
        m_authoredSimulationFrame = simulationFrame;
        m_authoredFrame = authoredFrame;
        m_authoredLogicFramesPerSecond = logicFramesPerSecond;
        m_authoredCursorInitialized = true;
        m_renderInterpolationAlpha = 0.0f;
        return;
    }
    if (m_authoredEpoch != epoch ||
        m_authoredLogicFramesPerSecond != logicFramesPerSecond) {
        reset();
        m_authoredEpoch = epoch;
        m_authoredSimulationFrame = simulationFrame;
        m_authoredFrame = authoredFrame;
        m_authoredLogicFramesPerSecond = logicFramesPerSecond;
        m_authoredCursorInitialized = true;
        return;
    }
    if (simulationFrame < m_authoredSimulationFrame) {
        // A rewind within one presentation epoch cannot retain future
        // particles. The lossless invocation stream will reconstruct them.
        reset();
        m_authoredEpoch = epoch;
        m_authoredSimulationFrame = simulationFrame;
        m_authoredFrame = authoredFrame;
        m_authoredLogicFramesPerSecond = logicFramesPerSecond;
        m_authoredCursorInitialized = true;
        return;
    }
    uint64_t remaining = authoredFrame > m_authoredFrame
        ? authoredFrame - m_authoredFrame : 0;
    while (remaining != 0) {
        const uint32_t slice = static_cast<uint32_t>(std::min<uint64_t>(
            remaining, std::numeric_limits<uint32_t>::max()));
        updateAuthoredFrames(slice);
        remaining -= slice;
    }
    m_authoredSimulationFrame = simulationFrame;
}

void ParticleRuntime::setParticleScale(float scale) noexcept {
    m_particleScale = std::isfinite(scale) ? std::max(0.0f, scale) : 1.0f;
}

void ParticleRuntime::setDynamicAdmissionPolicy(
    ParticlePriority minimumPriority,
    ParticlePriority minimumSkipPriority,
    uint32_t skipMask) noexcept {
    m_admission.minimumPriority =
        static_cast<size_t>(minimumPriority) < kPriorityCount
        ? minimumPriority : ParticlePriority::Invalid;
    m_admission.minimumSkipPriority =
        static_cast<size_t>(minimumSkipPriority) < kPriorityCount
        ? minimumSkipPriority : ParticlePriority::Invalid;
    m_admission.skipMask = skipMask;
}

void ParticleRuntime::reset() {
    m_blocks.clear();
    m_activeBlocks.clear();
    m_emitters.clear();
    m_particleLocations.clear();
    m_slaveEmitters.clear();
    m_controlledEmitters.clear();
    m_groundHeights.reset();
    m_particleCount = 0;
    m_priorityHeads = {};
    m_priorityTails = {};
    m_priorityCounts = {};
    m_ordinaryParticleCount = 0;
    m_fieldParticleCount = 0;
    m_alwaysRenderParticleCount = 0;
    m_nextAdmissionOrdinal = 1;
    m_particleGenerationCount = 0;
    m_authoredEpoch = 0;
    m_authoredSimulationFrame = 0;
    m_authoredFrame = 0;
    m_authoredLogicFramesPerSecond =
        kParticleAuthoredFramesPerSecond;
    m_authoredCursorInitialized = false;
    m_renderInterpolationAlpha = 0.0f;
    m_stats = {};
    m_lastPhaseProfile = {};
    m_phaseProfileOrdinal = 0;
    m_gpuBirthCommands.clear();
    m_gpuRetireCommands.clear();
    m_gpuCommandSequence = 0;
    if (m_gpuCommandCaptureEnabled) {
        m_gpuAuthorityEpoch = nextNonzeroCounter(m_gpuAuthorityEpoch);
    }
}

void ParticleRuntime::setGpuCommandCaptureEnabled(bool enabled) {
    if (enabled == m_gpuCommandCaptureEnabled) return;

    m_gpuBirthCommands.clear();
    m_gpuRetireCommands.clear();
    m_gpuCommandSequence = 0;
    m_gpuCommandCaptureEnabled = enabled;
    if (!enabled) return;

    // A newly enabled consumer must never inherit state from an older GPU
    // authority session. Seed every currently-live compatible particle after
    // advancing the epoch; subsequent mutations are appended incrementally.
    m_gpuAuthorityEpoch = nextNonzeroCounter(m_gpuAuthorityEpoch);
    m_gpuBirthCommands.reserve(m_particleCount);
    for (const AoSoABlockIndex blockIndex : m_activeBlocks) {
        const BillboardParticleBlock& block = m_blocks.get(blockIndex);
        for (size_t lane = 0; lane < block.count; ++lane) {
            appendGpuBirthCommand(block, lane);
        }
    }
}

ParticleGpuCommandBatch ParticleRuntime::takeGpuCommands() {
    ParticleGpuCommandBatch batch;
    batch.authorityEpoch = m_gpuAuthorityEpoch;
    batch.births.swap(m_gpuBirthCommands);
    batch.retires.swap(m_gpuRetireCommands);
    return batch;
}

std::optional<ParticleRuntimeParticle> ParticleRuntime::particle(ParticleHandle handle) const {
    const ParticleLocation* location = m_particleLocations.get(handle);
    if (!location || !m_blocks.contains(location->block)) return std::nullopt;
    const BillboardParticleBlock& block = m_blocks.get(location->block);
    if (location->lane >= block.count || block.handle[location->lane] != handle) {
        return std::nullopt;
    }
    return snapshotLane(block, location->lane);
}

container::Vector<ParticleRuntimeParticle> ParticleRuntime::snapshotParticles() const {
    container::Vector<ParticleRuntimeParticle> result;
    result.reserve(m_particleCount);
    for (const AoSoABlockIndex index : m_activeBlocks) {
        const BillboardParticleBlock& block = m_blocks.get(index);
        for (size_t lane = 0; lane < block.count; ++lane) {
            result.push_back(snapshotLane(block, lane));
        }
    }
    return result;
}

void ParticleRuntime::stepOneAuthoredFrame() {
    m_lastPhaseProfile = {};
    const auto emitterStarted = m_updateSettings.collectPhaseTimings
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    updateControlledEmitters();
    updateEmitters();
    if (m_updateSettings.collectPhaseTimings) {
        m_lastPhaseProfile.emitterUpdateNanoseconds =
            static_cast<uint64_t>(std::max<int64_t>(
                0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - emitterStarted)
                       .count()));
    }
    updateParticles();
    const auto emitterTailStarted = m_updateSettings.collectPhaseTimings
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    updateControlledEmitters();
    cleanupEmitterRelations();
    commitEmitterPositions();
    if (m_updateSettings.collectPhaseTimings) {
        m_lastPhaseProfile.emitterUpdateNanoseconds +=
            static_cast<uint64_t>(std::max<int64_t>(
                0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - emitterTailStarted)
                       .count()));
    }
}

void ParticleRuntime::commitEmitterPositions() noexcept {
    for (size_t denseIndex = 0; denseIndex < m_emitters.size(); ++denseIndex) {
        if (ParticleEmitterState* emitter = m_emitters.get(
                m_emitters.handle_at_dense_index(denseIndex))) {
            emitter->previousAuthoredPosition = emitter->position;
        }
    }
}

void ParticleRuntime::updateControlledEmitters() {
    for (const ControlledEmitterLink& link : m_controlledEmitters) {
        const std::optional<ParticleRuntimeParticle> control = particle(link.particle);
        ParticleEmitterState* emitter = m_emitters.get(link.emitter);
        if (!control || !emitter) continue;
        emitter->position = control->position;
    }
}

void ParticleRuntime::updateEmitters() {
    container::Vector<ParticleEmitterHandle>& frameEmitters =
        m_frameEmitterScratch;
    frameEmitters.clear();
    frameEmitters.reserve(m_emitters.size());
    for (size_t denseIndex = 0; denseIndex < m_emitters.size(); ++denseIndex) {
        frameEmitters.push_back(m_emitters.handle_at_dense_index(denseIndex));
    }

    for (const ParticleEmitterHandle emitter : frameEmitters) {
        ParticleEmitterState* state = m_emitters.get(emitter);
        const ParticleSystemTemplate* definition = state && m_catalog
            ? m_catalog->find(state->templateId)
            : nullptr;
        if (!state) continue;
        if (!definition || !supportedParticleKind(definition->kind)) {
            if (state) state->spawning = false;
        } else if (state->ageFrames < 0.0f) {
            state->ageFrames += 1.0f;
        } else {
            updateEmitterWind(*state, *definition);
            const bool withinLifetime = state->systemLifetimeOverrideFrames
                ? state->ageFrames < static_cast<float>(
                      *state->systemLifetimeOverrideFrames)
                : definition->systemLifetime == 0 ||
                      state->ageFrames < static_cast<float>(
                          definition->systemLifetime);
            if (!withinLifetime) state->spawning = false;

            if (!state->externallyDriven && state->spawning &&
                state->ageFrames >= state->nextBurstFrame &&
                (!definition->oneShot || !state->emittedBurst)) {
                emitBurst(emitter, *state, *definition);
                state = m_emitters.get(emitter);
                if (!state) continue;
                state->emittedBurst = true;
                if (definition->oneShot) state->spawning = false;
            }
            state->ageFrames += 1.0f;
        }

        if (!state->spawning && !state->retainedWhenStopped &&
            state->particleCount == 0) {
            (void)m_emitters.erase(emitter);
        }
    }
}

void ParticleRuntime::updateEmitterWind(
    ParticleEmitterState& state, const ParticleSystemTemplate& definition) {
    if (definition.windMotion == ParticleWindMotion::Circular) {
        state.windAngle = std::fmod(state.windAngle + state.windAngleChange, kTau);
        if (state.windAngle < 0.0f) state.windAngle += kTau;
        return;
    }
    if (definition.windMotion != ParticleWindMotion::PingPong) return;

    const float span = state.windEndAngle - state.windStartAngle;
    if (span <= kDirectionEpsilon) return;
    const float halfSpan = span * 0.5f;
    const float distanceFromCenter = std::abs(
        halfSpan - state.windAngle + state.windStartAngle);
    const float change = std::max(
        0.005f, (1.0f - distanceFromCenter / halfSpan) * state.windAngleChange);
    state.windAngle += state.windMovingToEnd ? change : -change;

    const bool reachedEnd = state.windMovingToEnd
        ? state.windAngle >= state.windEndAngle
        : state.windAngle <= state.windStartAngle;
    if (!reachedEnd) return;

    state.windMovingToEnd = !state.windMovingToEnd;
    const uint64_t epoch = static_cast<uint64_t>(std::max(0.0f, state.ageFrames));
    const uint64_t seed = splitMix64(state.seed ^ (epoch * kStreamStride));
    state.windAngleChange = sampleRange(
        {definition.windAngleChangeMinimum, definition.windAngleChangeMaximum},
        seed, StreamWindAngleChange);
    state.windStartAngle = sampleRange(
        {definition.windPingPongStartAngleMinimum,
         definition.windPingPongStartAngleMaximum},
        seed, StreamWindStartAngle);
    state.windEndAngle = sampleRange(
        {definition.windPingPongEndAngleMinimum,
         definition.windPingPongEndAngleMaximum},
        seed, StreamWindEndAngle);
    if (state.windStartAngle > state.windEndAngle) {
        std::swap(state.windStartAngle, state.windEndAngle);
    }
}

void ParticleRuntime::cleanupEmitterRelations() {
    for (const ControlledEmitterLink& link : m_controlledEmitters) {
        if (particle(link.particle) || !m_emitters.contains(link.emitter)) continue;
        if (ParticleEmitterState* emitter = m_emitters.get(link.emitter)) {
            emitter->spawning = false;
            if (emitter->particleCount == 0) (void)m_emitters.erase(link.emitter);
        }
    }
    std::erase_if(m_controlledEmitters, [this](const ControlledEmitterLink& link) {
        return !particle(link.particle) || !m_emitters.contains(link.emitter);
    });

    for (const SlaveEmitterLink& link : m_slaveEmitters) {
        if (m_emitters.contains(link.master) || !m_emitters.contains(link.slave)) continue;
        if (ParticleEmitterState* slave = m_emitters.get(link.slave)) {
            slave->spawning = false;
            if (slave->particleCount == 0) (void)m_emitters.erase(link.slave);
        }
    }
    std::erase_if(m_slaveEmitters, [this](const SlaveEmitterLink& link) {
        return !m_emitters.contains(link.master) || !m_emitters.contains(link.slave);
    });

    // updateEmitters() runs before updateParticles(), so the last live
    // particle can drain later in the same authored frame. Retire every
    // already-stopped empty emitter here as a post-particle sweep; otherwise
    // one-shot, per-particle attached, and slave emitters remain as inert
    // handles until another unrelated frame happens to visit them.
    size_t denseIndex = 0;
    while (denseIndex < m_emitters.size()) {
        const ParticleEmitterHandle emitter =
            m_emitters.handle_at_dense_index(denseIndex);
        const ParticleEmitterState* state = m_emitters.get(emitter);
        if (state && !state->spawning && !state->retainedWhenStopped &&
            state->particleCount == 0) {
            (void)m_emitters.erase(emitter);
            continue;
        }
        ++denseIndex;
    }
}

void ParticleRuntime::updateParticles() {
    if (m_particleCount == 0) return;

    // updateEmitters() has completed and no emitter/catalog/location/priority
    // mutation is allowed until every integration task joins. Const aliases
    // make that sealed frame-input boundary explicit for worker reads.
    const ParticleSystemCatalog* const catalogSnapshot = m_catalog.get();
    const ParticleEmitterStorage& emitterSnapshot = m_emitters;
    const size_t blockCount = m_activeBlocks.size();
    const bool useParallel = m_updateSettings.parallelIntegration &&
                             m_particleCount >= m_updateSettings.parallelParticleThreshold &&
                             blockCount > 1;
    const size_t blocksPerTask = useParallel ? m_updateSettings.blocksPerTask : blockCount;
    const size_t taskCount = (blockCount + blocksPerTask - 1) / blocksPerTask;
    m_lastPhaseProfile.activeBlocks = blockCount;
    m_lastPhaseProfile.integrationTasks = taskCount;
    m_lastPhaseProfile.parallelIntegration = useParallel;

    if (m_integrationBatches.size() < taskCount) {
        m_integrationBatches.resize(taskCount);
    }
    for (size_t taskIndex = 0; taskIndex < taskCount; ++taskIndex) {
        ParticleIntegrationBatchResult& result = m_integrationBatches[taskIndex];
        result.deaths.clear();
        result.integratedParticles = 0;
        result.expiredParticles = 0;
        const size_t firstBlock = taskIndex * blocksPerTask;
        const size_t lastBlock = std::min(firstBlock + blocksPerTask, blockCount);
        result.deaths.reserve((lastBlock - firstBlock) * kParticleAoSoAWidth);
    }

    const auto integrateBatch = [&](size_t taskIndex) {
        const size_t firstBlock = taskIndex * blocksPerTask;
        const size_t lastBlock = std::min(firstBlock + blocksPerTask, blockCount);
        integrateParticleBlocks(firstBlock, lastBlock, catalogSnapshot, emitterSnapshot,
                                m_integrationBatches[taskIndex]);
    };
    const auto integrationStarted = m_updateSettings.collectPhaseTimings
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    if (useParallel) {
        tf::Taskflow taskflow;
        for (size_t taskIndex = 0; taskIndex < taskCount; ++taskIndex) {
            taskflow.emplace([&, taskIndex] {
                const platform::runtime::ThreadRoleScope role(
                    platform::runtime::ThreadRole::RenderWorker);
                integrateBatch(taskIndex);
            });
        }
        render::parallelExecutor().run(taskflow).wait();
        ++m_stats.parallelIntegrationFrames;
        m_stats.parallelIntegrationTasks += taskCount;
    } else {
        integrateBatch(0);
        ++m_stats.serialIntegrationFrames;
    }
    if (m_updateSettings.collectPhaseTimings) {
        m_lastPhaseProfile.integrationNanoseconds =
            static_cast<uint64_t>(std::max<int64_t>(
                0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - integrationStarted)
                       .count()));
    }

    // Batches cover monotonically increasing block ranges and each batch
    // records lanes in ascending order. Walking both dimensions backwards is
    // therefore exactly original-dense-index descending order. All swap-last,
    // handle relocation, priority-chain and emitter-count writes remain here
    // on the owning thread.
    for (size_t taskIndex = 0; taskIndex < taskCount; ++taskIndex) {
        const ParticleIntegrationBatchResult& result = m_integrationBatches[taskIndex];
        m_stats.integratedParticles += result.integratedParticles;
        m_stats.expiredParticles += result.expiredParticles;
        m_stats.compactedDeadParticles += result.deaths.size();
        m_lastPhaseProfile.integratedParticles +=
            result.integratedParticles;
        m_lastPhaseProfile.compactedDeadParticles +=
            result.deaths.size();
    }
    const auto compactStarted = m_updateSettings.collectPhaseTimings
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    size_t nextDenseIndex = m_particleCount;
    for (size_t taskIndex = taskCount; taskIndex > 0; --taskIndex) {
        const auto& deaths = m_integrationBatches[taskIndex - 1].deaths;
        for (size_t deathIndex = deaths.size(); deathIndex > 0; --deathIndex) {
            const ParticleDeathRecord& death = deaths[deathIndex - 1];
            TD_ASSERT(death.denseIndex < nextDenseIndex);
            nextDenseIndex = death.denseIndex;
            eraseParticle(death.handle);
        }
    }
    if (m_updateSettings.collectPhaseTimings) {
        m_lastPhaseProfile.serialCompactNanoseconds =
            static_cast<uint64_t>(std::max<int64_t>(
                0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - compactStarted)
                       .count()));
    }
}

void ParticleRuntime::integrateParticleBlocks(size_t firstBlock, size_t lastBlock,
                                              const ParticleSystemCatalog* catalogSnapshot,
                                              const ParticleEmitterStorage& emitterSnapshot,
                                              ParticleIntegrationBatchResult& result) {
    for (size_t blockOrdinal = firstBlock; blockOrdinal < lastBlock; ++blockOrdinal) {
        BillboardParticleBlock& block = m_blocks.get(m_activeBlocks[blockOrdinal]);
        const size_t laneCount = block.count;
        for (size_t lane = 0; lane < laneCount; ++lane) {
            const size_t denseIndex = blockOrdinal * kParticleAoSoAWidth + lane;
            const ParticleHandle handle = block.handle[lane];
            const ParticleSystemTemplate* definition =
                catalogSnapshot ? catalogSnapshot->find(block.templateId[lane]) : nullptr;
            if (!definition) {
                result.deaths.push_back({
                    .denseIndex = denseIndex,
                    .handle = handle,
                });
                continue;
            }

            ++result.integratedParticles;

            block.previousX[lane] = block.positionX[lane];
            block.previousY[lane] = block.positionY[lane];
            block.previousZ[lane] = block.positionZ[lane];

            block.velocityZ[lane] += block.gravity[lane];
            block.velocityX[lane] *= block.velocityDamping[lane];
            block.velocityY[lane] *= block.velocityDamping[lane];
            block.velocityZ[lane] *= block.velocityDamping[lane];
            block.positionX[lane] += block.velocityX[lane] + block.driftX[lane];
            block.positionY[lane] += block.velocityY[lane] + block.driftY[lane];
            block.positionZ[lane] += block.velocityZ[lane] + block.driftZ[lane];

            if (block.windMotion[lane] == ParticleWindMotion::PingPong ||
                block.windMotion[lane] == ParticleWindMotion::Circular) {
                const ParticleEmitterState* emitter = emitterSnapshot.get(block.emitter[lane]);
                const ParticleVector3 origin =
                    emitter
                        ? emitter->position
                        : ParticleVector3{block.emitterOriginX[lane], block.emitterOriginY[lane],
                                          block.emitterOriginZ[lane]};
                const float deltaX = block.positionX[lane] - origin.x;
                const float deltaY = block.positionY[lane] - origin.y;
                const float deltaZ = block.positionZ[lane] - origin.z;
                const float distance =
                    std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
                if (distance < kWindOuterRadius) {
                    float strength = 2.0f * block.windRandomness[lane];
                    if (distance > kWindFullStrengthRadius) {
                        strength *=
                            1.0f - (distance - kWindFullStrengthRadius) / kWindFalloffDistance;
                    }
                    const float windAngle = emitter ? emitter->windAngle : 0.0f;
                    block.positionX[lane] += std::cos(windAngle) * strength;
                    block.positionY[lane] += std::sin(windAngle) * strength;
                }
            }

            block.angleX[lane] += block.angularRateX[lane];
            block.angleY[lane] += block.angularRateY[lane];
            block.angle[lane] += block.angularRate[lane];
            block.angularRateX[lane] *= block.angularDamping[lane];
            block.angularRateY[lane] *= block.angularDamping[lane];
            block.angularRate[lane] *= block.angularDamping[lane];
            if (block.particleUpTowardsEmitter[lane] != 0) {
                // RefCode stores the emitter origin in ParticleInfo at birth. It
                // does not chase a subsequently moved system while orienting an
                // existing particle (and a slave carries its master's origin).
                const float directionX = block.positionX[lane] - block.emitterOriginX[lane];
                const float directionY = block.positionY[lane] - block.emitterOriginY[lane];
                if (directionX != 0.0f || directionY != 0.0f) {
                    block.angle[lane] = particleUpAngle(directionX, directionY);
                }
            }

            block.size[lane] += block.sizeRate[lane];
            block.sizeRate[lane] *= block.sizeRateDamping[lane];
            block.ageFrames[lane] += 1.0f;
            block.alpha[lane] = std::clamp(
                evaluateAlpha(*definition, block.seed[lane], block.ageFrames[lane]), 0.0f, 1.0f);
            const ParticleVector3 baseColor = evaluateColor(*definition, block.ageFrames[lane],
                                                            {
                                                                block.laterColorKeyTintRed[lane],
                                                                block.laterColorKeyTintGreen[lane],
                                                                block.laterColorKeyTintBlue[lane],
                                                            });
            const float colorAdjustment = block.colorScale[lane] * block.ageFrames[lane];
            block.red[lane] = std::clamp(baseColor.x + colorAdjustment, 0.0f, 1.0f);
            block.green[lane] = std::clamp(baseColor.y + colorAdjustment, 0.0f, 1.0f);
            block.blue[lane] = std::clamp(baseColor.z + colorAdjustment, 0.0f, 1.0f);

            const bool finiteLifetime = block.lifetimeFrames[lane] > 0.0f;
            if ((finiteLifetime && block.ageFrames[lane] >= block.lifetimeFrames[lane]) ||
                shaderMakesParticleInvisible(*definition, block.ageFrames[lane], block.alpha[lane],
                                             block.red[lane], block.green[lane],
                                             block.blue[lane])) {
                ++result.expiredParticles;
                result.deaths.push_back({
                    .denseIndex = denseIndex,
                    .handle = handle,
                });
                continue;
            }
        }
    }
}

void ParticleRuntime::emitBurst(ParticleEmitterHandle emitter, ParticleEmitterState& state,
                                const ParticleSystemTemplate& definition) {
    const uint64_t burstSeed = splitMix64(state.seed ^
        (state.nextBurstOrdinal++ * kStreamStride));
    const float sampledCount = std::max(0.0f,
        sampleRange(definition.burstCount, burstSeed, StreamBurstCount) *
            state.burstCountMultiplier);
    const uint32_t count = sampledCount >= static_cast<float>(std::numeric_limits<uint32_t>::max())
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(sampledCount);
    container::Vector<std::pair<ParticleHandle, uint64_t>>& attachedParents =
        m_attachedParentScratch[std::min<size_t>(
            state.relationDepth, kMaximumRelatedSystemDepth)];
    attachedParents.clear();
    if (definition.perParticleAttachedSystem) {
        attachedParents.reserve(std::min<size_t>(
            count, m_maximumParticles - m_particleCount));
    }
    for (uint32_t particleIndex = 0; particleIndex < count; ++particleIndex) {
        const uint64_t particleSeed = splitMix64(state.seed ^
            (state.nextParticleOrdinal++ * kStreamStride));
        const ParticleVector3 currentPosition = state.position;
        const float pathAmount = count == 0
            ? 1.0f
            : static_cast<float>(particleIndex) /
                static_cast<float>(count);
        state.position = add(
            state.previousAuthoredPosition,
            multiply(subtract(currentPosition,
                              state.previousAuthoredPosition),
                     pathAmount));
        const std::optional<ParticleHandle> particleHandle =
            createParticle(emitter, state, definition, particleSeed);
        state.position = currentPosition;
        if (!particleHandle) continue;

        size_t slaveOrdinal = 0;
        for (const SlaveEmitterLink& link : m_slaveEmitters) {
            if (link.master != emitter) continue;
            ParticleEmitterState* slaveState = m_emitters.get(link.slave);
            const ParticleSystemTemplate* slaveDefinition = slaveState && m_catalog
                ? m_catalog->find(slaveState->templateId)
                : nullptr;
            if (!slaveState || !slaveDefinition) {
                ++m_stats.rejectedRelatedSystems;
                continue;
            }
            const uint64_t slaveSeed = splitMix64(
                particleSeed ^ StreamRelatedSystem ^
                (static_cast<uint64_t>(++slaveOrdinal) * kStreamStride));
            if (!createSlaveParticle(link.slave, *slaveState, *slaveDefinition,
                                     *particleHandle, link.offset, slaveSeed)) {
                ++m_stats.rejectedRelatedSystems;
            }
        }

        if (definition.perParticleAttachedSystem) {
            attachedParents.emplace_back(*particleHandle, particleSeed);
        }
    }

    const float delay = sampledWholeFrames(definition.burstDelay, burstSeed, StreamBurstDelay);
    state.nextBurstFrame = state.ageFrames + delay + 1.0f;
    const uint8_t relationDepth = state.relationDepth;
    for (const auto& [particleHandle, particleSeed] : attachedParents) {
        createPerParticleAttachedEmitter(
            particleHandle, definition, relationDepth,
            splitMix64(particleSeed ^ StreamRelatedSystem));
    }
    attachedParents.clear();
}

bool ParticleRuntime::evictOldestAtOrBelow(
    ParticlePriority maximumPriority, bool allowSamePriority,
    bool alwaysRenderPreemption) {
    const size_t maximumIndex = priorityIndex(maximumPriority);
    const size_t end = std::min(
        kPriorityCount, maximumIndex + (allowSamePriority ? 1u : 0u));
    for (size_t index = priorityIndex(ParticlePriority::Invalid);
         index < end; ++index) {
        const ParticleHandle oldest = m_priorityHeads[index];
        if (!oldest) continue;
        const size_t before = m_particleCount;
        eraseParticle(oldest);
        if (m_particleCount == before) continue;
        ++m_stats.evictedParticles;
        if (index < maximumIndex) {
            ++m_stats.evictedLowerPriorityParticles;
        } else {
            ++m_stats.evictedSamePriorityParticles;
        }
        if (alwaysRenderPreemption) ++m_stats.alwaysRenderPreemptions;
        return true;
    }
    return false;
}

bool ParticleRuntime::admitParticle(
    const ParticleSystemTemplate& definition, ParticlePriority priority) {
    if (!priorityAtLeast(priority, m_admission.minimumPriority)) {
        ++m_stats.rejectedParticles;
        ++m_stats.rejectedByPriority;
        return false;
    }

    if (!priorityAtLeast(priority, m_admission.minimumSkipPriority)) {
        ++m_particleGenerationCount;
        if ((m_particleGenerationCount & m_admission.skipMask) !=
            m_admission.skipMask) {
            ++m_stats.rejectedParticles;
            ++m_stats.rejectedBySkipMask;
            return false;
        }
    }

    const bool fieldParticle = priority == ParticlePriority::AreaEffect &&
        definition.groundAligned;
    if (fieldParticle &&
        m_fieldParticleCount >= m_admission.fieldParticleLimit) {
        ++m_stats.rejectedParticles;
        ++m_stats.rejectedFieldParticles;
        return false;
    }

    if (priority == ParticlePriority::AlwaysRender) {
        const bool beyondOrdinaryLimit =
            m_particleCount >= m_admission.ordinaryParticleLimit;
        // RefCode lets ALWAYS_RENDER escape the ordinary cap. The modern pool
        // stays bounded: at the hard ceiling a new logically-important
        // particle preempts the globally oldest strictly lower priority. If
        // the pool contains only ALWAYS_RENDER particles, the new particle is
        // rejected rather than replacing an equal-priority original. Thus
        // ordinary content can never consume the logical reserve.
        if (m_particleCount >= m_maximumParticles &&
            !evictOldestAtOrBelow(ParticlePriority::AlwaysRender, false, true)) {
            ++m_stats.rejectedParticles;
            ++m_stats.rejectedHardCeiling;
            return false;
        }
        if (beyondOrdinaryLimit) {
            ++m_stats.alwaysRenderAdmissionsBeyondOrdinaryLimit;
        }
        return true;
    }

    if (m_admission.ordinaryParticleLimit == 0) {
        ++m_stats.rejectedParticles;
        ++m_stats.rejectedOrdinaryCapacity;
        return false;
    }
    if (m_ordinaryParticleCount >= m_admission.ordinaryParticleLimit &&
        !evictOldestAtOrBelow(priority, false, false)) {
        ++m_stats.rejectedParticles;
        ++m_stats.rejectedOrdinaryCapacity;
        return false;
    }
    if (m_particleCount >= m_maximumParticles &&
        !evictOldestAtOrBelow(priority, false, false)) {
        ++m_stats.rejectedParticles;
        ++m_stats.rejectedHardCeiling;
        return false;
    }
    return true;
}

std::optional<ParticleHandle> ParticleRuntime::createParticle(
    ParticleEmitterHandle emitter, ParticleEmitterState& emitterState,
    const ParticleSystemTemplate& definition, uint64_t seed,
    ParticlePriority priorityOverride, bool enforceAboveGround,
    bool captureGpuBirth) {
    const ParticlePriority priority = priorityOverride == ParticlePriority::Count
        ? definition.priority : priorityOverride;
    const float motionScale = 0.5f + 0.5f * m_particleScale;
    const ParticleVector3 localPosition = multiply(
        sampleVolumePosition(
            definition, emitterState.emissionRadiusOverride, seed),
        motionScale);
    const ParticleVector3 rotatedPosition = rotateEuler(
        localPosition, emitterState.rollRadians, emitterState.pitchRadians,
        emitterState.yawRadians);
    const ParticleVector3 worldPosition = add(emitterState.position, rotatedPosition);
    if (enforceAboveGround && definition.emitAboveGroundOnly && m_groundHeights) {
        const std::optional<float> ground = sampleGroundHeight(
            *m_groundHeights, worldPosition.x, worldPosition.y);
        if (ground && worldPosition.z < *ground) {
            ++m_stats.rejectedBelowGround;
            return std::nullopt;
        }
    }
    if (!admitParticle(definition, priority)) return std::nullopt;

    AoSoABlockIndex blockIndex;
    if (m_activeBlocks.empty() ||
        m_blocks.get(m_activeBlocks.back()).count == kParticleAoSoAWidth) {
        const std::optional<AoSoABlockIndex> allocated = m_blocks.allocateBlock();
        if (!allocated) {
            ++m_stats.rejectedParticles;
            ++m_stats.rejectedHardCeiling;
            return std::nullopt;
        }
        blockIndex = *allocated;
        m_activeBlocks.push_back(blockIndex);
    } else {
        blockIndex = m_activeBlocks.back();
    }

    BillboardParticleBlock& block = m_blocks.get(blockIndex);
    const size_t lane = block.count;
    const ParticleHandle handle = m_particleLocations.emplace(
        ParticleLocation{.block = blockIndex, .lane = static_cast<uint8_t>(lane)});
    ParticleVector3 velocity = multiply(
        rotateEuler(
            sampleVelocity(definition, localPosition, seed),
            emitterState.rollRadians, emitterState.pitchRadians,
            emitterState.yawRadians),
        motionScale);
    velocity.x *= emitterState.velocityMultiplier.x;
    velocity.y *= emitterState.velocityMultiplier.y;
    velocity.z *= emitterState.velocityMultiplier.z;
    const ParticleVector3 drift = rotateEuler(
        definition.driftVelocity, emitterState.rollRadians, emitterState.pitchRadians,
        emitterState.yawRadians);

    block.positionX[lane] = worldPosition.x;
    block.positionY[lane] = worldPosition.y;
    block.positionZ[lane] = worldPosition.z;
    block.previousX[lane] = worldPosition.x;
    block.previousY[lane] = worldPosition.y;
    block.previousZ[lane] = worldPosition.z;
    block.velocityX[lane] = velocity.x;
    block.velocityY[lane] = velocity.y;
    block.velocityZ[lane] = velocity.z;
    block.driftX[lane] = drift.x;
    block.driftY[lane] = drift.y;
    block.driftZ[lane] = drift.z;
    block.emitterOriginX[lane] = emitterState.position.x;
    block.emitterOriginY[lane] = emitterState.position.y;
    block.emitterOriginZ[lane] = emitterState.position.z;
    block.windRandomness[lane] = std::lerp(
        0.7f, 1.3f, unitRandom(seed, StreamWindRandomness));
    block.gravity[lane] = definition.gravity;
    block.windMotion[lane] = definition.windMotion;
    block.particleUpTowardsEmitter[lane] = static_cast<uint8_t>(
        definition.particleUpTowardsEmitter);
    block.ageFrames[lane] = 0.0f;
    block.lifetimeFrames[lane] = std::max(0.0f,
        sampledWholeFrames(definition.lifetime, seed, StreamLifetime));
    block.size[lane] =
        sampleRange(definition.startSize, seed, StreamStartSize) *
            m_particleScale * emitterState.sizeMultiplier +
        emitterState.accumulatedSizeBonus;
    block.sizeRate[lane] =
        sampleRange(definition.sizeRate, seed, StreamSizeRate) *
        m_particleScale * emitterState.sizeMultiplier;
    block.sizeRateDamping[lane] = sampleRange(definition.sizeRateDamping, seed,
                                              StreamSizeRateDamping);
    block.angleX[lane] = sampleRange(definition.angleX, seed, StreamAngleX);
    block.angleY[lane] = sampleRange(definition.angleY, seed, StreamAngleY);
    block.angle[lane] = sampleRange(definition.angleZ, seed, StreamAngle);
    block.angularRateX[lane] = sampleRange(
        definition.angularRateX, seed, StreamAngularRateX);
    block.angularRateY[lane] = sampleRange(
        definition.angularRateY, seed, StreamAngularRateY);
    block.angularRate[lane] = sampleRange(definition.angularRateZ, seed, StreamAngularRate);
    block.angularDamping[lane] = sampleRange(definition.angularDamping, seed,
                                             StreamAngularDamping);
    block.velocityDamping[lane] = sampleRange(definition.velocityDamping, seed,
                                              StreamVelocityDamping);
    block.colorScale[lane] = sampleRange(definition.colorScale, seed, StreamColorScale) / 255.0f;
    block.alpha[lane] = std::clamp(sampleAlphaKey(definition, 0, seed), 0.0f, 1.0f);
    const ParticleVector3 color = colorValue(definition.colorKeys[0].color);
    block.red[lane] = color.x;
    block.green[lane] = color.y;
    block.blue[lane] = color.z;
    block.laterColorKeyTintRed[lane] = emitterState.laterColorKeyTint.x;
    block.laterColorKeyTintGreen[lane] = emitterState.laterColorKeyTint.y;
    block.laterColorKeyTintBlue[lane] = emitterState.laterColorKeyTint.z;
    block.seed[lane] = seed;
    block.ordinal[lane] = emitterState.nextParticleOrdinal == 0
        ? 0 : emitterState.nextParticleOrdinal - 1;
    block.templateId[lane] = definition.id;
    block.emitter[lane] = emitter;
    block.handle[lane] = handle;
    block.admissionOrdinal[lane] = m_nextAdmissionOrdinal++;
    block.priority[lane] = priority;
    block.fieldParticle[lane] = static_cast<uint8_t>(
        priority == ParticlePriority::AreaEffect &&
        definition.groundAligned);
    linkPriorityTail(handle, block, lane, priority);
    ++block.count;

    emitterState.accumulatedSizeBonus += sampleRange(definition.startSizeRate, seed,
                                                     StreamStartSizeRate);
    emitterState.accumulatedSizeBonus = std::min(emitterState.accumulatedSizeBonus,
                                                 kMaximumAccumulatedSizeBonus);
    ++emitterState.particleCount;
    ++m_particleCount;
    if (priority == ParticlePriority::AlwaysRender) {
        ++m_alwaysRenderParticleCount;
    } else {
        ++m_ordinaryParticleCount;
    }
    if (block.fieldParticle[lane] != 0) ++m_fieldParticleCount;
    ++m_stats.emittedParticles;
    m_stats.particleHighWater = std::max(m_stats.particleHighWater, m_particleCount);
    m_stats.ordinaryParticleHighWater = std::max(
        m_stats.ordinaryParticleHighWater, m_ordinaryParticleCount);
    m_stats.fieldParticleHighWater = std::max(
        m_stats.fieldParticleHighWater, m_fieldParticleCount);
    m_stats.alwaysRenderParticleHighWater = std::max(
        m_stats.alwaysRenderParticleHighWater, m_alwaysRenderParticleCount);
    if (captureGpuBirth) appendGpuBirthCommand(block, lane);
    return handle;
}

std::optional<ParticleHandle> ParticleRuntime::createSlaveParticle(
    ParticleEmitterHandle emitter, ParticleEmitterState& emitterState,
    const ParticleSystemTemplate& definition, ParticleHandle masterParticle,
    ParticleVector3 offset, uint64_t seed) {
    const ParticleLocation* masterLocation = m_particleLocations.get(masterParticle);
    if (!masterLocation || !m_blocks.contains(masterLocation->block)) return std::nullopt;
    const BillboardParticleBlock& masterBlock = m_blocks.get(masterLocation->block);
    const size_t masterLane = masterLocation->lane;
    if (masterLane >= masterBlock.count ||
        masterBlock.handle[masterLane] != masterParticle) {
        return std::nullopt;
    }

    ParticleEmitterState* masterEmitter =
        m_emitters.get(masterBlock.emitter[masterLane]);
    const ParticleSystemTemplate* masterDefinition = m_catalog
        ? m_catalog->find(masterBlock.templateId[masterLane])
        : nullptr;
    if (!masterEmitter || !masterDefinition) return std::nullopt;

    // RefCode does not merge the already-created master particle. It asks the
    // master system to generate a second independent ParticleInfo, then lets
    // the slave override lifetime/keys/rotation and multiply the size terms.
    const uint64_t masterSeed = splitMix64(seed ^ StreamRelatedSystem);
    const float motionScale = 0.5f + 0.5f * m_particleScale;
    const ParticleVector3 masterLocal = multiply(
        sampleVolumePosition(*masterDefinition,
                             masterEmitter->emissionRadiusOverride,
                             masterSeed),
        motionScale);
    const ParticleVector3 masterPosition = add(
        masterEmitter->position,
        rotateEuler(masterLocal, masterEmitter->rollRadians,
                    masterEmitter->pitchRadians,
                    masterEmitter->yawRadians));
    const ParticleVector3 masterVelocity = multiply(
        rotateEuler(sampleVelocity(*masterDefinition, masterLocal, masterSeed),
                    masterEmitter->rollRadians,
                    masterEmitter->pitchRadians,
                    masterEmitter->yawRadians),
        motionScale);
    const float masterSize =
        sampleRange(masterDefinition->startSize, masterSeed,
                    StreamStartSize) * m_particleScale +
        masterEmitter->accumulatedSizeBonus;
    const float masterSizeRate =
        sampleRange(masterDefinition->sizeRate, masterSeed,
                    StreamSizeRate) * m_particleScale;
    const float masterSizeRateDamping = sampleRange(
        masterDefinition->sizeRateDamping, masterSeed,
        StreamSizeRateDamping);
    const float masterVelocityDamping = sampleRange(
        masterDefinition->velocityDamping, masterSeed,
        StreamVelocityDamping);
    const float masterWindRandomness = std::lerp(
        0.7f, 1.3f, unitRandom(masterSeed, StreamWindRandomness));
    masterEmitter->accumulatedSizeBonus = std::min(
        masterEmitter->accumulatedSizeBonus +
            sampleRange(masterDefinition->startSizeRate, masterSeed,
                        StreamStartSizeRate),
        kMaximumAccumulatedSizeBonus);

    // The original creates the slave particle with the master's priority,
    // even though its remaining system-level behaviour belongs to the slave.
    ++emitterState.nextParticleOrdinal;
    const std::optional<ParticleHandle> slave = createParticle(
        emitter, emitterState, definition, seed,
        masterBlock.priority[masterLane], false, false);
    if (!slave) return std::nullopt;
    ParticleLocation* slaveLocation = m_particleLocations.get(*slave);
    if (!slaveLocation || !m_blocks.contains(slaveLocation->block)) return std::nullopt;
    BillboardParticleBlock& slaveBlock = m_blocks.get(slaveLocation->block);
    const size_t slaveLane = slaveLocation->lane;
    if (slaveLane >= slaveBlock.count || slaveBlock.handle[slaveLane] != *slave) {
        return std::nullopt;
    }

    slaveBlock.positionX[slaveLane] = masterPosition.x + offset.x;
    slaveBlock.positionY[slaveLane] = masterPosition.y + offset.y;
    slaveBlock.positionZ[slaveLane] = masterPosition.z + offset.z;
    slaveBlock.previousX[slaveLane] = masterPosition.x + offset.x;
    slaveBlock.previousY[slaveLane] = masterPosition.y + offset.y;
    slaveBlock.previousZ[slaveLane] = masterPosition.z + offset.z;
    slaveBlock.velocityX[slaveLane] = masterVelocity.x;
    slaveBlock.velocityY[slaveLane] = masterVelocity.y;
    slaveBlock.velocityZ[slaveLane] = masterVelocity.z;
    slaveBlock.velocityDamping[slaveLane] = masterVelocityDamping;
    slaveBlock.windRandomness[slaveLane] = masterWindRandomness;
    slaveBlock.emitterOriginX[slaveLane] = masterEmitter->position.x;
    slaveBlock.emitterOriginY[slaveLane] = masterEmitter->position.y;
    slaveBlock.emitterOriginZ[slaveLane] = masterEmitter->position.z;
    slaveBlock.particleUpTowardsEmitter[slaveLane] = static_cast<uint8_t>(
        masterDefinition->particleUpTowardsEmitter);
    slaveBlock.size[slaveLane] *= masterSize;
    slaveBlock.sizeRate[slaveLane] *= masterSizeRate;
    slaveBlock.sizeRateDamping[slaveLane] *= masterSizeRateDamping;
    appendGpuBirthCommand(slaveBlock, slaveLane);
    ++m_stats.emittedSlaveParticles;
    return slave;
}

void ParticleRuntime::createPerParticleAttachedEmitter(
    ParticleHandle particleHandle, const ParticleSystemTemplate& definition,
    uint8_t relationDepth, uint64_t seed) {
    if (!definition.perParticleAttachedSystem) {
        if (!definition.perParticleAttachedSystemName.empty()) {
            ++m_stats.rejectedRelatedSystems;
        }
        return;
    }
    if (relationDepth >= kMaximumRelatedSystemDepth) {
        ++m_stats.rejectedRelatedSystems;
        return;
    }
    const std::optional<ParticleRuntimeParticle> control = particle(particleHandle);
    if (!control) {
        ++m_stats.rejectedRelatedSystems;
        return;
    }

    container::Vector<ParticleTemplateId>& recursionStack =
        m_relatedSystemRecursionScratch[std::min<size_t>(
            static_cast<size_t>(relationDepth) + 1u,
            kMaximumRelatedSystemDepth)];
    recursionStack.clear();
    recursionStack.reserve(kMaximumRelatedSystemDepth);
    recursionStack.push_back(definition.id);
    const ParticleEmitterHandle emitter = createEmitterInternal({
        .templateId = definition.perParticleAttachedSystem,
        .position = control->position,
        .seed = seed,
    }, static_cast<uint8_t>(relationDepth + 1), false, recursionStack);
    if (!emitter) {
        ++m_stats.rejectedRelatedSystems;
        return;
    }
    m_controlledEmitters.push_back({
        .particle = particleHandle,
        .emitter = emitter,
    });
    primeEmitter(emitter);
    ++m_stats.spawnedAttachedEmitters;
}

void ParticleRuntime::linkPriorityTail(
    ParticleHandle handle, BillboardParticleBlock& block, size_t lane,
    ParticlePriority priority) {
    const size_t index = priorityIndex(priority);
    const ParticleHandle previous = m_priorityTails[index];
    block.priorityPrevious[lane] = previous;
    block.priorityNext[lane] = {};
    if (previous) {
        ParticleLocation* previousLocation = m_particleLocations.get(previous);
        if (previousLocation && m_blocks.contains(previousLocation->block)) {
            BillboardParticleBlock& previousBlock =
                m_blocks.get(previousLocation->block);
            if (previousLocation->lane < previousBlock.count &&
                previousBlock.handle[previousLocation->lane] == previous) {
                previousBlock.priorityNext[previousLocation->lane] = handle;
            }
        }
    } else {
        m_priorityHeads[index] = handle;
    }
    m_priorityTails[index] = handle;
    ++m_priorityCounts[index];
}

void ParticleRuntime::unlinkPriority(
    ParticleHandle handle, BillboardParticleBlock& block, size_t lane) {
    const size_t index = priorityIndex(block.priority[lane]);
    const ParticleHandle previous = block.priorityPrevious[lane];
    const ParticleHandle next = block.priorityNext[lane];
    if (previous) {
        ParticleLocation* previousLocation = m_particleLocations.get(previous);
        if (previousLocation && m_blocks.contains(previousLocation->block)) {
            BillboardParticleBlock& previousBlock =
                m_blocks.get(previousLocation->block);
            if (previousLocation->lane < previousBlock.count &&
                previousBlock.handle[previousLocation->lane] == previous) {
                previousBlock.priorityNext[previousLocation->lane] = next;
            }
        }
    } else if (m_priorityHeads[index] == handle) {
        m_priorityHeads[index] = next;
    }
    if (next) {
        ParticleLocation* nextLocation = m_particleLocations.get(next);
        if (nextLocation && m_blocks.contains(nextLocation->block)) {
            BillboardParticleBlock& nextBlock = m_blocks.get(nextLocation->block);
            if (nextLocation->lane < nextBlock.count &&
                nextBlock.handle[nextLocation->lane] == next) {
                nextBlock.priorityPrevious[nextLocation->lane] = previous;
            }
        }
    } else if (m_priorityTails[index] == handle) {
        m_priorityTails[index] = previous;
    }
    if (m_priorityCounts[index] > 0) --m_priorityCounts[index];
    block.priorityPrevious[lane] = {};
    block.priorityNext[lane] = {};
}

void ParticleRuntime::appendGpuBirthCommand(
    const BillboardParticleBlock& block, size_t lane) {
    if (!m_gpuCommandCaptureEnabled || lane >= block.count || !m_catalog) return;
    const ParticleSystemTemplate* definition =
        m_catalog->find(block.templateId[lane]);
    if (!definition || !definition->gpuCompatible()) return;

    const ParticleHandle particleHandle = block.handle[lane];
    const ParticleEmitterHandle emitterHandle = block.emitter[lane];
    if (!particleHandle) return;

    gpu_particle::GpuParticleBirthCommand command;
    gpu_particle::GpuParticleState& state = command.initialState;
    state.positionAndAge[0] = block.positionX[lane];
    state.positionAndAge[1] = block.positionY[lane];
    state.positionAndAge[2] = block.positionZ[lane];
    state.positionAndAge[3] = block.ageFrames[lane];
    state.previousAndLifetime[0] = block.previousX[lane];
    state.previousAndLifetime[1] = block.previousY[lane];
    state.previousAndLifetime[2] = block.previousZ[lane];
    state.previousAndLifetime[3] = block.lifetimeFrames[lane];
    state.velocityAndGravity[0] = block.velocityX[lane];
    state.velocityAndGravity[1] = block.velocityY[lane];
    state.velocityAndGravity[2] = block.velocityZ[lane];
    state.velocityAndGravity[3] = block.gravity[lane];
    state.driftAndVelocityDamping[0] = block.driftX[lane];
    state.driftAndVelocityDamping[1] = block.driftY[lane];
    state.driftAndVelocityDamping[2] = block.driftZ[lane];
    state.driftAndVelocityDamping[3] = block.velocityDamping[lane];
    state.sizeDynamicsAndAngle[0] = block.size[lane];
    state.sizeDynamicsAndAngle[1] = block.sizeRate[lane];
    state.sizeDynamicsAndAngle[2] = block.sizeRateDamping[lane];
    state.sizeDynamicsAndAngle[3] = block.angle[lane];
    state.angularDynamicsAndAlpha[0] = block.angularRate[lane];
    state.angularDynamicsAndAlpha[1] = block.angularDamping[lane];
    state.angularDynamicsAndAlpha[2] = block.colorScale[lane];
    state.angularDynamicsAndAlpha[3] = block.alpha[lane];
    state.colorAndWindRandomness[0] = block.red[lane];
    state.colorAndWindRandomness[1] = block.green[lane];
    state.colorAndWindRandomness[2] = block.blue[lane];
    state.colorAndWindRandomness[3] = block.windRandomness[lane];
    state.emitterOriginAndReserved[0] = block.emitterOriginX[lane];
    state.emitterOriginAndReserved[1] = block.emitterOriginY[lane];
    state.emitterOriginAndReserved[2] = block.emitterOriginZ[lane];
    state.laterColorKeyTintAndReserved[0] = block.laterColorKeyTintRed[lane];
    state.laterColorKeyTintAndReserved[1] = block.laterColorKeyTintGreen[lane];
    state.laterColorKeyTintAndReserved[2] = block.laterColorKeyTintBlue[lane];
    state.identityAndFlags[0] = block.templateId[lane].value;
    state.identityAndFlags[1] = static_cast<uint32_t>(block.seed[lane]);
    state.identityAndFlags[2] = static_cast<uint32_t>(block.seed[lane] >> 32u);
    state.identityAndFlags[3] = gpu_particle::StateAlive |
        (definition->groundAligned ? gpu_particle::StateGroundAligned : 0u) |
        (block.particleUpTowardsEmitter[lane] != 0
             ? gpu_particle::StateParticleUpTowardsEmitter : 0u) |
        (definition->shader == ParticleShader::AlphaTest
             ? gpu_particle::StateAlphaTest : 0u);
    state.authorityTokens[0] = particleHandle.index;
    state.authorityTokens[1] = particleHandle.generation;
    state.authorityTokens[2] = emitterHandle.index;
    state.authorityTokens[3] = emitterHandle.generation;
    static_assert(kParticleKeyframeCount == 8u,
                  "GPU particle curve contract requires exactly eight keys");
    const ParticleVector3 laterKeyTint = {
        block.laterColorKeyTintRed[lane],
        block.laterColorKeyTintGreen[lane],
        block.laterColorKeyTintBlue[lane],
    };
    for (size_t key = 0; key < kParticleKeyframeCount; ++key) {
        const size_t vectorIndex = key / 4u;
        const size_t componentIndex = key % 4u;
        state.alphaKeyValues[vectorIndex][componentIndex] =
            sampleAlphaKey(*definition, key, block.seed[lane]);
        state.alphaKeyFrames[vectorIndex][componentIndex] =
            definition->alphaKeys[key].frame;

        ParticleVector3 authoredColor = colorValue(
            definition->colorKeys[key].color);
        // RefCode deliberately leaves color key zero untouched and applies
        // the emitter/slave tint only to subsequent authored keys.
        if (key != 0u) {
            authoredColor.x *= laterKeyTint.x;
            authoredColor.y *= laterKeyTint.y;
            authoredColor.z *= laterKeyTint.z;
        }
        state.colorKeyValues[key][0] = authoredColor.x;
        state.colorKeyValues[key][1] = authoredColor.y;
        state.colorKeyValues[key][2] = authoredColor.z;
        state.colorKeyFrames[vectorIndex][componentIndex] =
            definition->colorKeys[key].frame;
    }
    command.destinationIndex = particleHandle.index;
    command.authorityEpoch = m_gpuAuthorityEpoch;
    m_gpuCommandSequence = nextNonzeroCounter(m_gpuCommandSequence);
    command.commandSequence = m_gpuCommandSequence;
    m_gpuBirthCommands.push_back(command);
}

void ParticleRuntime::appendGpuRetireCommand(
    const BillboardParticleBlock& block, size_t lane) {
    if (!m_gpuCommandCaptureEnabled || lane >= block.count || !m_catalog) return;
    const ParticleSystemTemplate* definition =
        m_catalog->find(block.templateId[lane]);
    if (!definition || !definition->gpuCompatible()) return;
    const ParticleHandle particleHandle = block.handle[lane];
    if (!particleHandle) return;

    gpu_particle::GpuParticleRetireCommand command;
    command.destinationIndex = particleHandle.index;
    command.particleGeneration = particleHandle.generation;
    command.authorityEpoch = m_gpuAuthorityEpoch;
    m_gpuCommandSequence = nextNonzeroCounter(m_gpuCommandSequence);
    command.commandSequence = m_gpuCommandSequence;
    m_gpuRetireCommands.push_back(command);
}

void ParticleRuntime::eraseParticle(ParticleHandle handle) {
    ParticleLocation* location = m_particleLocations.get(handle);
    if (!location || m_activeBlocks.empty()) return;
    const ParticleLocation removedLocation = *location;
    BillboardParticleBlock& removedBlock = m_blocks.get(removedLocation.block);
    if (removedLocation.lane >= removedBlock.count ||
        removedBlock.handle[removedLocation.lane] != handle) {
        return;
    }

    const ParticleEmitterHandle removedEmitter = removedBlock.emitter[removedLocation.lane];
    const ParticlePriority removedPriority = removedBlock.priority[removedLocation.lane];
    const bool removedField = removedBlock.fieldParticle[removedLocation.lane] != 0;
    appendGpuRetireCommand(removedBlock, removedLocation.lane);
    unlinkPriority(handle, removedBlock, removedLocation.lane);
    const AoSoABlockIndex lastBlockIndex = m_activeBlocks.back();
    BillboardParticleBlock& lastBlock = m_blocks.get(lastBlockIndex);
    const size_t lastLane = static_cast<size_t>(lastBlock.count - 1);
    if (removedLocation.block != lastBlockIndex || removedLocation.lane != lastLane) {
        const ParticleHandle movedHandle = lastBlock.handle[lastLane];
        copyLane(removedBlock, removedLocation.lane, lastBlock, lastLane);
        if (ParticleLocation* movedLocation = m_particleLocations.get(movedHandle)) {
            movedLocation->block = removedLocation.block;
            movedLocation->lane = removedLocation.lane;
        }
    }

    --lastBlock.count;
    (void)m_particleLocations.erase(handle);
    --m_particleCount;
    if (removedPriority == ParticlePriority::AlwaysRender) {
        if (m_alwaysRenderParticleCount > 0) --m_alwaysRenderParticleCount;
    } else if (m_ordinaryParticleCount > 0) {
        --m_ordinaryParticleCount;
    }
    if (removedField && m_fieldParticleCount > 0) --m_fieldParticleCount;
    if (lastBlock.count == 0) {
        (void)m_blocks.releaseBlock(lastBlockIndex);
        m_activeBlocks.pop_back();
    }

    if (ParticleEmitterState* emitter = m_emitters.get(removedEmitter)) {
        if (emitter->particleCount > 0) --emitter->particleCount;
    }
}

void ParticleRuntime::copyLane(BillboardParticleBlock& destination, size_t destinationLane,
                               const BillboardParticleBlock& source, size_t sourceLane) {
    destination.positionX[destinationLane] = source.positionX[sourceLane];
    destination.positionY[destinationLane] = source.positionY[sourceLane];
    destination.positionZ[destinationLane] = source.positionZ[sourceLane];
    destination.previousX[destinationLane] = source.previousX[sourceLane];
    destination.previousY[destinationLane] = source.previousY[sourceLane];
    destination.previousZ[destinationLane] = source.previousZ[sourceLane];
    destination.velocityX[destinationLane] = source.velocityX[sourceLane];
    destination.velocityY[destinationLane] = source.velocityY[sourceLane];
    destination.velocityZ[destinationLane] = source.velocityZ[sourceLane];
    destination.driftX[destinationLane] = source.driftX[sourceLane];
    destination.driftY[destinationLane] = source.driftY[sourceLane];
    destination.driftZ[destinationLane] = source.driftZ[sourceLane];
    destination.emitterOriginX[destinationLane] = source.emitterOriginX[sourceLane];
    destination.emitterOriginY[destinationLane] = source.emitterOriginY[sourceLane];
    destination.emitterOriginZ[destinationLane] = source.emitterOriginZ[sourceLane];
    destination.windRandomness[destinationLane] = source.windRandomness[sourceLane];
    destination.gravity[destinationLane] = source.gravity[sourceLane];
    destination.ageFrames[destinationLane] = source.ageFrames[sourceLane];
    destination.lifetimeFrames[destinationLane] = source.lifetimeFrames[sourceLane];
    destination.size[destinationLane] = source.size[sourceLane];
    destination.sizeRate[destinationLane] = source.sizeRate[sourceLane];
    destination.sizeRateDamping[destinationLane] = source.sizeRateDamping[sourceLane];
    destination.angleX[destinationLane] = source.angleX[sourceLane];
    destination.angleY[destinationLane] = source.angleY[sourceLane];
    destination.angle[destinationLane] = source.angle[sourceLane];
    destination.angularRateX[destinationLane] =
        source.angularRateX[sourceLane];
    destination.angularRateY[destinationLane] =
        source.angularRateY[sourceLane];
    destination.angularRate[destinationLane] = source.angularRate[sourceLane];
    destination.angularDamping[destinationLane] = source.angularDamping[sourceLane];
    destination.velocityDamping[destinationLane] = source.velocityDamping[sourceLane];
    destination.colorScale[destinationLane] = source.colorScale[sourceLane];
    destination.alpha[destinationLane] = source.alpha[sourceLane];
    destination.red[destinationLane] = source.red[sourceLane];
    destination.green[destinationLane] = source.green[sourceLane];
    destination.blue[destinationLane] = source.blue[sourceLane];
    destination.laterColorKeyTintRed[destinationLane] =
        source.laterColorKeyTintRed[sourceLane];
    destination.laterColorKeyTintGreen[destinationLane] =
        source.laterColorKeyTintGreen[sourceLane];
    destination.laterColorKeyTintBlue[destinationLane] =
        source.laterColorKeyTintBlue[sourceLane];
    destination.seed[destinationLane] = source.seed[sourceLane];
    destination.ordinal[destinationLane] = source.ordinal[sourceLane];
    destination.templateId[destinationLane] = source.templateId[sourceLane];
    destination.emitter[destinationLane] = source.emitter[sourceLane];
    destination.handle[destinationLane] = source.handle[sourceLane];
    destination.priorityPrevious[destinationLane] =
        source.priorityPrevious[sourceLane];
    destination.priorityNext[destinationLane] = source.priorityNext[sourceLane];
    destination.admissionOrdinal[destinationLane] =
        source.admissionOrdinal[sourceLane];
    destination.priority[destinationLane] = source.priority[sourceLane];
    destination.fieldParticle[destinationLane] =
        source.fieldParticle[sourceLane];
    destination.windMotion[destinationLane] = source.windMotion[sourceLane];
    destination.particleUpTowardsEmitter[destinationLane] =
        source.particleUpTowardsEmitter[sourceLane];
}

ParticleRuntimeParticle ParticleRuntime::snapshotLane(const BillboardParticleBlock& block,
                                                      size_t lane) const {
    return {
        .handle = block.handle[lane],
        .emitter = block.emitter[lane],
        .templateId = block.templateId[lane],
        .position = {block.positionX[lane], block.positionY[lane], block.positionZ[lane]},
        .previousPosition = {block.previousX[lane], block.previousY[lane], block.previousZ[lane]},
        .velocity = {block.velocityX[lane], block.velocityY[lane], block.velocityZ[lane]},
        .ageFrames = block.ageFrames[lane],
        .lifetimeFrames = block.lifetimeFrames[lane],
        .size = block.size[lane],
        .orientation = {block.angleX[lane], block.angleY[lane],
                        block.angle[lane]},
        .angle = block.angle[lane],
        .alpha = block.alpha[lane],
        .red = block.red[lane],
        .green = block.green[lane],
        .blue = block.blue[lane],
        .ordinal = block.ordinal[lane],
        .admissionOrdinal = block.admissionOrdinal[lane],
        .priority = block.priority[lane],
        .fieldParticle = block.fieldParticle[lane] != 0,
    };
}

} // namespace engine::fx
