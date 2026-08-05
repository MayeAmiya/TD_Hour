#pragma once

#include "core/container/container_types.h"
#include "game/data/base/SpecialPowerCatalog.h"

#include <cstdint>

namespace game {

struct ThingTemplate;

struct ObjectMissileLauncherBuildingRule final {
    container::String specialPowerTemplate;
    uint32_t doorOpenMilliseconds = 0;
    uint32_t doorWaitOpenMilliseconds = 0;
    uint32_t doorCloseMilliseconds = 0;
    container::String doorOpeningFx;
    container::String doorOpenFx;
    container::String doorWaitingToCloseFx;
    container::String doorClosingFx;
    container::String doorClosedFx;
    container::String doorOpenIdleAudio;
    uint32_t authoredOrder = 0;
};

struct ObjectMissileLauncherBuildingPlan final {
    container::Vector<ObjectMissileLauncherBuildingRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<
    const ObjectMissileLauncherBuildingPlan>
compileObjectMissileLauncherBuildingPlan(const ThingTemplate& templateData);

} // namespace game
