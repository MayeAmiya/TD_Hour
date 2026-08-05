#pragma once

#include "core/ecs/ObjectId.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/weapon/WeaponTemplate.h"

#include <cstdint>

namespace engine {

struct ObjectHistoricBonusWeaponFire final {
    ObjectId source = INVALID_OBJECT_ID;
    game::WeaponContentId content;
    LogicFixedVec3 position{};
    uint32_t sourceSequence = 0;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

} // namespace engine
