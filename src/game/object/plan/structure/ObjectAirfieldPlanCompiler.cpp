#include "game/object/plan/structure/ObjectAirfieldPlanTypes.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/base/SimulationRandom.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/PlayerRegistry.h"
#include "game/terrain/MapVisibilityAuthority.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

namespace game
{
namespace
{

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView moduleClass(const ModuleData& module) noexcept
{
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass} : container::StringView{module.type};
}

[[nodiscard]] const container::String* valueLast(const ModuleData& module, container::StringView key) noexcept
{
    for (auto found = module.values.rbegin(); found != module.values.rend(); ++found)
    {
        if (equalInsensitive(found->first, key))
            return &found->second;
    }
    for (const auto& [candidate, value] : module.properties)
    {
        if (equalInsensitive(candidate, key))
            return &value;
    }
    return nullptr;
}

[[nodiscard]] int32_t parseInt(container::StringView value, int32_t fallback = 0) noexcept
{
    container::String text{value};
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str())
        return fallback;
    if (parsed < std::numeric_limits<int32_t>::min())
    {
        return std::numeric_limits<int32_t>::min();
    }
    if (parsed > std::numeric_limits<int32_t>::max())
    {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(parsed);
}

[[nodiscard]] uint32_t parseMilliseconds(container::StringView value, uint32_t fallback = 0) noexcept
{
    const std::optional<float> parsed =
        parseContentFloat(value, {
            .source = __FILE__, .block = "Object", .module = "Airfield",
            .field = "Duration", .fallback = static_cast<float>(fallback)});
    if (!parsed || *parsed < 0.0f)
    {
        return fallback;
    }
    if (*parsed >= static_cast<float>(std::numeric_limits<uint32_t>::max()))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(std::ceil(*parsed));
}

[[nodiscard]] float parseFloat(container::StringView value, float fallback = 0.0f) noexcept
{
    return parseContentFloatOr(value, {
        .source = __FILE__, .block = "Object", .module = "Airfield",
        .field = "Real", .fallback = fallback});
}

[[nodiscard]] bool parseBool(container::StringView value, bool fallback = false) noexcept
{
    return equalInsensitive(value, "Yes") || equalInsensitive(value, "True") || value == "1" ||
           (!(equalInsensitive(value, "No") || equalInsensitive(value, "False") || value == "0") && fallback);
}

void readInt(const ModuleData& module, container::StringView key, int32_t& out)
{
    if (const container::String* value = valueLast(module, key))
    {
        out = parseInt(*value, out);
    }
}

void readDuration(const ModuleData& module, container::StringView key, uint32_t& out)
{
    if (const container::String* value = valueLast(module, key))
    {
        out = parseMilliseconds(*value, out);
    }
}

void readFloat(const ModuleData& module, container::StringView key, float& out)
{
    if (const container::String* value = valueLast(module, key))
    {
        out = parseFloat(*value, out);
    }
}

void readFixed(const ModuleData& module, container::StringView key,
               math::q32_32& out)
{
    if (const container::String* value = valueLast(module, key))
    {
        out = engine::LogicFixedVec3::scalarFromFloat(
            parseFloat(*value, out.to_float()));
    }
}

void readLegacyVelocity(const ModuleData& module, container::StringView key,
                        math::q32_32& out)
{
    if (const container::String* value = valueLast(module, key))
    {
        constexpr float kLegacyLogicFramesPerSecond = 30.0f;
        out = engine::LogicFixedVec3::scalarFromFloat(
            parseFloat(*value,
                       out.to_float() * kLegacyLogicFramesPerSecond) /
            kLegacyLogicFramesPerSecond);
    }
}

void readLegacyAngularVelocity(const ModuleData& module,
                               container::StringView key, math::q32_32& out)
{
    if (const container::String* value = valueLast(module, key))
    {
        constexpr float kDegreesToRadians =
            3.14159265358979323846f / 180.0f;
        constexpr float kLegacyLogicFramesPerSecond = 30.0f;
        out = engine::LogicFixedVec3::scalarFromFloat(
            parseFloat(*value,
                       out.to_float() * kLegacyLogicFramesPerSecond /
                           kDegreesToRadians) *
            kDegreesToRadians / kLegacyLogicFramesPerSecond);
    }
}

// RefCode INI::parseAngleReal: authored degrees stored as radians with no
// frame-rate factor.
void readAngleRadians(const ModuleData& module, container::StringView key,
                      math::q32_32& out)
{
    if (const container::String* value = valueLast(module, key))
    {
        constexpr float kDegreesToRadians =
            3.14159265358979323846f / 180.0f;
        out = engine::LogicFixedVec3::scalarFromFloat(
            parseFloat(*value, out.to_float() / kDegreesToRadians) *
            kDegreesToRadians);
    }
}

// RefCode INI::parseReal leaves RollRate/PitchRate in radians per legacy
// frame (see the module's own "@todo srj" note). Scale to radians per second
// so integratePhysicsOrientation, which multiplies by a real seconds delta,
// reproduces the original per-frame rotation.
void readLegacyPerFrameRateAsPerSecond(
    const ModuleData& module, container::StringView key, math::q32_32& out)
{
    if (const container::String* value = valueLast(module, key))
    {
        constexpr float kLegacyLogicFramesPerSecond = 30.0f;
        out = engine::LogicFixedVec3::scalarFromFloat(
            parseFloat(*value, out.to_float() / kLegacyLogicFramesPerSecond) *
            kLegacyLogicFramesPerSecond);
    }
}

void readPercent(const ModuleData& module, container::StringView key, float& out)
{
    if (const container::String* value = valueLast(module, key))
    {
        out = parseFloat(*value, out);
        if (value->find('%') != container::String::npos)
            out *= 0.01f;
    }
}

void readPercentFixed(const ModuleData& module, container::StringView key,
                      math::q32_32& out)
{
    if (const container::String* value = valueLast(module, key))
    {
        float parsed = parseFloat(*value, out.to_float());
        if (value->find('%') != container::String::npos)
            parsed *= 0.01f;
        out = engine::LogicFixedVec3::scalarFromFloat(parsed);
    }
}

void readBool(const ModuleData& module, container::StringView key, bool& out)
{
    if (const container::String* value = valueLast(module, key))
    {
        out = parseBool(*value, out);
    }
}

void readString(const ModuleData& module, container::StringView key, container::String& out)
{
    if (const container::String* value = valueLast(module, key))
        out = *value;
}

[[nodiscard]] container::Vector<container::String> splitTokens(container::StringView value)
{
    container::Vector<container::String> tokens;
    size_t begin = 0;
    while (begin < value.size())
    {
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
        {
            ++begin;
        }
        size_t end = begin;
        while (end < value.size() && std::isspace(static_cast<unsigned char>(value[end])) == 0)
        {
            ++end;
        }
        if (end > begin)
            tokens.emplace_back(value.substr(begin, end - begin));
        begin = end;
    }
    return tokens;
}

void readTokens(const ModuleData& module, container::StringView key, container::Vector<container::String>& out)
{
    if (const container::String* value = valueLast(module, key))
    {
        out = splitTokens(*value);
    }
}

void readPair(const ModuleData& module, container::StringView key, container::Array<container::String, 2>& out)
{
    if (const container::String* value = valueLast(module, key))
    {
        const container::Vector<container::String> tokens = splitTokens(*value);
        for (size_t index = 0; index < out.size() && index < tokens.size(); ++index)
        {
            out[index] = tokens[index];
        }
    }
}

[[nodiscard]] float parseLabeledFloat(container::StringView token, char label, float fallback) noexcept
{
    if (token.size() < 3 || std::toupper(static_cast<unsigned char>(token.front())) != label || token[1] != ':')
    {
        return fallback;
    }
    return parseFloat(token.substr(2), fallback);
}

void readRgb(const ModuleData& module, container::StringView key, float& red, float& green, float& blue)
{
    const container::String* value = valueLast(module, key);
    if (!value)
        return;
    const container::Vector<container::String> tokens = splitTokens(*value);
    for (const container::String& token : tokens)
    {
        if (token.empty())
            continue;
        switch (std::toupper(static_cast<unsigned char>(token.front())))
        {
        case 'R':
            red = parseLabeledFloat(token, 'R', red);
            break;
        case 'G':
            green = parseLabeledFloat(token, 'G', green);
            break;
        case 'B':
            blue = parseLabeledFloat(token, 'B', blue);
            break;
        default:
            break;
        }
    }
}

void readSpectreRadiusDecal(const ModuleData& module,
                            container::StringView childName,
                            ObjectSpectreRadiusDecalRule& out)
{
    const ModuleData* decal = nullptr;
    for (const ModuleData& child : module.children)
    {
        if (!equalInsensitive(moduleClass(child), childName))
            continue;
        // RadiusDecalTemplate is a single authored child block. Keep the last
        // declaration, matching the last-value rule used by ordinary fields.
        decal = &child;
    }
    if (!decal)
        return;

    readString(*decal, "Texture", out.texture);
    if (const container::String* style = valueLast(*decal, "Style"))
    {
        if (style->find("ADDITIVE") != container::String::npos ||
            style->find("Additive") != container::String::npos)
            out.shadowTypeMask = 0x40u;
        else if (style->find("NONE") != container::String::npos ||
                 style->find("None") != container::String::npos)
            out.shadowTypeMask = 0u;
        else
            out.shadowTypeMask = 0x20u;
    }
    readPercentFixed(*decal, "OpacityMin", out.minimumOpacity);
    readPercentFixed(*decal, "OpacityMax", out.maximumOpacity);
    readDuration(*decal, "OpacityThrobTime", out.opacityThrobMilliseconds);
    readBool(*decal, "OnlyVisibleToOwningPlayer",
             out.onlyVisibleToOwningPlayer);

    if (const container::String* color = valueLast(*decal, "Color"))
    {
        container::Array<uint8_t, 4> parsed{0, 0, 0, 255};
        for (const container::String& token : splitTokens(*color))
        {
            if (token.empty())
                continue;
            const char channel = static_cast<char>(
                std::toupper(static_cast<unsigned char>(token.front())));
            size_t index = 4;
            if (channel == 'R') index = 0;
            else if (channel == 'G') index = 1;
            else if (channel == 'B') index = 2;
            else if (channel == 'A') index = 3;
            if (index >= parsed.size())
                continue;
            const float value = parseLabeledFloat(
                token, channel, static_cast<float>(parsed[index]));
            parsed[index] = static_cast<uint8_t>(std::clamp(
                value, 0.0f, 255.0f));
        }
        out.color = parsed;
        out.usesPlayerColor = std::all_of(
            parsed.begin(), parsed.end(),
            [](uint8_t value) { return value == 0; });
    }
}

void readCoord3(const ModuleData& module, container::StringView key, float& x, float& y, float& z)
{
    const container::String* value = valueLast(module, key);
    if (!value)
        return;
    const container::Vector<container::String> tokens = splitTokens(*value);
    for (const container::String& token : tokens)
    {
        if (token.empty())
            continue;
        switch (std::toupper(static_cast<unsigned char>(token.front())))
        {
        case 'X':
            x = parseLabeledFloat(token, 'X', x);
            break;
        case 'Y':
            y = parseLabeledFloat(token, 'Y', y);
            break;
        case 'Z':
            z = parseLabeledFloat(token, 'Z', z);
            break;
        default:
            break;
        }
    }
}

void readCoord3Fixed(const ModuleData& module, container::StringView key,
                     math::q32_32& x, math::q32_32& y,
                     math::q32_32& z)
{
    float parsedX = x.to_float();
    float parsedY = y.to_float();
    float parsedZ = z.to_float();
    readCoord3(module, key, parsedX, parsedY, parsedZ);
    x = engine::LogicFixedVec3::scalarFromFloat(parsedX);
    y = engine::LogicFixedVec3::scalarFromFloat(parsedY);
    z = engine::LogicFixedVec3::scalarFromFloat(parsedZ);
}

template <typename Rule>
void stableSortRules(container::Vector<Rule>& rules)
{
    std::stable_sort(rules.begin(),
                     rules.end(),
                     [](const Rule& left, const Rule& right) { return left.authoredOrder < right.authoredOrder; });
}

} // namespace

