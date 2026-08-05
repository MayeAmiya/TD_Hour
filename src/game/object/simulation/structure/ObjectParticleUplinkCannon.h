#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/base/DamageTypes.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/combat/ObjectFireUpdates.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>

#include "game/object/plan/structure/ObjectParticleUplinkCannonPlanTypes.h"
namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;

enum class ObjectParticleUplinkPhase : uint8_t {
    Idle,
    Charging,
    Preparing,
    AlmostReady,
    ReadyToFire,
    Prefire,
    Firing,
    Postfire,
    Packing,
};

enum class ObjectParticleUplinkLaserPhase : uint8_t {
    None,
    Born,
    Decaying,
    Dead,
};

enum class ObjectParticleUplinkTargetMode : uint8_t {
    Automatic,
    Manual,
    Waypoint,
};

enum class ObjectParticleUplinkBeamLane : uint8_t {
    GroundToOrbit,
    OrbitToTarget,
};

enum class ObjectParticleUplinkBeamControl : uint8_t {
    Begin,
    Update,
    BeginDecay,
    End,
};

// Audio emitter keys share the presentation transform namespace with stable
// ObjectId values. Object IDs are 32-bit; map a rare small beam hash into the
// reserved high half while leaving the ordinary 64-bit beam identity intact.
[[nodiscard]] constexpr uint64_t particleUplinkAudioEmitterKey(
    uint64_t beamIdentity) noexcept {
    return beamIdentity != 0 && beamIdentity <= UINT32_MAX
        ? beamIdentity | (uint64_t{1} << 63u) : beamIdentity;
}

struct ObjectParticleUplinkRuntime final {
    SpecialPowerContentId specialPower = INVALID_SPECIAL_POWER_CONTENT_ID;
    uint64_t beginChargeTicks = 0;
    uint64_t raiseAntennaTicks = 0;
    uint64_t readyDelayTicks = 0;
    uint64_t widthGrowTicks = 0;
    uint64_t beamTravelTicks = 0;
    uint64_t totalFiringTicks = 0;
    uint64_t launchFxDelayTicks = 0;
    uint64_t doubleClickTicks = 0;
    math::q32_32 templateLaserRadius{int32_t{13}};
    math::q32_32 damagePerPulse{};

    ObjectParticleUplinkPhase phase = ObjectParticleUplinkPhase::Idle;
    ObjectParticleUplinkLaserPhase laserPhase =
        ObjectParticleUplinkLaserPhase::None;
    ObjectParticleUplinkTargetMode targetMode =
        ObjectParticleUplinkTargetMode::Automatic;

    LogicFixedVec3 initialTargetPosition{};
    LogicFixedVec3 currentTargetPosition{};
    LogicFixedVec3 overrideTargetDestination{};
    uint32_t nextWaypointId = UINT32_MAX;
    uint32_t waypointAdvanceCount = 0;

    uint64_t startAttackTick = 0;
    uint64_t startDecayTick = 0;
    uint64_t orbitalBirthTick = 0;
    uint64_t orbitalDecayStartTick = 0;
    uint64_t orbitalDeathTick = 0;
    uint64_t endGroundDecayTick = 0;
    uint64_t nextLaunchFxTick = 0;
    uint64_t nextScorchTick = 0;
    uint64_t nextDamagePulseTick = 0;
    uint64_t lastDrivingClickTick = 0;
    uint64_t secondLastDrivingClickTick = 0;
    uint64_t activationSequence = 0;
    uint64_t groundBeamIdentity = 0;
    uint64_t orbitalBeamIdentity = 0;
    uint64_t pendingEndGroundBeamIdentity = 0;
    uint64_t pendingEndOrbitalBeamIdentity = 0;
    uint32_t scorchMarksMade = 0;
    uint32_t damagePulsesMade = 0;
    bool groundBeamAlive = false;
    bool orbitalBeamAlive = false;
    bool groundBeamDecaying = false;
    bool orbitalBeamDecaying = false;
    bool attackActive = false;
};

struct ObjectParticleUplinkComponent final {
    container::SharedPtr<const game::ObjectParticleUplinkCannonPlan> plan;
    container::Vector<ObjectParticleUplinkRuntime> instances;
};

