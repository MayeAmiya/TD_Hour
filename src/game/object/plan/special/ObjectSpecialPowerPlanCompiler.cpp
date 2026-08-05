#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/plan/special/ObjectSpecialPowerPlanTypes.h"

#include "game/command/CommandButtonStore.h"
#include "game/base/SimulationRandom.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/contracts/ObjectRelationshipPolicy.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>

namespace game {
namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return module.moduleClass;
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto it = module.values.rbegin(); it != module.values.rend(); ++it) {
        if (equalInsensitive(it->first, key)) return &it->second;
    }
    return nullptr;
}

[[nodiscard]] container::String trim(container::StringView value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return container::String{value};
}

[[nodiscard]] std::optional<uint32_t> parseUnsigned(
    container::StringView value) noexcept {
    const container::String cleaned = trim(value);
    if (cleaned.empty()) return std::nullopt;
    uint32_t parsed = 0;
    const char* begin = cleaned.data();
    const char* end = begin + cleaned.size();
    const auto [cursor, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || cursor != end) return std::nullopt;
    return parsed;
}

[[nodiscard]] std::optional<math::q32_32> parsePercentFixed(
    container::StringView value) noexcept {
    container::String cleaned = trim(value);
    bool authoredAsPercent = false;
    if (!cleaned.empty() && cleaned.back() == '%') {
        authoredAsPercent = true;
        cleaned.pop_back();
        cleaned = trim(cleaned);
    }
    if (cleaned.empty()) return std::nullopt;

    double parsed = 0.0;
    const char* begin = cleaned.data();
    const char* end = begin + cleaned.size();
    const auto [cursor, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || cursor != end || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    const double normalized = authoredAsPercent ? parsed / 100.0 : parsed;
    constexpr double kMaximumExclusive =
        static_cast<double>(std::numeric_limits<int32_t>::max()) + 1.0;
    if (!std::isfinite(normalized) || normalized < 0.0 ||
        normalized >= kMaximumExclusive) {
        return std::nullopt;
    }
    return math::q32_32{normalized};
}

[[nodiscard]] std::optional<math::q32_32> parseNonNegativeFixed(
    container::StringView value) noexcept {
    const container::String cleaned = trim(value);
    if (cleaned.empty()) return std::nullopt;
    double parsed = 0.0;
    const char* begin = cleaned.data();
    const char* end = begin + cleaned.size();
    const auto [cursor, error] = std::from_chars(begin, end, parsed);
    constexpr double kMaximumExclusive =
        static_cast<double>(std::numeric_limits<int32_t>::max()) + 1.0;
    if (error != std::errc{} || cursor != end || !std::isfinite(parsed) ||
        parsed < 0.0 || parsed >= kMaximumExclusive) {
        return std::nullopt;
    }
    return math::q32_32{parsed};
}

[[nodiscard]] std::optional<bool> parseBoolean(
    container::StringView value) noexcept {
    const container::String cleaned = trim(value);
    if (equalInsensitive(cleaned, "Yes") || equalInsensitive(cleaned, "True") ||
        equalInsensitive(cleaned, "On") || cleaned == "1") return true;
    if (equalInsensitive(cleaned, "No") || equalInsensitive(cleaned, "False") ||
        equalInsensitive(cleaned, "Off") || cleaned == "0") return false;
    return std::nullopt;
}

[[nodiscard]] container::Vector<container::StringView> splitWhitespace(
    container::StringView value) {
    container::Vector<container::StringView> result;
    while (!value.empty()) {
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.front()))) {
            value.remove_prefix(1);
        }
        if (value.empty()) break;
        size_t length = 0;
        while (length < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[length]))) {
            ++length;
        }
        result.push_back(value.substr(0, length));
        value.remove_prefix(length);
    }
    return result;
}

