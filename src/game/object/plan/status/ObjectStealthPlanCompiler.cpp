#include "game/object/plan/status/ObjectStealthPlanTypes.h"

#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/contracts/ObjectDeathReaction.h"
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
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace game {
namespace {

using container::asciiEqualIgnoreCase;

using container::trimAsciiView;

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto found = module.values.rbegin();
         found != module.values.rend(); ++found) {
        if (asciiEqualIgnoreCase(found->first, key)) return &found->second;
    }
    for (const auto& [candidate, value] : module.properties) {
        if (asciiEqualIgnoreCase(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] container::Vector<container::StringView> splitTokens(
    container::StringView value) {
    container::Vector<container::StringView> output;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t\r\n,", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t\r\n,", cursor);
        output.push_back(value.substr(
            cursor, end == container::StringView::npos
                        ? value.size() - cursor : end - cursor));
        if (end == container::StringView::npos) break;
        cursor = end + 1;
    }
    return output;
}

[[nodiscard]] std::optional<float> parseFloat(
    container::StringView value) noexcept {
    return parseContentFloat(value, {
        .source = __FILE__, .block = "Object", .module = "Stealth",
        .field = "Real"});
}

[[nodiscard]] uint32_t parseMilliseconds(
    container::StringView value, uint32_t fallback) noexcept {
    const std::optional<float> parsed = parseFloat(value);
    if (!parsed || *parsed < 0.0f) return fallback;
    if (*parsed >= static_cast<float>(
            std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(std::ceil(*parsed));
}

[[nodiscard]] bool parseBool(
    container::StringView value, bool fallback) noexcept {
    value = trimAsciiView(value);
    if (asciiEqualIgnoreCase(value, "YES") ||
        asciiEqualIgnoreCase(value, "TRUE") || value == "1") return true;
    if (asciiEqualIgnoreCase(value, "NO") ||
        asciiEqualIgnoreCase(value, "FALSE") || value == "0") return false;
    return fallback;
}

[[nodiscard]] math::q32_32 parsePercent(
    container::StringView value, math::q32_32 fallback) noexcept {
    value = trimAsciiView(value);
    if (!value.empty() && value.back() == '%') value.remove_suffix(1);
    const std::optional<float> parsed = parseFloat(value);
    return parsed ? math::q32_32{*parsed * 0.01f} : fallback;
}

[[nodiscard]] std::optional<ObjectStealthForbiddenCondition>
parseForbiddenCondition(container::StringView token) noexcept {
    struct Entry final {
        container::StringView name;
        ObjectStealthForbiddenCondition value;
    };
    constexpr Entry entries[] = {
        {"ATTACKING", ObjectStealthForbiddenCondition::Attacking},
        {"MOVING", ObjectStealthForbiddenCondition::Moving},
        {"USING_ABILITY", ObjectStealthForbiddenCondition::UsingAbility},
        {"FIRING_PRIMARY", ObjectStealthForbiddenCondition::FiringPrimary},
        {"FIRING_SECONDARY", ObjectStealthForbiddenCondition::FiringSecondary},
        {"FIRING_TERTIARY", ObjectStealthForbiddenCondition::FiringTertiary},
        {"NO_BLACK_MARKET", ObjectStealthForbiddenCondition::NoBlackMarket},
        {"TAKING_DAMAGE", ObjectStealthForbiddenCondition::TakingDamage},
        {"RIDERS_ATTACKING", ObjectStealthForbiddenCondition::RidersAttacking},
    };
    for (const Entry& entry : entries) {
        if (asciiEqualIgnoreCase(token, entry.name)) return entry.value;
    }
    return std::nullopt;
}

} // namespace

container::SharedPtr<const ObjectStealthPlan>
compileObjectStealthPlan(const ThingTemplate& templateData) {
    const ModuleData* selected = nullptr;
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module), "StealthUpdate")) {
            continue;
        }
        // getStealth() is a single interface. Final recipe validation owns
        // duplicate-interface diagnostics; retain the first authored host.
        if (!selected || module.authoredOrder < selected->authoredOrder) {
            selected = &module;
        }
    }
    if (!selected) return {};
    auto plan = std::make_shared<ObjectStealthPlan>();
    plan->authoredOrder = selected->authoredOrder;
    const auto duration = [&](container::StringView key,
                              uint32_t& destination) {
        if (const container::String* value = moduleValueLast(*selected, key)) {
            destination = parseMilliseconds(*value, destination);
        }
    };
    duration("StealthDelay", plan->stealthDelayMilliseconds);
    duration("BlackMarketCheckDelay", plan->blackMarketCheckMilliseconds);
    duration("PulseFrequency", plan->pulseMilliseconds);
    duration("DisguiseTransitionTime",
             plan->disguiseTransitionMilliseconds);
    duration("DisguiseRevealTransitionTime",
             plan->disguiseRevealTransitionMilliseconds);
    if (const container::String* value =
            moduleValueLast(*selected, "MoveThresholdSpeed")) {
        if (const std::optional<float> parsed = parseFloat(*value)) {
            plan->moveThresholdUnitsPerSecond = math::q32_32::max(
                math::q32_32{}, math::q32_32{*parsed});
        }
    }
    if (const container::String* value =
            moduleValueLast(*selected, "StealthForbiddenConditions")) {
        enum class ParseMode : uint8_t { Unset, Absolute, Delta };
        ParseMode mode = ParseMode::Unset;
        bool mixedSyntaxReported = false;
        for (container::StringView token : splitTokens(*value)) {
            bool add = true;
            bool signedToken = false;
            if (!token.empty() &&
                (token.front() == '+' || token.front() == '-')) {
                signedToken = true;
                add = token.front() == '+';
                token.remove_prefix(1);
            }
            if (asciiEqualIgnoreCase(token, "NONE")) {
                if (signedToken || mode == ParseMode::Delta) {
                    if (!mixedSyntaxReported) {
                        plan->diagnostics.push_back(
                            "StealthForbiddenConditions mixes absolute and +/- syntax");
                        mixedSyntaxReported = true;
                    }
                    continue;
                }
                mode = ParseMode::Absolute;
                plan->forbiddenConditions = 0;
                continue;
            }
            const ParseMode tokenMode = signedToken
                ? ParseMode::Delta : ParseMode::Absolute;
            if (mode != ParseMode::Unset && mode != tokenMode &&
                !mixedSyntaxReported) {
                plan->diagnostics.push_back(
                    "StealthForbiddenConditions mixes absolute and +/- syntax");
                mixedSyntaxReported = true;
            }
            mode = tokenMode;
            if (const std::optional<ObjectStealthForbiddenCondition> parsed =
                    parseForbiddenCondition(token)) {
                const ObjectStealthForbiddenMask bit =
                    objectStealthForbiddenBit(*parsed);
                if (add) {
                    plan->forbiddenConditions =
                        static_cast<ObjectStealthForbiddenMask>(
                            plan->forbiddenConditions | bit);
                } else {
                    plan->forbiddenConditions =
                        static_cast<ObjectStealthForbiddenMask>(
                            plan->forbiddenConditions & ~bit);
                }
            } else {
                plan->diagnostics.push_back(
                    "unknown StealthForbiddenConditions token '" +
                    container::String{token} + "'");
            }
        }
    }
    const auto statusMask = [&](container::StringView key,
                                ObjectStatusMask& destination) {
        if (const container::String* value = moduleValueLast(*selected, key)) {
            const ObjectStatusMaskParseResult parsed =
                parseObjectStatusMask(*value);
            destination = parsed.mask;
            if (!parsed.resolved) {
                plan->diagnostics.push_back(
                    container::String{key} + " contains an unknown status");
            }
        }
    };
    statusMask("HintDetectableConditions", plan->hintDetectableStatuses);
    statusMask("RequiredStatus", plan->requiredStatuses);
    statusMask("ForbiddenStatus", plan->forbiddenStatuses);
    if (const container::String* value =
            moduleValueLast(*selected, "FriendlyOpacityMin")) {
        plan->friendlyOpacityMinimum = math::q32_32::clamp(
            parsePercent(*value, plan->friendlyOpacityMinimum),
            math::q32_32{}, math::q32_32{int32_t{1}});
    }
    if (const container::String* value =
            moduleValueLast(*selected, "FriendlyOpacityMax")) {
        plan->friendlyOpacityMaximum = math::q32_32::clamp(
            parsePercent(*value, plan->friendlyOpacityMaximum),
            math::q32_32{}, math::q32_32{int32_t{1}});
    }
    if (const container::String* value =
            moduleValueLast(*selected, "RevealDistanceFromTarget")) {
        if (const std::optional<float> parsed = parseFloat(*value)) {
            plan->revealDistanceFromTarget = math::q32_32::max(
                math::q32_32{}, math::q32_32{*parsed});
        }
    }
    const auto boolean = [&](container::StringView key, bool& destination) {
        if (const container::String* value = moduleValueLast(*selected, key)) {
            destination = parseBool(*value, destination);
        }
    };
    boolean("DisguisesAsTeam", plan->disguisesAsTeam);
    boolean("OrderIdleEnemiesToAttackMeUponReveal",
            plan->orderIdleEnemiesToAttackOnReveal);
    boolean("InnateStealth", plan->innateStealth);
    boolean("UseRiderStealth", plan->useRiderStealth);
    boolean("GrantedBySpecialPower", plan->grantedBySpecialPower);
    const auto text = [&](container::StringView key,
                          container::String& destination) {
        if (const container::String* value = moduleValueLast(*selected, key)) {
            const container::StringView authored = trimAsciiView(*value);
            if (!authored.empty() && !asciiEqualIgnoreCase(authored, "NONE")) {
                destination.assign(authored);
            }
        }
    };
    text("DisguiseFX", plan->disguiseFx);
    text("DisguiseRevealFX", plan->disguiseRevealFx);
    text("EnemyDetectionEvaEvent", plan->enemyDetectionEva);
    text("OwnDetectionEvaEvent", plan->ownDetectionEva);
    return plan;
}