container::SharedPtr<const ObjectAirfieldPlan> compileObjectAirfieldPlan(const ThingTemplate& templateData)
{
    auto plan = std::make_shared<ObjectAirfieldPlan>();
    for (const ModuleData& module : templateData.modules)
    {
        const container::StringView klass = moduleClass(module);
        if (equalInsensitive(klass, "ParkingPlaceBehavior"))
        {
            ObjectParkingPlaceRule rule{.authoredOrder = module.authoredOrder};
            readInt(module, "NumRows", rule.rows);
            readInt(module, "NumCols", rule.cols);
            readFixed(module, "ApproachHeight", rule.approachHeightFixed);
            readFixed(module, "LandingDeckHeightOffset",
                      rule.landingDeckHeightOffsetFixed);
            readBool(module, "HasRunways", rule.hasRunways);
            readBool(module, "ParkInHangars", rule.parkInHangars);
            readFixed(module, "HealAmountPerSecond",
                      rule.healAmountPerSecondFixed);
            plan->parkingPlaces.push_back(rule);
            continue;
        }
        if (equalInsensitive(klass, "FlightDeckBehavior"))
        {
            ObjectFlightDeckRule rule{.authoredOrder = module.authoredOrder};
            readInt(module, "NumRunways", rule.runways);
            readInt(module, "NumSpacesPerRunway", rule.spacesPerRunway);
            readString(module, "PayloadTemplate", rule.payloadTemplate);
            readFixed(module, "ApproachHeight", rule.approachHeightFixed);
            readFixed(module, "LandingDeckHeightOffset",
                      rule.landingDeckHeightOffsetFixed);
            readFixed(module, "HealAmountPerSecond",
                      rule.healAmountPerSecondFixed);
            readDuration(module, "ParkingCleanupPeriod", rule.cleanupMilliseconds);
            readDuration(module, "HumanFollowPeriod", rule.humanFollowMilliseconds);
            readDuration(module, "ReplacementDelay", rule.replacementMilliseconds);
            readDuration(module, "DockAnimationDelay", rule.dockAnimationMilliseconds);
            readDuration(module, "LaunchWaveDelay", rule.launchWaveMilliseconds);
            readDuration(module, "LaunchRampDelay", rule.launchRampMilliseconds);
            readDuration(module, "LowerRampDelay", rule.lowerRampMilliseconds);
            readDuration(module, "CatapultFireDelay", rule.catapultFireMilliseconds);
            const size_t runwayCount = static_cast<size_t>(std::max(0, rule.runways));
            rule.runwayDefinitions.resize(runwayCount);
            for (size_t runwayIndex = 0; runwayIndex < runwayCount; ++runwayIndex)
            {
                ObjectFlightDeckRunwayRule& runway = rule.runwayDefinitions[runwayIndex];
                const container::String prefix = "Runway" + std::to_string(runwayIndex + 1u);
                readTokens(module, prefix + "Spaces", runway.spaceBones);
                readPair(module, prefix + "Takeoff", runway.takeoffBones);
                readPair(module, prefix + "Landing", runway.landingBones);
                readTokens(module, prefix + "Taxi", runway.taxiBones);
                readTokens(module, prefix + "Creation", runway.creationBones);
                readString(module, prefix + "CatapultSystem", runway.catapultParticleSystem);
            }
            plan->flightDecks.push_back(std::move(rule));
            continue;
        }
        if (equalInsensitive(klass, "JetAIUpdate"))
        {
            ObjectJetAiRule rule{.authoredOrder = module.authoredOrder};
            readPercentFixed(module, "OutOfAmmoDamagePerSecond",
                             rule.outOfAmmoDamagePerSecondPercentFixed);
            readPercentFixed(module, "TakeoffDistForMaxLift",
                             rule.takeoffDistForMaxLiftPercentFixed);
            readFixed(module, "MinHeight", rule.minHeightFixed);
            readFixed(module, "ParkingOffset", rule.parkingOffsetFixed);
            readFixed(module, "SneakyOffsetWhenAttacking",
                      rule.sneakyOffsetWhenAttackingFixed);
            readBool(module, "NeedsRunway", rule.needsRunway);
            readBool(module, "KeepsParkingSpaceWhenAirborne", rule.keepsParkingSpaceWhenAirborne);
            readDuration(module, "TakeoffPause", rule.takeoffPauseMilliseconds);
            readString(module, "AttackLocomotorType", rule.attackLocomotorType);
            readDuration(module, "AttackLocomotorPersistTime", rule.attackLocomotorPersistMilliseconds);
            readDuration(module, "AttackersMissPersistTime", rule.attackersMissPersistMilliseconds);
            readString(module, "ReturnForAmmoLocomotorType", rule.returnForAmmoLocomotorType);
            readDuration(module, "LockonTime", rule.lockonMilliseconds);
            readString(module, "LockonCursor", rule.lockonCursor);
            readFixed(module, "LockonInitialDist",
                      rule.lockonInitialDistanceFixed);
            readFixed(module, "LockonFreq", rule.lockonFrequencyFixed);
            if (const container::String* value = valueLast(module, "LockonAngleSpin"))
            {
                // Legacy parseAngleReal treats bare values as degrees.
                rule.lockonAngleSpinFixed =
                    engine::LogicFixedVec3::scalarFromFloat(parseFloat(
                    *value, rule.lockonAngleSpinFixed.to_float() *
                        (180.0f / std::numbers::pi_v<float>)) *
                    (std::numbers::pi_v<float> / 180.0f));
            }
            readBool(module, "LockonBlinky", rule.lockonBlinky);
            readDuration(module, "ReturnToBaseIdleTime", rule.returnToBaseIdleMilliseconds);
            plan->jetAi.push_back(std::move(rule));
            continue;
        }
        if (equalInsensitive(klass, "ChinookAIUpdate"))
        {
            ObjectChinookAiRule rule{.authoredOrder = module.authoredOrder};
            readString(module, "RotorWashParticleSystem", rule.rotorWashParticleSystem);
            readString(module, "RopeName", rule.ropeName);
            if (int32_t ropes = static_cast<int32_t>(rule.numRopes); valueLast(module, "NumRopes"))
            {
                readInt(module, "NumRopes", ropes);
                rule.numRopes = static_cast<uint32_t>(std::max(0, ropes));
            }
            readDuration(module, "PerRopeDelayMin", rule.perRopeDelayMinMilliseconds);
            readDuration(module, "PerRopeDelayMax", rule.perRopeDelayMaxMilliseconds);
            readLegacyVelocity(module, "RappelSpeed",
                               rule.rappelSpeedPerLegacyFrameFixed);
            readLegacyVelocity(module, "RopeDropSpeed",
                               rule.ropeDropSpeedPerLegacyFrameFixed);
            readFloat(module, "RopeWidth", rule.ropeWidth);
            readRgb(module, "RopeColor", rule.ropeColorRed, rule.ropeColorGreen, rule.ropeColorBlue);
            readFixed(module, "RopeFinalHeight", rule.ropeFinalHeightFixed);
            readFixed(module, "RopeWobbleLen", rule.ropeWobbleLengthFixed);
            readFloat(module, "RopeWobbleAmplitude", rule.ropeWobbleAmplitude);
            readLegacyAngularVelocity(
                module, "RopeWobbleRate",
                rule.ropeWobbleRatePerLegacyFrameFixed);
            readFixed(module, "MinDropHeight", rule.minDropHeightFixed);
            readBool(module, "WaitForRopesToDrop", rule.waitForRopesToDrop);
            plan->chinookAi.push_back(std::move(rule));
            continue;
        }
        if (equalInsensitive(klass, "SpectreGunshipUpdate"))
        {
            ObjectSpectreGunshipRule rule{.authoredOrder = module.authoredOrder};
            readString(module, "SpecialPowerTemplate", rule.specialPowerTemplate);
            readString(module, "HowitzerWeaponTemplate", rule.howitzerWeaponTemplate);
            readString(module, "GattlingTemplateName", rule.gattlingTemplateName);
            readString(module, "GattlingStrafeFXParticleSystem", rule.gattlingStrafeFxParticleSystem);
            readDuration(module, "OrbitTime", rule.orbitMilliseconds);
            readDuration(module, "HowitzerFiringRate", rule.howitzerFiringRateMilliseconds);
            readDuration(module, "HowitzerFollowLag", rule.howitzerFollowLagMilliseconds);
            readFixed(module, "AttackAreaRadius",
                      rule.attackAreaRadiusFixed);
            readFixed(module, "TargetingReticleRadius",
                      rule.targetingReticleRadiusFixed);
            readFixed(module, "GunshipOrbitRadius",
                      rule.gunshipOrbitRadiusFixed);
            readFixed(module, "StrafingIncrement",
                      rule.strafingIncrementFixed);
            readFixed(module, "OrbitInsertionSlope",
                      rule.orbitInsertionSlopeFixed);
            readFixed(module, "RandomOffsetForHowitzer",
                      rule.randomOffsetForHowitzerFixed);
            readSpectreRadiusDecal(
                module, "AttackAreaDecal", rule.attackAreaDecal);
            readSpectreRadiusDecal(
                module, "TargetingReticleDecal",
                rule.targetingReticleDecal);
            plan->spectreGunships.push_back(std::move(rule));
            continue;
        }
        if (equalInsensitive(klass, "SpectreGunshipDeploymentUpdate"))
        {
            ObjectSpectreDeploymentRule rule{.authoredOrder = module.authoredOrder};
            readString(module, "GunshipTemplateName", rule.gunshipTemplateName);
            readString(module, "RequiredScience", rule.requiredScience);
            readString(module, "SpecialPowerTemplate", rule.specialPowerTemplate);
            readString(module, "CreateLocation", rule.createLocation);
            readFixed(module, "AttackAreaRadius",
                      rule.attackAreaRadiusFixed);
            plan->spectreDeployments.push_back(std::move(rule));
            continue;
        }
        if (equalInsensitive(klass, "JetSlowDeathBehavior") || equalInsensitive(klass, "HelicopterSlowDeathBehavior"))
        {
            ObjectAircraftSlowDeathRule rule{
                .authoredOrder = module.authoredOrder,
                .kind = equalInsensitive(klass, "HelicopterSlowDeathBehavior")
                    ? ObjectAircraftSlowDeathKind::Helicopter
                    : ObjectAircraftSlowDeathKind::Jet,
                .moduleClass = container::String{klass}};
            readString(module, "FXInitialDeath", rule.fxInitialDeath);
            readString(module, "OCLInitialDeath", rule.oclInitialDeath);
            readString(module, "FXSecondary", rule.fxSecondary);
            readString(module, "OCLSecondary", rule.oclSecondary);
            readString(module, "FXHitGround", rule.fxHitGround);
            readString(module, "OCLHitGround", rule.oclHitGround);
            readString(module, "FXFinalBlowUp", rule.fxFinalBlowUp);
            readString(module, "OCLFinalBlowUp", rule.oclFinalBlowUp);
            readString(module, "FXOnGroundDeath", rule.fxOnGroundDeath);
            readString(module, "OCLOnGroundDeath", rule.oclOnGroundDeath);
            readString(module, "DeathLoopSound", rule.deathLoopSound);
            readString(module, "SoundDeathLoop", rule.deathLoopSound);
            readString(module, "FinalRubbleObject", rule.finalRubbleObject);
            readString(module, "BladeObjectName", rule.bladeObjectName);
            readString(module, "BladeBoneName", rule.bladeBoneName);
            readString(module, "FXBlade", rule.fxBlade);
            readString(module, "OCLBlade", rule.oclBlade);
            readString(module, "OCLEjectPilot", rule.oclEjectPilot);
            readString(module, "AttachParticle", rule.attachParticle);
            readString(module, "AttachParticleBone", rule.attachParticleBone);
            readCoord3Fixed(module, "AttachParticleLoc",
                            rule.attachParticleXFixed,
                            rule.attachParticleYFixed,
                            rule.attachParticleZFixed);
            readDuration(module, "DelaySecondaryFromInitialDeath", rule.delaySecondaryMilliseconds);
            readDuration(module, "DelayFinalBlowUpFromHitGround", rule.delayFinalBlowUpMilliseconds);
            readDuration(module, "DelayFromGroundToFinalDeath", rule.delayFromGroundToFinalDeathMilliseconds);
            readDuration(module, "DestructionDelay", rule.destructionDelayMilliseconds);
            readLegacyPerFrameRateAsPerSecond(
                module, "RollRate", rule.rollRateRadiansPerSecondFixed);
            readPercentFixed(module, "RollRateDelta",
                             rule.rollRateDeltaFixed);
            readLegacyPerFrameRateAsPerSecond(
                module, "PitchRate", rule.pitchRateRadiansPerSecondFixed);
            readPercentFixed(module, "FallHowFast", rule.fallHowFastFixed);
            // RefCode increments its spiral heading by this value once per
            // logic frame, so it stays a per-legacy-frame quantity and is
            // resampled at the session rate by the runtime.
            readLegacyAngularVelocity(
                module, "SpiralOrbitTurnRate",
                rule.spiralOrbitTurnRateRadiansPerLegacyFrameFixed);
            // parseVelocityReal divides authored units-per-second by the
            // legacy frame rate, so the authored number already *is* the
            // per-second value.
            readFixed(module, "SpiralOrbitForwardSpeed",
                      rule.spiralOrbitForwardSpeedUnitsPerSecondFixed);
            readFixed(module, "SpiralOrbitForwardSpeedDamping",
                      rule.spiralOrbitForwardSpeedDampingFixed);
            // parseAngularVelocityReal converts authored degrees-per-second to
            // radians per legacy frame; scaling back to per second leaves the
            // authored magnitude expressed in radians.
            readAngleRadians(module, "MinSelfSpin",
                             rule.minSelfSpinRadiansPerSecondFixed);
            readAngleRadians(module, "MaxSelfSpin",
                             rule.maxSelfSpinRadiansPerSecondFixed);
            readDuration(module, "SelfSpinUpdateDelay",
                         rule.selfSpinUpdateDelayMilliseconds);
            // parseAngleReal keeps this in radians and RefCode divides it by
            // the legacy frame rate at the point of use, which is the same as
            // treating the authored magnitude as radians per second.
            readAngleRadians(module, "SelfSpinUpdateAmount",
                             rule.selfSpinUpdateAmountRadiansPerSecondFixed);
            readDuration(module, "MinBladeFlyOffDelay",
                         rule.minBladeFlyOffDelayMilliseconds);
            readDuration(module, "MaxBladeFlyOffDelay",
                         rule.maxBladeFlyOffDelayMilliseconds);
            // parseAccelerationReal divides authored units-per-second-squared
            // by the legacy frame rate twice, so the authored number is the
            // per-second-squared value.
            readFixed(module, "MaxBraking",
                      rule.maxBrakingUnitsPerSecondSquaredFixed);
            plan->slowDeaths.push_back(std::move(rule));
        }
    }
    stableSortRules(plan->parkingPlaces);
    stableSortRules(plan->flightDecks);
    stableSortRules(plan->jetAi);
    stableSortRules(plan->chinookAi);
    stableSortRules(plan->spectreGunships);
    stableSortRules(plan->spectreDeployments);
    stableSortRules(plan->slowDeaths);
    if (plan->parkingPlaces.empty() && plan->flightDecks.empty() && plan->jetAi.empty() && plan->chinookAi.empty() &&
        plan->spectreGunships.empty() && plan->spectreDeployments.empty() && plan->slowDeaths.empty())
    {
        return nullptr;
    }
    return plan;
}

} // namespace game
