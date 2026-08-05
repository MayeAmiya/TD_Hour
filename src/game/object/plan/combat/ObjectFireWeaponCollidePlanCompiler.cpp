#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/plan/combat/ObjectFireWeaponCollidePlanTypes.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
namespace game {
namespace {

using container::asciiEqualIgnoreCase;

using container::trimAsciiView;

[[nodiscard]] container::StringView moduleClass(const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto found = module.values.rbegin(); found != module.values.rend();
         ++found) {
        if (asciiEqualIgnoreCase(found->first, key)) return &found->second;
    }
    for (const auto& [candidate, value] : module.properties) {
        if (asciiEqualIgnoreCase(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] bool parseBoolean(container::StringView value,
                                bool fallback = false) noexcept {
    value = trimAsciiView(value);
    if (asciiEqualIgnoreCase(value, "YES") ||
        asciiEqualIgnoreCase(value, "TRUE") || value == "1") return true;
    if (asciiEqualIgnoreCase(value, "NO") ||
        asciiEqualIgnoreCase(value, "FALSE") || value == "0") return false;
    return fallback;
}

void appendDiagnostic(ObjectFireWeaponCollidePlan& plan,
                      const ModuleData& module, container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
                           : !module.tag.empty() ? module.tag
                                                 : container::String{moduleClass(module)};
    plan.diagnostics.push_back(tag + ": " + std::move(message));
}

} // namespace

container::SharedPtr<const ObjectFireWeaponCollidePlan>
compileObjectFireWeaponCollidePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectFireWeaponCollidePlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module),
                                  "FireWeaponCollide")) continue;
        ObjectFireWeaponCollideRule rule;
        rule.authoredOrder = module.authoredOrder;
        if (const container::String* value =
                moduleValueLast(module, "CollideWeapon")) {
            const container::StringView name = trimAsciiView(*value);
            if (!asciiEqualIgnoreCase(name, "NONE")) {
                rule.collideWeapon = container::String{name};
            }
        }
        if (rule.collideWeapon.empty()) {
            appendDiagnostic(*plan, module, "CollideWeapon is required");
        }
        if (const container::String* value = moduleValueLast(module, "FireOnce")) {
            rule.fireOnce = parseBoolean(*value);
        }
        const auto parseStatus = [&](container::StringView key,
                                     ObjectStatusMask& destination) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            const ObjectStatusMaskParseResult parsed =
                parseObjectStatusMask(*value);
            if (parsed.resolved) destination = parsed.mask;
            else appendDiagnostic(
                *plan, module,
                container::String{key} + " contains an unknown object status");
        };
        parseStatus("RequiredStatus", rule.requiredStatus);
        parseStatus("ForbiddenStatus", rule.forbiddenStatus);
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty()) return nullptr;
    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectFireWeaponCollideRule& left,
           const ObjectFireWeaponCollideRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan;
}

} // namespace game
