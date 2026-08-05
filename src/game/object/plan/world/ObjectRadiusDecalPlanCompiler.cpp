#include "game/object/plan/world/ObjectRadiusDecalPlanTypes.h"

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

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return !module.moduleClass.empty()
        ? container::StringView{module.moduleClass}
        : container::StringView{module.type};
}

} // namespace

container::SharedPtr<const ObjectRadiusDecalPlan>
compileObjectRadiusDecalPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectRadiusDecalPlan>();
    std::optional<uint32_t> deliverPayloadAuthoredOrder;
    for (const ModuleData& module : templateData.modules) {
        const container::StringView type = moduleClass(module);
        if (container::asciiEqualIgnoreCase(type, "RadiusDecalUpdate")) {
            plan->rules.push_back({.authoredOrder = module.authoredOrder});
        } else if (!deliverPayloadAuthoredOrder &&
                   container::asciiEqualIgnoreCase(
                       type, "DeliverPayloadAIUpdate")) {
            deliverPayloadAuthoredOrder = module.authoredOrder;
        }
    }
    // DeliverPayloadAIUpdate 在原运行时直接拥有 RadiusDecal；没有通用
    // RadiusDecalUpdate 时，冻结出同样的稀疏宿主。
    if (plan->rules.empty() && deliverPayloadAuthoredOrder) {
        plan->rules.push_back({.authoredOrder = *deliverPayloadAuthoredOrder});
    }
    if (plan->rules.empty()) return nullptr;
    std::sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectRadiusDecalRule& left,
           const ObjectRadiusDecalRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan;
}

} // namespace game
