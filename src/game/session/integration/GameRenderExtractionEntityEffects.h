#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "presentation/render/RenderGameDataSettings.h"
#include "presentation/render/RenderSceneSnapshot.h"

#include <cstdint>

namespace engine::render_extraction_detail {

struct DisabledTintEnvelope final {
    render::RenderTintEnvelopeMode mode =
        render::RenderTintEnvelopeMode::None;
    uint64_t startTick = 0;
    float releaseStartScale = 0.0f;
    float scale = 0.0f;
};

void appendBodyParticleDescriptors(
    container::Vector<render::RenderParticleSystemBone>& destination,
    const RenderBodyParticleGameData& settings,
    render::RenderEntityId instanceId,
    ObjectId objectId,
    ObjectBodyDamageState damageState,
    bool aflame);

[[nodiscard]] DisabledTintEnvelope disabledTintEnvelope(
    const ecs::registry& registry,
    ecs::entity entity,
    uint64_t confirmedTick) noexcept;

[[nodiscard]] math::quat extractPhysicsOrientation(
    const ObjectPhysicsComponent& physics) noexcept;

} // namespace engine::render_extraction_detail