// Complete phase presentation value.  It preserves every direct particle and
// connector asset required by the old client effects without storing emitter
// or Drawable handles in simulation.
struct ObjectParticleUplinkPhaseEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectParticleUplinkPhase phase = ObjectParticleUplinkPhase::Idle;
    container::String outerBonePrefix;
    uint32_t outerBoneCount = 0;
    container::String outerParticleSystem;
    container::String connectorBone;
    container::String connectorLaser;
    container::String connectorFlare;
    container::String fireBone;
    container::String laserBaseParticleSystem;
    uint32_t authoredOrder = 0;
    uint64_t activationSequence = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectParticleUplinkBeamEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectParticleUplinkBeamLane lane =
        ObjectParticleUplinkBeamLane::GroundToOrbit;
    ObjectParticleUplinkBeamControl control =
        ObjectParticleUplinkBeamControl::Begin;
    uint64_t identity = 0;
    container::String beamTemplate;
    container::String sourceBone;
    LogicFixedVec3 sourcePosition{};
    LogicFixedVec3 targetPosition{};
    uint32_t widthGrowFrames = 0;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectParticleUplinkScorchEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    LogicFixedVec3 position{};
    math::q32_32 radius{};
    container::String groundHitFx;
    uint32_t authoredOrder = 0;
    uint64_t sequence = 0;
    uint64_t confirmedTick = 0;
};

// Authoritative shroud transaction detached from the scorch/ground-hit
// presentation event.  The simulation journal owns its stable ordering;
// presentation may still draw the mark after gameplay has consumed this copy.
struct ObjectParticleUplinkRevealRequest final {
    ObjectId source = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    LogicFixedVec3 position{};
    math::q32_32 revealRange{};
    uint32_t authoredOrder = 0;
    uint32_t scorchOrdinal = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectParticleUplinkFxEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    LogicFixedVec3 position{};
    container::String fxList;
    container::String boneName;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectParticleUplinkRemnantSpawnRequest final {
    ObjectId source = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectTeamId primaryTeam = INVALID_OBJECT_TEAM_ID;
    container::String objectTemplate;
    LogicFixedVec3 position{};
    uint32_t authoredOrder = 0;
    uint32_t damagePulseOrdinal = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
};

void notifyParticleUplinkSpecialPowerActivated(
    ecs::registry& registry, ecs::entity entity,
    SpecialPowerContentId specialPower, ObjectOrderSource source,
    ObjectId targetObject, const LogicFixedVec3& targetPosition,
    uint64_t activationSequence, uint64_t confirmedTick) noexcept;

class ObjectParticleUplinkCannonSystem final {
public:
    void initializeObject(
        ecs::registry& registry, ecs::entity entity,
        const GameContentSnapshot& content, uint32_t logicFramesPerSecond,
        uint64_t confirmedTick) const;

    void update(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const game::terrain::TerrainLogic& terrain,
        uint32_t logicFramesPerSecond, uint64_t confirmedTick,
        uint64_t& nextEmissionSequence,
        container::Vector<ObjectDamageRequest>& outDamage,
        container::Vector<ObjectParticleUplinkPhaseEvent>& outPhases,
        container::Vector<ObjectParticleUplinkBeamEvent>& outBeams,
        container::Vector<ObjectParticleUplinkScorchEvent>& outScorches,
        container::Vector<ObjectParticleUplinkRevealRequest>& outReveals,
        container::Vector<ObjectParticleUplinkFxEvent>& outFx,
        container::Vector<ObjectParticleUplinkRemnantSpawnRequest>& outRemnants,
        container::Vector<ObjectFireAudioCommand>& outAudio) const;

    [[nodiscard]] bool setOverridableDestination(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, const LogicFixedVec3& destination,
        uint64_t confirmedTick) const;

    [[nodiscard]] bool setWaypointDestination(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const game::terrain::TerrainLogic& terrain, ObjectId object,
        SpecialPowerContentId specialPower, uint32_t waypointId,
        uint64_t confirmedTick) const;

    void onObjectReclaim(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick,
        container::Vector<ObjectParticleUplinkBeamEvent>& outBeams,
        container::Vector<ObjectParticleUplinkPhaseEvent>& outPhases,
        container::Vector<ObjectFireAudioCommand>& outAudio) const;
};

} // namespace engine
