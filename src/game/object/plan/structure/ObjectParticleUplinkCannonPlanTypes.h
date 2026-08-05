#pragma once

#include "core/container/container_types.h"
#include "game/base/DamageTypes.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/player/PlayerTypes.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace game {

struct ThingTemplate;
namespace terrain { class TerrainLogic; }

// Immutable projection of ParticleUplinkCannonUpdate.  Asset names remain
// content identities; every live clock, target and beam identity is sparse
// ECS state and never points at an INI parser or a renderer object.
struct ObjectParticleUplinkCannonRule final {
    container::String specialPowerTemplate;
    uint32_t beginChargeMilliseconds = 0;
    uint32_t raiseAntennaMilliseconds = 0;
    uint32_t readyDelayMilliseconds = 0;
    uint32_t widthGrowMilliseconds = 0;
    uint32_t beamTravelMilliseconds = 0;
    uint32_t totalFiringMilliseconds = 0;
    math::q32_32 revealRange{};

    container::String outerEffectBoneName;
    uint32_t outerEffectNumBones = 0;
    container::String outerNodesLightFlareParticleSystem;
    container::String outerNodesMediumFlareParticleSystem;
    container::String outerNodesIntenseFlareParticleSystem;
    container::String connectorBoneName;
    container::String connectorMediumLaserName;
    container::String connectorIntenseLaserName;
    container::String connectorMediumFlare;
    container::String connectorIntenseFlare;
    container::String fireBoneName;
    container::String laserBaseLightFlareParticleSystemName;
    container::String laserBaseMediumFlareParticleSystemName;
    container::String laserBaseIntenseFlareParticleSystemName;
    container::String particleBeamLaserName;

    math::q32_32 swathOfDeathDistance{};
    math::q32_32 swathOfDeathAmplitude{};
    uint32_t totalScorchMarks = 0;
    math::q32_32 scorchMarkScalar{int32_t{1}};
    container::String beamLaunchFx;
    uint32_t delayBetweenLaunchFxMilliseconds = 1000;
    container::String groundHitFx;

    math::q32_32 damagePerSecond{};
    uint32_t totalDamagePulses = 0;
    DamageType damageType = DamageType::LASER;
    DeathType deathType = DeathType::LASERED;
    math::q32_32 damageRadiusScalar{int32_t{1}};

    container::String poweringUpSoundLoop;
    container::String unpackToIdleSoundLoop;
    container::String firingToPackSoundLoop;
    container::String groundAnnihilationSoundLoop;
    container::String damagePulseRemnantObjectName;

    math::q32_32 manualDrivingSpeed{};
    math::q32_32 manualFastDrivingSpeed{};
    uint32_t doubleClickToFastDriveDelayMilliseconds = 500;
    uint32_t authoredOrder = 0;
};

struct ObjectParticleUplinkCannonPlan final {
    container::Vector<ObjectParticleUplinkCannonRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectParticleUplinkCannonPlan>
compileObjectParticleUplinkCannonPlan(const ThingTemplate& templateData);

} // namespace game
