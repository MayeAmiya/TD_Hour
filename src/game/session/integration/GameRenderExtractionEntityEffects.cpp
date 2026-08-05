#include "GameRenderExtractionEntityEffects.h"

#include "game/object/simulation/status/ObjectDisabled.h"

#include <algorithm>
#include <limits>

namespace engine::render_extraction_detail {
namespace {

constexpr uint32_t kMaximumAutoBodyParticleBones = 16u;

[[nodiscard]] uint64_t mixBodyParticleIdentity(
    uint64_t seed, uint64_t value) noexcept {
    seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6u) + (seed >> 2u);
    return seed;
}

void appendBodyParticleDescriptor(
    container::Vector<render::RenderParticleSystemBone>& destination,
    render::RenderEntityId instanceId,
    ObjectId objectId,
    uint32_t category,
    ObjectBodyDamageState damageState,
    bool aflame,
    const RenderBodyParticleChannel& boneChannel,
    const RenderBodyParticleChannel& systemChannel,
    uint32_t countModifier) {
    if (boneChannel.prefix.empty() ||
        systemChannel.particleSystem.empty() ||
        boneChannel.maximumSystems == 0u) {
        return;
    }

    const uint32_t maximumEmitters = static_cast<uint32_t>(std::min<uint64_t>(
        kMaximumAutoBodyParticleBones,
        static_cast<uint64_t>(boneChannel.maximumSystems) * countModifier));
    uint64_t identity = mixBodyParticleIdentity(
        0xB0D1F17E5A17C1E5ull, instanceId);
    identity = mixBodyParticleIdentity(identity, category);
    identity = mixBodyParticleIdentity(
        identity, static_cast<uint8_t>(damageState));
    identity = mixBodyParticleIdentity(identity, aflame ? 1u : 0u);
    if (identity == 0u) identity = 1u;

    uint64_t selectionGroup = mixBodyParticleIdentity(
        0xA670B0D1CA7E600Dull, objectId.value);
    selectionGroup = mixBodyParticleIdentity(selectionGroup, category);
    if (selectionGroup == 0u) selectionGroup = 1u;

    destination.push_back({
        .identity = identity,
        .boneName = boneChannel.prefix,
        .particleSystem = systemChannel.particleSystem,
        .followsAnimatedBone = false,
        .numberedBonePrefix = true,
        .maximumEmitters = maximumEmitters,
        .selectionGroup = selectionGroup,
    });
}

} // namespace

void appendBodyParticleDescriptors(
    container::Vector<render::RenderParticleSystemBone>& destination,
    const RenderBodyParticleGameData& settings,
    render::RenderEntityId instanceId,
    ObjectId objectId,
    ObjectBodyDamageState damageState,
    bool aflame) {
    const uint32_t countModifier = aflame ? 2u : 1u;
    const auto append = [&](uint32_t category,
                            const RenderBodyParticleChannel& boneChannel,
                            const RenderBodyParticleChannel& systemChannel) {
        appendBodyParticleDescriptor(
            destination, instanceId, objectId, category, damageState, aflame,
            boneChannel, systemChannel, countModifier);
    };
    append(0u, settings.fireSmall,
           aflame ? settings.fireMedium : settings.fireSmall);
    append(1u, settings.fireMedium,
           aflame ? settings.fireLarge : settings.fireMedium);
    append(2u, settings.fireLarge, settings.fireLarge);
    append(3u, settings.smokeSmall,
           aflame ? settings.fireSmall : settings.smokeSmall);
    append(4u, settings.smokeMedium,
           aflame ? settings.fireSmall : settings.smokeMedium);
    append(5u, settings.smokeLarge,
           aflame ? settings.fireSmall : settings.smokeLarge);
    if (aflame) append(6u, settings.aflame, settings.aflame);
}

