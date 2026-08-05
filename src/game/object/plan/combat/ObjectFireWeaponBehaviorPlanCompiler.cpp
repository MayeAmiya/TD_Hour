#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/plan/combat/ObjectFireWeaponBehaviorPlanTypes.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
namespace game {
namespace {

using container::asciiEqualIgnoreCase;
constexpr auto trim = container::trimAsciiView;

[[nodiscard]] container::StringView moduleClass(const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto found = module.values.rbegin(); found != module.values.rend(); ++found) {
        if (asciiEqualIgnoreCase(found->first, key)) return &found->second;
    }
    for (const auto& [candidate, value] : module.properties) {
        if (asciiEqualIgnoreCase(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] container::Vector<container::StringView> splitTokens(container::StringView value) {
    container::Vector<container::StringView> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t,", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t,", cursor);
        result.push_back(value.substr(cursor, end - cursor));
        cursor = end;
    }
    return result;
}

void parseStringVector(const ModuleData& module, container::StringView key,
                       container::Vector<container::String>& destination) {
    destination.clear();
    const container::String* value = moduleValueLast(module, key);
    if (!value) return;
    for (const container::StringView token : splitTokens(*value)) {
        if (!token.empty()) destination.emplace_back(token);
    }
}

[[nodiscard]] bool parseBoolean(container::StringView value,
                                bool fallback = false) noexcept {
    value = trim(value);
    if (asciiEqualIgnoreCase(value, "YES") ||
        asciiEqualIgnoreCase(value, "TRUE") || value == "1") return true;
    if (asciiEqualIgnoreCase(value, "NO") ||
        asciiEqualIgnoreCase(value, "FALSE") || value == "0") return false;
    return fallback;
}

[[nodiscard]] std::optional<float> parseFloat(container::StringView value) noexcept {
    return parseContentFloat(value, {
        .source = __FILE__, .block = "Object",
        .module = "FireWeaponBehavior", .field = "Real"});
}

[[nodiscard]] uint64_t damageTypeBit(DamageType type) noexcept {
    const uint8_t index = static_cast<uint8_t>(type);
    return index < 64 ? uint64_t{1} << index : 0;
}

[[nodiscard]] std::optional<uint64_t> parseDamageTypeMask(
    container::StringView value) noexcept {
    uint64_t mask = 0;
    bool any = false;
    for (container::StringView token : splitTokens(value)) {
        if (asciiEqualIgnoreCase(token, "ALL")) {
            mask = static_cast<uint8_t>(DamageType::COUNT) >= 64
                ? std::numeric_limits<uint64_t>::max()
                : (uint64_t{1} << static_cast<uint8_t>(DamageType::COUNT)) - 1u;
            any = true;
            continue;
        }
        if (asciiEqualIgnoreCase(token, "NONE")) {
            mask = 0;
            any = true;
            continue;
        }
        bool remove = false;
        if (!token.empty() && (token.front() == '+' || token.front() == '-')) {
            remove = token.front() == '-';
            token.remove_prefix(1);
        }
        const std::optional<DamageType> parsed = tryParseDamageType(token);
        if (!parsed) return std::nullopt;
        const uint64_t bit = damageTypeBit(*parsed);
        if (remove) mask &= ~bit;
        else mask |= bit;
        any = true;
    }
    return any ? std::optional<uint64_t>{mask} : std::nullopt;
}

void parseUpgradeMux(const ModuleData& module, ObjectUpgradeMuxRecipe& mux) {
    parseStringVector(module, "TriggeredBy", mux.triggeredBy);
    parseStringVector(module, "ConflictsWith", mux.conflictsWith);
    parseStringVector(module, "RemovesUpgrades", mux.removesUpgrades);
    if (const container::String* value = moduleValueLast(module, "FXListUpgrade")) {
        mux.upgradeFx = container::String(trim(*value));
    }
    if (const container::String* value = moduleValueLast(module, "RequiresAllTriggers")) {
        mux.requiresAllTriggers = parseBoolean(*value);
    }
}

void appendDiagnostic(container::Vector<container::String>& diagnostics,
                      const ModuleData& module, container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
                           : !module.tag.empty() ? module.tag
                                                 : container::String{moduleClass(module)};
    diagnostics.push_back(tag + ": " + std::move(message));
}

[[nodiscard]] bool containsUpgrade(container::Span<const container::String> upgrades,
                                   container::StringView sought) noexcept {
    return std::any_of(upgrades.begin(), upgrades.end(),
        [sought](const container::String& value) {
            return asciiEqualIgnoreCase(value, sought);
        });
}

} // namespace

void compileObjectUpgradeMuxRecipe(
    ObjectUpgradeMuxRecipe& mux,
    const engine::UpgradeCatalog* catalog) noexcept {
    if (!catalog) return;
    const auto compile = [catalog](
        container::Span<const container::String> names) {
        engine::UpgradeMask result;
        for (const container::String& name : names) {
            if (const engine::UpgradeDefinition* definition =
                    catalog->find(name)) {
                engine::upgradeMaskSet(result, definition->id);
            }
        }
        return result;
    };
    mux.triggeredByMask = compile(mux.triggeredBy);
    mux.conflictsWithMask = compile(mux.conflictsWith);
    mux.removesUpgradesMask = compile(mux.removesUpgrades);
    mux.masksCompiled = true;
}

bool objectFireWeaponUpgradeHasConflict(
    const ObjectUpgradeMuxRecipe& mux,
    const engine::UpgradeMask& playerCompleted,
    const engine::UpgradeMask& objectCompleted,
    const engine::UpgradeCatalog* catalog) noexcept {
    static_cast<void>(catalog);
    if (!mux.masksCompiled) return false;
    return (playerCompleted | objectCompleted)
        .test_for_any(mux.conflictsWithMask);
}

bool objectFireWeaponUpgradeMatches(
    const ObjectUpgradeMuxRecipe& mux,
    const engine::UpgradeMask& playerCompleted,
    const engine::UpgradeMask& objectCompleted,
    const engine::UpgradeCatalog* catalog) noexcept {
    if (!mux.masksCompiled || mux.triggeredByMask.none() ||
        objectFireWeaponUpgradeHasConflict(mux, playerCompleted,
                                           objectCompleted, catalog)) {
        return false;
    }
    const engine::UpgradeMask completed =
        playerCompleted | objectCompleted;
    return mux.requiresAllTriggers
        ? completed.test_for_all(mux.triggeredByMask)
        : completed.test_for_any(mux.triggeredByMask);
}

bool objectFireWeaponUpgradeTriggeredBy(
    const ObjectUpgradeMuxRecipe& mux,
    engine::UpgradeContentId upgrade) noexcept {
    return mux.masksCompiled &&
        engine::upgradeMaskTest(mux.triggeredByMask, upgrade);
}

container::SharedPtr<const ObjectFireWeaponWhenDamagedPlan>
compileObjectFireWeaponWhenDamagedPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog) {
    auto plan = std::make_shared<ObjectFireWeaponWhenDamagedPlan>();
    static constexpr container::Array<container::StringView, 4> stateNames{
        "Pristine", "Damaged", "ReallyDamaged", "Rubble"};
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module),
                                  "FireWeaponWhenDamagedBehavior")) continue;
        ObjectFireWeaponWhenDamagedParameters parameters;
        parameters.authoredOrder = module.authoredOrder;
        if (const container::String* value = moduleValueLast(module, "StartsActive")) {
            parameters.startsActive = parseBoolean(*value);
        }
        if (const container::String* value = moduleValueLast(module, "DamageTypes")) {
            if (const std::optional<uint64_t> parsed = parseDamageTypeMask(*value)) {
                parameters.damageTypeMask = *parsed;
            } else {
                appendDiagnostic(plan->diagnostics, module,
                                 "DamageTypes contains an unknown damage type");
            }
        } else {
            parameters.damageTypeMask =
                (uint64_t{1} << static_cast<uint8_t>(DamageType::COUNT)) - 1u;
        }
        if (const container::String* value = moduleValueLast(module, "DamageAmount")) {
            if (const std::optional<float> parsed = parseFloat(*value)) {
                parameters.damageAmount = math::q32_32{*parsed};
            } else {
                appendDiagnostic(plan->diagnostics, module,
                                 "DamageAmount must be a finite scalar");
            }
        }
        for (size_t index = 0; index < stateNames.size(); ++index) {
            const container::String reaction =
                "ReactionWeapon" + container::String(stateNames[index]);
            const container::String continuous =
                "ContinuousWeapon" + container::String(stateNames[index]);
            if (const container::String* value = moduleValueLast(module, reaction)) {
                if (!asciiEqualIgnoreCase(trim(*value), "NONE")) {
                    parameters.reactionWeapons[index] = container::String(trim(*value));
                }
            }
            if (const container::String* value = moduleValueLast(module, continuous)) {
                if (!asciiEqualIgnoreCase(trim(*value), "NONE")) {
                    parameters.continuousWeapons[index] = container::String(trim(*value));
                }
            }
        }
        parseUpgradeMux(module, parameters.upgradeMux);
        compileObjectUpgradeMuxRecipe(
            parameters.upgradeMux, upgradeCatalog);
        plan->rules.push_back(std::move(parameters));
    }
    if (plan->rules.empty()) return nullptr;
    std::sort(plan->rules.begin(), plan->rules.end(),
              [](const auto& left, const auto& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    return plan;
}

container::SharedPtr<const ObjectFireWeaponWhenDeadParameters>
compileObjectFireWeaponWhenDeadParameters(
    const ModuleData& module,
    container::Vector<container::String>* diagnostics,
    const engine::UpgradeCatalog* upgradeCatalog) {
    auto parameters = std::make_shared<ObjectFireWeaponWhenDeadParameters>();
    if (const container::String* value = moduleValueLast(module, "StartsActive")) {
        parameters->startsActive = parseBoolean(*value);
    }
    if (const container::String* value = moduleValueLast(module, "DeathWeapon")) {
        if (!asciiEqualIgnoreCase(trim(*value), "NONE")) {
            parameters->deathWeapon = container::String(trim(*value));
        }
    }
    if (parameters->deathWeapon.empty() && diagnostics) {
        appendDiagnostic(*diagnostics, module, "DeathWeapon is required");
    }
    parseUpgradeMux(module, parameters->upgradeMux);
    compileObjectUpgradeMuxRecipe(parameters->upgradeMux, upgradeCatalog);
    return parameters;
}

} // namespace game
