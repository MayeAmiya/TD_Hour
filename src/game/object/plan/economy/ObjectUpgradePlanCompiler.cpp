#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/contracts/ObjectExperienceLimits.h"
#include "game/object/plan/economy/ObjectUpgradePlanTypes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <utility>

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/terrain/TerrainLogic.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/content/runtime/GameContentSnapshot.h"

namespace game
{
namespace
{

using container::asciiEqualIgnoreCase;

using container::trimAsciiView;

[[nodiscard]] container::StringView moduleClass(const ModuleData& module) noexcept
{
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass} : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(const ModuleData& module, container::StringView key) noexcept
{
    for (auto found = module.values.rbegin(); found != module.values.rend(); ++found)
    {
        if (asciiEqualIgnoreCase(found->first, key))
            return &found->second;
    }
    for (const auto& [entryKey, value] : module.properties)
    {
        if (asciiEqualIgnoreCase(entryKey, key))
            return &value;
    }
    return nullptr;
}

[[nodiscard]] container::Vector<container::StringView> splitTokens(container::StringView text)
{
    container::Vector<container::StringView> result;
    while (true)
    {
        text = trimAsciiView(text);
        if (text.empty())
            break;
        size_t end = 0;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && text[end] != ',')
        {
            ++end;
        }
        if (end != 0)
            result.push_back(text.substr(0, end));
        text.remove_prefix(end);
        while (!text.empty() && text.front() == ',')
            text.remove_prefix(1);
    }
    return result;
}

void appendModuleTokens(const ModuleData& module, container::StringView key, container::Vector<container::String>& destination)
{
    // UpgradeMuxData uses INI::parseAsciiStringVector, whose non-append
    // parser clears the field for every repeated declaration. Preserve the
    // final authored line (including intentional duplicate tokens) instead of
    // merging inherited/repeated text like an append-style field.
    destination.clear();
    const container::String* value = moduleValueLast(module, key);
    if (!value)
        return;
    for (const container::StringView token : splitTokens(*value))
    {
        if (!token.empty())
            destination.emplace_back(token);
    }
}

void appendModuleTokensAppend(const ModuleData& module, container::StringView key,
                              container::Vector<container::String>& destination)
{
    bool foundOrderedValue = false;
    for (const auto& [entryKey, value] : module.values)
    {
        if (!asciiEqualIgnoreCase(entryKey, key))
            continue;
        foundOrderedValue = true;
        for (const container::StringView token : splitTokens(value))
        {
            if (!token.empty()) destination.emplace_back(token);
        }
    }
    if (foundOrderedValue)
        return;
    for (const auto& [entryKey, value] : module.properties)
    {
        if (!asciiEqualIgnoreCase(entryKey, key))
            continue;
        for (const container::StringView token : splitTokens(value))
        {
            if (!token.empty()) destination.emplace_back(token);
        }
    }
}

[[nodiscard]] bool applyKindOfMaskLine(
    container::StringView value,
    container::Vector<container::String>& destination)
{
    const container::Vector<container::StringView> tokens = splitTokens(value);
    bool foundNormal = false;
    bool foundAddOrSubtract = false;
    const auto findToken = [&destination](container::StringView sought) {
        return std::find_if(
            destination.begin(), destination.end(),
            [sought](const container::String& existing) {
                return asciiEqualIgnoreCase(existing, sought);
            });
    };
    for (container::StringView token : tokens)
    {
        if (asciiEqualIgnoreCase(token, "NONE"))
        {
            if (foundNormal || foundAddOrSubtract)
                return false;
            destination.clear();
            return true;
        }

        const bool add = !token.empty() && token.front() == '+';
        const bool subtract = !token.empty() && token.front() == '-';
        if (add || subtract)
        {
            if (foundNormal)
                return false;
            token.remove_prefix(1);
            if (token.empty() || asciiEqualIgnoreCase(token, "NONE"))
                return false;
            foundAddOrSubtract = true;
            const auto found = findToken(token);
            if (subtract)
            {
                if (found != destination.end())
                    destination.erase(found);
            }
            else if (found == destination.end())
            {
                destination.emplace_back(token);
            }
            continue;
        }

        if (foundAddOrSubtract)
            return false;
        if (!foundNormal)
            destination.clear();
        foundNormal = true;
        if (findToken(token) == destination.end())
            destination.emplace_back(token);
    }
    return true;
}

[[nodiscard]] bool parseModuleKindOfMask(
    const ModuleData& module,
    container::StringView key,
    container::Vector<container::String>& destination)
{
    destination.clear();
    bool foundOrderedValue = false;
    bool valid = true;
    for (const auto& [entryKey, value] : module.values)
    {
        if (!asciiEqualIgnoreCase(entryKey, key))
            continue;
        foundOrderedValue = true;
        valid = applyKindOfMaskLine(value, destination) && valid;
    }
    if (!foundOrderedValue)
    {
        for (const auto& [entryKey, value] : module.properties)
        {
            if (!asciiEqualIgnoreCase(entryKey, key))
                continue;
            valid = applyKindOfMaskLine(value, destination) && valid;
        }
    }
    std::sort(destination.begin(), destination.end(),
              [](const container::String& left,
                 const container::String& right) {
                  const size_t count = std::min(left.size(), right.size());
                  for (size_t index = 0; index < count; ++index)
                  {
                      const char lowerLeft = container::asciiLower(left[index]);
                      const char lowerRight = container::asciiLower(right[index]);
                      if (lowerLeft != lowerRight)
                          return lowerLeft < lowerRight;
                  }
                  return left.size() < right.size();
              });
    return valid;
}

[[nodiscard]] std::optional<ObjectStatusMaskParseResult> moduleStatusMask(const ModuleData& module,
                                                                          container::StringView key)
{
    ObjectStatusMaskParseResult aggregate;
    bool found = false;
    // BitFlags::parseFromINI mutates its existing mask. A normal token line
    // replaces it, while a later +/- line extends or subtracts from the
    // previous value. Preserve that behavior across repeated authored fields.
    for (const auto& [entryKey, value] : module.values)
    {
        if (!asciiEqualIgnoreCase(entryKey, key))
            continue;
        const ObjectStatusMaskParseResult parsed = parseObjectStatusMask(value, aggregate.mask);
        aggregate.mask = parsed.mask;
        aggregate.resolved = aggregate.resolved && parsed.resolved;
        found = true;
    }
    if (found)
        return aggregate;
    for (const auto& [entryKey, value] : module.properties)
    {
        if (!asciiEqualIgnoreCase(entryKey, key))
            continue;
        return parseObjectStatusMask(value);
    }
    return std::nullopt;
}

[[nodiscard]] bool parseBoolean(container::StringView value, bool fallback = false) noexcept
{
    value = trimAsciiView(value);
    if (asciiEqualIgnoreCase(value, "yes") || asciiEqualIgnoreCase(value, "true") || value == "1")
    {
        return true;
    }
    if (asciiEqualIgnoreCase(value, "no") || asciiEqualIgnoreCase(value, "false") || value == "0")
    {
        return false;
    }
    return fallback;
}

[[nodiscard]] std::optional<float> parseFiniteFloat(container::StringView value) noexcept
{
    return parseContentFloat(value, {
        .source = __FILE__, .block = "Object", .module = "Upgrade",
        .field = "Real"});
}

[[nodiscard]] std::optional<math::q32_32> parseExperienceScalarDelta(
    container::StringView value, math::q32_32 fallback = {}) noexcept
{
    const game::ContentFloatContext context{
        .source = __FILE__, .block = "Object", .module = "Upgrade",
        .field = "AddXPScalar", .fallback = fallback.to_float()};
    const std::optional<float> parsed =
        parseContentFloat(value, context);
    if (!parsed) return fallback;
    constexpr float kQ32Minimum =
        static_cast<float>(std::numeric_limits<int32_t>::min());
    constexpr float kQ32MaximumExclusive =
        static_cast<float>(std::numeric_limits<int32_t>::max());
    if (*parsed < kQ32Minimum || *parsed >= kQ32MaximumExclusive) {
        warnContentFloatFallback(
            value, context,
            "finite numeric prefix is outside the Q32.32 field range; retained the prior/default value");
        return fallback;
    }
    if (*parsed < -static_cast<float>(engine::kMaximumExperienceScalarInteger) ||
        *parsed > static_cast<float>(engine::kMaximumExperienceScalarInteger)) {
        processContentDiagnostics().warn({
            .source = container::String{context.source},
            .block = container::String{context.block},
            .module = container::String{context.module},
            .field = container::String{context.field},
            .rawValue = container::String{value},
            .adoptedValue = std::to_string(*parsed),
            .reason = "authored experience scalar exceeds the runtime recommendation; retained and will be bounded at the consumption boundary",
        });
    }
    constexpr long double kFixedScale = 4294967296.0L;
    return math::q32_32::from_raw(static_cast<int64_t>(
        static_cast<long double>(*parsed) * kFixedScale));
}

[[nodiscard]] std::optional<uint32_t> parseDurationUnsignedMilliseconds(container::StringView value) noexcept
{
    value = trimAsciiView(value);
    if (value.empty())
        return std::nullopt;
    uint64_t parsed = 0;
    for (const char character : value)
    {
        if (character < '0' || character > '9')
            return std::nullopt;
        const uint64_t digit = static_cast<uint64_t>(character - '0');
        if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 10u)
        {
            return std::nullopt;
        }
        parsed = parsed * 10u + digit;
    }
    return static_cast<uint32_t>(parsed);
}

