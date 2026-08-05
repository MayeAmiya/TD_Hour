#include "game/object/plan/lifecycle/ObjectCleanupHazardPlanTypes.h"

#include "core/container/string_utils.h"
#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
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
    for (auto it = module.values.rbegin(); it != module.values.rend(); ++it) {
        if (equalInsensitive(it->first, key)) return &it->second;
    }
    const auto found = module.properties.find(container::String{key});
    return found == module.properties.end() ? nullptr : &found->second;
}

[[nodiscard]] container::String trim(container::StringView value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return container::String{value};
}

[[nodiscard]] std::optional<uint32_t> parseMilliseconds(
    container::StringView value) noexcept {
    const container::String text = trim(value);
    if (text.empty()) return std::nullopt;
    double parsed = 0.0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [cursor, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || cursor != end || !std::isfinite(parsed) ||
        parsed < 0.0) {
        return std::nullopt;
    }
    if (parsed >= static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(std::ceil(parsed));
}

[[nodiscard]] std::optional<math::q32_32> parseNonNegativeFixed(
    container::StringView value) noexcept {
    const container::String text = trim(value);
    if (text.empty()) return std::nullopt;
    double parsed = 0.0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [cursor, error] = std::from_chars(begin, end, parsed);
    constexpr double kMaximumExclusive =
        static_cast<double>(std::numeric_limits<int32_t>::max()) + 1.0;
    if (error != std::errc{} || cursor != end || !std::isfinite(parsed) ||
        parsed < 0.0 || parsed >= kMaximumExclusive) {
        return std::nullopt;
    }
    return math::q32_32{parsed};
}

} // namespace

container::SharedPtr<const ObjectCleanupHazardPlan>
compileObjectCleanupHazardPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectCleanupHazardPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module), "CleanupHazardUpdate")) {
            continue;
        }

        ObjectCleanupHazardRule rule;
        rule.authoredOrder = module.authoredOrder;
        const container::String tag =
            !module.moduleTag.empty() ? module.moduleTag : module.tag;
        if (const container::String* value =
                moduleValueLast(module, "WeaponSlot")) {
            if (const std::optional<WeaponSlot> parsed =
                    tryParseWeaponSlot(trim(*value))) {
                rule.weaponSlot = *parsed;
            } else {
                plan->diagnostics.push_back(
                    tag + ": WeaponSlot must be PRIMARY, SECONDARY or TERTIARY");
            }
        }
        if (const container::String* value =
                moduleValueLast(module, "ScanRate")) {
            if (const std::optional<uint32_t> parsed =
                    parseMilliseconds(*value)) {
                rule.scanRateMilliseconds = *parsed;
            } else {
                plan->diagnostics.push_back(
                    tag + ": ScanRate must be non-negative milliseconds");
            }
        }
        if (const container::String* value =
                moduleValueLast(module, "ScanRange")) {
            if (const std::optional<math::q32_32> parsed =
                    parseNonNegativeFixed(*value)) {
                rule.scanRange = *parsed;
            } else {
                plan->diagnostics.push_back(
                    tag + ": ScanRange must be non-negative and finite");
            }
        }
        plan->rules.push_back(rule);
    }

    if (plan->rules.empty()) return nullptr;
    std::sort(plan->rules.begin(), plan->rules.end(),
              [](const ObjectCleanupHazardRule& left,
                 const ObjectCleanupHazardRule& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    return plan;
}

} // namespace game
