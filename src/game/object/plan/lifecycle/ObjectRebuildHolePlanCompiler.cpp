#include "game/object/plan/lifecycle/ObjectRebuildHolePlanTypes.h"
#include "core/container/string_utils.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>

namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView moduleClass(
    const game::ModuleData& module) noexcept {
    return module.moduleClass.empty() ? container::StringView{module.type}
                                      : container::StringView{module.moduleClass};
}

[[nodiscard]] const container::String* moduleValueLast(
    const game::ModuleData& module, container::StringView key) noexcept {
    for (auto found = module.values.rbegin(); found != module.values.rend(); ++found)
        if (equalInsensitive(found->first, key)) return &found->second;
    for (const auto& [entryKey, value] : module.properties)
        if (equalInsensitive(entryKey, key)) return &value;
    return nullptr;
}

[[nodiscard]] container::StringView trim(container::StringView value) noexcept {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

[[nodiscard]] std::optional<uint32_t> parseUnsigned(
    container::StringView value) noexcept {
    value = trim(value);
    uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (value.empty() || value.front() == '-' || error != std::errc{} ||
        end != value.data() + value.size() ||
        parsed > std::numeric_limits<uint32_t>::max()) return std::nullopt;
    return static_cast<uint32_t>(parsed);
}

[[nodiscard]] std::optional<math::q32_32> parseFixed(
    container::StringView value, bool percent) noexcept {
    value = trim(value);
    if (percent && !value.empty() && value.back() == '%') value.remove_suffix(1);
    double parsed = 0.0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() ||
        !std::isfinite(parsed) || parsed < 0.0) return std::nullopt;
    if (percent) parsed /= 100.0;
    return math::q32_32{parsed};
}

[[nodiscard]] std::optional<bool> parseBool(container::StringView value) noexcept {
    value = trim(value);
    if (equalInsensitive(value, "YES") || equalInsensitive(value, "TRUE") || value == "1")
        return true;
    if (equalInsensitive(value, "NO") || equalInsensitive(value, "FALSE") || value == "0")
        return false;
    return std::nullopt;
}

[[nodiscard]] uint64_t millisecondsToTicks(uint32_t milliseconds,
                                           uint32_t fps) noexcept {
    if (milliseconds == 0) return 0;
    return (static_cast<uint64_t>(milliseconds) * std::max(1u, fps) + 999u) / 1000u;
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return left > std::numeric_limits<uint64_t>::max() - right
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

} // namespace

namespace game {

container::SharedPtr<const ObjectRebuildHolePlan>
compileObjectRebuildHolePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectRebuildHolePlan>();
    for (const ModuleData& module : templateData.modules) {
        const container::StringView klass = moduleClass(module);
        if (equalInsensitive(klass, "RebuildHoleBehavior")) {
            ObjectRebuildHoleBehaviorRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const container::String* value = moduleValueLast(module, "WorkerObjectName"))
                rule.workerTemplate = *value;
            if (const container::String* value = moduleValueLast(module, "WorkerRespawnDelay"))
                if (const std::optional<uint32_t> parsed = parseUnsigned(*value))
                    rule.workerRespawnDelayMilliseconds = *parsed;
            if (const container::String* value =
                    moduleValueLast(module, "HoleHealthRegen%PerSecond"))
                if (const std::optional<math::q32_32> parsed = parseFixed(*value, true))
                    rule.healthRegenRatioPerSecond = *parsed;
            plan->behaviors.push_back(std::move(rule));
        } else if (equalInsensitive(klass, "RebuildHoleExposeDie")) {
            ObjectRebuildHoleExposeRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const container::String* value = moduleValueLast(module, "HoleName"))
                rule.holeTemplate = *value;
            if (const container::String* value = moduleValueLast(module, "HoleMaxHealth"))
                if (const std::optional<math::q32_32> parsed = parseFixed(*value, false))
                    rule.holeMaximumHealth = *parsed;
            if (const container::String* value = moduleValueLast(module, "TransferAttackers"))
                if (const std::optional<bool> parsed = parseBool(*value))
                    rule.transferAttackers = *parsed;
            plan->exposes.push_back(std::move(rule));
        }
    }
    if (plan->behaviors.empty() && plan->exposes.empty()) return nullptr;
    const auto byOrder = [](const auto& left, const auto& right) {
        return left.authoredOrder < right.authoredOrder;
    };
    std::stable_sort(plan->behaviors.begin(), plan->behaviors.end(), byOrder);
    std::stable_sort(plan->exposes.begin(), plan->exposes.end(), byOrder);
    return plan;
}

} // namespace game
