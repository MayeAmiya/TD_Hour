#include "game/object/plan/combat/ObjectTacticalPlanTypes.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "core/math/fixed/q32_32_trig.h"

#include "game/base/SimulationRandom.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/contracts/ObjectToppleMath.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/plan/combat/ObjectNeutronMissileSlowDeathPlanTypes.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/MapVisibilityAuthority.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>

namespace game {
namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* valueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto found = module.values.rbegin(); found != module.values.rend(); ++found) {
        if (equalInsensitive(found->first, key)) return &found->second;
    }
    const auto found = module.properties.find(container::String{key});
    return found == module.properties.end() ? nullptr : &found->second;
}

[[nodiscard]] bool parseBool(container::StringView value) noexcept {
    return equalInsensitive(value, "YES") || equalInsensitive(value, "TRUE") || value == "1";
}

[[nodiscard]] float parseFloat(container::StringView value, float fallback = 0.0f) noexcept {
    return parseContentFloatOr(value, {
        .source = __FILE__, .block = "Object", .module = "Tactical",
        .field = "Real", .fallback = fallback});
}

[[nodiscard]] math::q32_32 parseFixed(container::StringView value,
                                     float fallback = 0.0f) noexcept {
    return math::q32_32{parseFloat(value, fallback)};
}

[[nodiscard]] math::q32_32 parsePercent(container::StringView value,
                                       float fallback) noexcept {
    const bool authoredPercent = value.find('%') != container::StringView::npos;
    float parsed = parseFloat(value, fallback);
    if (authoredPercent) parsed *= 0.01f;
    return math::q32_32{parsed};
}

[[nodiscard]] uint32_t parseMilliseconds(container::StringView value,
                                         uint32_t fallback = 0) noexcept {
    const float parsed = parseFloat(value, static_cast<float>(fallback));
    if (parsed <= 0.0f) return parsed == 0.0f ? 0u : fallback;
    if (parsed >= static_cast<float>(UINT32_MAX)) return UINT32_MAX;
    return static_cast<uint32_t>(std::ceil(parsed));
}

[[nodiscard]] int32_t parseInteger(container::StringView value,
                                   int32_t fallback = 0) noexcept {
    container::String text{value};
    char* end = nullptr;
    const long result = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str()) return fallback;
    return static_cast<int32_t>(std::clamp<long>(
        result, std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max()));
}

[[nodiscard]] ObjectMaxHealthChangeType parseMaxHealthChangeType(
    container::StringView value) noexcept {
    if (equalInsensitive(value, "SAME_CURRENTHEALTH") ||
        equalInsensitive(value, "SAME_CURRENT_HEALTH")) {
        return ObjectMaxHealthChangeType::SameCurrentHealth;
    }
    if (equalInsensitive(value, "ADD_CURRENT_HEALTH_TOO")) {
        return ObjectMaxHealthChangeType::AddCurrentHealthToo;
    }
    if (equalInsensitive(value, "FULLY_HEAL")) {
        return ObjectMaxHealthChangeType::FullyHeal;
    }
    return ObjectMaxHealthChangeType::PreserveRatio;
}

[[nodiscard]] container::Vector<container::String> splitTokens(
    container::StringView value) {
    container::Vector<container::String> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t\r\n", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t\r\n", cursor);
        result.emplace_back(value.substr(
            cursor, end == container::StringView::npos ? value.size() - cursor
                                                        : end - cursor));
        if (end == container::StringView::npos) break;
        cursor = end + 1;
    }
    return result;
}

[[nodiscard]] WeaponSlot parseWeaponSlot(container::StringView value) noexcept {
    if (equalInsensitive(value, "SECONDARY")) return WeaponSlot::Secondary;
    if (equalInsensitive(value, "TERTIARY")) return WeaponSlot::Tertiary;
    return WeaponSlot::Primary;
}

template <typename T>
void sortByAuthoredOrder(container::Vector<T>& rules) {
    std::stable_sort(rules.begin(), rules.end(), [](const T& left, const T& right) {
        return left.authoredOrder < right.authoredOrder;
    });
}

} // namespace