DisabledTintEnvelope disabledTintEnvelope(
    const ecs::registry& registry,
    ecs::entity entity,
    uint64_t confirmedTick) noexcept {
    constexpr float kEnvelopeFrames = 30.0f;
    constexpr ObjectDisabledMask kNoTintReasons =
        objectDisabledBit(ObjectDisabledReason::Held) |
        objectDisabledBit(ObjectDisabledReason::Unmanned) |
        objectDisabledBit(ObjectDisabledReason::ScriptDisabled);
    const ObjectDisabledMask active =
        objectDisabledMask(registry, entity, confirmedTick) &
        (objectDisabledKnownMask() & ~kNoTintReasons);
    if (active != 0) {
        if ((active & objectDisabledBit(ObjectDisabledReason::Subdued)) != 0) {
            return {
                .mode = render::RenderTintEnvelopeMode::Constant,
                .scale = 1.0f,
            };
        }
        uint64_t firstStart = std::numeric_limits<uint64_t>::max();
        for (uint8_t value = 0;
             value < static_cast<uint8_t>(ObjectDisabledReason::Count);
             ++value) {
            const ObjectDisabledReason reason =
                static_cast<ObjectDisabledReason>(value);
            const ObjectDisabledMask bit = objectDisabledBit(reason);
            if ((active & bit) == 0) continue;
            firstStart = std::min(
                firstStart,
                objectDisabledStartedAt(registry, entity, reason));
        }
        if (firstStart == std::numeric_limits<uint64_t>::max() ||
            confirmedTick < firstStart) {
            return {
                .mode = render::RenderTintEnvelopeMode::Constant,
                .scale = 1.0f,
            };
        }
        return {
            .mode = render::RenderTintEnvelopeMode::Attack,
            .startTick = firstStart,
            .scale = std::min(
                1.0f,
                static_cast<float>(confirmedTick - firstStart + 1u) /
                    kEnvelopeFrames),
        };
    }

    const uint64_t empUntil = objectDisabledUntil(
        registry, entity, ObjectDisabledReason::Emp);
    const uint64_t empStart = objectDisabledStartedAt(
        registry, entity, ObjectDisabledReason::Emp);
    if (empUntil == 0 || empUntil == OBJECT_DISABLED_FOREVER_TICK ||
        confirmedTick < empUntil) {
        return {};
    }
    const uint64_t releaseAge = confirmedTick - empUntil;
    if (releaseAge >= 30u) return {};
    const uint64_t attackFrames = empUntil > empStart
        ? empUntil - empStart : 0;
    const float releaseStart = std::min(
        1.0f, static_cast<float>(attackFrames) / kEnvelopeFrames);
    return {
        .mode = render::RenderTintEnvelopeMode::Release,
        .startTick = empUntil,
        .releaseStartScale = releaseStart,
        .scale = releaseStart *
            (1.0f - static_cast<float>(releaseAge) / kEnvelopeFrames),
    };
}

math::quat extractPhysicsOrientation(
    const ObjectPhysicsComponent& physics) noexcept {
    const bool basisCurrent = physics.orientationBasisValid &&
        physics.yaw.raw() == physics.orientationProjectionYaw.raw() &&
        physics.pitch.raw() == physics.orientationProjectionPitch.raw() &&
        physics.roll.raw() == physics.orientationProjectionRoll.raw();
    if (basisCurrent) {
        const math::vec3 localX{
            physics.orientationX.x.to_float(),
            physics.orientationX.y.to_float(),
            physics.orientationX.z.to_float()};
        const math::vec3 localY{
            physics.orientationY.x.to_float(),
            physics.orientationY.y.to_float(),
            physics.orientationY.z.to_float()};
        const math::vec3 localZ{
            physics.orientationZ.x.to_float(),
            physics.orientationZ.y.to_float(),
            physics.orientationZ.z.to_float()};
        return math::quat::from_matrix(
            math::transform::from_axes(localX, localY, localZ, {}))
            .normalized();
    }
    const math::quat roll = math::quat::from_axis_angle(
        {1.0f, 0.0f, 0.0f}, -physics.roll.to_float());
    const math::quat pitch = math::quat::from_axis_angle(
        {0.0f, 1.0f, 0.0f}, physics.pitch.to_float());
    const math::quat yaw = math::quat::from_axis_angle(
        {0.0f, 0.0f, 1.0f}, physics.yaw.to_float());
    return ((roll * pitch) * yaw).normalized();
}

} // namespace engine::render_extraction_detail
