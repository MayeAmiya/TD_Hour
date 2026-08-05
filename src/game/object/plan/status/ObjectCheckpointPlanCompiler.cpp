#include "game/object/plan/status/ObjectCheckpointPlanTypes.h"

#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

namespace game {
namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto found = module.values.rbegin(); found != module.values.rend();
         ++found) {
        if (equalInsensitive(found->first, key)) return &found->second;
    }
    for (const auto& [candidate, value] : module.properties) {
        if (equalInsensitive(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] uint32_t parseMilliseconds(container::StringView value,
                                         uint32_t fallback) noexcept {
    const std::optional<float> parsed =
        parseContentFloat(value, {
            .source = __FILE__, .block = "Object", .module = "Checkpoint",
            .field = "Duration", .fallback = static_cast<float>(fallback)});
    if (!parsed || *parsed < 0.0f) {
        return fallback;
    }
    if (*parsed >= static_cast<float>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(std::ceil(*parsed));
}

} // namespace

container::SharedPtr<const ObjectCheckpointPlan>
compileObjectCheckpointPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectCheckpointPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module), "CheckpointUpdate")) {
            continue;
        }
        ObjectCheckpointRule rule{
            .authoredOrder = module.authoredOrder,
        };
        if (const container::String* value =
                moduleValueLast(module, "ScanDelayTime")) {
            rule.scanDelayMilliseconds =
                parseMilliseconds(*value, rule.scanDelayMilliseconds);
        }
        plan->rules.push_back(rule);
    }
    if (plan->rules.empty()) return nullptr;
    std::stable_sort(plan->rules.begin(), plan->rules.end(),
                     [](const ObjectCheckpointRule& left,
                        const ObjectCheckpointRule& right) {
                         return left.authoredOrder < right.authoredOrder;
                     });
    return plan;
}

} // namespace game
