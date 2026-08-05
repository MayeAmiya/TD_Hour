#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/plan/status/ObjectAutoHealPlanTypes.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"

namespace game
{
namespace
{

using container::asciiEqualIgnoreCase;

using container::trimAsciiView;

[[nodiscard]] container::Vector<container::StringView> splitTokens(container::StringView value)
{
    container::Vector<container::StringView> result;
    size_t cursor = 0;
    while (cursor < value.size())
    {
        while (cursor < value.size() &&
               (std::isspace(static_cast<unsigned char>(value[cursor])) || value[cursor] == ','))
        {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < value.size() && !std::isspace(static_cast<unsigned char>(value[cursor])) &&
               value[cursor] != ',')
        {
            ++cursor;
        }
        if (begin != cursor)
            result.push_back(value.substr(begin, cursor - begin));
    }
    return result;
}

[[nodiscard]] const container::String* moduleValue(const ModuleData& module, container::StringView key) noexcept
{
    // `values` is source-ordered and therefore authoritative for normal INI
    // input. The unordered lookup is only a convenience for compact fixtures
    // assembled directly by focused probes.
    for (auto found = module.values.rbegin(); found != module.values.rend(); ++found)
    {
        if (asciiEqualIgnoreCase(found->first, key)) return &found->second;
    }
    for (const auto& [entryKey, value] : module.properties)
    {
        if (asciiEqualIgnoreCase(entryKey, key))
            return &value;
    }
    return nullptr;
}

void appendModuleTokens(const ModuleData& module, container::StringView key, container::Vector<container::String>& destination)
{
    // UpgradeMuxData uses the non-append vector parser: repeated fields
    // replace the previous vector wholesale. Preserve its last-line behavior
    // rather than merging trigger/conflict/removal tokens in the compiler.
    destination.clear();
    const container::String* value = moduleValue(module, key);
    if (!value) return;
    for (const container::StringView token : splitTokens(*value))
    {
        if (!token.empty()) destination.emplace_back(token);
    }
}

[[nodiscard]] bool parseBoolean(container::StringView value, bool fallback = false) noexcept
{
    value = trimAsciiView(value);
    if (asciiEqualIgnoreCase(value, "YES") || asciiEqualIgnoreCase(value, "TRUE") || value == "1")
    {
        return true;
    }
    if (asciiEqualIgnoreCase(value, "NO") || asciiEqualIgnoreCase(value, "FALSE") || value == "0")
    {
        return false;
    }
    return fallback;
}

[[nodiscard]] float parseFiniteFloat(container::StringView value, float fallback = 0.0f) noexcept
{
    return parseContentFloatOr(value, {
        .source = __FILE__, .block = "Object",
        .module = "AutoHealBehavior", .field = "Real",
        .fallback = fallback});
}

[[nodiscard]] int32_t parseInt(container::StringView value, int32_t fallback = 0) noexcept
{
    value = trimAsciiView(value);
    int32_t result = fallback;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    return error == std::errc{} && end == value.data() + value.size() ? result : fallback;
}

[[nodiscard]] uint32_t parseMilliseconds(container::StringView value, uint32_t fallback) noexcept
{
    const std::optional<float> parsed =
        parseContentFloat(value, {
            .source = __FILE__, .block = "Object",
            .module = "AutoHealBehavior", .field = "Duration",
            .fallback = static_cast<float>(fallback)});
    if (!parsed)
        return fallback;
    if (*parsed <= 0.0f)
        return 0;
    constexpr float maximum = static_cast<float>(std::numeric_limits<uint32_t>::max());
    if (*parsed >= maximum)
        return std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(std::ceil(*parsed));
}

[[nodiscard]] container::StringView moduleClass(const ModuleData& module) noexcept
{
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass} : container::StringView{module.type};
}

} // namespace

