#include "game/object/plan/combat/ObjectNeutronMissileSlowDeathPlanTypes.h"
#include "core/container/string_utils.h"

#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <memory>
#include <optional>

namespace {
using namespace engine;
using Fixed = math::q32_32;

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView moduleClass(
    const game::ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* valueLast(
    const game::ModuleData& module, container::StringView key) noexcept {
    for (auto it = module.values.rbegin(); it != module.values.rend(); ++it)
        if (equalInsensitive(it->first, key)) return &it->second;
    for (const auto& [name, value] : module.properties)
        if (equalInsensitive(name, key)) return &value;
    return nullptr;
}

[[nodiscard]] container::String trim(container::StringView value) {
    while (!value.empty() && std::isspace(
        static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(
        static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return container::String{value};
}

[[nodiscard]] std::optional<double> number(container::StringView value) {
    const container::String text = trim(value);
    double result = 0.0;
    const auto [cursor, error] = std::from_chars(
        text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || cursor != text.data() + text.size() ||
        !std::isfinite(result)) return std::nullopt;
    return result;
}

void readFixed(const game::ModuleData& module, const container::String& key,
               Fixed& output) {
    const container::String* value = valueLast(module, key);
    if (value) if (const auto parsed = number(*value)) output = Fixed{*parsed};
}

void readDuration(const game::ModuleData& module, const container::String& key,
                  Fixed& output) {
    const container::String* value = valueLast(module, key);
    if (!value) return;
    if (const auto parsed = number(*value); parsed && *parsed >= 0.0) {
        output = Fixed{*parsed};
    }
}

[[nodiscard]] bool readBool(const game::ModuleData& module,
                            const container::String& key) {
    const container::String* value = valueLast(module, key);
    if (!value) return false;
    const container::String text = trim(*value);
    return equalInsensitive(text, "yes") || equalInsensitive(text, "true") ||
           text == "1";
}

} // namespace

namespace game {
container::SharedPtr<const ObjectNeutronMissileSlowDeathPlan>
compileObjectNeutronMissileSlowDeathPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectNeutronMissileSlowDeathPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module),
                              "NeutronMissileSlowDeathBehavior")) continue;
        ObjectNeutronMissileSlowDeathRule rule;
        rule.authoredOrder = module.authoredOrder;
        readFixed(module, "ScorchMarkSize", rule.scorchMarkSize);
        if (const container::String* fx = valueLast(module, "FXList"))
            rule.fxList = trim(*fx);
        for (size_t i = 0; i < rule.blasts.size(); ++i) {
            ObjectNeutronMissileBlastRule& blast = rule.blasts[i];
            const container::String prefix = "Blast" + std::to_string(i + 1);
            blast.enabled = readBool(module, prefix + "Enabled");
            readDuration(module, prefix + "Delay", blast.delayMilliseconds);
            readDuration(module, prefix + "ScorchDelay",
                         blast.scorchDelayMilliseconds);
            readFixed(module, prefix + "InnerRadius", blast.innerRadius);
            readFixed(module, prefix + "OuterRadius", blast.outerRadius);
            readFixed(module, prefix + "MaxDamage", blast.maximumDamage);
            readFixed(module, prefix + "MinDamage", blast.minimumDamage);
            readFixed(module, prefix + "ToppleSpeed", blast.toppleSpeed);
            readFixed(module, prefix + "PushForce", blast.pushForce);
            if (blast.outerRadius < blast.innerRadius) {
                plan->diagnostics.push_back(
                    "Neutron blast outer radius is smaller than inner radius");
            }
        }
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty()) return nullptr;
    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const auto& a, const auto& b) { return a.authoredOrder < b.authoredOrder; });
    return plan;
}
} // namespace game
