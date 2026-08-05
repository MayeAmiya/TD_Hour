#pragma once

#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "math/fixed/q32_32.h"

namespace engine
{

[[nodiscard]] ObjectBodyDamageState objectBodyDamageStateFor(
    ObjectHealthComponent::Scalar health,
    ObjectHealthComponent::Scalar maximum,
    const ObjectSimulationRules& rules) noexcept;

void projectObjectBodyDamageVisual(ObjectBodyDamageState state,
                                   RenderModelComponent& visual) noexcept;

// ActiveBody always keeps its logical BodyDamageType current, including while
// a structure is being built.  Drawable deliberately suppresses that state
// until OBJECT_STATUS_UNDER_CONSTRUCTION is cleared because construction art
// has no damaged/really-damaged variants.  All presentation consumers use
// this projection instead of interpreting the construction-progress HP ratio.
[[nodiscard]] ObjectBodyDamageState objectBodyDamagePresentationState(
    const ecs::registry& registry, ecs::entity entity,
    ObjectBodyDamageState logicalState) noexcept;

void projectObjectBodyDamageVisual(
    const ecs::registry& registry, ecs::entity entity,
    ObjectBodyDamageState logicalState,
    RenderModelComponent& visual) noexcept;

// ActiveBody::setMaxHealth(..., PRESERVE_RATIO) expressed as one total,
// fixed-point mutation. Veterancy supplies old/new global health multipliers;
// the helper updates max/initial/current together and refreshes the existing
// renderer-neutral body-state projection.
[[nodiscard]] bool scaleObjectMaximumHealthPreserveRatio(
    ObjectHealthComponent& health,
    math::q32_32 numerator,
    math::q32_32 denominator,
    const ObjectSimulationRules& rules,
    RenderModelComponent* visual = nullptr) noexcept;

} // namespace engine
