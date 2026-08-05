#include "game/object/plan/world/ObjectSpyVisionPlanTypes.h"
#include "core/container/string_utils.h"

#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace game {
namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView trim(container::StringView value) noexcept {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

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

[[nodiscard]] std::optional<uint32_t> parseUnsigned(
    container::StringView value) noexcept {
    value = trim(value);
    uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::optional<bool> parseBoolean(
    container::StringView value) noexcept {
    value = trim(value);
    if (equalInsensitive(value, "yes") || equalInsensitive(value, "true") ||
        value == "1") return true;
    if (equalInsensitive(value, "no") || equalInsensitive(value, "false") ||
        value == "0") return false;
    return std::nullopt;
}

[[nodiscard]] container::Vector<container::String> splitTokens(
    container::StringView value) {
    container::Vector<container::String> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t,", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t,", cursor);
        result.emplace_back(value.substr(cursor, end - cursor));
        cursor = end;
    }
    return result;
}

void parseUpgradeTokens(const ModuleData& module, container::StringView key,
                        container::Vector<container::String>& destination) {
    destination.clear();
    const container::String* value = moduleValueLast(module, key);
    if (!value) return;
    destination = splitTokens(*value);
}

void parseKindMask(const ModuleData& module, ObjectSpyVisionRule& rule) {
    const container::String* value = moduleValueLast(module, "SpyOnKindof");
    if (!value) return;
    rule.spyOnAll = false;
    for (container::String token : splitTokens(*value)) {
        container::StringView view = trim(token);
        bool excluded = false;
        if (!view.empty() && (view.front() == '+' || view.front() == '-')) {
            excluded = view.front() == '-';
            view.remove_prefix(1);
        }
        if (view.empty()) continue;
        if (equalInsensitive(view, "ALL")) {
            rule.spyOnAll = !excluded;
            rule.spyOnNone = excluded;
            continue;
        }
        if (equalInsensitive(view, "NONE")) {
            rule.spyOnNone = !excluded;
            continue;
        }
        const std::optional<ObjectKindOf> kind = parseObjectKindOf(view);
        if (!kind) continue;
        setObjectKind(excluded ? rule.excludedKinds : rule.includedKinds,
                      *kind);
    }
}

void appendDiagnostic(ObjectSpyVisionPlan& plan, const ModuleData& module,
                      container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
        : !module.tag.empty() ? module.tag
                              : container::String{moduleClass(module)};
    plan.diagnostics.push_back(tag + ": " + std::move(message));
}

} // namespace

container::SharedPtr<const ObjectSpyVisionPlan>
compileObjectSpyVisionPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog) {
    auto plan = std::make_shared<ObjectSpyVisionPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module), "SpyVisionUpdate")) continue;
        ObjectSpyVisionRule rule{.authoredOrder = module.authoredOrder};
        parseUpgradeTokens(module, "TriggeredBy", rule.triggeredBy);
        parseUpgradeTokens(module, "ConflictsWith", rule.conflictsWith);
        parseKindMask(module, rule);

        const auto boolean = [&](container::StringView key,
                                 bool& destination) {
            if (const container::String* value = moduleValueLast(module, key)) {
                if (const std::optional<bool> parsed = parseBoolean(*value)) {
                    destination = *parsed;
                } else {
                    appendDiagnostic(*plan, module,
                        container::String{key} + " must be Yes or No");
                }
            }
        };
        boolean("NeedsUpgrade", rule.needsUpgrade);
        boolean("SelfPowered", rule.selfPowered);
        boolean("RequiresAllTriggers", rule.requiresAllTriggers);

        const auto duration = [&](container::StringView key,
                                  uint32_t& destination) {
            if (const container::String* value = moduleValueLast(module, key)) {
                if (const std::optional<uint32_t> parsed = parseUnsigned(*value)) {
                    destination = *parsed;
                } else {
                    appendDiagnostic(*plan, module,
                        container::String{key} +
                            " must be unsigned milliseconds");
                }
            }
        };
        duration("SelfPoweredDuration",
                 rule.selfPoweredDurationMilliseconds);
        duration("SelfPoweredInterval",
                 rule.selfPoweredIntervalMilliseconds);
        if (rule.needsUpgrade && rule.triggeredBy.empty()) {
            appendDiagnostic(*plan, module,
                             "NeedsUpgrade requires TriggeredBy");
        }
        if (upgradeCatalog) {
            for (const container::String& name : rule.triggeredBy) {
                if (const engine::UpgradeDefinition* definition =
                        upgradeCatalog->find(name)) {
                    engine::upgradeMaskSet(
                        rule.triggeredByMask, definition->id);
                }
            }
            for (const container::String& name : rule.conflictsWith) {
                if (const engine::UpgradeDefinition* definition =
                        upgradeCatalog->find(name)) {
                    engine::upgradeMaskSet(
                        rule.conflictsWithMask, definition->id);
                }
            }
            rule.upgradeMasksCompiled = true;
        }
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty()) return nullptr;
    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectSpyVisionRule& left,
           const ObjectSpyVisionRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan;
}

} // namespace game
