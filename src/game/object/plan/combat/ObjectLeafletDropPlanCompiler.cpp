#include "game/object/plan/combat/ObjectLeafletDropPlanTypes.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>

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

template <typename Integer>
[[nodiscard]] std::optional<Integer> parseInteger(
    container::StringView value) noexcept {
    value = trim(value);
    if (value.empty()) return std::nullopt;
    Integer parsed{};
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) return std::nullopt;
    return parsed;
}

[[nodiscard]] std::optional<math::q32_32> parseFixed(
    container::StringView value) noexcept {
    const std::optional<float> parsed =
        parseContentFloat(value, {
            .source = __FILE__, .block = "Object",
            .module = "LeafletDropBehavior", .field = "Real"});
    return parsed ? std::optional<math::q32_32>{math::q32_32{*parsed}}
                  : std::nullopt;
}

void appendDiagnostic(container::Vector<container::String>& diagnostics,
                      const ModuleData& module, container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
        : !module.tag.empty() ? module.tag : container::String{moduleClass(module)};
    diagnostics.push_back(tag + ": " + std::move(message));
}

} // namespace

container::SharedPtr<const ObjectLeafletDropPlan>
compileObjectLeafletDropPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectLeafletDropPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module), "LeafletDropBehavior")) continue;
        ObjectLeafletDropParameters rule;
        rule.authoredOrder = module.authoredOrder;
        const auto readDuration = [&](container::StringView key,
                                      uint32_t& destination,
                                      bool* authored = nullptr) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            if (authored) *authored = true;
            if (const auto parsed = parseInteger<uint32_t>(*value)) {
                destination = *parsed;
            } else {
                appendDiagnostic(plan->diagnostics, module,
                    container::String{key} + " must be unsigned milliseconds");
            }
        };
        readDuration("Delay", rule.delayMilliseconds, &rule.delayAuthored);
        readDuration("DisabledDuration", rule.disabledDurationMilliseconds);
        if (const container::String* value =
                moduleValueLast(module, "AffectRadius")) {
            if (const auto parsed = parseFixed(*value)) {
                rule.radius = std::max(*parsed, math::q32_32{});
            } else {
                appendDiagnostic(plan->diagnostics, module,
                                 "AffectRadius must be a finite scalar");
            }
        }
        if (const container::String* value =
                moduleValueLast(module, "LeafletFXParticleSystem")) {
            const container::StringView particle = trim(*value);
            if (!particle.empty() && !equalInsensitive(particle, "NONE")) {
                rule.particleSystem = container::String{particle};
            }
        }
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty()) return {};
    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectLeafletDropParameters& left,
           const ObjectLeafletDropParameters& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan;
}

} // namespace game