[[nodiscard]] std::optional<ObjectMaxHealthChangeType> parseMaxHealthChangeType(container::StringView value) noexcept
{
    value = trimAsciiView(value);
    if (asciiEqualIgnoreCase(value, "SAME_CURRENTHEALTH"))
    {
        return ObjectMaxHealthChangeType::SameCurrentHealth;
    }
    if (asciiEqualIgnoreCase(value, "PRESERVE_RATIO"))
    {
        return ObjectMaxHealthChangeType::PreserveRatio;
    }
    if (asciiEqualIgnoreCase(value, "ADD_CURRENT_HEALTH_TOO"))
    {
        return ObjectMaxHealthChangeType::AddCurrentHealthToo;
    }
    if (asciiEqualIgnoreCase(value, "FULLY_HEAL"))
    {
        return ObjectMaxHealthChangeType::FullyHeal;
    }
    return std::nullopt;
}

void appendDiagnostic(ObjectUpgradePlan& plan, const ModuleData& module, container::StringView message)
{
    const container::String tag = module.moduleTag.empty() ? "<untagged>" : module.moduleTag;
    plan.diagnostics.push_back("module '" + tag + "': " + container::String(message));
}

[[nodiscard]] std::optional<ObjectUpgradeOperation> operationForModule(container::StringView module) noexcept
{
    if (asciiEqualIgnoreCase(module, "MaxHealthUpgrade"))
    {
        return ObjectUpgradeOperation::MaxHealth;
    }
    if (asciiEqualIgnoreCase(module, "ArmorUpgrade"))
    {
        return ObjectUpgradeOperation::ArmorSetPlayerUpgrade;
    }
    if (asciiEqualIgnoreCase(module, "WeaponSetUpgrade"))
    {
        return ObjectUpgradeOperation::WeaponSetPlayerUpgrade;
    }
    if (asciiEqualIgnoreCase(module, "WeaponBonusUpgrade"))
    {
        return ObjectUpgradeOperation::WeaponBonusPlayerUpgrade;
    }
    if (asciiEqualIgnoreCase(module, "PowerPlantUpgrade"))
    {
        return ObjectUpgradeOperation::PowerPlant;
    }
    if (asciiEqualIgnoreCase(module, "StatusBitsUpgrade"))
    {
        return ObjectUpgradeOperation::StatusBits;
    }
    if (asciiEqualIgnoreCase(module, "ModelConditionUpgrade"))
    {
        return ObjectUpgradeOperation::ModelCondition;
    }
    if (asciiEqualIgnoreCase(module, "LocomotorSetUpgrade"))
    {
        return ObjectUpgradeOperation::LocomotorSet;
    }
    if (asciiEqualIgnoreCase(module, "GrantScienceUpgrade"))
    {
        return ObjectUpgradeOperation::GrantScience;
    }
    if (asciiEqualIgnoreCase(module, "CommandSetUpgrade"))
    {
        return ObjectUpgradeOperation::CommandSet;
    }
    if (asciiEqualIgnoreCase(module, "SubObjectsUpgrade"))
    {
        return ObjectUpgradeOperation::SubObjects;
    }
    if (asciiEqualIgnoreCase(module, "ExperienceScalarUpgrade"))
    {
        return ObjectUpgradeOperation::ExperienceScalar;
    }
    if (asciiEqualIgnoreCase(module, "CostModifierUpgrade"))
    {
        return ObjectUpgradeOperation::CostModifier;
    }
    if (asciiEqualIgnoreCase(module, "StealthUpgrade"))
    {
        return ObjectUpgradeOperation::Stealth;
    }
    if (asciiEqualIgnoreCase(module, "ObjectCreationUpgrade"))
    {
        return ObjectUpgradeOperation::ObjectCreation;
    }
    if (asciiEqualIgnoreCase(module, "ReplaceObjectUpgrade"))
    {
        return ObjectUpgradeOperation::ReplaceObject;
    }
    if (asciiEqualIgnoreCase(module, "ActiveShroudUpgrade"))
    {
        return ObjectUpgradeOperation::ActiveShroud;
    }
    if (asciiEqualIgnoreCase(module, "RadarUpgrade"))
    {
        return ObjectUpgradeOperation::Radar;
    }
    if (asciiEqualIgnoreCase(module, "PassengersFireUpgrade"))
    {
        return ObjectUpgradeOperation::PassengersFire;
    }
    if (asciiEqualIgnoreCase(module, "UnpauseSpecialPowerUpgrade"))
    {
        return ObjectUpgradeOperation::UnpauseSpecialPower;
    }
    return std::nullopt;
}

} // namespace