container::SharedPtr<const ObjectAutoHealPlan> compileObjectAutoHealPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog)
{
    auto plan = std::make_shared<ObjectAutoHealPlan>();
    for (const ModuleData& module : templateData.modules)
    {
        if (!asciiEqualIgnoreCase(moduleClass(module), "AutoHealBehavior"))
            continue;

        ObjectAutoHealParameters parameters;
        parameters.authoredOrder = module.authoredOrder;
        if (const container::String* value = moduleValue(module, "StartsActive"))
        {
            parameters.startsActive = parseBoolean(*value);
        }
        if (const container::String* value = moduleValue(module, "SingleBurst"))
        {
            parameters.singleBurst = parseBoolean(*value);
        }
        if (const container::String* value = moduleValue(module, "HealingAmount"))
        {
            parameters.healingAmount = math::q32_32{parseInt(*value)};
        }
        if (const container::String* value = moduleValue(module, "HealingDelay"))
        {
            parameters.healingDelayMilliseconds = parseMilliseconds(*value, parameters.healingDelayMilliseconds);
        }
        if (const container::String* value = moduleValue(module, "StartHealingDelay"))
        {
            parameters.startHealingDelayMilliseconds = parseMilliseconds(*value, 0);
        }
        if (const container::String* value = moduleValue(module, "Radius"))
        {
            parameters.radius = math::q32_32{parseFiniteFloat(*value)};
        }
        if (const container::String* value = moduleValue(module, "AffectsWholePlayer"))
        {
            parameters.affectsWholePlayer = parseBoolean(*value);
        }
        if (const container::String* value = moduleValue(module, "SkipSelfForHealing"))
        {
            parameters.skipSelfForHealing = parseBoolean(*value);
        }
        if (const container::String* value = moduleValue(module, "KindOf"))
        {
            static_cast<void>(compileObjectKindOfMask(
                *value, parameters.kindOfMask));
        }
        if (const container::String* value = moduleValue(module, "ForbiddenKindOf"))
        {
            static_cast<void>(compileObjectKindOfMask(
                *value, parameters.forbiddenKindOfMask));
        }
        if (const container::String* value = moduleValue(module, "RadiusParticleSystemName"))
        {
            parameters.radiusParticleSystemName = *value;
        }
        if (const container::String* value = moduleValue(module, "UnitHealPulseParticleSystemName"))
        {
            parameters.unitHealPulseParticleSystemName = *value;
        }
        if (const container::String* value = moduleValue(module, "FXListUpgrade"))
        {
            parameters.upgradeFx = *value;
        }
        if (const container::String* value = moduleValue(module, "RequiresAllTriggers"))
        {
            parameters.requiresAllTriggers = parseBoolean(*value);
        }
        appendModuleTokens(module, "TriggeredBy", parameters.triggeredBy);
        appendModuleTokens(module, "ConflictsWith", parameters.conflictsWith);
        appendModuleTokens(module, "RemovesUpgrades", parameters.removesUpgrades);

        if (upgradeCatalog) {
            const auto compile = [upgradeCatalog](
                container::Span<const container::String> names) {
                engine::UpgradeMask result;
                for (const container::String& name : names) {
                    if (const engine::UpgradeDefinition* definition =
                            upgradeCatalog->find(name)) {
                        engine::upgradeMaskSet(result, definition->id);
                    }
                }
                return result;
            };
            parameters.triggeredByMask = compile(parameters.triggeredBy);
            parameters.conflictsWithMask = compile(parameters.conflictsWith);
            parameters.removesUpgradesMask = compile(parameters.removesUpgrades);
            parameters.upgradeMasksCompiled = true;
        }

        plan->rules.push_back(std::move(parameters));
    }
    if (plan->rules.empty()) return nullptr;
    std::sort(plan->rules.begin(), plan->rules.end(),
              [](const ObjectAutoHealParameters& left,
                 const ObjectAutoHealParameters& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    return plan;
}

bool objectAutoHealUpgradeMatches(
    const ObjectAutoHealParameters& parameters,
    const engine::UpgradeMask& completedUpgrades,
    const engine::UpgradeCatalog* catalog) noexcept
{
    return objectAutoHealUpgradeMatches(parameters, completedUpgrades, {}, catalog);
}

bool objectAutoHealUpgradeMatches(
    const ObjectAutoHealParameters& parameters,
    const engine::UpgradeMask& playerCompletedUpgrades,
    const engine::UpgradeMask& objectCompletedUpgrades,
    const engine::UpgradeCatalog* catalog) noexcept
{
    static_cast<void>(catalog);
    if (!parameters.upgradeMasksCompiled ||
        parameters.triggeredByMask.none()) return false;
    const engine::UpgradeMask completed =
        playerCompletedUpgrades | objectCompletedUpgrades;
    if (completed.test_for_any(parameters.conflictsWithMask)) return false;
    if (parameters.requiresAllTriggers)
        return completed.test_for_all(parameters.triggeredByMask);
    return completed.test_for_any(parameters.triggeredByMask);
}

} // namespace game
