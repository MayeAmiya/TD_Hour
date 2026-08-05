#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/plan/movement/ObjectPhysicsPlanTypes.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace game {
namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] const container::String* valueOf(
    const ModuleData& module, container::StringView key) noexcept {
    const auto found = std::find_if(
        module.values.rbegin(), module.values.rend(),
        [key](const auto& entry) {
            return equalInsensitive(entry.first, key);
        });
    return found == module.values.rend() ? nullptr : &found->second;
}

[[nodiscard]] float finiteFloat(container::StringView text,
                                float fallback) noexcept {
    return parseContentFloatOr(text, {
        .source = __FILE__, .block = "Object", .module = "PhysicsBehavior",
        .field = "Real", .fallback = fallback});
}

[[nodiscard]] bool boolean(container::StringView text,
                           bool fallback) noexcept {
    if (equalInsensitive(text, "yes") || equalInsensitive(text, "true") ||
        text == "1") return true;
    if (equalInsensitive(text, "no") || equalInsensitive(text, "false") ||
        text == "0") return false;
    return fallback;
}

[[nodiscard]] math::q32_32 fixed(float value, float fallback = 0.0f) noexcept {
    if (!std::isfinite(value)) value = fallback;
    constexpr double minimum =
        static_cast<double>(std::numeric_limits<int32_t>::min());
    constexpr double maximum =
        static_cast<double>(std::numeric_limits<int32_t>::max());
    return math::q32_32{std::clamp(static_cast<double>(value),
                                  minimum, maximum)};
}

} // namespace

container::SharedPtr<const ObjectPhysicsPlan>
compileObjectPhysicsPlan(const ThingTemplate& templateData) {
    const auto found = std::find_if(
        templateData.modules.begin(), templateData.modules.end(),
        [](const ModuleData& module) {
            return equalInsensitive(module.moduleClass, "PhysicsBehavior");
        });
    if (found == templateData.modules.end()) return {};

    const ModuleData& module = *found;
    const auto real = [&module](container::StringView key,
                                float fallback) noexcept {
        const container::String* value = valueOf(module, key);
        return value ? finiteFloat(*value, fallback) : fallback;
    };
    const auto nonNegative = [&real](container::StringView key,
                                     float fallback) noexcept {
        return std::max(0.0f, real(key, fallback));
    };
    const auto flag = [&module](container::StringView key,
                                bool fallback) noexcept {
        const container::String* value = valueOf(module, key);
        return value ? boolean(*value, fallback) : fallback;
    };

    auto plan = std::make_shared<ObjectPhysicsPlan>();
    plan->mass = fixed(std::max(0.0001f, real("Mass", 1.0f)), 1.0f);
    plan->shockResistance = fixed(nonNegative("ShockResistance", 0.0f));
    plan->shockMaxYaw = fixed(nonNegative("ShockMaxYaw", 0.05f));
    plan->shockMaxPitch = fixed(nonNegative("ShockMaxPitch", 0.025f));
    plan->shockMaxRoll = fixed(nonNegative("ShockMaxRoll", 0.025f));

    // Constructor defaults are legacy per-frame coefficients; explicit INI
    // values are friction-per-second. The plan stores one canonical unit.
    constexpr float kLegacyFramesPerSecond = 30.0f;
    plan->forwardFrictionPerSecond = fixed(nonNegative(
        "ForwardFriction", 0.15f * kLegacyFramesPerSecond));
    plan->lateralFrictionPerSecond = fixed(nonNegative(
        "LateralFriction", 0.15f * kLegacyFramesPerSecond));
    plan->zFrictionPerSecond = fixed(nonNegative(
        "ZFriction", 0.8f * kLegacyFramesPerSecond));
    plan->aerodynamicFrictionPerSecond = fixed(nonNegative(
        "AerodynamicFriction", 0.0f));
    plan->centerOfMassOffset = fixed(real("CenterOfMassOffset", 0.0f));
    plan->minimumFallHeight = fixed(nonNegative(
        "MinFallHeightForDamage", 40.0f));
    plan->fallHeightDamageFactor = fixed(
        real("FallHeightDamageFactor", 1.0f), 1.0f);
    plan->pitchRollYawFactor = fixed(
        real("PitchRollYawFactor", 2.0f), 2.0f);
    plan->allowBouncing = flag("AllowBouncing", false);
    plan->allowCollideForce = flag("AllowCollideForce", true);
    plan->killWhenRestingOnGround = flag(
        "KillWhenRestingOnGround", false);
    if (const container::String* value = valueOf(
            module, "VehicleCrashesIntoBuildingWeaponTemplate")) {
        plan->crashIntoBuildingWeapon = *value;
    }
    if (const container::String* value = valueOf(
            module, "VehicleCrashesIntoNonBuildingWeaponTemplate")) {
        plan->crashIntoNonBuildingWeapon = *value;
    }
    return plan;
}

} // namespace game
