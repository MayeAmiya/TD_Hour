#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/plan/lifecycle/ObjectCreatePlanTypes.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

namespace game {
namespace {

using container::asciiEqualIgnoreCase;
constexpr auto trim = container::trimAsciiView;

[[nodiscard]] container::StringView moduleClass(const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

// FieldParse scalar fields are last-write-wins. Keep the ordered values as
// authority and fall back to properties only for hand-built probe recipes.
[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    const container::String* result = nullptr;
    for (const auto& [entryKey, value] : module.values) {
        if (asciiEqualIgnoreCase(entryKey, key)) result = &value;
    }
    if (result) return result;
    for (const auto& [entryKey, value] : module.properties) {
        if (asciiEqualIgnoreCase(entryKey, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] std::optional<ObjectStatusMaskParseResult> moduleStatusMask(
    const ModuleData& module, container::StringView key) {
    ObjectStatusMaskParseResult aggregate;
    bool found = false;
    // ObjectStatusMaskType mutates an existing mask: a plain repeated value
    // replaces it, while +/- lines extend or subtract from the prior value.
    for (const auto& [entryKey, value] : module.values) {
        if (!asciiEqualIgnoreCase(entryKey, key)) continue;
        const ObjectStatusMaskParseResult parsed =
            parseObjectStatusMask(value, aggregate.mask);
        aggregate.mask = parsed.mask;
        aggregate.resolved = aggregate.resolved && parsed.resolved;
        found = true;
    }
    if (found) return aggregate;
    for (const auto& [entryKey, value] : module.properties) {
        if (asciiEqualIgnoreCase(entryKey, key))
            return parseObjectStatusMask(value);
    }
    return std::nullopt;
}

void appendDiagnostic(ObjectCreatePlan& plan, const ModuleData& module,
                      container::String message) {
    const container::StringView tag = module.moduleTag.empty()
        ? container::StringView{"<untagged>"}
        : container::StringView{module.moduleTag};
    plan.diagnostics.push_back(
        container::String{moduleClass(module)} + " (tag '" + container::String{tag} +
        "') " + std::move(message));
}

[[nodiscard]] std::optional<ObjectVeterancyLevel> parseVeterancyLevel(
    container::StringView authored) noexcept {
    const container::StringView value = trim(authored);
    if (asciiEqualIgnoreCase(value, "REGULAR"))
        return ObjectVeterancyLevel::Regular;
    if (asciiEqualIgnoreCase(value, "VETERAN"))
        return ObjectVeterancyLevel::Veteran;
    if (asciiEqualIgnoreCase(value, "ELITE"))
        return ObjectVeterancyLevel::Elite;
    if (asciiEqualIgnoreCase(value, "HEROIC"))
        return ObjectVeterancyLevel::Heroic;
    return std::nullopt;
}

[[nodiscard]] bool isNoScience(container::StringView authored) noexcept {
    const container::StringView value = trim(authored);
    return value.empty() || asciiEqualIgnoreCase(value, "NONE");
}

} // namespace

container::SharedPtr<const ObjectCreatePlan>
compileObjectCreatePlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog) {
    auto plan = std::make_shared<ObjectCreatePlan>();
    bool foundTypedCreateModule = false;
    for (const ModuleData& module : templateData.modules) {
        const container::StringView type = moduleClass(module);
        if (asciiEqualIgnoreCase(type, "LockWeaponCreate")) {
            foundTypedCreateModule = true;
            ObjectLockWeaponCreate payload;
            if (const container::String* value = moduleValueLast(module, "SlotToLock")) {
                const std::optional<WeaponSlot> parsed =
                    tryParseWeaponSlot(trim(*value));
                if (!parsed) {
                    appendDiagnostic(
                        *plan, module,
                        "has invalid SlotToLock value '" + *value + "'");
                    continue;
                }
                payload.weaponSlot = *parsed;
            }
            plan->rules.push_back({
                .authoredOrder = module.authoredOrder,
                .payload = std::move(payload),
            });
            continue;
        }

        if (asciiEqualIgnoreCase(type, "GrantUpgradeCreate")) {
            foundTypedCreateModule = true;
            const container::String* authoredUpgrade =
                moduleValueLast(module, "UpgradeToGrant");
            if (!authoredUpgrade || trim(*authoredUpgrade).empty()) {
                appendDiagnostic(*plan, module,
                                 "requires a non-empty UpgradeToGrant");
                continue;
            }
            ObjectGrantUpgradeCreate payload{
                .upgrade = container::String{trim(*authoredUpgrade)},
            };
            if (upgradeCatalog) {
                if (const engine::UpgradeDefinition* definition =
                        upgradeCatalog->find(payload.upgrade)) {
                    payload.upgradeId = definition->id;
                }
            }
            if (const std::optional<ObjectStatusMaskParseResult> parsed =
                    moduleStatusMask(module, "ExemptStatus")) {
                if (!parsed->resolved) {
                    appendDiagnostic(
                        *plan, module,
                        "has an unresolved ExemptStatus value");
                    continue;
                }
                payload.exemptStatuses = parsed->mask;
            }
            plan->rules.push_back({
                .authoredOrder = module.authoredOrder,
                .payload = std::move(payload),
            });
            continue;
        }

        if (asciiEqualIgnoreCase(type, "VeterancyGainCreate")) {
            foundTypedCreateModule = true;
            ObjectVeterancyGainCreate payload;
            if (const container::String* value = moduleValueLast(module, "StartingLevel")) {
                const std::optional<ObjectVeterancyLevel> parsed =
                    parseVeterancyLevel(*value);
                if (!parsed) {
                    appendDiagnostic(
                        *plan, module,
                        "has invalid StartingLevel value '" + *value + "'");
                    continue;
                }
                payload.startingLevel = *parsed;
            }
            if (const container::String* value = moduleValueLast(module, "ScienceRequired")) {
                if (!isNoScience(*value))
                    payload.scienceRequired = container::String{trim(*value)};
            }
            plan->rules.push_back({
                .authoredOrder = module.authoredOrder,
                .payload = std::move(payload),
            });
            continue;
        }

        if (asciiEqualIgnoreCase(type, "SupplyCenterCreate")) {
            foundTypedCreateModule = true;
            plan->rules.push_back({
                .authoredOrder = module.authoredOrder,
                .payload = ObjectSupplyCenterCreate{},
            });
            continue;
        }

        if (asciiEqualIgnoreCase(type, "SupplyWarehouseCreate")) {
            foundTypedCreateModule = true;
            plan->rules.push_back({
                .authoredOrder = module.authoredOrder,
                .payload = ObjectSupplyWarehouseCreate{},
            });
        }
    }

    std::stable_sort(
        plan->rules.begin(), plan->rules.end(),
        [](const ObjectCreateRule& left, const ObjectCreateRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });

    if (!foundTypedCreateModule) return nullptr;
    return plan;
}

} // namespace game
