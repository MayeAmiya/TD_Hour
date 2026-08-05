#include "game/object/plan/containment/ObjectSpawnSlavePlanTypes.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"

#include "game/base/DamageTypes.h"
#include "game/base/SimulationRandom.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/PlayerRegistry.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <type_traits>

namespace game {
namespace {

[[nodiscard]] bool equalIgnoreCase(container::StringView a,
                                   container::StringView b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

[[nodiscard]] container::StringView trim(container::StringView value) noexcept {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

[[nodiscard]] const container::String* valueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto it = module.values.rbegin(); it != module.values.rend(); ++it) {
        if (equalIgnoreCase(it->first, key)) return &it->second;
    }
    return nullptr;
}

[[nodiscard]] uint32_t unsignedValue(container::StringView value,
                                     uint32_t fallback = 0) noexcept {
    const container::StringView text = trim(value);
    uint32_t result = fallback;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                        result);
    return parsed.ec == std::errc{} ? result : fallback;
}

[[nodiscard]] float floatValue(container::StringView value,
                               float fallback = 0.0f) noexcept {
    return parseContentFloatOr(value, {
        .source = __FILE__, .block = "Object", .module = "SpawnBehavior",
        .field = "Real", .fallback = fallback});
}

[[nodiscard]] bool booleanValue(container::StringView value,
                                bool fallback = false) noexcept {
    const container::StringView text = trim(value);
    if (equalIgnoreCase(text, "yes") || equalIgnoreCase(text, "true") ||
        text == "1") return true;
    if (equalIgnoreCase(text, "no") || equalIgnoreCase(text, "false") ||
        text == "0") return false;
    return fallback;
}

[[nodiscard]] container::Vector<container::String> tokens(
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

[[nodiscard]] uint64_t damageMask(container::StringView value) {
    uint64_t result = 0;
    for (const container::String& token : tokens(value)) {
        const std::optional<DamageType> type = tryParseDamageType(token);
        if (type && static_cast<uint32_t>(*type) < 64u)
            result |= UINT64_C(1) << static_cast<uint32_t>(*type);
    }
    return result;
}

void parseInitialPayload(container::StringView value, container::String& name,
                         uint32_t& count) {
    for (const container::String& token : tokens(value)) {
        const size_t colon = token.find(':');
        if (colon == container::String::npos) continue;
        const container::StringView key{token.data(), colon};
        const container::StringView payload{token.data() + colon + 1,
                                            token.size() - colon - 1};
        if (equalIgnoreCase(key, "Name")) name = payload;
        else if (equalIgnoreCase(key, "Count")) count = unsignedValue(payload);
    }
}

} // namespace

container::SharedPtr<const ObjectSpawnSlavePlan>
compileObjectSpawnSlavePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectSpawnSlavePlan>();
    plan->hiveStructureBody =
        templateData.body.kind == ObjectBodyKind::HiveStructure;
    for (const ModuleData& module : templateData.modules) {
        if (equalIgnoreCase(module.moduleClass, "SpawnBehavior")) {
            ObjectSpawnRule rule;
            rule.authoredOrder = module.authoredOrder;
            for (const auto& [key, value] : module.values) {
                if (equalIgnoreCase(key, "SpawnTemplateName"))
                    rule.templateNames.emplace_back(trim(value));
            }
            if (const auto* v = valueLast(module, "SpawnNumber"))
                rule.spawnNumber = unsignedValue(*v);
            if (const auto* v = valueLast(module, "SpawnReplaceDelay"))
                rule.replacementDelayMilliseconds = unsignedValue(*v);
            if (const auto* v = valueLast(module, "InitialBurst"))
                rule.initialBurst = unsignedValue(*v);
            if (const auto* v = valueLast(module, "OneShot"))
                rule.oneShot = booleanValue(*v);
            if (const auto* v = valueLast(module, "CanReclaimOrphans"))
                rule.canReclaimOrphans = booleanValue(*v);
            if (const auto* v = valueLast(module, "AggregateHealth"))
                rule.aggregateHealth = booleanValue(*v);
            if (const auto* v = valueLast(module, "ExitByBudding"))
                rule.exitByBudding = booleanValue(*v);
            if (const auto* v = valueLast(module, "SpawnedRequireSpawner"))
                rule.spawnedRequireSpawner = booleanValue(*v);
            if (const auto* v = valueLast(module, "SlavesHaveFreeWill"))
                rule.slavesHaveFreeWill = booleanValue(*v);
            if (const auto* v = valueLast(
                    module, "PropagateDamageTypesToSlavesWhenExisting"))
                rule.propagateDamageTypesMask = damageMask(*v);
            plan->spawns.push_back(std::move(rule));
        } else if (equalIgnoreCase(module.moduleClass, "MobNexusContain")) {
            ObjectMobNexusRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const auto* v = valueLast(module, "Slots"))
                rule.slots = unsignedValue(*v, 1);
            if (const auto* v = valueLast(module, "ExitPitchRate"))
                rule.exitPitchRate = math::q32_32{floatValue(*v)};
            if (const auto* v = valueLast(module, "HealthRegen%PerSec"))
                rule.healthRegenPerSecond = math::q32_32{floatValue(*v) / 100.0f};
            if (const auto* v = valueLast(module, "ExitBone")) rule.exitBone = *v;
            if (const auto* v = valueLast(module, "InitialPayload"))
                parseInitialPayload(*v, rule.initialPayloadTemplate,
                                    rule.initialPayloadCount);
            if (const auto* v = valueLast(module, "ScatterNearbyOnExit"))
                rule.scatterNearbyOnExit = booleanValue(*v);
            if (const auto* v = valueLast(module, "OrientLikeContainerOnExit"))
                rule.orientLikeContainerOnExit = booleanValue(*v);
            if (const auto* v = valueLast(module, "KeepContainerVelocityOnExit"))
                rule.keepContainerVelocityOnExit = booleanValue(*v);
            plan->mobNexus.push_back(std::move(rule));
        } else if (equalIgnoreCase(module.moduleClass, "HordeUpdate")) {
            ObjectHordeRule rule;
            rule.authoredOrder = module.authoredOrder;
            // HORDE_FIXED is the corrected modern default used by RefCode
            // when retail-CRC compatibility is not requested. Stock content
            // explicitly authors HORDE and retains its classic semantics.
            rule.action = "HORDE_FIXED";
            rule.actionKind = ObjectHordeActionKind::HordeFixed;
            if (const auto* v = valueLast(module, "UpdateRate"))
                rule.updateRateMilliseconds = unsignedValue(*v);
            if (const auto* v = valueLast(module, "KindOf"))
                static_cast<void>(compileObjectKindOfMask(*v, rule.kindOf));
            if (const auto* v = valueLast(module, "Count")) rule.count = unsignedValue(*v);
            if (const auto* v = valueLast(module, "Radius"))
                rule.radius = math::q32_32{std::max(0.0f, floatValue(*v))};
            if (const auto* v = valueLast(module, "RubOffRadius"))
                rule.rubOffRadius = math::q32_32{std::max(0.0f, floatValue(*v))};
            if (const auto* v = valueLast(module, "AlliesOnly"))
                rule.alliesOnly = booleanValue(*v);
            if (const auto* v = valueLast(module, "ExactMatch"))
                rule.exactMatch = booleanValue(*v);
            if (const auto* v = valueLast(module, "AllowedNationalism"))
                rule.allowedNationalism = booleanValue(*v);
            if (const auto* v = valueLast(module, "Action")) {
                rule.action = *v;
                if (equalIgnoreCase(*v, "HORDE"))
                    rule.actionKind = ObjectHordeActionKind::Horde;
                else if (equalIgnoreCase(*v, "HORDE_FIXED"))
                    rule.actionKind = ObjectHordeActionKind::HordeFixed;
            }
            if (const auto* v = valueLast(module, "FlagSubObjectNames"))
                rule.flagSubObjectNames = tokens(*v);
            plan->hordes.push_back(std::move(rule));
        } else if (equalIgnoreCase(module.moduleClass,
                                   "TensileFormationUpdate")) {
            ObjectTensileFormationRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const auto* v = valueLast(module, "Enabled"))
                rule.enabled = booleanValue(*v);
            if (const auto* v = valueLast(module, "CrackSound"))
                rule.crackSound = *v;
            plan->tensileFormations.push_back(std::move(rule));
        } else if (equalIgnoreCase(module.moduleClass, "SlavedUpdate")) {
            ObjectSlavedRule rule;
            rule.authoredOrder = module.authoredOrder;
            const auto u = [&](container::StringView key, uint32_t& out) {
                if (const auto* v = valueLast(module, key)) out = unsignedValue(*v);
            };
            const auto f = [&](container::StringView key, math::q32_32& out) {
                if (const auto* v = valueLast(module, key)) out = math::q32_32{floatValue(*v)};
            };
            u("GuardMaxRange", rule.guardMaxRange); u("GuardWanderRange", rule.guardWanderRange);
            u("AttackRange", rule.attackRange); u("AttackWanderRange", rule.attackWanderRange);
            u("ScoutRange", rule.scoutRange); u("ScoutWanderRange", rule.scoutWanderRange);
            u("RepairRange", rule.repairRange);
            u("DistToTargetToGrantRangeBonus", rule.distanceToTargetForRangeBonus);
            u("RepairWhenBelowHealth%", rule.repairWhenBelowHealthPercent);
            u("RepairMinReadyTime", rule.repairMinReadyMilliseconds);
            u("RepairMaxReadyTime", rule.repairMaxReadyMilliseconds);
            u("RepairMinWeldTime", rule.repairMinWeldMilliseconds);
            u("RepairMaxWeldTime", rule.repairMaxWeldMilliseconds);
            f("RepairMinAltitude", rule.repairMinAltitude);
            f("RepairMaxAltitude", rule.repairMaxAltitude);
            f("RepairRatePerSecond", rule.repairRatePerSecond);
            if (const auto* v = valueLast(module, "RepairWeldingSys")) rule.repairWeldingSystem = *v;
            if (const auto* v = valueLast(module, "RepairWeldingFXBone")) rule.repairWeldingFxBone = *v;
            if (const auto* v = valueLast(module, "StayOnSameLayerAsMaster"))
                rule.stayOnSameLayerAsMaster = booleanValue(*v);
            plan->slaved.push_back(std::move(rule));
        } else if (equalIgnoreCase(module.moduleClass,
                                   "MobMemberSlavedUpdate")) {
            ObjectMobMemberSlavedRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const auto* v = valueLast(module, "MustCatchUpRadius"))
                rule.mustCatchUpRadius = unsignedValue(*v, 50);
            if (const auto* v = valueLast(module, "NoNeedToCatchUpRadius"))
                rule.noNeedToCatchUpRadius = unsignedValue(*v, 25);
            if (const auto* v = valueLast(module, "CatchUpCrisisBailTime"))
                rule.catchUpCrisisBailFrames = unsignedValue(*v, 999999);
            if (const auto* v = valueLast(module, "Squirrelliness"))
                rule.squirrelliness = math::q32_32{
                    std::clamp(floatValue(*v), 0.0f, 1.0f)};
            plan->mobMemberSlaved.push_back(std::move(rule));
        }
    }
    if (!plan->hiveStructureBody && plan->spawns.empty() &&
        plan->mobNexus.empty() && plan->hordes.empty() &&
        plan->tensileFormations.empty() && plan->slaved.empty() &&
        plan->mobMemberSlaved.empty()) return {};
    return plan;
}

} // namespace game