[[nodiscard]] std::optional<ObjectSpecialPowerCreateLocation>
parseCreateLocation(container::StringView value) noexcept {
    if (equalInsensitive(value, "CREATE_AT_EDGE_NEAR_SOURCE")) {
        return ObjectSpecialPowerCreateLocation::EdgeNearSource;
    }
    if (equalInsensitive(value, "CREATE_AT_EDGE_NEAR_TARGET")) {
        return ObjectSpecialPowerCreateLocation::EdgeNearTarget;
    }
    if (equalInsensitive(value, "CREATE_AT_LOCATION")) {
        return ObjectSpecialPowerCreateLocation::AtLocation;
    }
    if (equalInsensitive(value, "USE_OWNER_OBJECT")) {
        return ObjectSpecialPowerCreateLocation::UseOwnerObject;
    }
    if (equalInsensitive(value, "CREATE_ABOVE_LOCATION")) {
        return ObjectSpecialPowerCreateLocation::AboveLocation;
    }
    if (equalInsensitive(value, "CREATE_AT_EDGE_FARTHEST_FROM_TARGET")) {
        return ObjectSpecialPowerCreateLocation::EdgeFarthestFromTarget;
    }
    return std::nullopt;
}

[[nodiscard]] bool isSpecialPowerModule(container::StringView value) noexcept {
    constexpr container::StringView names[] = {
        "BaikonurLaunchPower",
        "CashHackSpecialPower",
        "DefectorSpecialPower",
        "DemoralizeSpecialPower",
        "OCLSpecialPower",
        "FireWeaponPower",
        "SpecialAbility",
        "SpyVisionSpecialPower",
        "CashBountyPower",
        "CleanupAreaPower",
    };
    return std::any_of(std::begin(names), std::end(names),
        [value](container::StringView name) {
            return equalInsensitive(value, name);
        });
}

[[nodiscard]] bool hasParticleUplinkForPower(
    const ThingTemplate& templateData,
    container::StringView specialPowerTemplate) noexcept {
    if (specialPowerTemplate.empty()) return false;
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module),
                              "ParticleUplinkCannonUpdate")) {
            continue;
        }
        const container::String* value =
            moduleValueLast(module, "SpecialPowerTemplate");
        if (value && equalInsensitive(trim(*value), specialPowerTemplate))
            return true;
    }
    return false;
}

} // namespace

