#include "game/object/plan/structure/ObjectTechBuildingPlanTypes.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"

namespace game
{
namespace
{

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView moduleClass(const ModuleData& module) noexcept
{
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass} : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(const ModuleData& module, container::StringView key) noexcept
{
    for (auto found = module.values.rbegin(); found != module.values.rend(); ++found)
    {
        if (equalInsensitive(found->first, key))
            return &found->second;
    }
    for (const auto& [candidate, value] : module.properties)
    {
        if (equalInsensitive(candidate, key))
            return &value;
    }
    return nullptr;
}

[[nodiscard]] uint32_t parseMilliseconds(container::StringView value, uint32_t fallback) noexcept
{
    const std::optional<float> parsed =
        parseContentFloat(value, {
            .source = __FILE__, .block = "Object", .module = "TechBuilding",
            .field = "Duration", .fallback = static_cast<float>(fallback)});
    if (!parsed || *parsed < 0.0f)
    {
        return fallback;
    }
    if (*parsed >= static_cast<float>(std::numeric_limits<uint32_t>::max()))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(std::ceil(*parsed));
}

} // namespace

container::SharedPtr<const ObjectTechBuildingPlan> compileObjectTechBuildingPlan(const ThingTemplate& templateData)
{
    auto plan = std::make_shared<ObjectTechBuildingPlan>();
    for (const ModuleData& module : templateData.modules)
    {
        if (equalInsensitive(moduleClass(module), "TechBuildingBehavior"))
        {
            ObjectTechBuildingRule rule{
                .authoredOrder = module.authoredOrder,
            };
            if (const container::String* value = moduleValueLast(module, "PulseFX"))
            {
                rule.pulseFx = *value;
            }
            if (const container::String* value = moduleValueLast(module, "PulseFXRate"))
            {
                rule.pulseFxRateMilliseconds = parseMilliseconds(*value, rule.pulseFxRateMilliseconds);
            }
            plan->techBuildings.push_back(std::move(rule));
        }
        else if (equalInsensitive(moduleClass(module), "BeaconClientUpdate"))
        {
            ObjectBeaconClientRule rule{
                .authoredOrder = module.authoredOrder,
            };
            if (const container::String* value = moduleValueLast(module, "RadarPulseFrequency"))
            {
                rule.radarPulseFrequencyMilliseconds = parseMilliseconds(*value, rule.radarPulseFrequencyMilliseconds);
            }
            if (const container::String* value = moduleValueLast(module, "RadarPulseDuration"))
            {
                rule.radarPulseDurationMilliseconds = parseMilliseconds(*value, rule.radarPulseDurationMilliseconds);
            }
            plan->beacons.push_back(rule);
        }
    }
    if (plan->techBuildings.empty() && plan->beacons.empty())
        return nullptr;
    const auto byOrder = [](const auto& left, const auto& right) { return left.authoredOrder < right.authoredOrder; };
    std::stable_sort(plan->techBuildings.begin(), plan->techBuildings.end(), byOrder);
    std::stable_sort(plan->beacons.begin(), plan->beacons.end(), byOrder);
    return plan;
}

} // namespace game