container::SharedPtr<const ObjectTacticalPlan>
compileObjectTacticalPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog) {
    auto plan = std::make_shared<ObjectTacticalPlan>();
    for (const ModuleData& module : templateData.modules) {
        const container::StringView klass = moduleClass(module);
        if (equalInsensitive(klass, "PropagandaTowerBehavior")) {
            ObjectPropagandaTowerRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const auto* value = valueLast(module, "Radius")) rule.radius = parseFixed(*value, 1.0f);
            if (const auto* value = valueLast(module, "DelayBetweenUpdates")) rule.scanDelayMilliseconds = parseMilliseconds(*value, 100u);
            if (const auto* value = valueLast(module, "HealPercentEachSecond")) rule.healPercentPerSecond = parsePercent(*value, 0.01f);
            if (const auto* value = valueLast(module, "UpgradedHealPercentEachSecond")) rule.upgradedHealPercentPerSecond = parsePercent(*value, 0.02f);
            if (const auto* value = valueLast(module, "PulseFX")) rule.pulseFx = *value;
            if (const auto* value = valueLast(module, "UpgradedPulseFX")) rule.upgradedPulseFx = *value;
            if (const auto* value = valueLast(module, "UpgradeRequired")) rule.upgradeRequired = *value;
            if (upgradeCatalog) {
                if (const engine::UpgradeDefinition* definition =
                        upgradeCatalog->find(rule.upgradeRequired)) {
                    rule.upgradeRequiredId = definition->id;
                }
            }
            if (const auto* value = valueLast(module, "AffectsSelf")) rule.affectsSelf = parseBool(*value);
            plan->propagandaTowers.push_back(std::move(rule));
        } else if (equalInsensitive(klass, "AssistedTargetingUpdate")) {
            ObjectAssistedTargetingRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const auto* value = valueLast(module, "AssistingClipSize")) rule.assistingClipSize = static_cast<uint32_t>(std::max(0, parseInteger(*value)));
            if (const auto* value = valueLast(module, "AssistingWeaponSlot")) rule.assistingWeaponSlot = parseWeaponSlot(*value);
            if (const auto* value = valueLast(module, "LaserFromAssisted")) rule.laserFromAssisted = *value;
            if (const auto* value = valueLast(module, "LaserToTarget")) rule.laserToTarget = *value;
            plan->assistedTargeting.push_back(std::move(rule));
        } else if (equalInsensitive(klass, "DeployStyleAIUpdate")) {
            ObjectDeployStyleRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const auto* value = valueLast(module, "PackTime")) rule.packMilliseconds = parseMilliseconds(*value);
            if (const auto* value = valueLast(module, "UnpackTime")) rule.unpackMilliseconds = parseMilliseconds(*value);
            if (const auto* value = valueLast(module, "ResetTurretBeforePacking")) rule.resetTurretBeforePacking = parseBool(*value);
            if (const auto* value = valueLast(module, "TurretsFunctionOnlyWhenDeployed")) rule.turretsFunctionOnlyWhenDeployed = parseBool(*value);
            if (const auto* value = valueLast(module, "TurretsMustCenterBeforePacking")) rule.turretsMustCenterBeforePacking = parseBool(*value);
            if (const auto* value = valueLast(module, "ManualDeployAnimations")) rule.manualDeployAnimations = parseBool(*value);
            plan->deployStyles.push_back(rule);
        } else if (equalInsensitive(klass, "ToppleUpdate")) {
            ObjectToppleRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const auto* value = valueLast(module, "ToppleFX")) rule.toppleFx = *value;
            if (const auto* value = valueLast(module, "BounceFX")) rule.bounceFx = *value;
            if (const auto* value = valueLast(module, "StumpName")) rule.stumpName = *value;
            if (const auto* value = valueLast(module, "InitialVelocityPercent")) rule.initialVelocityPercent = parsePercent(*value, 0.01f);
            if (const auto* value = valueLast(module, "InitialAccelPercent")) rule.initialAccelerationPercent = parsePercent(*value, 0.01f);
            if (const auto* value = valueLast(module, "BounceVelocityPercent")) rule.bounceVelocityPercent = parsePercent(*value, 0.3f);
            if (const auto* value = valueLast(module, "KillWhenStartToppling")) rule.killWhenStartToppling = parseBool(*value);
            if (const auto* value = valueLast(module, "KillWhenFinishedToppling")) rule.killWhenFinishedToppling = parseBool(*value);
            if (const auto* value = valueLast(module, "KillStumpWhenToppled")) rule.killStumpWhenToppled = parseBool(*value);
            if (const auto* value = valueLast(module, "ToppleLeftOrRightOnly")) rule.toppleLeftOrRightOnly = parseBool(*value);
            if (const auto* value = valueLast(module, "ReorientToppledRubble")) rule.reorientToppledRubble = parseBool(*value);
            plan->topple.push_back(std::move(rule));
        } else if (equalInsensitive(klass, "BattlePlanUpdate")) {
            ObjectBattlePlanRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const auto* value = valueLast(module, "SpecialPowerTemplate")) rule.specialPowerTemplate = *value;
            if (const auto* value = valueLast(module, "BombardmentPlanAnimationTime")) rule.bombardmentAnimationMilliseconds = parseMilliseconds(*value);
            if (const auto* value = valueLast(module, "HoldTheLinePlanAnimationTime")) rule.holdTheLineAnimationMilliseconds = parseMilliseconds(*value);
            if (const auto* value = valueLast(module, "SearchAndDestroyPlanAnimationTime")) rule.searchAndDestroyAnimationMilliseconds = parseMilliseconds(*value);
            if (const auto* value = valueLast(module, "TransitionIdleTime")) rule.transitionIdleMilliseconds = parseMilliseconds(*value);
            if (const auto* value = valueLast(module, "BattlePlanChangeParalyzeTime")) rule.paralyzeMilliseconds = parseMilliseconds(*value);
            if (const auto* value = valueLast(module, "ValidMemberKindOf"))
                static_cast<void>(compileObjectKindOfMask(
                    *value, rule.validMemberKinds));
            if (const auto* value = valueLast(module, "InvalidMemberKindOf"))
                static_cast<void>(compileObjectKindOfMask(
                    *value, rule.invalidMemberKinds));
            if (const auto* value = valueLast(module, "HoldTheLinePlanArmorDamageScalar")) rule.holdTheLineArmorDamageScalar = parseFixed(*value, 1.0f);
            if (const auto* value = valueLast(module, "SearchAndDestroyPlanSightRangeScalar")) rule.searchAndDestroySightRangeScalar = parseFixed(*value, 1.0f);
            if (const auto* value = valueLast(module, "StrategyCenterSearchAndDestroySightRangeScalar")) rule.strategyCenterSearchSightScalar = parseFixed(*value, 1.0f);
            if (const auto* value = valueLast(module, "StrategyCenterHoldTheLineMaxHealthScalar")) rule.strategyCenterHoldHealthScalar = parseFixed(*value, 1.0f);
            if (const auto* value = valueLast(module, "StrategyCenterHoldTheLineMaxHealthChangeType")) rule.strategyCenterHoldHealthChangeType = parseMaxHealthChangeType(*value);
            if (const auto* value = valueLast(module, "StrategyCenterSearchAndDestroyDetectsStealth")) rule.strategyCenterDetectsStealth = parseBool(*value);
            if (const auto* value = valueLast(module, "BombardmentPlanUnpackSoundName")) rule.bombardmentUnpackSound = *value;
            if (const auto* value = valueLast(module, "BombardmentPlanPackSoundName")) rule.bombardmentPackSound = *value;
            if (const auto* value = valueLast(module, "BombardmentMessageLabel")) rule.bombardmentMessageLabel = *value;
            if (const auto* value = valueLast(module, "BombardmentAnnouncementName")) rule.bombardmentAnnouncement = *value;
            if (const auto* value = valueLast(module, "HoldTheLinePlanUnpackSoundName")) rule.holdTheLineUnpackSound = *value;
            if (const auto* value = valueLast(module, "HoldTheLinePlanPackSoundName")) rule.holdTheLinePackSound = *value;
            if (const auto* value = valueLast(module, "HoldTheLineMessageLabel")) rule.holdTheLineMessageLabel = *value;
            if (const auto* value = valueLast(module, "HoldTheLineAnnouncementName")) rule.holdTheLineAnnouncement = *value;
            if (const auto* value = valueLast(module, "SearchAndDestroyPlanUnpackSoundName")) rule.searchAndDestroyUnpackSound = *value;
            if (const auto* value = valueLast(module, "SearchAndDestroyPlanIdleLoopSoundName")) rule.searchAndDestroyIdleLoopSound = *value;
            if (const auto* value = valueLast(module, "SearchAndDestroyPlanPackSoundName")) rule.searchAndDestroyPackSound = *value;
            if (const auto* value = valueLast(module, "SearchAndDestroyMessageLabel")) rule.searchAndDestroyMessageLabel = *value;
            if (const auto* value = valueLast(module, "SearchAndDestroyAnnouncementName")) rule.searchAndDestroyAnnouncement = *value;
            if (const auto* value = valueLast(module, "VisionObjectName")) rule.visionObjectName = *value;
            if (rule.strategyCenterSearchSightScalar <= math::q32_32{}) {
                plan->diagnostics.push_back(
                    "BattlePlanUpdate StrategyCenterSearchAndDestroySightRangeScalar must be positive; using 1");
                rule.strategyCenterSearchSightScalar = math::q32_32{int32_t{1}};
            }
            if (rule.strategyCenterHoldHealthScalar <= math::q32_32{}) {
                plan->diagnostics.push_back(
                    "BattlePlanUpdate StrategyCenterHoldTheLineMaxHealthScalar must be positive; using 1");
                rule.strategyCenterHoldHealthScalar = math::q32_32{int32_t{1}};
            }
            plan->battlePlans.push_back(std::move(rule));
        } else if (equalInsensitive(klass, "SpecialAbilityUpdate")) {
            ObjectSpecialAbilityUpdateRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const auto* value = valueLast(module, "SpecialPowerTemplate")) rule.specialPowerTemplate = *value;
            if (const auto* value = valueLast(module, "StartAbilityRange")) rule.startAbilityRange = parseFixed(*value, 10'000'000.0f);
            if (const auto* value = valueLast(module, "AbilityAbortRange")) rule.abilityAbortRange = parseFixed(*value, 10'000'000.0f);
            if (const auto* value = valueLast(module, "PreparationTime")) rule.preparationMilliseconds = parseMilliseconds(*value);
            if (const auto* value = valueLast(module, "PersistentPrepTime")) rule.persistentPrepMilliseconds = parseMilliseconds(*value);
            if (const auto* value = valueLast(module, "PackTime")) rule.packMilliseconds = parseMilliseconds(*value);
            if (const auto* value = valueLast(module, "UnpackTime")) rule.unpackMilliseconds = parseMilliseconds(*value);
            if (const auto* value = valueLast(module, "PreTriggerUnstealthTime")) rule.preTriggerUnstealthMilliseconds = parseMilliseconds(*value);
            if (const auto* value = valueLast(module, "EffectDuration")) rule.effectDurationMilliseconds = parseMilliseconds(*value);
            if (const auto* value = valueLast(module, "EffectValue")) rule.effectValue = parseInteger(*value, 1);
            if (const auto* value = valueLast(module, "AwardXPForTriggering")) rule.awardExperienceForTriggering = parseInteger(*value);
            if (const auto* value = valueLast(module, "SkillPointsForTriggering")) rule.skillPointsForTriggering = parseInteger(*value, -1);
            if (const auto* value = valueLast(module, "SpecialObject")) rule.specialObject = *value;
            if (const auto* value = valueLast(module, "SpecialObjectAttachToBone")) rule.specialObjectAttachToBone = *value;
            if (const auto* value = valueLast(module, "DisableFXParticleSystem")) rule.disableFxParticleSystem = *value;
            if (const auto* value = valueLast(module, "PackSound")) rule.packSound = *value;
            if (const auto* value = valueLast(module, "UnpackSound")) rule.unpackSound = *value;
            if (const auto* value = valueLast(module, "PrepSoundLoop")) rule.prepSoundLoop = *value;
            if (const auto* value = valueLast(module, "TriggerSound")) rule.triggerSound = *value;
            if (const auto* value = valueLast(module, "MaxSpecialObjects")) rule.maximumSpecialObjects = static_cast<uint32_t>(std::max(0, parseInteger(*value, 1)));
            if (const auto* value = valueLast(module, "FleeRangeAfterCompletion")) rule.fleeRangeAfterCompletion = parseFixed(*value);
            if (const auto* value = valueLast(module, "PackUnpackVariationFactor")) rule.packUnpackVariationFactor = parseFixed(*value);
            if (const auto* value = valueLast(module, "SkipPackingWithNoTarget")) rule.skipPackingWithNoTarget = parseBool(*value);
            if (const auto* value = valueLast(module, "SpecialObjectsPersistent")) rule.specialObjectsPersistent = parseBool(*value);
            if (const auto* value = valueLast(module, "UniqueSpecialObjectTargets")) rule.uniqueSpecialObjectTargets = parseBool(*value);
            if (const auto* value = valueLast(module, "SpecialObjectsPersistWhenOwnerDies")) rule.specialObjectsPersistWhenOwnerDies = parseBool(*value);
            if (const auto* value = valueLast(module, "AlwaysValidateSpecialObjects")) rule.alwaysValidateSpecialObjects = parseBool(*value);
            if (const auto* value = valueLast(module, "FlipOwnerAfterPacking")) rule.flipOwnerAfterPacking = parseBool(*value);
            if (const auto* value = valueLast(module, "FlipOwnerAfterUnpacking")) rule.flipOwnerAfterUnpacking = parseBool(*value);
            if (const auto* value = valueLast(module, "DoCaptureFX")) rule.doCaptureFx = parseBool(*value);
            if (const auto* value = valueLast(module, "LoseStealthOnTrigger")) rule.loseStealthOnTrigger = parseBool(*value);
            if (const auto* value = valueLast(module, "ApproachRequiresLOS")) rule.approachRequiresLineOfSight = parseBool(*value);
            if (const auto* value = valueLast(module, "NeedToFaceTarget")) rule.needToFaceTarget = parseBool(*value);
            if (const auto* value = valueLast(module, "PersistenceRequiresRecharge")) rule.persistenceRequiresRecharge = parseBool(*value);
            plan->specialAbilities.push_back(std::move(rule));
        } else if (equalInsensitive(klass, "CommandButtonHuntUpdate")) {
            ObjectCommandButtonHuntRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const auto* value = valueLast(module, "ScanRate")) rule.scanRateMilliseconds = parseMilliseconds(*value, 1000u);
            if (const auto* value = valueLast(module, "ScanRange")) rule.scanRange = parseFixed(*value, 9999.0f);
            plan->commandButtonHunts.push_back(rule);
        } else if (equalInsensitive(klass, "WanderAIUpdate")) {
            plan->wanderAuthoredOrders.push_back(module.authoredOrder);
        }
    }
    sortByAuthoredOrder(plan->propagandaTowers);
    sortByAuthoredOrder(plan->assistedTargeting);
    sortByAuthoredOrder(plan->deployStyles);
    sortByAuthoredOrder(plan->topple);
    sortByAuthoredOrder(plan->battlePlans);
    sortByAuthoredOrder(plan->specialAbilities);
    sortByAuthoredOrder(plan->commandButtonHunts);
    std::sort(plan->wanderAuthoredOrders.begin(), plan->wanderAuthoredOrders.end());
    const bool empty = plan->propagandaTowers.empty() && plan->assistedTargeting.empty() &&
        plan->deployStyles.empty() && plan->topple.empty() && plan->battlePlans.empty() &&
        plan->specialAbilities.empty() && plan->commandButtonHunts.empty() &&
        plan->wanderAuthoredOrders.empty();
    return empty ? nullptr : plan;
}

} // namespace game