container::SharedPtr<const ObjectStealthDetectorPlan>
compileObjectStealthDetectorPlan(const ThingTemplate& templateData) {
    const ModuleData* selected = nullptr;
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(
                moduleClass(module), "StealthDetectorUpdate")) {
            continue;
        }
        if (!selected || module.authoredOrder < selected->authoredOrder) {
            selected = &module;
        }
    }
    if (!selected) return {};

    auto plan = std::make_shared<ObjectStealthDetectorPlan>();
    plan->authoredOrder = selected->authoredOrder;
    if (const container::String* value =
            moduleValueLast(*selected, "DetectionRate")) {
        plan->detectionRateMilliseconds = parseMilliseconds(
            *value, plan->detectionRateMilliseconds);
    }
    if (const container::String* value =
            moduleValueLast(*selected, "DetectionRange")) {
        if (const std::optional<float> parsed = parseFloat(*value)) {
            plan->detectionRange = math::q32_32::max(
                math::q32_32{}, math::q32_32{*parsed});
        }
    }
    const auto boolean = [&](container::StringView key, bool& destination) {
        if (const container::String* value = moduleValueLast(*selected, key)) {
            destination = parseBool(*value, destination);
        }
    };
    boolean("InitiallyDisabled", plan->initiallyDisabled);
    boolean("CanDetectWhileGarrisoned",
            plan->canDetectWhileGarrisoned);
    boolean("CanDetectWhileContained", plan->canDetectWhileContained);

    const auto text = [&](container::StringView key,
                          container::String& destination) {
        if (const container::String* value = moduleValueLast(*selected, key)) {
            const container::StringView authored = trimAsciiView(*value);
            if (!authored.empty() && !asciiEqualIgnoreCase(authored, "NONE")) {
                destination.assign(authored);
            }
        }
    };
    text("PingSound", plan->pingSound);
    text("LoudPingSound", plan->loudPingSound);
    text("IRBeaconParticleSysName", plan->beaconParticleSystem);
    text("IRParticleSysName", plan->scanParticleSystem);
    text("IRBrightParticleSysName", plan->brightScanParticleSystem);
    text("IRGridParticleSysName", plan->gridParticleSystem);
    text("IRParticleSysBone", plan->particleBone);

    const auto kinds = [&](container::StringView key,
                           ObjectKindOfMask& output) {
        const container::String* value = moduleValueLast(*selected, key);
        if (!value) return;
        if (!compileObjectKindOfMask(*value, output)) {
            plan->diagnostics.push_back(
                container::String{key} + " contains an unknown KindOf");
        }
    };
    kinds("ExtraRequiredKindOf", plan->extraRequiredKinds);
    kinds("ExtraForbiddenKindOf", plan->extraForbiddenKinds);
    return plan;
}

