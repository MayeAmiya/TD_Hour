#include "game/object/plan/combat/ObjectProjectilePlanTypes.h"
#include "game/object/plan/combat/ObjectProjectilePlanParsing.h"
#include "game/object/definition/ThingModuleRecipe.h"

#include "core/container/string_utils.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>

namespace engine {

using namespace game::object_projectile_plan_detail;
using container::asciiEqualIgnoreCase;

namespace {

[[nodiscard]] ObjectProjectileBehaviorKind classifyProjectileTemplate(
    const game::ThingTemplate& templateData) noexcept {
    if (hasModule(templateData, "DumbProjectileBehavior")) {
        return ObjectProjectileBehaviorKind::DumbBezier;
    }
    if (hasModule(templateData, "MissileAIUpdate")) {
        return ObjectProjectileBehaviorKind::MissileAI;
    }
    if (hasModule(templateData, "NeutronMissileUpdate")) {
        return ObjectProjectileBehaviorKind::NeutronMissile;
    }
    if (hasModule(templateData, "EMPUpdate") ||
        hasModule(templateData, "NeutronBlastBehavior")) {
        return ObjectProjectileBehaviorKind::PlacedHelper;
    }
    return ObjectProjectileBehaviorKind::Unsupported;
}

} // namespace

container::SharedPtr<const ObjectProjectilePlan>
compileObjectProjectilePlan(const game::ThingTemplate& templateData) {
    const ObjectProjectileBehaviorKind kind =
        classifyProjectileTemplate(templateData);
    if (kind == ObjectProjectileBehaviorKind::Unsupported) return nullptr;

    auto plan = std::make_shared<ObjectProjectilePlan>();
    plan->behaviorKind = kind;
    const game::ModuleData* dumb = findModule(
        templateData, "DumbProjectileBehavior");
    const game::ModuleData* missile = findModule(
        templateData, "MissileAIUpdate");
    const game::ModuleData* neutron = findModule(
        templateData, "NeutronMissileUpdate");
    const game::ModuleData* flight = dumb ? dumb : missile ? missile : neutron;
    if (!flight) return plan;

    if (const container::String* value = moduleValue(
            *flight, "GarrisonHitKillRequiredKindOf")) {
        static_cast<void>(game::compileObjectKindOfMask(
            *value, plan->garrisonHitRequiredKindMask));
    }
    if (const container::String* value = moduleValue(
            *flight, "GarrisonHitKillForbiddenKindOf")) {
        static_cast<void>(game::compileObjectKindOfMask(
            *value, plan->garrisonHitForbiddenKindMask));
    }
    if (const container::String* value = moduleValue(
            *flight, "GarrisonHitKillCount")) {
        plan->garrisonHitKillCount = parseUnsigned(*value, 0);
    }
    if (const container::String* value = moduleValue(
            *flight, "GarrisonHitKillFX");
        value && !asciiEqualIgnoreCase(*value, "NONE")) {
        plan->garrisonHitFx = *value;
    }
    if (const container::String* value = moduleValue(
            *flight, "DetonateCallsKill")) {
        plan->detonateCallsKill = parseBool(*value, false);
    }

    if (dumb) {
        plan->firstHeight = fixedFinite(moduleValue(*dumb, "FirstHeight")
            ? parseFiniteFloat(*moduleValue(*dumb, "FirstHeight"), 0.0f)
            : 0.0f);
        plan->secondHeight = fixedFinite(moduleValue(*dumb, "SecondHeight")
            ? parseFiniteFloat(*moduleValue(*dumb, "SecondHeight"), 0.0f)
            : 0.0f);
        plan->firstPercentIndent = fixedFinite(
            moduleValue(*dumb, "FirstPercentIndent")
                ? parsePercentToUnit(*moduleValue(
                      *dumb, "FirstPercentIndent"), 0.0f)
                : 0.0f);
        plan->secondPercentIndent = fixedFinite(
            moduleValue(*dumb, "SecondPercentIndent")
                ? parsePercentToUnit(*moduleValue(
                      *dumb, "SecondPercentIndent"), 0.0f)
                : 0.0f);
        plan->targetAdjustDistancePerSecond = fixedFinite(
            moduleValue(*dumb, "FlightPathAdjustDistPerSecond")
                ? std::max(0.0f, parseFiniteFloat(*moduleValue(
                      *dumb, "FlightPathAdjustDistPerSecond"), 0.0f))
                : 0.0f);
        plan->orientToFlightPath = moduleValue(*dumb, "OrientToFlightPath")
            ? parseBool(*moduleValue(*dumb, "OrientToFlightPath"), true)
            : true;
        plan->tumbleRandomly = moduleValue(*dumb, "TumbleRandomly")
            ? parseBool(*moduleValue(*dumb, "TumbleRandomly"), false)
            : false;
        plan->maximumLifespanMilliseconds = moduleValue(*dumb, "MaxLifespan")
            ? parseMilliseconds(*moduleValue(*dumb, "MaxLifespan"),
                                kDefaultDumbProjectileLifespanMilliseconds)
            : kDefaultDumbProjectileLifespanMilliseconds;
        return plan;
    }

    if (missile) {
        if (const container::String* value = moduleValue(
                *missile, "IgnitionFX");
            value && !asciiEqualIgnoreCase(*value, "NONE")) {
            plan->ignitionFx = *value;
        }
        plan->tryToFollowTarget = moduleValue(*missile, "TryToFollowTarget")
            ? parseBool(*moduleValue(*missile, "TryToFollowTarget"), true)
            : true;
        plan->useWeaponSpeed = moduleValue(*missile, "UseWeaponSpeed")
            ? parseBool(*moduleValue(*missile, "UseWeaponSpeed"), false)
            : false;
        plan->detonateOnNoFuel = moduleValue(*missile, "DetonateOnNoFuel")
            ? parseBool(*moduleValue(*missile, "DetonateOnNoFuel"), false)
            : false;
        const auto nonNegative = [missile](container::StringView key,
                                           float fallback) {
            return fixedFinite(moduleValue(*missile, key)
                ? std::max(0.0f, parseFiniteFloat(
                      *moduleValue(*missile, key), fallback))
                : fallback);
        };
        plan->noTurnDistance = nonNegative(
            "DistanceToTravelBeforeTurning", 0.0f);
        plan->diveDistance = nonNegative(
            "DistanceToTargetBeforeDiving", 0.0f);
        plan->lockDistance = nonNegative("DistanceToTargetForLock", 75.0f);
        plan->distanceScatterWhenJammed = nonNegative(
            "DistanceScatterWhenJammed", 75.0f);
        plan->initialVelocity = nonNegative("InitialVelocity", 0.0f);
        plan->ignitionDelayMilliseconds = moduleValue(*missile, "IgnitionDelay")
            ? parseMilliseconds(*moduleValue(*missile, "IgnitionDelay"), 0)
            : 0;
        plan->fuelLifetimeMilliseconds = moduleValue(*missile, "FuelLifetime")
            ? parseMilliseconds(*moduleValue(*missile, "FuelLifetime"), 0)
            : 0;
        if (const container::String* value = moduleValue(
                *missile, "KillSelfDelay")) {
            plan->killSelfDelayMilliseconds = parseMilliseconds(*value, 0);
        }
        return plan;
    }

    if (!neutron) return plan;
    if (const container::String* value = moduleValue(*neutron, "LaunchFX");
        value && !asciiEqualIgnoreCase(*value, "NONE")) {
        plan->launchFx = *value;
    }
    if (const container::String* value = moduleValue(*neutron, "IgnitionFX");
        value && !asciiEqualIgnoreCase(*value, "NONE")) {
        plan->ignitionFx = *value;
    }
    plan->targetFromDirectlyAbove = fixedFinite(
        moduleValue(*neutron, "TargetFromDirectlyAbove")
            ? parseFiniteFloat(*moduleValue(
                  *neutron, "TargetFromDirectlyAbove"), 0.0f)
            : 0.0f);
    plan->noTurnDistance = fixedFinite(
        moduleValue(*neutron, "DistanceToTravelBeforeTurning")
            ? std::max(0.0f, parseFiniteFloat(*moduleValue(
                  *neutron, "DistanceToTravelBeforeTurning"), 0.0f))
            : 0.0f);
    const float turnDegreesPerSecond = moduleValue(*neutron, "MaxTurnRate")
        ? std::max(0.0f, parseFiniteFloat(
              *moduleValue(*neutron, "MaxTurnRate"), 999.0f))
        : 999.0f;
    plan->maximumTurnRateRadiansPerSecond = fixedFinite(
        turnDegreesPerSecond * 0.01745329251994329577f);
    plan->forwardDamping = fixedFinite(moduleValue(*neutron, "ForwardDamping")
        ? parseFiniteFloat(*moduleValue(*neutron, "ForwardDamping"), 0.0f)
        : 0.0f);
    plan->relativeSpeed = fixedFinite(moduleValue(*neutron, "RelativeSpeed")
        ? parseFiniteFloat(*moduleValue(*neutron, "RelativeSpeed"), 1.0f)
        : 1.0f, 1.0f);
    plan->specialSpeedMilliseconds = moduleValue(*neutron, "SpecialSpeedTime")
        ? parseMilliseconds(*moduleValue(*neutron, "SpecialSpeedTime"), 0)
        : 0;
    plan->specialSpeedHeight = fixedFinite(moduleValue(
        *neutron, "SpecialSpeedHeight")
            ? parseFiniteFloat(*moduleValue(
                  *neutron, "SpecialSpeedHeight"), 0.0f)
            : 0.0f);
    plan->specialAccelerationFactor = fixedFinite(moduleValue(
        *neutron, "SpecialAccelFactor")
            ? std::max(0.01f, parseFiniteFloat(*moduleValue(
                  *neutron, "SpecialAccelFactor"), 1.0f))
            : 1.0f);
    plan->specialJitterDistance = fixedFinite(moduleValue(
        *neutron, "SpecialJitterDistance")
            ? std::max(0.0f, parseFiniteFloat(*moduleValue(
                  *neutron, "SpecialJitterDistance"), 0.0f))
            : 0.0f);

    const game::ModuleData* decal = neutron
        ? moduleChild(*neutron, "DeliveryDecal") : nullptr;
    if (!decal) return plan;
    ObjectProjectileDeliveryDecalPlan output{
        .authoredOrder = neutron->authoredOrder,
    };
    if (const container::String* value = moduleValue(*decal, "Texture")) {
        if (!asciiEqualIgnoreCase(*value, "NONE")) output.texture = *value;
    }
    output.radius = fixedFinite(moduleValue(*neutron, "DeliveryDecalRadius")
        ? std::max(0.0f, parseFiniteFloat(*moduleValue(
            *neutron, "DeliveryDecalRadius"), 0.0f)) : 0.0f);
    if (const container::String* value = moduleValue(*decal, "Style")) {
        output.shadowTypeMask = parseDecalStyle(*value);
    }
    if (const container::String* value = moduleValue(*decal, "OpacityMin")) {
        output.minimumOpacity = fixedFinite(
            parsePercentToUnit(*value, 1.0f), 1.0f);
    }
    if (const container::String* value = moduleValue(*decal, "OpacityMax")) {
        output.maximumOpacity = fixedFinite(
            parsePercentToUnit(*value, 1.0f), 1.0f);
    }
    if (output.maximumOpacity < output.minimumOpacity) {
        output.maximumOpacity = output.minimumOpacity;
    }
    if (const container::String* value = moduleValue(
            *decal, "OpacityThrobTime")) {
        output.opacityThrobMilliseconds = parseMilliseconds(*value, 1000);
    }
    if (const container::String* value = moduleValue(*decal, "Color")) {
        output.color = parseDecalColor(*value);
        output.usesPlayerColor = std::all_of(
            output.color.begin(), output.color.end(),
            [](uint8_t channel) { return channel == 0; });
    }
    if (const container::String* value = moduleValue(
            *decal, "OnlyVisibleToOwningPlayer")) {
        output.onlyVisibleToOwningPlayer = parseBool(*value, true);
    }
    if (output.texture.empty() || output.radius <= kFixedZero ||
        output.shadowTypeMask == 0) {
        return plan;
    }
    plan->deliveryDecal = std::move(output);
    return plan;
}

} // namespace engine
