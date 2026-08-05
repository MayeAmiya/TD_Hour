#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/plan/combat/ObjectCombatInitializationPlanTypes.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace engine::object_combat_detail {

constexpr auto asciiEqualIgnoreCase = container::asciiEqualIgnoreCase;

[[nodiscard]] const container::String* moduleValue(const game::ModuleData& module,
                                               container::StringView key) noexcept {
    const auto found = std::find_if(module.values.begin(), module.values.end(),
        [key](const auto& entry) { return asciiEqualIgnoreCase(entry.first, key); });
    return found == module.values.end() ? nullptr : &found->second;
}

[[nodiscard]] bool parseBool(container::StringView value,
                             bool fallback = false) noexcept {
    if (asciiEqualIgnoreCase(value, "yes") || asciiEqualIgnoreCase(value, "true") || value == "1") {
        return true;
    }
    if (asciiEqualIgnoreCase(value, "no") || asciiEqualIgnoreCase(value, "false") || value == "0") {
        return false;
    }
    return fallback;
}

[[nodiscard]] float parseFiniteFloat(
    container::StringView value, float fallback = 0.0f) noexcept {
    return game::parseContentFloatOr(value, {
        .source = __FILE__, .block = "Object", .module = "TurretAI",
        .field = "Real", .fallback = fallback});
}

[[nodiscard]] uint8_t parseControlledWeaponSlots(
    container::StringView value) noexcept {
    uint8_t result = 0;
    while (!value.empty()) {
        const size_t begin = value.find_first_not_of(" \t\r\n");
        if (begin == container::StringView::npos) break;
        value.remove_prefix(begin);
        const size_t end = value.find_first_of(" \t\r\n");
        const container::StringView token = value.substr(0, end);
        if (asciiEqualIgnoreCase(token, "PRIMARY")) result |= 1u << 0u;
        else if (asciiEqualIgnoreCase(token, "SECONDARY")) result |= 1u << 1u;
        else if (asciiEqualIgnoreCase(token, "TERTIARY")) result |= 1u << 2u;
        if (end == container::StringView::npos) break;
        value.remove_prefix(end);
    }
    return result;
}

struct SlottedTurretReal final {
    size_t slot = 0;
    float value = 0.0f;
};

[[nodiscard]] std::optional<SlottedTurretReal> parseSlottedTurretReal(
    container::StringView source, float fallback) noexcept {
    source = container::trimAsciiView(source);
    const size_t split = source.find_first_of(" \t\r\n");
    if (split == container::StringView::npos) return std::nullopt;
    const container::StringView slotToken = source.substr(0, split);
    const container::StringView scalarText =
        container::trimAsciiView(source.substr(split));
    if (scalarText.empty()) return std::nullopt;

    size_t slot = 0;
    if (asciiEqualIgnoreCase(slotToken, "PRIMARY")) slot = 0;
    else if (asciiEqualIgnoreCase(slotToken, "SECONDARY")) slot = 1;
    else if (asciiEqualIgnoreCase(slotToken, "TERTIARY")) slot = 2;
    else return std::nullopt;
    return SlottedTurretReal{
        .slot = slot,
        .value = parseFiniteFloat(scalarText, fallback),
    };
}

[[nodiscard]] uint32_t parseDurationMilliseconds(
    container::StringView source, uint32_t fallback) noexcept {
    const float milliseconds = std::max(
        0.0f, parseFiniteFloat(source, static_cast<float>(fallback)));
    return static_cast<uint32_t>(std::min<float>(
        milliseconds, static_cast<float>(UINT32_MAX)));
}

} // namespace engine::object_combat_detail

