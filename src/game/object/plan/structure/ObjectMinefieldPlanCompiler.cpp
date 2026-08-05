#include "game/object/plan/structure/ObjectMinefieldPlanTypes.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"
#include "core/container/string_utils.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace {

using namespace engine;
using Fixed = math::q32_32;

constexpr auto asciiEqual = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView moduleClass(
    const game::ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* valueLast(
    const game::ModuleData& module, container::StringView key) noexcept {
    for (auto it = module.values.rbegin(); it != module.values.rend(); ++it) {
        if (asciiEqual(it->first, key)) return &it->second;
    }
    for (const auto& [name, value] : module.properties) {
        if (asciiEqual(name, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] container::String trim(container::StringView value) {
    return container::trimAsciiCopy(value);
}

[[nodiscard]] bool parseBool(container::StringView value,
                             bool fallback = false) noexcept {
    const container::String text = trim(value);
    if (asciiEqual(text, "yes") || asciiEqual(text, "true") || text == "1") {
        return true;
    }
    if (asciiEqual(text, "no") || asciiEqual(text, "false") || text == "0") {
        return false;
    }
    return fallback;
}

[[nodiscard]] std::optional<double> parseNumber(
    container::StringView value, bool percentage = false) noexcept {
    container::String text = trim(value);
    bool percent = false;
    if (!text.empty() && text.back() == '%') {
        percent = true;
        text.pop_back();
        text = trim(text);
    }
    double parsed = 0.0;
    const auto [cursor, error] =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || cursor != text.data() + text.size() ||
        !std::isfinite(parsed)) return std::nullopt;
    if (percent || percentage) parsed /= 100.0;
    return parsed;
}

[[nodiscard]] uint32_t parseUnsigned(container::StringView value,
                                     uint32_t fallback) noexcept {
    const std::optional<double> parsed = parseNumber(value);
    if (!parsed || *parsed < 0.0) return fallback;
    return static_cast<uint32_t>(std::min<double>(
        std::numeric_limits<uint32_t>::max(), std::ceil(*parsed)));
}

void readString(const game::ModuleData& module, container::StringView key,
                container::String& output) {
    if (const container::String* value = valueLast(module, key)) {
        output = trim(*value);
    }
}

void readFixed(const game::ModuleData& module, container::StringView key,
               Fixed& output, bool percentage = false) {
    if (const container::String* value = valueLast(module, key)) {
        if (const std::optional<double> parsed = parseNumber(*value, percentage)) {
            output = Fixed{*parsed};
        }
    }
}

void readUnsigned(const game::ModuleData& module, container::StringView key,
                  uint32_t& output) {
    if (const container::String* value = valueLast(module, key)) {
        output = parseUnsigned(*value, output);
    }
}

void readBool(const game::ModuleData& module, container::StringView key,
              bool& output) {
    if (const container::String* value = valueLast(module, key)) {
        output = parseBool(*value, output);
    }
}

[[nodiscard]] container::Vector<container::String> splitWords(
    container::StringView value) {
    container::Vector<container::String> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() &&
               (std::isspace(static_cast<unsigned char>(value[cursor])) ||
                value[cursor] == ',')) ++cursor;
        const size_t begin = cursor;
        while (cursor < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[cursor])) &&
               value[cursor] != ',') ++cursor;
        if (begin != cursor) result.emplace_back(value.substr(begin, cursor - begin));
    }
    return result;
}

[[nodiscard]] game::WeaponSlot parseWeaponSlot(
    container::StringView value, game::WeaponSlot fallback) noexcept {
    if (asciiEqual(trim(value), "PRIMARY")) return game::WeaponSlot::Primary;
    if (asciiEqual(trim(value), "SECONDARY")) return game::WeaponSlot::Secondary;
    if (asciiEqual(trim(value), "TERTIARY")) return game::WeaponSlot::Tertiary;
    return fallback;
}

} // namespace

