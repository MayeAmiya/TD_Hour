#pragma once

#include "core/container/container_types.h"

#include "game/object/weapon/WeaponTemplate.h"
#include "game/object/definition/ObjectKindOf.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <optional>
namespace game {

struct ThingTemplate;

struct ObjectWeaponBonusUpdateParameters final {
    uint32_t authoredOrder = 0;
    ObjectKindOfMask requiredAffectKinds{};
    ObjectKindOfMask forbiddenAffectKinds{};
    uint32_t bonusDurationMilliseconds = 0;
    uint32_t bonusDelayMilliseconds = 0;
    math::q32_32 bonusRange{};
    WeaponBonusCondition bonusCondition = WeaponBonusCondition::Count;
};

struct ObjectWeaponBonusUpdatePlan final {
    container::Vector<ObjectWeaponBonusUpdateParameters> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectWeaponBonusUpdatePlan>
compileObjectWeaponBonusUpdatePlan(const ThingTemplate& templateData);

} // namespace game

