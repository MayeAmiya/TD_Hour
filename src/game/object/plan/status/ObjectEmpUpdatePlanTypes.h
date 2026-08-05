#pragma once

#include "core/container/container_types.h"

#include "game/object/weapon/WeaponTemplate.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"
#include <cstdint>
#include <limits>
namespace game {

struct ThingTemplate;

struct ObjectEmpParameters final {
    uint32_t authoredOrder = 0;
    uint32_t lifetimeMilliseconds = 0;
    uint32_t startFadeMilliseconds = 0;
    uint32_t disabledDurationMilliseconds = 0;
    math::q32_32 startScale{1};
    math::q32_32 targetScaleMinimum{1};
    math::q32_32 targetScaleMaximum{1};
    // Authored presentation values; they never enter deterministic gameplay state.
    container::Array<float, 3> startColor{1.0f, 1.0f, 1.0f};
    container::Array<float, 3> endColor{};
    container::String disableParticleSystem;
    math::q32_32 sparksPerCubicFoot = math::q32_32::from_fraction(1, 1000);
    math::q32_32 effectRadius{200};
    WeaponAffectsMask doesNotAffect = 0;
    container::Vector<container::String> victimRequiredKindOf;
    container::Vector<container::String> victimForbiddenKindOf;
    bool doesNotAffectOwnBuildings = false;
};

struct ObjectEmpPlan final {
    container::Vector<ObjectEmpParameters> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectEmpPlan>
compileObjectEmpPlan(const ThingTemplate& templateData);

} // namespace game
