#include "game/object/plan/combat/ObjectFireWeaponUpdatePlanTypes.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <memory>
#include <optional>

namespace game {
namespace {

using container::asciiEqualIgnoreCase;
constexpr auto trim = container::trimAsciiView;

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return !module.moduleClass.empty()
        ? container::StringView{module.moduleClass}
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

[[nodiscard]] std::optional<uint32_t> parseUnsignedMilliseconds(
    container::StringView value) noexcept {
    value = trim(value);
    if (value.empty()) return std::nullopt;
    uint64_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(parsed);
}

void appendDiagnostic(ObjectFireWeaponUpdatePlan& plan,
                      const ModuleData& module, container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
        : !module.tag.empty() ? module.tag
                              : container::String{moduleClass(module)};
    plan.diagnostics.push_back(tag + ": " + std::move(message));
}

} // namespace

container::SharedPtr<const ObjectFireWeaponUpdatePlan>
compileObjectFireWeaponUpdatePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectFireWeaponUpdatePlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module), "FireWeaponUpdate")) {
            continue;
        }
        ObjectFireWeaponUpdateParameters parameters;
        parameters.authoredOrder = module.authoredOrder;
        if (const container::String* value = moduleValueLast(module, "Weapon")) {
            const container::StringView name = trim(*value);
            if (!asciiEqualIgnoreCase(name, "NONE")) {
                parameters.weapon = container::String{name};
            }
        }
        if (parameters.weapon.empty()) {
            appendDiagnostic(*plan, module, "Weapon is required");
        }
        const auto parseDuration = [&](container::StringView key,
                                       uint32_t& destination) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            const std::optional<uint32_t> parsed =
                parseUnsignedMilliseconds(*value);
            if (parsed) destination = *parsed;
            else appendDiagnostic(
                *plan, module,
                container::String{key} +
                    " must be an unsigned millisecond value");
        };
        parseDuration("InitialDelay", parameters.initialDelayMilliseconds);
        parseDuration("ExclusiveWeaponDelay",
                      parameters.exclusiveWeaponDelayMilliseconds);
        plan->rules.push_back(std::move(parameters));
    }
    if (plan->rules.empty()) return nullptr;
    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectFireWeaponUpdateParameters& left,
           const ObjectFireWeaponUpdateParameters& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan;
}

} // namespace game
