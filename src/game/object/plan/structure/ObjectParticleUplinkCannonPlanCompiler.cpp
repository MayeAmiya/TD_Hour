#include "game/object/plan/structure/ObjectParticleUplinkCannonPlanTypes.h"
#include "core/container/string_utils.h"

#include "presentation/fx/runtime/LegacyBeamTemplate.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>

namespace game {
namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView trimView(
    container::StringView value) noexcept {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] container::String trim(container::StringView value) {
    return container::String{trimView(value)};
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto iterator = module.values.rbegin();
         iterator != module.values.rend(); ++iterator) {
        if (equalInsensitive(iterator->first, key)) return &iterator->second;
    }
    return nullptr;
}

[[nodiscard]] std::optional<uint32_t> parseUnsigned(
    container::StringView value) noexcept {
    value = trimView(value);
    uint64_t parsed = 0;
    const auto [cursor, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (value.empty() || error != std::errc{} ||
        cursor != value.data() + value.size() || parsed > UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(parsed);
}

[[nodiscard]] std::optional<math::q32_32> parseNonNegativeFixed(
    container::StringView value) noexcept {
    value = trimView(value);
    double parsed = 0.0;
    const auto [cursor, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (value.empty() || error != std::errc{} ||
        cursor != value.data() + value.size() || !std::isfinite(parsed) ||
        parsed < 0.0 ||
        parsed >= static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return std::nullopt;
    }
    return math::q32_32{parsed};
}

[[nodiscard]] std::optional<math::q32_32> parseFiniteFixed(
    container::StringView value) noexcept {
    value = trimView(value);
    double parsed = 0.0;
    const auto [cursor, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (value.empty() || error != std::errc{} ||
        cursor != value.data() + value.size() || !std::isfinite(parsed) ||
        parsed <= static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        parsed >= static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return std::nullopt;
    }
    return math::q32_32{parsed};
}

[[nodiscard]] bool knownDeathType(container::StringView value) noexcept {
    constexpr container::StringView names[] = {
        "NORMAL", "NONE", "CRUSHED", "BURNED", "EXPLODED", "POISONED",
        "TOPPLED", "FLOODED", "SUICIDED", "LASERED", "DETONATED",
        "SPLATTED", "POISONED_BETA", "EXTRA_2", "EXTRA_3", "EXTRA_4",
        "EXTRA_5", "EXTRA_6", "EXTRA_7", "EXTRA_8", "POISONED_GAMMA",
    };
    return std::any_of(std::begin(names), std::end(names),
        [value](container::StringView name) {
            return equalDamageTypeToken(trimView(value), name);
        });
}

} // namespace

container::SharedPtr<const ObjectParticleUplinkCannonPlan>
compileObjectParticleUplinkCannonPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectParticleUplinkCannonPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(module.moduleClass,
                              "ParticleUplinkCannonUpdate")) {
            continue;
        }

        ObjectParticleUplinkCannonRule rule;
        rule.authoredOrder = module.authoredOrder;
        const container::String tag = !module.moduleTag.empty()
            ? module.moduleTag : module.tag;
        const auto name = [&](container::StringView key,
                              container::String& destination) {
            if (const container::String* value = moduleValueLast(module, key))
                destination = trim(*value);
        };
        const auto duration = [&](container::StringView key,
                                  uint32_t& destination) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            if (const std::optional<uint32_t> parsed = parseUnsigned(*value))
                destination = *parsed;
            else
                plan->diagnostics.push_back(
                    tag + ": " + container::String{key} +
                    " must be unsigned milliseconds");
        };
        const auto count = [&](container::StringView key,
                               uint32_t& destination) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            if (const std::optional<uint32_t> parsed = parseUnsigned(*value))
                destination = *parsed;
            else
                plan->diagnostics.push_back(
                    tag + ": " + container::String{key} +
                    " must be an unsigned count");
        };
        const auto fixed = [&](container::StringView key,
                               math::q32_32& destination) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            if (const std::optional<math::q32_32> parsed =
                    parseNonNegativeFixed(*value)) {
                destination = *parsed;
            } else {
                plan->diagnostics.push_back(
                    tag + ": " + container::String{key} +
                    " must be a finite non-negative scalar");
            }
        };
        const auto signedFixed = [&](container::StringView key,
                                     math::q32_32& destination) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            if (const std::optional<math::q32_32> parsed =
                    parseFiniteFixed(*value)) {
                destination = *parsed;
            } else {
                plan->diagnostics.push_back(
                    tag + ": " + container::String{key} +
                    " must be a finite Q32.32 scalar");
            }
        };

        name("SpecialPowerTemplate", rule.specialPowerTemplate);
        duration("BeginChargeTime", rule.beginChargeMilliseconds);
        duration("RaiseAntennaTime", rule.raiseAntennaMilliseconds);
        duration("ReadyDelayTime", rule.readyDelayMilliseconds);
        duration("WidthGrowTime", rule.widthGrowMilliseconds);
        duration("BeamTravelTime", rule.beamTravelMilliseconds);
        duration("TotalFiringTime", rule.totalFiringMilliseconds);
        fixed("RevealRange", rule.revealRange);

        name("OuterEffectBoneName", rule.outerEffectBoneName);
        count("OuterEffectNumBones", rule.outerEffectNumBones);
        name("OuterNodesLightFlareParticleSystem",
             rule.outerNodesLightFlareParticleSystem);
        name("OuterNodesMediumFlareParticleSystem",
             rule.outerNodesMediumFlareParticleSystem);
        name("OuterNodesIntenseFlareParticleSystem",
             rule.outerNodesIntenseFlareParticleSystem);
        name("ConnectorBoneName", rule.connectorBoneName);
        name("ConnectorMediumLaserName", rule.connectorMediumLaserName);
        name("ConnectorIntenseLaserName", rule.connectorIntenseLaserName);
        name("ConnectorMediumFlare", rule.connectorMediumFlare);
        name("ConnectorIntenseFlare", rule.connectorIntenseFlare);
        name("FireBoneName", rule.fireBoneName);
        name("LaserBaseLightFlareParticleSystemName",
             rule.laserBaseLightFlareParticleSystemName);
        name("LaserBaseMediumFlareParticleSystemName",
             rule.laserBaseMediumFlareParticleSystemName);
        name("LaserBaseIntenseFlareParticleSystemName",
             rule.laserBaseIntenseFlareParticleSystemName);
        name("ParticleBeamLaserName", rule.particleBeamLaserName);

        signedFixed("SwathOfDeathDistance", rule.swathOfDeathDistance);
        signedFixed("SwathOfDeathAmplitude", rule.swathOfDeathAmplitude);
        count("TotalScorchMarks", rule.totalScorchMarks);
        fixed("ScorchMarkScalar", rule.scorchMarkScalar);
        name("BeamLaunchFX", rule.beamLaunchFx);
        duration("DelayBetweenLaunchFX",
                 rule.delayBetweenLaunchFxMilliseconds);
        name("GroundHitFX", rule.groundHitFx);

        fixed("DamagePerSecond", rule.damagePerSecond);
        count("TotalDamagePulses", rule.totalDamagePulses);
        fixed("DamageRadiusScalar", rule.damageRadiusScalar);
        if (const container::String* value =
                moduleValueLast(module, "DamageType")) {
            if (const std::optional<DamageType> parsed =
                    tryParseDamageType(trimView(*value))) {
                rule.damageType = *parsed;
            } else {
                plan->diagnostics.push_back(
                    tag + ": unknown DamageType '" + trim(*value) + "'");
            }
        }
        if (const container::String* value =
                moduleValueLast(module, "DeathType")) {
            if (knownDeathType(*value))
                rule.deathType = parseDeathType(trimView(*value));
            else
                plan->diagnostics.push_back(
                    tag + ": unknown DeathType '" + trim(*value) + "'");
        }

        name("PoweringUpSoundLoop", rule.poweringUpSoundLoop);
        name("UnpackToIdleSoundLoop", rule.unpackToIdleSoundLoop);
        name("FiringToPackSoundLoop", rule.firingToPackSoundLoop);
        name("GroundAnnihilationSoundLoop",
             rule.groundAnnihilationSoundLoop);
        name("DamagePulseRemnantObjectName",
             rule.damagePulseRemnantObjectName);
        fixed("ManualDrivingSpeed", rule.manualDrivingSpeed);
        fixed("ManualFastDrivingSpeed", rule.manualFastDrivingSpeed);
        duration("DoubleClickToFastDriveDelay",
                 rule.doubleClickToFastDriveDelayMilliseconds);

        if (rule.specialPowerTemplate.empty())
            plan->diagnostics.push_back(
                tag + ": SpecialPowerTemplate is required");
        if (rule.particleBeamLaserName.empty())
            plan->diagnostics.push_back(
                tag + ": ParticleBeamLaserName is required");
        if (rule.totalDamagePulses == 0 &&
            rule.damagePerSecond > math::q32_32{}) {
            plan->diagnostics.push_back(
                tag + ": TotalDamagePulses=0 disables authored damage");
        }
        if (rule.totalScorchMarks == 0 &&
            (!rule.groundHitFx.empty() || rule.revealRange > math::q32_32{})) {
            plan->diagnostics.push_back(
                tag + ": TotalScorchMarks=0 disables scorch/ground-hit/reveal");
        }
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty()) return nullptr;
    std::stable_sort(
        plan->rules.begin(), plan->rules.end(),
        [](const ObjectParticleUplinkCannonRule& left,
           const ObjectParticleUplinkCannonRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan;
}

} // namespace game