container::SharedPtr<const ObjectGrantStealthPlan>
compileObjectGrantStealthPlan(const ThingTemplate& templateData) {
    const ModuleData* selected = nullptr;
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(
                moduleClass(module), "GrantStealthBehavior")) {
            continue;
        }
        if (!selected || module.authoredOrder < selected->authoredOrder) {
            selected = &module;
        }
    }
    if (!selected) return {};

    auto plan = std::make_shared<ObjectGrantStealthPlan>();
    plan->authoredOrder = selected->authoredOrder;
    const auto radius = [&](container::StringView key,
                            math::q32_32& destination) {
        if (const container::String* value = moduleValueLast(*selected, key)) {
            if (const std::optional<float> parsed = parseFloat(*value)) {
                destination = math::q32_32::max(
                    math::q32_32{}, math::q32_32{*parsed});
            }
        }
    };
    radius("StartRadius", plan->startRadius);
    radius("FinalRadius", plan->finalRadius);
    radius("RadiusGrowRate", plan->radiusGrowPerFrame);
    if (plan->startRadius < plan->finalRadius &&
        plan->radiusGrowPerFrame <= math::q32_32{}) {
        plan->diagnostics.push_back(
            "RadiusGrowRate must be positive while FinalRadius exceeds StartRadius");
    }
    if (const container::String* value = moduleValueLast(*selected, "KindOf")) {
        plan->allKindsAllowed = false;
        for (container::StringView token : splitTokens(*value)) {
            bool add = true;
            if (!token.empty() &&
                (token.front() == '+' || token.front() == '-')) {
                add = token.front() == '+';
                token.remove_prefix(1);
            }
            if (asciiEqualIgnoreCase(token, "ALL")) {
                plan->allKindsAllowed = add;
                if (add) plan->allowedKinds.clear();
                continue;
            }
            if (asciiEqualIgnoreCase(token, "NONE")) {
                if (add) {
                    plan->allKindsAllowed = false;
                    plan->allowedKinds.clear();
                }
                continue;
            }
            if (token.empty()) continue;
            const std::optional<ObjectKindOf> kind = parseObjectKindOf(token);
            if (!kind) {
                plan->diagnostics.push_back(
                    "KindOf contains unknown token '" +
                    container::String{token} + "'");
                continue;
            }
            if (add) {
                setObjectKind(plan->allowedKinds, *kind);
            } else {
                setObjectKind(plan->forbiddenKinds, *kind);
            }
        }
    }
    if (const container::String* value =
            moduleValueLast(*selected, "RadiusParticleSystemName")) {
        const container::StringView authored = trimAsciiView(*value);
        if (!authored.empty() && !asciiEqualIgnoreCase(authored, "NONE")) {
            plan->radiusParticleSystem.assign(authored);
        }
    }
    return plan;
}

} // namespace game