container::SharedPtr<const ObjectUpgradePlan> compileObjectUpgradePlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog)
{
    auto plan = std::make_shared<ObjectUpgradePlan>();
    for (const ModuleData& module : templateData.modules)
    {
        const container::StringView type = moduleClass(module);
        if (asciiEqualIgnoreCase(type, "PowerPlantUpdate"))
        {
            if (plan->powerPlant)
            {
                appendDiagnostic(
                    *plan, module, "duplicate PowerPlantUpdate is unsupported; source Object has one host");
                continue;
            }
            auto powerPlant = std::make_shared<ObjectPowerPlantPlan>();
            powerPlant->authoredOrder = module.authoredOrder;
            if (const container::String* value = moduleValueLast(module, "RodsExtendTime"))
            {
                const std::optional<uint32_t> duration = parseDurationUnsignedMilliseconds(*value);
                if (!duration)
                {
                    appendDiagnostic(
                        *plan, module, "RodsExtendTime must be an unsigned integer duration in milliseconds");
                }
                else
                {
                    powerPlant->rodsExtendMilliseconds = *duration;
                }
            }
            plan->powerPlant = std::move(powerPlant);
            continue;
        }
        if (asciiEqualIgnoreCase(type, "RadarUpdate"))
        {
            if (plan->radarUpdate)
            {
                appendDiagnostic(
                    *plan, module,
                    "duplicate RadarUpdate is unsupported; source Object has one host");
                continue;
            }
            auto radarUpdate = std::make_shared<ObjectRadarUpdatePlan>();
            radarUpdate->authoredOrder = module.authoredOrder;
            if (const container::String* value =
                    moduleValueLast(module, "RadarExtendTime"))
            {
                const std::optional<uint32_t> duration =
                    parseDurationUnsignedMilliseconds(*value);
                if (!duration)
                {
                    appendDiagnostic(
                        *plan, module,
                        "RadarExtendTime must be an unsigned duration");
                }
                else
                {
                    radarUpdate->extendMilliseconds = *duration;
                }
            }
            plan->radarUpdate = std::move(radarUpdate);
            continue;
        }

        const std::optional<ObjectUpgradeOperation> operation = operationForModule(type);
        if (!operation)
            continue;

        ObjectUpgradeRule rule;
        rule.authoredOrder = module.authoredOrder;
        rule.operation = *operation;
        appendModuleTokens(module, "TriggeredBy", rule.triggeredBy);
        appendModuleTokens(module, "ConflictsWith", rule.conflictsWith);
        appendModuleTokens(module, "RemovesUpgrades", rule.removesUpgrades);
        if (const container::String* value = moduleValueLast(module, "RequiresAllTriggers"))
        {
            rule.requiresAllTriggers = parseBoolean(*value);
        }
        if (const container::String* value = moduleValueLast(module, "FXListUpgrade"))
        {
            rule.upgradeFx = *value;
        }

        if (rule.operation == ObjectUpgradeOperation::MaxHealth)
        {
            if (const container::String* value = moduleValueLast(module, "AddMaxHealth"))
            {
                const std::optional<float> parsed = parseFiniteFloat(*value);
                if (!parsed)
                {
                    appendDiagnostic(*plan, module, "AddMaxHealth must be a finite real value");
                }
                else
                {
                    rule.addMaxHealth = math::q32_32{*parsed};
                }
            }
            if (const container::String* value = moduleValueLast(module, "ChangeType"))
            {
                const std::optional<ObjectMaxHealthChangeType> parsed = parseMaxHealthChangeType(*value);
                if (!parsed)
                {
                    appendDiagnostic(*plan,
                                     module,
                                     "ChangeType must be SAME_CURRENTHEALTH, PRESERVE_RATIO, "
                                     "ADD_CURRENT_HEALTH_TOO, or FULLY_HEAL");
                }
                else
                {
                    rule.maxHealthChangeType = *parsed;
                }
            }
        }

        if (rule.operation == ObjectUpgradeOperation::StatusBits)
        {
            if (const std::optional<ObjectStatusMaskParseResult> parsed = moduleStatusMask(module, "StatusToSet"))
            {
                rule.statusToSet = parsed->mask;
                if (!parsed->resolved)
                {
                    appendDiagnostic(*plan, module, "StatusToSet contains an unknown or invalid ObjectStatus mask");
                }
            }
            if (const std::optional<ObjectStatusMaskParseResult> parsed = moduleStatusMask(module, "StatusToClear"))
            {
                rule.statusToClear = parsed->mask;
                if (!parsed->resolved)
                {
                    appendDiagnostic(*plan, module, "StatusToClear contains an unknown or invalid ObjectStatus mask");
                }
            }
        }

        if (rule.operation == ObjectUpgradeOperation::ModelCondition)
        {
            if (const container::String* value = moduleValueLast(module, "ConditionFlag"))
            {
                const container::Vector<container::StringView> tokens = splitTokens(*value);
                const std::optional<uint32_t> condition =
                    tokens.empty() ? std::nullopt : tryParseModelConditionFlag(tokens.front());
                if (!condition)
                {
                    appendDiagnostic(*plan, module, "ConditionFlag must name one valid ModelCondition flag");
                }
                else
                {
                    rule.modelCondition.set(*condition);
                }
            }
        }

        if (rule.operation == ObjectUpgradeOperation::GrantScience)
        {
            const container::String* value = moduleValueLast(module, "GrantScience");
            const container::StringView science = value
                ? trimAsciiView(*value) : container::StringView{};
            if (science.empty())
            {
                appendDiagnostic(*plan, module, "GrantScience must name one Science definition");
            }
            else
            {
                // ScienceCatalog identity is intentionally case-sensitive,
                // matching the source NameKey. Validation against the sealed
                // catalog happens at the confirmed execution boundary because
                // Object recipes compile before a session chooses its catalog.
                rule.grantScience.assign(science);
            }
        }

        if (rule.operation == ObjectUpgradeOperation::CommandSet)
        {
            if (const container::String* value = moduleValueLast(module, "CommandSet"))
                rule.commandSet.assign(trimAsciiView(*value));
            if (const container::String* value = moduleValueLast(module, "CommandSetAlt"))
                rule.commandSetAlt.assign(trimAsciiView(*value));
            if (const container::String* value = moduleValueLast(module, "TriggerAlt"))
            {
                const container::StringView trigger = trimAsciiView(*value);
                rule.triggerAlt.assign(trigger.empty() ? container::StringView{"none"} : trigger);
            }
        }

        if (rule.operation == ObjectUpgradeOperation::SubObjects)
        {
            // These two fields use parseAsciiStringVectorAppend in RefCode:
            // repeated authored lines accumulate in source order instead of
            // replacing the previous vector like UpgradeMux fields do.
            appendModuleTokensAppend(module, "ShowSubObjects", rule.showSubObjects);
            appendModuleTokensAppend(module, "HideSubObjects", rule.hideSubObjects);
        }

        if (rule.operation == ObjectUpgradeOperation::ExperienceScalar)
        {
            if (const container::String* value = moduleValueLast(module, "AddXPScalar"))
            {
                const std::optional<math::q32_32> parsed =
                    parseExperienceScalarDelta(
                        *value, rule.addExperienceScalar);
                if (!parsed)
                {
                    appendDiagnostic(
                        *plan, module,
                        "AddXPScalar must be a finite real value in [-32768, 32768]");
                }
                else
                {
                    rule.addExperienceScalar = *parsed;
                }
            }
        }

        if (rule.operation == ObjectUpgradeOperation::CostModifier)
        {
            const bool validKindOf = parseModuleKindOfMask(
                module, "EffectKindOf", rule.costModifierKinds);
            rule.costModifierKindMask.clear();
            for (const container::String& authoredKind :
                     rule.costModifierKinds) {
                const std::optional<ObjectKindOf> kind =
                    parseObjectKindOf(authoredKind);
                if (kind) setObjectKind(rule.costModifierKindMask, *kind);
            }
            if (!validKindOf)
            {
                appendDiagnostic(
                    *plan, module,
                    "EffectKindOf may not mix ordinary tokens with +/- edits on one line");
            }
            if (const container::String* value =
                    moduleValueLast(module, "Percentage"))
            {
                container::StringView authored = trimAsciiView(*value);
                if (!authored.empty() && authored.back() == '%')
                    authored.remove_suffix(1);
                const std::optional<float> parsed =
                    parseFiniteFloat(authored);
                if (!parsed)
                {
                    appendDiagnostic(
                        *plan, module,
                        "Percentage must be a finite signed percent");
                }
                else
                {
                    rule.costModifierPercentage =
                        math::q32_32{*parsed * 0.01f};
                }
            }
        }
        if (rule.operation == ObjectUpgradeOperation::ObjectCreation)
        {
            if (const container::String* value =
                    moduleValueLast(module, "UpgradeObject"))
            {
                rule.objectCreationList = *value;
                if (asciiEqualIgnoreCase(rule.objectCreationList, "None")) {
                    rule.objectCreationList.clear();
                }
            }
        }
        if (rule.operation == ObjectUpgradeOperation::ReplaceObject)
        {
            if (const container::String* value =
                    moduleValueLast(module, "ReplaceObject"))
            {
                rule.replacementObject = *value;
            }
            if (rule.replacementObject.empty())
            {
                appendDiagnostic(
                    *plan, module,
                    "ReplaceObject must name an object template");
            }
        }
        if (rule.operation == ObjectUpgradeOperation::ActiveShroud)
        {
            if (const container::String* value =
                    moduleValueLast(module, "NewShroudRange"))
            {
                if (const std::optional<float> parsed =
                        parseFiniteFloat(*value))
                {
                    rule.newShroudRange = math::q32_32{*parsed};
                }
                else
                {
                    appendDiagnostic(
                        *plan, module,
                        "NewShroudRange must be a finite real value");
                }
            }
        }
        if (rule.operation == ObjectUpgradeOperation::Radar)
        {
            if (const container::String* value =
                    moduleValueLast(module, "DisableProof"))
            {
                rule.radarDisableProof = parseBoolean(*value);
            }
        }
        if (rule.operation == ObjectUpgradeOperation::UnpauseSpecialPower)
        {
            if (const container::String* value =
                    moduleValueLast(module, "SpecialPowerTemplate"))
            {
                rule.specialPowerTemplate.assign(trimAsciiView(*value));
            }
            if (rule.specialPowerTemplate.empty())
            {
                appendDiagnostic(*plan, module,
                                 "SpecialPowerTemplate is required");
            }
        }

        if (upgradeCatalog) {
            const auto compileMask =
                [upgradeCatalog](
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
            rule.triggeredByMask = compileMask(rule.triggeredBy);
            rule.conflictsWithMask = compileMask(rule.conflictsWith);
            rule.removesUpgradesMask = compileMask(rule.removesUpgrades);
            if (!rule.triggerAlt.empty() &&
                !asciiEqualIgnoreCase(rule.triggerAlt, "none")) {
                if (const engine::UpgradeDefinition* definition =
                        upgradeCatalog->find(rule.triggerAlt)) {
                    rule.triggerAltId = definition->id;
                }
            }
            rule.upgradeMasksCompiled = true;
        }
        rule.appliesChemicalSuitsDecal = std::any_of(
            rule.triggeredBy.begin(), rule.triggeredBy.end(),
            [](const container::String& name) {
                return asciiEqualIgnoreCase(
                    name,
                    engine::well_known_upgrade::AmericaChemicalSuits);
            });

        plan->rules.push_back(std::move(rule));
    }

    if (plan->rules.empty() && !plan->powerPlant && !plan->radarUpdate)
        return nullptr;
    std::sort(plan->rules.begin(),
              plan->rules.end(),
              [](const ObjectUpgradeRule& left, const ObjectUpgradeRule& right)
              { return left.authoredOrder < right.authoredOrder; });
    return plan;
}

bool objectUpgradeMatches(const ObjectUpgradeRule& rule,
                          const engine::UpgradeMask& completedUpgrades,
                          const engine::UpgradeCatalog* catalog) noexcept
{
    return objectUpgradeMatches(rule, completedUpgrades, {}, catalog);
}

bool objectUpgradeMatches(const ObjectUpgradeRule& rule,
                          const engine::UpgradeMask& playerCompletedUpgrades,
                          const engine::UpgradeMask& objectCompletedUpgrades,
                          const engine::UpgradeCatalog* catalog) noexcept
{
    static_cast<void>(catalog);
    if (!rule.upgradeMasksCompiled || rule.triggeredByMask.none())
        return false;
    const engine::UpgradeMask completed =
        playerCompletedUpgrades | objectCompletedUpgrades;
    if (completed.test_for_any(rule.conflictsWithMask))
        return false;
    return rule.requiresAllTriggers
        ? completed.test_for_all(rule.triggeredByMask)
        : completed.test_for_any(rule.triggeredByMask);
}

} // namespace game
