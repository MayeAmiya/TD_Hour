#include "game/object/contracts/ObjectOnDeletePlan.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <algorithm>
#include <memory>
#include <optional>

namespace game {
namespace {

[[nodiscard]] std::optional<ObjectOnDeleteCapability> classify(
    const ModuleData& module) noexcept {
    const container::StringView moduleClass = module.moduleClass.empty()
        ? container::StringView{module.type}
        : container::StringView{module.moduleClass};
    const auto equals = [moduleClass](container::StringView expected) {
        return container::asciiEqualIgnoreCase(moduleClass, expected);
    };
    if (equals("DozerAIUpdate") || equals("WorkerAIUpdate")) {
        return ObjectOnDeleteCapability::Builder;
    }
    if (equals("JetAIUpdate")) {
        return ObjectOnDeleteCapability::JetReservations;
    }
    if (equals("BattlePlanUpdate")) {
        return ObjectOnDeleteCapability::BattlePlan;
    }
    if (equals("PropagandaTowerBehavior")) {
        return ObjectOnDeleteCapability::PropagandaTower;
    }
    if (equals("SpawnBehavior")) {
        return ObjectOnDeleteCapability::Spawn;
    }
    if ((module.interfaceMask & ModuleRecipeInterfaceContain) != 0) {
        return ObjectOnDeleteCapability::Containment;
    }
    return std::nullopt;
}

} // namespace

container::SharedPtr<const ObjectOnDeletePlan>
compileObjectOnDeletePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectOnDeletePlan>();
    for (const ModuleData& module : templateData.modules) {
        const std::optional<ObjectOnDeleteCapability> capability =
            classify(module);
        if (!capability) continue;
        plan->entries.push_back({
            .capability = *capability,
            .authoredOrder = module.authoredOrder,
        });
    }
    std::stable_sort(
        plan->entries.begin(), plan->entries.end(),
        [](const ObjectOnDeleteEntry& left,
           const ObjectOnDeleteEntry& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan->entries.empty()
        ? container::SharedPtr<const ObjectOnDeletePlan>{}
        : std::move(plan);
}

} // namespace game
