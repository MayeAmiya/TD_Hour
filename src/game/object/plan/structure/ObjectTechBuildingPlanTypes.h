#pragma once

#include <cstdint>

#include "core/container/container_types.h"

namespace game
{

struct ThingTemplate;

struct ObjectTechBuildingRule final
{
    uint32_t authoredOrder = 0;
    container::String pulseFx;
    uint32_t pulseFxRateMilliseconds = 0;
};

struct ObjectBeaconClientRule final
{
    uint32_t authoredOrder = 0;
    uint32_t radarPulseFrequencyMilliseconds = 1000;
    uint32_t radarPulseDurationMilliseconds = 500;
};

struct ObjectTechBuildingPlan final
{
    container::Vector<ObjectTechBuildingRule> techBuildings;
    container::Vector<ObjectBeaconClientRule> beacons;
};

[[nodiscard]] container::SharedPtr<const ObjectTechBuildingPlan> compileObjectTechBuildingPlan(
    const ThingTemplate& templateData);

} // namespace game