namespace game {

container::SharedPtr<const ObjectMinefieldPlan>
compileObjectMinefieldPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog) {
    auto plan = std::make_shared<ObjectMinefieldPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (asciiEqual(moduleClass(module), "GenerateMinefieldBehavior")) {
            ObjectGenerateMinefieldRule rule;
            rule.authoredOrder = module.authoredOrder;
            readString(module, "MineName", rule.mineName);
            readString(module, "UpgradedMineName", rule.upgradedMineName);
            readString(module, "UpgradedTriggeredBy", rule.upgradedTriggeredBy);
            readString(module, "GenerationFX", rule.generationFx);
            if (valueLast(module, "DistanceAroundObject")) {
                readFixed(module, "DistanceAroundObject", rule.distanceAroundObject);
                rule.hasAuthoredDistanceAroundObject = true;
            }
            if (valueLast(module, "MinesPerSquareFoot")) {
                readFixed(module, "MinesPerSquareFoot", rule.minesPerSquareFoot);
                rule.hasAuthoredMinesPerSquareFoot = true;
            }
            readFixed(module, "RandomJitter", rule.randomJitter, true);
            readFixed(module, "SkipIfThisMuchUnderStructure",
                      rule.skipIfThisMuchUnderStructure, true);
            readBool(module, "GenerateOnlyOnDeath", rule.generateOnlyOnDeath);
            readBool(module, "BorderOnly", rule.borderOnly);
            readBool(module, "SmartBorder", rule.smartBorder);
            readBool(module, "SmartBorderSkipInterior", rule.smartBorderSkipInterior);
            readBool(module, "AlwaysCircular", rule.alwaysCircular);
            readBool(module, "Upgradable", rule.upgradable);
            readBool(module, "RequiresAllTriggers", rule.requiresAllTriggers);
            if (const container::String* value = valueLast(module, "TriggeredBy"))
                rule.triggeredBy = splitWords(*value);
            if (const container::String* value = valueLast(module, "ConflictsWith"))
                rule.conflictsWith = splitWords(*value);
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
                const container::StringView upgradedTrigger =
                    rule.upgradedTriggeredBy.empty()
                        ? engine::well_known_upgrade::ChinaEmpMines
                        : container::StringView{rule.upgradedTriggeredBy};
                if (const engine::UpgradeDefinition* definition =
                        upgradeCatalog->find(upgradedTrigger)) {
                    rule.upgradedTriggerId = definition->id;
                }
                rule.upgradeMasksCompiled = true;
            }
            if (rule.mineName.empty()) {
                plan->diagnostics.push_back(
                    "GenerateMinefieldBehavior is missing MineName");
            }
            plan->generators.push_back(std::move(rule));
            continue;
        }
        if (asciiEqual(moduleClass(module), "MinefieldBehavior")) {
            ObjectMinefieldRule rule;
            rule.authoredOrder = module.authoredOrder;
            readString(module, "DetonationWeapon", rule.detonationWeapon);
            readString(module, "CreationList", rule.creationList);
            readUnsigned(module, "CreatorDeathCheckRate",
                         rule.creatorDeathCheckMilliseconds);
            readUnsigned(module, "ScootFromStartingPointTime", rule.scootMilliseconds);
            readUnsigned(module, "NumVirtualMines", rule.numVirtualMines);
            rule.numVirtualMines = std::max<uint32_t>(1, rule.numVirtualMines);
            readFixed(module, "RepeatDetonateMoveThresh",
                      rule.repeatDetonateMoveThreshold);
            readFixed(module, "DegenPercentPerSecondAfterCreatorDies",
                      rule.healthPercentToDrainPerSecond, true);
            readBool(module, "StopsRegenAfterCreatorDies",
                     rule.stopsRegenAfterCreatorDies);
            readBool(module, "Regenerates", rule.regenerates);
            readBool(module, "WorkersDetonate", rule.workersDetonate);
            if (const container::String* value = valueLast(module, "DetonatedBy")) {
                rule.detonatedBy = 0;
                for (const container::String& word : splitWords(*value)) {
                    if (asciiEqual(word, "ALLIES"))
                        rule.detonatedBy |= static_cast<ObjectMineRelationshipMask>(
                            ObjectMineRelationship::Allies);
                    else if (asciiEqual(word, "ENEMIES"))
                        rule.detonatedBy |= static_cast<ObjectMineRelationshipMask>(
                            ObjectMineRelationship::Enemies);
                    else if (asciiEqual(word, "NEUTRAL"))
                        rule.detonatedBy |= static_cast<ObjectMineRelationshipMask>(
                            ObjectMineRelationship::Neutral);
                }
            }
            plan->mines.push_back(std::move(rule));
            continue;
        }
        if (asciiEqual(moduleClass(module), "DemoTrapUpdate")) {
            ObjectDemoTrapRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const container::String* value = valueLast(module, "DetonationWeaponSlot"))
                rule.detonationWeaponSlot = parseWeaponSlot(*value, rule.detonationWeaponSlot);
            if (const container::String* value = valueLast(module, "ProximityModeWeaponSlot"))
                rule.proximityModeWeaponSlot = parseWeaponSlot(*value, rule.proximityModeWeaponSlot);
            if (const container::String* value = valueLast(module, "ManualModeWeaponSlot"))
                rule.manualModeWeaponSlot = parseWeaponSlot(*value, rule.manualModeWeaponSlot);
            readFixed(module, "TriggerDetonationRange", rule.triggerDetonationRange);
            readUnsigned(module, "ScanRate", rule.scanMilliseconds);
            readString(module, "DetonationWeapon", rule.detonationWeapon);
            readBool(module, "DefaultProximityMode", rule.defaultsToProximityMode);
            readBool(module, "AutoDetonationWithFriendsInvolved",
                     rule.friendlyDetonation);
            readBool(module, "DetonateWhenKilled", rule.detonateWhenKilled);
            if (const container::String* value = valueLast(module, "IgnoreTargetTypes"))
                static_cast<void>(game::compileObjectKindOfMask(
                    *value, rule.ignoreTargetKindMask));
            if (rule.detonationWeaponSlot == rule.proximityModeWeaponSlot ||
                rule.detonationWeaponSlot == rule.manualModeWeaponSlot ||
                rule.proximityModeWeaponSlot == rule.manualModeWeaponSlot) {
                plan->diagnostics.push_back(
                    "DemoTrapUpdate requires three distinct WeaponSlot values");
            }
            plan->demoTraps.push_back(std::move(rule));
        }
    }
    if (plan->generators.empty() && plan->mines.empty() &&
        plan->demoTraps.empty()) return nullptr;
    const auto order = [](const auto& left, const auto& right) {
        return left.authoredOrder < right.authoredOrder;
    };
    std::stable_sort(plan->generators.begin(), plan->generators.end(), order);
    std::stable_sort(plan->mines.begin(), plan->mines.end(), order);
    std::stable_sort(plan->demoTraps.begin(), plan->demoTraps.end(), order);
    return plan;
}

} // namespace game
