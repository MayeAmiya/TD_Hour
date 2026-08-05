#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"

namespace game
{

struct ThingTemplate;
namespace terrain {
struct MapVisibilitySnapshot;
class TerrainLogic;
}

struct ObjectSpectreRadiusDecalRule final
{
    container::String texture;
    uint32_t shadowTypeMask = 0x20u;
    math::q32_32 minimumOpacity{int32_t{1}};
    math::q32_32 maximumOpacity{int32_t{1}};
    uint32_t opacityThrobMilliseconds = 1000;
    container::Array<uint8_t, 4> color{0, 0, 0, 0};
    bool usesPlayerColor = true;
    bool onlyVisibleToOwningPlayer = true;
};

struct ObjectParkingPlaceRule final
{
    uint32_t authoredOrder = 0;
    int32_t rows = 0;
    int32_t cols = 0;
    math::q32_32 approachHeightFixed{};
    math::q32_32 landingDeckHeightOffsetFixed{};
    math::q32_32 healAmountPerSecondFixed{};
    bool hasRunways = false;
    bool parkInHangars = false;
};

struct ObjectFlightDeckRunwayRule final
{
    container::Vector<container::String> spaceBones;
    container::Array<container::String, 2> takeoffBones;
    container::Array<container::String, 2> landingBones;
    container::Vector<container::String> taxiBones;
    container::Vector<container::String> creationBones;
    container::String catapultParticleSystem;
};

struct ObjectFlightDeckRule final
{
    uint32_t authoredOrder = 0;
    int32_t runways = 0;
    int32_t spacesPerRunway = 0;
    container::String payloadTemplate;
    math::q32_32 approachHeightFixed{};
    math::q32_32 landingDeckHeightOffsetFixed{};
    math::q32_32 healAmountPerSecondFixed{};
    uint32_t cleanupMilliseconds = 0;
    uint32_t humanFollowMilliseconds = 0;
    uint32_t replacementMilliseconds = 0;
    uint32_t dockAnimationMilliseconds = 0;
    uint32_t launchWaveMilliseconds = 0;
    uint32_t launchRampMilliseconds = 0;
    uint32_t lowerRampMilliseconds = 0;
    uint32_t catapultFireMilliseconds = 0;
    container::Vector<ObjectFlightDeckRunwayRule> runwayDefinitions;
};

struct ObjectJetAiRule final
{
    uint32_t authoredOrder = 0;
    // Retail parses and exposes TakeoffDistForMaxLift but never reads it in
    // JetAIUpdate, its states, or ParkingPlaceBehavior. Retain the authored
    // value for content compatibility without inventing new flight behavior.
    math::q32_32 takeoffDistForMaxLiftPercentFixed{};
    math::q32_32 outOfAmmoDamagePerSecondPercentFixed{};
    math::q32_32 minHeightFixed{};
    math::q32_32 parkingOffsetFixed{};
    math::q32_32 sneakyOffsetWhenAttackingFixed{};
    bool needsRunway = true;
    bool keepsParkingSpaceWhenAirborne = true;
    uint32_t takeoffPauseMilliseconds = 0;
    container::String attackLocomotorType = "SET_NORMAL";
    uint32_t attackLocomotorPersistMilliseconds = 0;
    uint32_t attackersMissPersistMilliseconds = 0;
    container::String returnForAmmoLocomotorType = "SET_NORMAL";
    uint32_t lockonMilliseconds = 0;
    container::String lockonCursor;
    // JetAIUpdateModuleData constructor defaults. LockonAngleSpin is stored
    // in radians after INI projection, matching parseAngleReal.
    math::q32_32 lockonInitialDistanceFixed{int32_t{100}};
    math::q32_32 lockonFrequencyFixed = math::q32_32::from_fraction(1, 2);
    math::q32_32 lockonAngleSpinFixed =
        math::q32_32::from_raw(53972150818ll);
    bool lockonBlinky = false;
    uint32_t returnToBaseIdleMilliseconds = 0;
};

struct ObjectChinookAiRule final
{
    // ChinookAIUpdate's supply-truck fields are compiled into
    // ObjectEconomyPlan.  Airfield owns only helicopter flight, rotor-wash
    // presentation and combat-drop rope behavior.
    uint32_t authoredOrder = 0;
    container::String rotorWashParticleSystem;
    container::String ropeName = "GenericRope";
    uint32_t numRopes = 4;
    uint32_t perRopeDelayMinMilliseconds = 0x7fffffffu;
    uint32_t perRopeDelayMaxMilliseconds = 0x7fffffffu;
    // Zero means the legacy constructor default, derived from the frozen
    // session gravity at CombatDrop Begin. An explicitly authored positive
    // velocity is stored in legacy 30 Hz units and resampled by the runtime.
    math::q32_32 rappelSpeedPerLegacyFrameFixed{};
    math::q32_32 ropeDropSpeedPerLegacyFrameFixed =
        math::q32_32::from_raw(std::numeric_limits<int64_t>::max());
    // Authored presentation values; they never enter deterministic gameplay state.
    float ropeWidth = 0.5f;
    float ropeColorRed = 0.9f;
    float ropeColorGreen = 0.8f;
    float ropeColorBlue = 0.7f;
    math::q32_32 ropeFinalHeightFixed{};
    math::q32_32 ropeWobbleLengthFixed{int32_t{10}};
    float ropeWobbleAmplitude = 1.0f;
    math::q32_32 ropeWobbleRatePerLegacyFrameFixed =
        math::q32_32::from_fraction(1, 10);
    math::q32_32 minDropHeightFixed{int32_t{30}};
    bool waitForRopesToDrop = true;
};

struct ObjectSpectreGunshipRule final
{
    uint32_t authoredOrder = 0;
    container::String specialPowerTemplate;
    container::String howitzerWeaponTemplate;
    container::String gattlingTemplateName;
    container::String gattlingStrafeFxParticleSystem;
    uint32_t orbitMilliseconds = 0;
    uint32_t howitzerFiringRateMilliseconds = 10;
    uint32_t howitzerFollowLagMilliseconds = 0;
    math::q32_32 attackAreaRadiusFixed{int32_t{200}};
    math::q32_32 targetingReticleRadiusFixed{int32_t{25}};
    math::q32_32 gunshipOrbitRadiusFixed{int32_t{250}};
    math::q32_32 strafingIncrementFixed{int32_t{20}};
    math::q32_32 orbitInsertionSlopeFixed =
        math::q32_32::from_fraction(7, 10);
    math::q32_32 randomOffsetForHowitzerFixed{int32_t{20}};
    ObjectSpectreRadiusDecalRule attackAreaDecal;
    ObjectSpectreRadiusDecalRule targetingReticleDecal;
};

struct ObjectSpectreDeploymentRule final
{
    uint32_t authoredOrder = 0;
    container::String gunshipTemplateName;
    container::String requiredScience;
    container::String specialPowerTemplate;
    container::String createLocation =
        "CREATE_AT_EDGE_FARTHEST_FROM_TARGET";
    math::q32_32 attackAreaRadiusFixed{int32_t{200}};
};

// Load-time classification of aircraft slow-death modules. Frame paths
// branch on this enum; moduleClass remains for diagnostics/events only.
enum class ObjectAircraftSlowDeathKind : uint8_t {
    Jet = 0,
    Helicopter,
};

struct ObjectAircraftSlowDeathRule final
{
    uint32_t authoredOrder = 0;
    ObjectAircraftSlowDeathKind kind = ObjectAircraftSlowDeathKind::Jet;
    container::String moduleClass;
    container::String fxInitialDeath;
    container::String oclInitialDeath;
    container::String fxSecondary;
    container::String oclSecondary;
    container::String fxHitGround;
    container::String oclHitGround;
    container::String fxFinalBlowUp;
    container::String oclFinalBlowUp;
    container::String fxOnGroundDeath;
    container::String oclOnGroundDeath;
    container::String deathLoopSound;
    container::String finalRubbleObject;
    container::String bladeObjectName;
    container::String bladeBoneName;
    container::String fxBlade;
    container::String oclBlade;
    container::String oclEjectPilot;
    container::String attachParticle;
    container::String attachParticleBone;
    math::q32_32 attachParticleXFixed{};
    math::q32_32 attachParticleYFixed{};
    math::q32_32 attachParticleZFixed{};
    uint32_t delaySecondaryMilliseconds = 0;
    uint32_t delayFinalBlowUpMilliseconds = 0;
    uint32_t delayFromGroundToFinalDeathMilliseconds = 0;
    uint32_t destructionDelayMilliseconds = 0;
    // Motion parameters. Units are explicit in the names because RefCode's
    // INI parse functions differ per field and the legacy 30 Hz logic rate is
    // baked into several of them:
    //   RollRate/PitchRate       INI::parseReal            radians/legacy frame
    //   RollRateDelta/FallHowFast INI::parsePercentToReal   unitless fraction
    //   SpiralOrbitTurnRate      parseAngularVelocityReal  radians/legacy frame
    //   Min/MaxSelfSpin          parseAngularVelocityReal  radians/legacy frame
    //   SelfSpinUpdateAmount     parseAngleReal            radians
    //   SpiralOrbitForwardSpeed  parseVelocityReal         units/legacy frame
    //   MaxBraking               parseAccelerationReal     units/legacy frame^2
    //   SelfSpinUpdateDelay      parseDurationReal         legacy frames
    // Rates that this port integrates with a real seconds delta are stored per
    // second; recurrences RefCode advances once per logic frame stay per
    // legacy frame so the runtime can resample them at the session rate.
    math::q32_32 rollRateRadiansPerSecondFixed{};
    // Multiplied into the live roll rate once per logic frame.
    math::q32_32 rollRateDeltaFixed{int32_t{1}};
    math::q32_32 pitchRateRadiansPerSecondFixed{};
    // Fraction of gravity the wreck is allowed to fall at. 0 hovers, 1 is a
    // full free fall.
    math::q32_32 fallHowFastFixed{};
    math::q32_32 spiralOrbitTurnRateRadiansPerLegacyFrameFixed{};
    math::q32_32 spiralOrbitForwardSpeedUnitsPerSecondFixed{};
    // Multiplied into the live spiral speed once per logic frame.
    math::q32_32 spiralOrbitForwardSpeedDampingFixed{int32_t{1}};
    math::q32_32 minSelfSpinRadiansPerSecondFixed{};
    math::q32_32 maxSelfSpinRadiansPerSecondFixed{};
    uint32_t selfSpinUpdateDelayMilliseconds = 0;
    math::q32_32 selfSpinUpdateAmountRadiansPerSecondFixed{};
    uint32_t minBladeFlyOffDelayMilliseconds = 0;
    uint32_t maxBladeFlyOffDelayMilliseconds = 0;
    math::q32_32 maxBrakingUnitsPerSecondSquaredFixed{
        int32_t{99999}};
};

struct ObjectAirfieldPlan final
{
    container::Vector<ObjectParkingPlaceRule> parkingPlaces;
    container::Vector<ObjectFlightDeckRule> flightDecks;
    container::Vector<ObjectJetAiRule> jetAi;
    container::Vector<ObjectChinookAiRule> chinookAi;
    container::Vector<ObjectSpectreGunshipRule> spectreGunships;
    container::Vector<ObjectSpectreDeploymentRule> spectreDeployments;
    container::Vector<ObjectAircraftSlowDeathRule> slowDeaths;
};

[[nodiscard]] container::SharedPtr<const ObjectAirfieldPlan> compileObjectAirfieldPlan(
    const ThingTemplate& templateData);

} // namespace game