container::SharedPtr<const ObjectSpecialPowerPlan>
compileObjectSpecialPowerPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectSpecialPowerPlan>();
    for (const ModuleData& module : templateData.modules) {
        const container::StringView className = moduleClass(module);
        if (equalInsensitive(className, "SpecialPowerCreate")) {
            plan->hasSpecialPowerCreate = true;
            continue;
        }
        if (!isSpecialPowerModule(className)) continue;

        ObjectSpecialPowerRule rule;
        rule.authoredOrder = module.authoredOrder;
        rule.moduleClass = container::String{className};
        rule.moduleTag = !module.moduleTag.empty() ? module.moduleTag : module.tag;
        if (equalInsensitive(className, "SpyVisionSpecialPower")) {
            rule.kind = ObjectSpecialPowerKind::SpyVision;
        } else if (equalInsensitive(className, "OCLSpecialPower")) {
            rule.kind = ObjectSpecialPowerKind::ObjectCreationList;
        } else if (equalInsensitive(className, "FireWeaponPower")) {
            rule.kind = ObjectSpecialPowerKind::FireWeapon;
        } else if (equalInsensitive(className, "SpecialAbility")) {
            rule.kind = ObjectSpecialPowerKind::SpecialAbility;
        } else if (equalInsensitive(className, "CashHackSpecialPower")) {
            rule.kind = ObjectSpecialPowerKind::CashHack;
        } else if (equalInsensitive(className, "CashBountyPower")) {
            rule.kind = ObjectSpecialPowerKind::CashBounty;
        } else if (equalInsensitive(className, "DefectorSpecialPower")) {
            rule.kind = ObjectSpecialPowerKind::Defector;
        } else if (equalInsensitive(className, "CleanupAreaPower")) {
            rule.kind = ObjectSpecialPowerKind::CleanupArea;
        } else if (equalInsensitive(className, "BaikonurLaunchPower")) {
            rule.kind = ObjectSpecialPowerKind::BaikonurLaunch;
        }

        if (const container::String* value =
                moduleValueLast(module, "SpecialPowerTemplate")) {
            rule.specialPowerTemplate = trim(*value);
        }
        if (equalInsensitive(className, "SpecialAbility") &&
            hasParticleUplinkForPower(
                templateData, rule.specialPowerTemplate)) {
            rule.kind = ObjectSpecialPowerKind::ParticleUplink;
        }
        if (rule.specialPowerTemplate.empty()) {
            plan->diagnostics.push_back(
                rule.moduleTag + ": SpecialPowerTemplate is required");
        }
        if (const container::String* value =
                moduleValueLast(module, "InitiateSound")) {
            rule.initiateSound = trim(*value);
        }

        const auto duration = [&](container::StringView key,
                                  uint32_t& destination) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            const std::optional<uint32_t> parsed = parseUnsigned(*value);
            if (parsed) {
                destination = *parsed;
            } else {
                plan->diagnostics.push_back(
                    rule.moduleTag + ": " + container::String{key} +
                    " must be unsigned milliseconds");
            }
        };
        if (rule.kind == ObjectSpecialPowerKind::BaikonurLaunch) {
            if (const container::String* value =
                    moduleValueLast(module, "DetonationObject")) {
                rule.detonationObject = trim(*value);
            }
            if (rule.detonationObject.empty()) {
                plan->diagnostics.push_back(
                    rule.moduleTag + ": DetonationObject is required");
            }
        } else if (rule.kind == ObjectSpecialPowerKind::SpyVision) {
            duration("BaseDuration", rule.baseDurationMilliseconds);
            duration("BonusDurationPerCaptured",
                     rule.bonusDurationPerCapturedMilliseconds);
            duration("MaxDuration", rule.maximumDurationMilliseconds);
        } else if (rule.kind ==
                       ObjectSpecialPowerKind::ObjectCreationList) {
            if (const container::String* value = moduleValueLast(module, "OCL")) {
                rule.objectCreationList = trim(*value);
            }
            if (const container::String* value =
                    moduleValueLast(module, "ReferenceObject")) {
                rule.referenceObject = trim(*value);
            }
            if (const container::String* value =
                    moduleValueLast(module, "CreateLocation")) {
                const container::String cleaned = trim(*value);
                if (const auto parsed = parseCreateLocation(cleaned)) {
                    rule.createLocation = *parsed;
                } else {
                    plan->diagnostics.push_back(
                        rule.moduleTag +
                        ": CreateLocation is not a supported OCL location");
                }
            }
            if (const container::String* value = moduleValueLast(
                    module, "OCLAdjustPositionToPassable")) {
                if (const std::optional<bool> parsed = parseBoolean(*value)) {
                    rule.adjustPositionToPassable = *parsed;
                } else {
                    plan->diagnostics.push_back(
                        rule.moduleTag +
                        ": OCLAdjustPositionToPassable must be Yes or No");
                }
            }
            for (const auto& [key, value] : module.values) {
                if (!equalInsensitive(key, "UpgradeOCL")) continue;
                const auto tokens = splitWhitespace(value);
                if (tokens.size() != 2) {
                    plan->diagnostics.push_back(
                        rule.moduleTag +
                        ": UpgradeOCL requires Science and OCL names");
                    continue;
                }
                rule.upgradeObjectCreationLists.push_back({
                    .science = container::String{tokens[0]},
                    .objectCreationList = container::String{tokens[1]},
                });
            }
            // OCLSpecialPower has no effect at all without a list to run.  The
            // runtime consumer still reports the activation as successful (and
            // therefore consumes recharge), so a dropped/absent OCL is
            // otherwise completely silent at every layer.
            if (rule.objectCreationList.empty() &&
                rule.upgradeObjectCreationLists.empty()) {
                plan->diagnostics.push_back(
                    rule.moduleTag +
                    ": OCL is required for OCLSpecialPower");
            }
        } else if (rule.kind == ObjectSpecialPowerKind::FireWeapon) {
            if (const container::String* value =
                    moduleValueLast(module, "MaxShotsToFire")) {
                if (const std::optional<uint32_t> parsed =
                        parseUnsigned(*value)) {
                    rule.maximumShotsToFire = *parsed;
                } else {
                    plan->diagnostics.push_back(
                        rule.moduleTag +
                        ": MaxShotsToFire must be unsigned");
                }
            }
        } else if (rule.kind == ObjectSpecialPowerKind::CashHack) {
            if (const container::String* value =
                    moduleValueLast(module, "MoneyAmount")) {
                if (const std::optional<uint32_t> parsed =
                        parseUnsigned(*value)) {
                    rule.moneyAmount = *parsed;
                } else {
                    plan->diagnostics.push_back(
                        rule.moduleTag + ": MoneyAmount must be unsigned");
                }
            }
            for (const auto& [key, value] : module.values) {
                if (!equalInsensitive(key, "UpgradeMoneyAmount")) continue;
                const auto tokens = splitWhitespace(value);
                if (tokens.size() != 2) {
                    plan->diagnostics.push_back(
                        rule.moduleTag +
                        ": UpgradeMoneyAmount requires Science and amount");
                    continue;
                }
                const std::optional<uint32_t> amount =
                    parseUnsigned(tokens[1]);
                if (!amount) {
                    plan->diagnostics.push_back(
                        rule.moduleTag +
                        ": UpgradeMoneyAmount amount must be unsigned");
                    continue;
                }
                rule.upgradeMoneyAmounts.push_back({
                    .science = container::String{tokens[0]},
                    .amount = *amount,
                });
            }
        } else if (rule.kind == ObjectSpecialPowerKind::CashBounty) {
            if (const container::String* value =
                    moduleValueLast(module, "Bounty")) {
                if (const std::optional<math::q32_32> parsed =
                        parsePercentFixed(*value)) {
                    rule.bountyPercent = *parsed;
                } else {
                    plan->diagnostics.push_back(
                        rule.moduleTag +
                        ": Bounty must be a non-negative finite percent");
                }
            }
        } else if (rule.kind == ObjectSpecialPowerKind::Defector) {
            if (const container::String* value =
                    moduleValueLast(module, "FatCursorRadius")) {
                if (const std::optional<math::q32_32> parsed =
                        parseNonNegativeFixed(*value)) {
                    rule.fatCursorRadius = *parsed;
                } else {
                    plan->diagnostics.push_back(
                        rule.moduleTag +
                        ": FatCursorRadius must be non-negative and finite");
                }
            }
        } else if (rule.kind == ObjectSpecialPowerKind::CleanupArea) {
            if (const container::String* value =
                    moduleValueLast(module,
                                    "MaxMoveDistanceFromLocation")) {
                if (const std::optional<math::q32_32> parsed =
                        parseNonNegativeFixed(*value)) {
                    rule.maxMoveDistanceFromLocation = *parsed;
                } else {
                    plan->diagnostics.push_back(
                        rule.moduleTag +
                        ": MaxMoveDistanceFromLocation must be non-negative and finite");
                }
            }
        }
        const auto commonBoolean = [&](container::StringView key,
                                       bool& destination) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            if (const std::optional<bool> parsed = parseBoolean(*value)) {
                destination = *parsed;
            } else {
                plan->diagnostics.push_back(
                    rule.moduleTag + ": " + container::String{key} +
                    " must be Yes or No");
            }
        };
        commonBoolean("ScriptedSpecialPowerOnly", rule.scriptedOnly);
        commonBoolean("UpdateModuleStartsAttack",
                      rule.updateModuleStartsAttack);
        if (const container::String* value =
                moduleValueLast(module, "StartsPaused")) {
            if (const std::optional<bool> parsed = parseBoolean(*value)) {
                rule.startsPaused = *parsed;
            } else {
                plan->diagnostics.push_back(
                    rule.moduleTag + ": StartsPaused must be Yes or No");
            }
        }
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty() && !plan->hasSpecialPowerCreate) return nullptr;
    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectSpecialPowerRule& left,
           const ObjectSpecialPowerRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan;
}

} // namespace game