namespace engine {

container::SharedPtr<const ObjectCombatInitializationPlan>
compileObjectCombatInitializationPlan(
    const game::ThingTemplate& templateData) {
    using namespace object_combat_detail;
    constexpr float kDegreesToRadians =
        3.14159265358979323846f / 180.0f;
    auto plan = std::make_shared<ObjectCombatInitializationPlan>();
    bool hasCompiledData = false;

    for (const game::ModuleData& module : templateData.modules) {
        if (asciiEqualIgnoreCase(
                module.moduleClass, "PointDefenseLaserUpdate")) {
            ObjectPointDefenseLaserRulePlan rule;
            if (const container::String* weapon =
                    moduleValue(module, "WeaponTemplate")) {
                rule.weaponTemplate = *weapon;
            }
            if (const container::String* primary =
                    moduleValue(module, "PrimaryTargetTypes")) {
                static_cast<void>(game::compileObjectKindOfMask(
                    *primary, rule.primaryTargetKindMask));
            }
            if (const container::String* secondary =
                    moduleValue(module, "SecondaryTargetTypes")) {
                static_cast<void>(game::compileObjectKindOfMask(
                    *secondary, rule.secondaryTargetKindMask));
            }
            if (const container::String* scan =
                    moduleValue(module, "ScanRate")) {
                rule.scanRateMilliseconds = static_cast<uint32_t>(std::clamp(
                    parseFiniteFloat(*scan), 0.0f,
                    static_cast<float>(std::numeric_limits<uint32_t>::max())));
            }
            if (const container::String* range =
                    moduleValue(module, "ScanRange")) {
                rule.scanRange = math::q32_32{
                    std::max(0.0f, parseFiniteFloat(*range))};
            }
            if (const container::String* prediction =
                    moduleValue(module, "PredictTargetVelocityFactor")) {
                rule.predictTargetVelocityFactor = math::q32_32{
                    parseFiniteFloat(*prediction)};
            }
            rule.authoredOrder = module.authoredOrder;
            if (!rule.weaponTemplate.empty() &&
                rule.scanRange > math::q32_32{}) {
                plan->pointDefenseRules.push_back(std::move(rule));
                hasCompiledData = true;
            }
        }

        if (!module.isAiModule) continue;
        if (const container::String* linked =
                moduleValue(module, "TurretsLinked")) {
            plan->turretsLinked = parseBool(*linked);
            hasCompiledData = true;
        }
        for (const game::ModuleData& child : module.children) {
            size_t index = plan->turrets.size();
            if (asciiEqualIgnoreCase(child.type, "Turret")) index = 0;
            else if (asciiEqualIgnoreCase(child.type, "AltTurret")) index = 1;
            if (index >= plan->turrets.size()) continue;
            hasCompiledData = true;
            ObjectTurretRecipe& turret = plan->turrets[index];
            if (const container::String* slots =
                    moduleValue(child, "ControlledWeaponSlots")) {
                turret.controlledWeaponSlots =
                    parseControlledWeaponSlots(*slots);
            }
            if (const container::String* rate =
                    moduleValue(child, "TurretTurnRate")) {
                turret.turnRateRadiansPerSecond = math::q32_32{
                    std::max(0.0f, parseFiniteFloat(*rate)) *
                    kDegreesToRadians};
            }
            if (const container::String* rate =
                    moduleValue(child, "TurretPitchRate")) {
                turret.pitchRateRadiansPerSecond = math::q32_32{
                    std::max(0.0f, parseFiniteFloat(*rate)) *
                    kDegreesToRadians};
            }
            if (const container::String* allows =
                    moduleValue(child, "AllowsPitch")) {
                turret.allowsPitch = parseBool(*allows);
            }
            if (const container::String* minimum =
                    moduleValue(child, "MinPhysicalPitch")) {
                turret.minimumPitchRadians = math::q32_32{
                    parseFiniteFloat(*minimum) *
                    kDegreesToRadians};
            }
            if (const container::String* natural =
                    moduleValue(child, "NaturalTurretAngle")) {
                turret.naturalYawRadians = math::q32_32{
                    parseFiniteFloat(*natural) * kDegreesToRadians};
            }
            if (const container::String* natural =
                    moduleValue(child, "NaturalTurretPitch")) {
                turret.naturalPitchRadians = math::q32_32{
                    parseFiniteFloat(*natural) * kDegreesToRadians};
            }
            if (const container::String* firePitch =
                    moduleValue(child, "FirePitch")) {
                turret.firePitchRadians = math::q32_32{
                    parseFiniteFloat(*firePitch) * kDegreesToRadians};
            }
            if (const container::String* groundPitch =
                    moduleValue(child, "GroundUnitPitch")) {
                turret.groundUnitPitchRadians = math::q32_32{
                    parseFiniteFloat(*groundPitch) * kDegreesToRadians};
            }
            for (const auto& [key, value] : child.values) {
                if (asciiEqualIgnoreCase(key, "TurretFireAngleSweep")) {
                    if (const auto parsed = parseSlottedTurretReal(value, 0.0f)) {
                        turret.fireAngleSweepRadians[parsed->slot] =
                            math::q32_32{std::max(0.0f, parsed->value) *
                                           kDegreesToRadians};
                    }
                } else if (asciiEqualIgnoreCase(
                               key, "TurretSweepSpeedModifier")) {
                    if (const auto parsed = parseSlottedTurretReal(value, 1.0f)) {
                        turret.sweepSpeedModifier[parsed->slot] =
                            math::q32_32{std::max(0.0f, parsed->value)};
                    }
                }
            }
            if (const container::String* minimum =
                    moduleValue(child, "MinIdleScanAngle")) {
                turret.minimumIdleScanAngleRadians = math::q32_32{
                    std::max(0.0f, parseFiniteFloat(*minimum)) *
                    kDegreesToRadians};
            }
            if (const container::String* maximum =
                    moduleValue(child, "MaxIdleScanAngle")) {
                turret.maximumIdleScanAngleRadians = math::q32_32{
                    std::max(0.0f, parseFiniteFloat(*maximum)) *
                    kDegreesToRadians};
            }
            if (const container::String* minimum =
                    moduleValue(child, "MinIdleScanInterval")) {
                turret.minimumIdleScanIntervalMilliseconds =
                    parseDurationMilliseconds(
                        *minimum,
                        turret.minimumIdleScanIntervalMilliseconds);
            }
            if (const container::String* maximum =
                    moduleValue(child, "MaxIdleScanInterval")) {
                turret.maximumIdleScanIntervalMilliseconds =
                    parseDurationMilliseconds(
                        *maximum,
                        turret.maximumIdleScanIntervalMilliseconds);
            }
            if (turret.maximumIdleScanAngleRadians <
                turret.minimumIdleScanAngleRadians) {
                std::swap(turret.minimumIdleScanAngleRadians,
                          turret.maximumIdleScanAngleRadians);
            }
            if (turret.maximumIdleScanIntervalMilliseconds <
                turret.minimumIdleScanIntervalMilliseconds) {
                std::swap(turret.minimumIdleScanIntervalMilliseconds,
                          turret.maximumIdleScanIntervalMilliseconds);
            }
            if (const container::String* recenter =
                    moduleValue(child, "RecenterTime")) {
                turret.recenterMilliseconds = parseDurationMilliseconds(
                    *recenter, turret.recenterMilliseconds);
            }
            if (const container::String* firesWhileTurning =
                    moduleValue(child, "FiresWhileTurning")) {
                turret.firesWhileTurning = parseBool(*firesWhileTurning);
            }
        }
    }
    return hasCompiledData
        ? container::SharedPtr<const ObjectCombatInitializationPlan>{
              std::move(plan)}
        : nullptr;
}

} // namespace engine
