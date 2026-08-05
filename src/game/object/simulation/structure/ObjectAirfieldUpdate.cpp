#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/structure/ObjectAirfieldDetail.h"
#include "core/container/string_utils.h"

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

#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/base/SimulationRandom.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/object/simulation/world/ObjectRadiusDecal.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/runtime/ObjectDeathEvents.h"
#include "game/object/simulation/lifecycle/ObjectDeleteWalk.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/PlayerRegistry.h"
#include "game/terrain/MapVisibilityAuthority.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

namespace engine
{
using namespace airfield_detail;

namespace {

[[nodiscard]] uint64_t reserveGameplayOrdinal(uint64_t& next) noexcept {
    const uint64_t result = next;
    ++next;
    if (next == 0) ++next;
    return result;
}

// ---------------------------------------------------------------------------
// Aircraft slow-death motion
//
// RefCode keeps the AI/Locomotor modules installed on a dying aircraft and
// lets PhysicsBehavior::doPhysics tumble and drop it, while the locomotor's
// lift is lowered so the wreck descends at a fraction of gravity. This port
// separates the two lanes: shared Movement owns ordinary locomotion and never
// integrates a free attitude, so the wreck is handed to the free-body physics
// lane through the same forceFreeBodyTranslation latch the SlowDeath fling
// already uses. The lane change also retires any terrain-slope conform on the
// object (ObjectSimulationPhysics does that explicitly when it reaches the
// free-body integrator), so a dying aircraft can never be conforming and
// tumbling at the same time. In practice aircraft appearances never conform
// anyway, since locomotorConformsToTerrainSlope() accepts only Treads,
// FourWheels and Motorcycle.
//
// Everything below is confirmed-tick simulation state in Q32.32. There is no
// float arithmetic and no draw from the shared gameplay RNG.
// ---------------------------------------------------------------------------

constexpr math::q32_32 kFixedZero{};
constexpr math::q32_32 kFixedOne{int32_t{1}};
// RefCode's applyMotiveForce receives SpiralOrbitForwardSpeed, which
// INI::parseVelocityReal already divided by the legacy 30 Hz logic rate, and
// treats it as a force (acceleration = force / mass, in units per legacy frame
// squared). Reproducing the same physical acceleration in units per second
// squared means multiplying the stored per-second speed by the legacy rate
// once: (authored / 30) * 30^2 == authored * 30. This factor is part of the
// authored meaning of the field and is deliberately not the session frame
// rate.
constexpr math::q32_32 kLegacyLogicFramesPerSecond{int32_t{30}};

[[nodiscard]] math::q32_32 slowDeathGravityMagnitude(
    const ObjectSimulationRules& rules) noexcept {
    return math::q32_32::abs(rules.gravityUnitsPerSecondSq);
}

// True while RefCode's m_timerOnGroundFrame / m_hitGroundFrame is still zero,
// i.e. the wreck has not touched down yet.
[[nodiscard]] bool aircraftSlowDeathBeforeGroundContact(
    ObjectAircraftSlowDeathPhase phase) noexcept {
    return phase == ObjectAircraftSlowDeathPhase::InitialDeath ||
           phase == ObjectAircraftSlowDeathPhase::Secondary;
}

// True for every phase in which RefCode's update() is still running motion,
// which continues after ground contact because the jet keeps applying its
// (rapidly decaying) roll rate right up to the final explosion.
[[nodiscard]] bool aircraftSlowDeathMotionActive(
    ObjectAircraftSlowDeathPhase phase) noexcept {
    return aircraftSlowDeathBeforeGroundContact(phase) ||
           phase == ObjectAircraftSlowDeathPhase::HitGround;
}

// Hands the wreck to the free-body lane and seeds the state RefCode
// initializes in JetSlowDeathBehavior::beginSlowDeath() and
// HelicopterSlowDeathBehavior::beginSlowDeath().
void beginAircraftSlowDeathMotion(
    ecs::registry& registry, ecs::entity entity,
    const game::ObjectAircraftSlowDeathRule& rule,
    ObjectAircraftSlowDeathRuntime& runtime,
    const ObjectSimulationRules& rules) {
    if (runtime.motionOwnedByPhysics) return;
    ObjectPhysicsComponent* physics =
        ecs::try_get<ObjectPhysicsComponent>(registry, entity);
    TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, entity);
    if (!physics || !transform) return;

    const LogicFixedVec3 position =
        readAuthoritativeObjectPosition(registry, entity, *transform);
    const math::q32_32 yaw =
        readAuthoritativeObjectYaw(registry, entity, *transform);
    physics->position = position;
    physics->lastPublishedPosition = position;
    physics->collisionStartPosition = position;
    physics->hasAuthoritativePosition = true;
    physics->yaw = yaw;
    physics->lastPublishedYaw = yaw;
    physics->collisionStartYaw = yaw;
    // The Physics pass has been flattening pitch/roll every tick while
    // locomotion owned this object, so the tumble starts from the live heading
    // and a level attitude, exactly like RefCode's transform at the moment the
    // slow death begins.
    object_simulation_detail::rebuildPhysicsOrientation(*physics);

    const math::q32_32_sincos heading = math::fixed_sincos(yaw);
    if (ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(registry, entity)) {
        // Carry the locomotor's momentum into the free body. Without this a
        // shot-down jet would stop dead in the air and drop vertically.
        physics->velocityUnitsPerSecond = {
            locomotion->forwardSpeed * heading.cosine,
            locomotion->forwardSpeed * heading.sine,
            locomotion->verticalSpeed,
        };
        locomotion->forwardSpeed = kFixedZero;
        locomotion->verticalSpeed = kFixedZero;
        locomotion->hasActiveMove = false;
        locomotion->state = ObjectLocomotionState::Idle;
        // Locomotor::setMaxLift( -gravity * (1 - FallHowFast) ) and, for the
        // helicopter, Locomotor::setMaxBraking( MaxBraking ). Shared Movement
        // stops owning this entity the moment forceFreeBodyTranslation is set,
        // so these keep the authored intent on the retained locomotor while
        // the residual lift force applied every tick below is what actually
        // produces the descent.
        const math::q32_32 fallHowFast = math::q32_32::clamp(
            rule.fallHowFastFixed, kFixedZero, kFixedOne);
        const math::q32_32 lift =
            slowDeathGravityMagnitude(rules) * (kFixedOne - fallHowFast);
        locomotion->lift = lift;
        locomotion->damagedLift = lift;
        if (rule.kind == game::ObjectAircraftSlowDeathKind::Jet) {
            // JetSlowDeathBehavior additionally forbids any further turning.
            locomotion->maximumTurnRate = kFixedZero;
            locomotion->damagedMaximumTurnRate = kFixedZero;
        } else {
            locomotion->braking =
                math::q32_32::max(
                    kFixedZero, rule.maxBrakingUnitsPerSecondSquaredFixed);
            locomotion->hasFiniteBraking = true;
            locomotion->brakingIsInfinite = false;
        }
    }
    ecs::remove<ObjectAIPathMovementComponent>(registry, entity);

    physics->forceFreeBodyTranslation = true;
    physics->allowToFall = true;
    physics->ownsAttitude = true;
    physics->sleeping = false;
    // The locomotion-owned branch of the Physics pass never maintains this
    // latch, so seed it here: a wreck that reaches the ground on its very
    // first free-body tick must still publish the Landed event that drives
    // notifyAircraftHitGround().
    physics->wasAirborneLastFrame = true;

    if (rule.kind == game::ObjectAircraftSlowDeathKind::Jet) {
        runtime.rollRateRadiansPerSecond = rule.rollRateRadiansPerSecondFixed;
    } else {
        // The spiral heading starts at the object's current facing and is
        // deliberately independent of the model's own orientation afterwards.
        runtime.spiralForwardAngleRadians = yaw;
        runtime.spiralForwardSpeedUnitsPerSecond =
            rule.spiralOrbitForwardSpeedUnitsPerSecondFixed;
        runtime.selfSpinRadiansPerSecond =
            rule.minSelfSpinRadiansPerSecondFixed;
        runtime.selfSpinTowardsMaximum = true;
    }
    runtime.motionOwnedByPhysics = true;
}

// One confirmed tick of RefCode's JetSlowDeathBehavior::update() /
// HelicopterSlowDeathBehavior::update() motion block.
void advanceAircraftSlowDeathMotion(
    ecs::registry& registry, ecs::entity entity,
    const game::ObjectAircraftSlowDeathRule& rule,
    ObjectAircraftSlowDeathRuntime& runtime,
    const ObjectSimulationRules& rules, uint64_t confirmedTick) {
    if (!runtime.motionOwnedByPhysics ||
        !aircraftSlowDeathMotionActive(runtime.phase)) {
        return;
    }
    ObjectPhysicsComponent* physics =
        ecs::try_get<ObjectPhysicsComponent>(registry, entity);
    if (!physics) return;
    physics->sleeping = false;

    // Locomotor::setMaxLift( -gravity * (1 - FallHowFast) ) balanced against
    // the gravity the free-body integrator applies: the net descent is exactly
    // FallHowFast of gravity, which is the authored meaning of the field.
    // acceleration = pendingForce / mass, so the force is mass-scaled here.
    const math::q32_32 fallHowFast = math::q32_32::clamp(
        rule.fallHowFastFixed, kFixedZero, kFixedOne);
    physics->pendingForce.z += physics->mass * slowDeathGravityMagnitude(rules) *
        (kFixedOne - fallHowFast);

    if (rule.kind == game::ObjectAircraftSlowDeathKind::Jet) {
        // physics->setRollRate( m_rollRate ); m_rollRate *= RollRateDelta.
        // RefCode applies this unconditionally every frame, including after
        // ground contact, which is why the decayed rate is republished rather
        // than left to the integrator.
        physics->rollRate = runtime.rollRateRadiansPerSecond;
        runtime.rollRateRadiansPerSecond *= rule.rollRateDeltaFixed;
        return;
    }

    if (!aircraftSlowDeathBeforeGroundContact(runtime.phase)) return;

    // Ping-pong the self spin between MinSelfSpin and MaxSelfSpin. RefCode
    // adds SelfSpinUpdateAmount / LOGICFRAMES_PER_SECOND to a per-frame rate;
    // in per-second units that is the authored angle itself. The unit mismatch
    // (an angle added to an angular rate) is RefCode's and is reproduced
    // rather than "corrected", because the resulting wobble is the visible
    // retail behaviour.
    const uint64_t selfSpinDelayFrames = millisecondsToFrames(
        rule.selfSpinUpdateDelayMilliseconds, rules.logicFramesPerSecond);
    if (rule.selfSpinUpdateDelayMilliseconds != 0 &&
        confirmedTick > runtime.lastSelfSpinUpdateTick &&
        confirmedTick - runtime.lastSelfSpinUpdateTick >
            selfSpinDelayFrames) {
        if (runtime.selfSpinTowardsMaximum) {
            runtime.selfSpinRadiansPerSecond +=
                rule.selfSpinUpdateAmountRadiansPerSecondFixed;
            if (runtime.selfSpinRadiansPerSecond >
                    rule.maxSelfSpinRadiansPerSecondFixed) {
                runtime.selfSpinRadiansPerSecond =
                    rule.maxSelfSpinRadiansPerSecondFixed;
                runtime.selfSpinTowardsMaximum = false;
            }
        } else {
            runtime.selfSpinRadiansPerSecond -=
                rule.selfSpinUpdateAmountRadiansPerSecondFixed;
            if (runtime.selfSpinRadiansPerSecond <
                    rule.minSelfSpinRadiansPerSecondFixed) {
                runtime.selfSpinRadiansPerSecond =
                    rule.minSelfSpinRadiansPerSecondFixed;
                runtime.selfSpinTowardsMaximum = true;
            }
        }
        runtime.lastSelfSpinUpdateTick = confirmedTick;
    }

    // Matrix3D::In_Place_Pre_Rotate_Z( m_selfSpin * m_orbitDirection ) is a
    // rotation about the object's own Z axis, which is precisely what
    // integratePhysicsOrientation's yaw term does to the authoritative basis.
    // ORBIT_DIRECTION_LEFT is +1, and RefCode never selects the right-hand
    // variant, so the sign is fixed.
    physics->yawRate = runtime.selfSpinRadiansPerSecond;

    // physics->applyMotiveForce( cos/sin(m_forwardAngle) * m_forwardSpeed ).
    const math::q32_32_sincos spiral =
        math::fixed_sincos(runtime.spiralForwardAngleRadians);
    const math::q32_32 forceMagnitude =
        runtime.spiralForwardSpeedUnitsPerSecond * kLegacyLogicFramesPerSecond;
    physics->pendingForce.x += spiral.cosine * forceMagnitude;
    physics->pendingForce.y += spiral.sine * forceMagnitude;
    // applyMotiveForce also extends the motive window, which suspends forward
    // friction for MOTIVE_FRAMES exactly as the shared ingress does.
    const uint64_t motiveFrames = std::max<uint64_t>(
        1u, rules.logicFramesPerSecond / 3u);
    physics->motiveForceExpiresTick =
        saturatingAdd(confirmedTick, motiveFrames);

    // m_forwardAngle += SpiralOrbitTurnRate * m_orbitDirection, applied once
    // per logic frame, then m_forwardSpeed *= SpiralOrbitForwardSpeedDamping.
    runtime.spiralForwardAngleRadians += legacyAuthoredPerFrameAtSessionRate(
        rule.spiralOrbitTurnRateRadiansPerLegacyFrameFixed, rules);
    runtime.spiralForwardSpeedUnitsPerSecond *=
        rule.spiralOrbitForwardSpeedDampingFixed;
}

void appendAirfieldGameplay(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectAirfieldEvent& event,
    container::Vector<ObjectSlowDeathPhaseEvent>& outSlowDeathPhases,
    container::Vector<ObjectDeleteDestroyRequest>& outDestroyRequests,
    uint64_t& nextGameplaySubmissionOrdinal) {
    if (event.kind == ObjectAirfieldEventKind::AircraftSlowDeathPhase) {
        if (event.fx.empty() && event.ocl.empty() &&
            event.payloadTemplate.empty()) {
            return;
        }
        game::ObjectSlowDeathPhase phase =
            game::ObjectSlowDeathPhase::Midpoint;
        if (event.slowDeathPhase ==
                ObjectAircraftSlowDeathPhase::InitialDeath ||
            event.slowDeathPhase ==
                ObjectAircraftSlowDeathPhase::OnGroundDeath) {
            phase = game::ObjectSlowDeathPhase::Initial;
        } else if (event.slowDeathPhase ==
                   ObjectAircraftSlowDeathPhase::FinalBlowUp) {
            phase = game::ObjectSlowDeathPhase::Final;
        }
        const std::optional<ecs::entity> entity =
            lifecycle.entityFromIdIncludingPending(event.object);
        const OwnerComponent* owner = entity
            ? ecs::try_get<OwnerComponent>(registry, *entity) : nullptr;
        const PrimaryTeamComponent* team = entity
            ? ecs::try_get<PrimaryTeamComponent>(registry, *entity) : nullptr;
        const ObjectFixedTransformComponent* transform = entity
            ? ecs::try_get<ObjectFixedTransformComponent>(registry, *entity)
            : nullptr;
        outSlowDeathPhases.push_back({
            .object = event.object,
            .sourceSequence = static_cast<uint32_t>(event.moduleIndex),
            .authoredOrder = event.authoredOrder,
            .sourcePathfindLayer = event.sourcePathfindLayer,
            .phase = phase,
            .fx = event.fx.empty()
                ? std::optional<container::String>{}
                : std::optional<container::String>{event.fx},
            .ocl = event.ocl.empty()
                ? std::optional<container::String>{}
                : std::optional<container::String>{event.ocl},
            .rubbleObject = event.payloadTemplate.empty()
                ? std::optional<container::String>{}
                : std::optional<container::String>{event.payloadTemplate},
            .rubbleOwner = owner ? owner->player : INVALID_PLAYER_ID,
            .rubblePrimaryTeam = team ? team->team : INVALID_OBJECT_TEAM_ID,
            .rubbleTransform = transform
                ? *transform : ObjectFixedTransformComponent{},
            .hasRubbleSpawnState = owner && team && transform &&
                owner->player && team->team,
            .fxEmissionSequence =
                reserveGameplayOrdinal(nextGameplaySubmissionOrdinal),
            .confirmedTick = event.confirmedTick,
        });
        return;
    }

    if (event.kind !=
            ObjectAirfieldEventKind::AircraftTerminalDestroyRequested &&
        event.kind !=
            ObjectAirfieldEventKind::SpectreObjectDestroyRequested) {
        return;
    }
    outDestroyRequests.push_back({
        .object = event.object,
        .reason = event.kind ==
                ObjectAirfieldEventKind::AircraftTerminalDestroyRequested
            ? ObjectDestroyReason::Combat : ObjectDestroyReason::System,
        .authoredOrder = event.authoredOrder,
        .submissionOrdinal =
            reserveGameplayOrdinal(nextGameplaySubmissionOrdinal),
        .confirmedTick = event.confirmedTick,
    });
}

} // namespace

bool ObjectAirfieldSystem::beginAircraftSlowDeathOnDie(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules,
    const game::terrain::TerrainLogic* terrain,
    ObjectId aircraft, uint32_t authoredOrder,
    uint64_t confirmedTick,
    container::Vector<ObjectAirfieldEvent>& outEvents,
    container::Vector<ObjectSlowDeathPhaseEvent>& outSlowDeathPhases,
    container::Vector<ObjectDeleteDestroyRequest>& outDestroyRequests,
    uint64_t& nextGameplaySubmissionOrdinal,
    bool bypassJetGroundDeathGate) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(aircraft);
    ObjectAirfieldComponent* component = entity
        ? ecs::try_get<ObjectAirfieldComponent>(registry, *entity)
        : nullptr;
    if (!entity || !component || !component->plan) return false;
    const size_t count = std::min(component->slowDeaths.size(),
                                  component->plan->slowDeaths.size());
    for (size_t index = 0; index < count; ++index) {
        ObjectAircraftSlowDeathRuntime& runtime =
            component->slowDeaths[index];
        const game::ObjectAircraftSlowDeathRule& rule =
            component->plan->slowDeaths[index];
        if (rule.authoredOrder != authoredOrder ||
            runtime.phase != ObjectAircraftSlowDeathPhase::Alive) {
            continue;
        }
        const ObjectTerrainLayerComponent* terrainLayer =
            ecs::try_get<ObjectTerrainLayerComponent>(registry, *entity);
        const uint32_t sourcePathfindLayer = terrainLayer
            ? terrainLayer->pathfindLayer
            : game::terrain::kGroundPathfindLayer;
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, *entity);
        const bool onDeck = status && status->hasAny(
            game::objectStatusBit(
                game::ObjectStatusFlag::DeckHeightOffset));
        const bool jetGroundDeath =
            !bypassJetGroundDeathGate &&
            rule.kind == game::ObjectAircraftSlowDeathKind::Jet &&
            (onDeck || !object_simulation_detail::significantlyAboveTerrain(
                registry, *entity, terrain, rules));
        if (jetGroundDeath) {
            runtime.phase = ObjectAircraftSlowDeathPhase::OnGroundDeath;
            runtime.destroyDueTick = confirmedTick;
            ObjectAirfieldEvent event = makeSlowDeathEvent(
                aircraft, sourcePathfindLayer, rule,
                ObjectAircraftSlowDeathPhase::OnGroundDeath,
                confirmedTick, confirmedTick, rule.fxOnGroundDeath,
                rule.oclOnGroundDeath, rule.deathLoopSound,
                rule.finalRubbleObject);
            event.moduleIndex = index;
            appendAirfieldGameplay(
                registry, lifecycle, event, outSlowDeathPhases,
                outDestroyRequests, nextGameplaySubmissionOrdinal);
            outEvents.push_back(std::move(event));
            runtime.terminalDestroyEventEmitted = true;
            ObjectAirfieldEvent terminal = makeRuntimeEvent(
                ObjectAirfieldEventKind::AircraftTerminalDestroyRequested,
                aircraft, rule.authoredOrder, rule.moduleClass, index,
                confirmedTick);
            terminal.dueTick = confirmedTick;
            appendAirfieldGameplay(
                registry, lifecycle, terminal, outSlowDeathPhases,
                outDestroyRequests, nextGameplaySubmissionOrdinal);
            outEvents.push_back(std::move(terminal));
            return true;
        }

        runtime.phase = ObjectAircraftSlowDeathPhase::InitialDeath;
        runtime.initialEventEmitted = true;
        runtime.secondaryDueTick = saturatingAdd(
            confirmedTick, millisecondsToFrames(
                rule.delaySecondaryMilliseconds,
                rules.logicFramesPerSecond));
        runtime.finalBlowUpDueTick = saturatingAdd(
            confirmedTick, millisecondsToFrames(
                rule.delayFinalBlowUpMilliseconds,
                rules.logicFramesPerSecond));
        runtime.groundToFinalDueTick = saturatingAdd(
            confirmedTick, millisecondsToFrames(
                rule.delayFromGroundToFinalDeathMilliseconds,
                rules.logicFramesPerSecond));
        runtime.destroyDueTick = saturatingAdd(
            confirmedTick, millisecondsToFrames(
                rule.destructionDelayMilliseconds,
                rules.logicFramesPerSecond));
        // RefCode draws GameLogicRandomValueReal( MinBladeFlyOffDelay,
        // MaxBladeFlyOffDelay ). Taking the maximum instead made every blade
        // detach at the same authored instant.
        runtime.bladeDetachDueTick = saturatingAdd(
            confirmedTick,
            aircraftSlowDeathBladeDelayFrames(
                aircraft, rule.authoredOrder, confirmedTick,
                rule.minBladeFlyOffDelayMilliseconds,
                rule.maxBladeFlyOffDelayMilliseconds,
                rules.logicFramesPerSecond));
        beginAircraftSlowDeathMotion(
            registry, *entity, rule, runtime, rules);
        ObjectAirfieldEvent event = makeSlowDeathEvent(
            aircraft, sourcePathfindLayer, rule,
            ObjectAircraftSlowDeathPhase::InitialDeath,
            confirmedTick, confirmedTick, rule.fxInitialDeath,
            rule.oclInitialDeath, rule.deathLoopSound);
        event.moduleIndex = index;
        event.particleSystem = rule.attachParticle;
        event.boneName = rule.attachParticleBone;
        event.localOffset = {rule.attachParticleXFixed,
                             rule.attachParticleYFixed,
                             rule.attachParticleZFixed};
        appendAirfieldGameplay(
            registry, lifecycle, event, outSlowDeathPhases,
            outDestroyRequests, nextGameplaySubmissionOrdinal);
        outEvents.push_back(std::move(event));
        return true;
    }
    return false;
}

bool ObjectAirfieldSystem::notifyAircraftHitGround(ecs::registry& registry,
                                                   const ObjectLifecycle& lifecycle,
                                                   const ObjectSimulationRules& rules,
                                                   ObjectId aircraft,
                                                   uint64_t confirmedTick,
                                                   container::Vector<ObjectAirfieldEvent>& outEvents,
                                                   container::Vector<ObjectSlowDeathPhaseEvent>& outSlowDeathPhases,
                                                   container::Vector<ObjectDeleteDestroyRequest>& outDestroyRequests,
                                                   uint64_t& nextGameplaySubmissionOrdinal) const
{
    if (!aircraft || !objectEffectivelyDead(registry, lifecycle, aircraft))
    {
        return false;
    }
    const std::optional<ecs::entity> entity = lifecycle.entityFromIdIncludingPending(aircraft);
    if (!entity)
        return false;
    ObjectAirfieldComponent* component = ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component || !component->plan)
        return false;
    const ObjectTerrainLayerComponent* terrainLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, *entity);
    const uint32_t sourcePathfindLayer = terrainLayer
        ? terrainLayer->pathfindLayer
        : game::terrain::kGroundPathfindLayer;
    bool handled = false;
    for (size_t index = 0; index < component->slowDeaths.size() && index < component->plan->slowDeaths.size(); ++index)
    {
        ObjectAircraftSlowDeathRuntime& runtime = component->slowDeaths[index];
        const game::ObjectAircraftSlowDeathRule& rule = component->plan->slowDeaths[index];
        // An Alive runtime did not win the authored DieMux/filter walk and
        // must never be discovered merely because another module's aircraft
        // later touches terrain.
        if (runtime.phase == ObjectAircraftSlowDeathPhase::Alive) continue;
        if (runtime.phase == ObjectAircraftSlowDeathPhase::FinalBlowUp ||
            runtime.phase == ObjectAircraftSlowDeathPhase::OnGroundDeath)
        {
            continue;
        }
        runtime.phase = ObjectAircraftSlowDeathPhase::HitGround;
        const bool helicopter =
            rule.kind == game::ObjectAircraftSlowDeathKind::Helicopter;
        // JetSlowDeathBehavior::update(): "start us rolling on another axis
        // too" the instant the wreck touches down. PitchRate is a jet-only
        // field; the helicopter has no equivalent.
        if (!helicopter && runtime.motionOwnedByPhysics)
        {
            if (ObjectPhysicsComponent* physics =
                    ecs::try_get<ObjectPhysicsComponent>(registry, *entity))
            {
                physics->pitchRate = rule.pitchRateRadiansPerSecondFixed;
                physics->sleeping = false;
            }
        }
        const uint32_t delay =
            helicopter ? rule.delayFromGroundToFinalDeathMilliseconds : rule.delayFinalBlowUpMilliseconds;
        runtime.finalBlowUpDueTick =
            saturatingAdd(confirmedTick, millisecondsToFrames(delay, rules.logicFramesPerSecond));
        ObjectAirfieldEvent impact = makeSlowDeathEvent(aircraft,
                                                        sourcePathfindLayer,
                                                        rule,
                                                        ObjectAircraftSlowDeathPhase::HitGround,
                                                        confirmedTick,
                                                        confirmedTick,
                                                        rule.fxHitGround,
                                                        rule.oclHitGround);
        impact.moduleIndex = index;
        appendAirfieldGameplay(
            registry, lifecycle, impact, outSlowDeathPhases,
            outDestroyRequests, nextGameplaySubmissionOrdinal);
        outEvents.push_back(std::move(impact));
        handled = true;
        if (runtime.finalBlowUpDueTick == confirmedTick)
        {
            runtime.phase = ObjectAircraftSlowDeathPhase::FinalBlowUp;
            ObjectAirfieldEvent final = makeSlowDeathEvent(aircraft,
                                                           sourcePathfindLayer,
                                                           rule,
                                                           ObjectAircraftSlowDeathPhase::FinalBlowUp,
                                                           confirmedTick,
                                                           confirmedTick,
                                                           rule.fxFinalBlowUp,
                                                           rule.oclFinalBlowUp,
                                                           {},
                                                           rule.finalRubbleObject);
            final.moduleIndex = index;
            appendAirfieldGameplay(
                registry, lifecycle, final, outSlowDeathPhases,
                outDestroyRequests, nextGameplaySubmissionOrdinal);
            outEvents.push_back(std::move(final));
        }
    }
    return handled;
}

void ObjectAirfieldSystem::update(ecs::registry& registry,
                                  const ObjectLifecycle& lifecycle,
                                  const ObjectSimulationRules& rules,
                                  uint64_t confirmedTick,
                                  container::Vector<ObjectAirfieldEvent>& outEvents,
                                  container::Vector<ObjectDamageRequest>& outDamage,
                                  container::Vector<ObjectSlowDeathPhaseEvent>& outSlowDeathPhases,
                                  container::Vector<ObjectDeleteDestroyRequest>& outDestroyRequests,
                                  uint64_t& nextGameplaySubmissionOrdinal,
                                  const GameContentSnapshot* content,
                                  const PlayerRegistry* players,
                                  const game::terrain::TerrainLogic* terrain,
                                  const game::terrain::MapVisibilitySnapshot*
                                      visibility,
                                  SimulationRandom* random,
                                  container::Vector<ObjectSystemWeaponFireCommand>*
                                      outWeaponCommands,
                                  container::Vector<ObjectAirfieldServiceRequest>*
                                      outServiceRequests,
                                  container::Vector<
                                      ObjectAirfieldAutomaticProductionRequest>*
                                      outAutomaticProductionRequests,
                                   container::Vector<
                                       ObjectChinookRopePresentationEvent>*
                                       outRopeEvents,
                                   container::Vector<ObjectRadiusDecalEvent>*
                                       outRadiusDecalEvents) const
{
    struct Candidate final
    {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto reserveWeaponEmissionSequence = [&]() {
        return reserveGameplayOrdinal(nextGameplaySubmissionOrdinal);
    };
    const auto view = ecs::view<const ObjectIdentityComponent, ObjectAirfieldComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view)
    {
        const ObjectIdentityComponent& identity = view.template get<const ObjectIdentityComponent>(entity);
        if (identity.id && !lifecycle.isPendingDestroy(identity.id))
        {
            candidates.push_back({identity.id, entity});
        }
    }
    std::sort(candidates.begin(),
              candidates.end(),
              [](const Candidate& left, const Candidate& right) { return left.object < right.object; });
    for (const Candidate& candidate : candidates)
    {
        ObjectAirfieldComponent& component = ecs::get<ObjectAirfieldComponent>(registry, candidate.entity);
        if (!component.plan)
            continue;
        for (size_t index = 0; index < component.jetAi.size() && index < component.plan->jetAi.size(); ++index)
        {
            ObjectJetAiRuntime& runtime = component.jetAi[index];
            const game::ObjectJetAiRule& rule = component.plan->jetAi[index];
            runtime.lockonTargeters.erase(
                std::remove_if(
                    runtime.lockonTargeters.begin(),
                    runtime.lockonTargeters.end(),
                    [&](ObjectId targeter) {
                        const std::optional<ecs::entity> targeterEntity =
                            lifecycle.entityFromId(targeter);
                        const ObjectWeaponComponent* targeterWeapons =
                            targeterEntity
                                ? ecs::try_get<ObjectWeaponComponent>(
                                      registry, *targeterEntity)
                                : nullptr;
                        return !targeterWeapons ||
                            targeterWeapons->jetLockonTarget !=
                                candidate.object;
                    }),
                runtime.lockonTargeters.end());
            if (runtime.lockonTargeters.empty())
                runtime.lockonReadyTick = 0;
            const ObjectKindOfComponent* aircraftKinds =
                ecs::try_get<ObjectKindOfComponent>(registry,
                                                     candidate.entity);
            const bool producedAtHelipad = aircraftKinds &&
                game::objectHasKind(
                    aircraftKinds->mask,
                    game::ObjectKindOf::ProducedAtHelipad);
            const auto stateForPhase = [](ObjectJetAirfieldPhase phase) noexcept {
                switch (phase) {
                case ObjectJetAirfieldPhase::Parked:
                    return ObjectAircraftRuntimeState::Parked;
                case ObjectJetAirfieldPhase::AwaitTakeoffClearance:
                case ObjectJetAirfieldPhase::TaxiToTakeoff:
                case ObjectJetAirfieldPhase::PauseBeforeTakeoff:
                    return ObjectAircraftRuntimeState::Taxiing;
                case ObjectJetAirfieldPhase::TakingOff:
                    return ObjectAircraftRuntimeState::TakingOff;
                case ObjectJetAirfieldPhase::Airborne:
                    return ObjectAircraftRuntimeState::Airborne;
                case ObjectJetAirfieldPhase::ReturningToBase:
                case ObjectJetAirfieldPhase::ReturningToDeadAirfield:
                case ObjectJetAirfieldPhase::CirclingDeadAirfield:
                    return ObjectAircraftRuntimeState::ReturningToBase;
                case ObjectJetAirfieldPhase::AwaitLandingClearance:
                case ObjectJetAirfieldPhase::Landing:
                    return ObjectAircraftRuntimeState::Landing;
                case ObjectJetAirfieldPhase::TaxiToParking:
                case ObjectJetAirfieldPhase::OrientForParking:
                    return ObjectAircraftRuntimeState::Taxiing;
                case ObjectJetAirfieldPhase::Reloading:
                    return ObjectAircraftRuntimeState::Reloading;
                }
                return ObjectAircraftRuntimeState::Idle;
            };
            const auto setPhase = [&](ObjectJetAirfieldPhase phase) {
                if (runtime.phase == phase) return;
                if (runtime.phase == ObjectJetAirfieldPhase::TakingOff &&
                    phase != ObjectJetAirfieldPhase::TakingOff) {
                    if (ObjectLocomotionComponent* locomotion =
                            ecs::try_get<ObjectLocomotionComponent>(
                                registry, candidate.entity);
                        locomotion && !locomotion->profiles.empty()) {
                        const game::FrozenLocomotorTemplate& profile =
                            locomotion->profiles.front();
                        locomotion->lift = profile.fixed.lift;
                        locomotion->damagedLift =
                            profile.fixed.damagedLift;
                    }
                }
                runtime.phase = phase;
                runtime.state = stateForPhase(phase);
                runtime.phaseEnteredTick = confirmedTick;
                runtime.route.clear();
                runtime.nextRoutePoint = 0;
                if (phase != ObjectJetAirfieldPhase::Airborne) {
                    if (ObjectLocomotionComponent* locomotion =
                            ecs::try_get<ObjectLocomotionComponent>(
                                registry, candidate.entity)) {
                        locomotion->hasActiveMove = false;
                        locomotion->forwardSpeed = {};
                        locomotion->verticalSpeed = {};
                        locomotion->state = ObjectLocomotionState::Idle;
                    }
                    ecs::remove<ObjectAIPathMovementComponent>(
                        registry, candidate.entity);
                }
                outEvents.push_back(makeRuntimeEvent(
                    ObjectAirfieldEventKind::AircraftStateChanged,
                    candidate.object, rule.authoredOrder, "JetAIUpdate",
                    index, confirmedTick, runtime.state, runtime.phase));
                if (phase == ObjectJetAirfieldPhase::CirclingDeadAirfield) {
                    // JetAIUpdateStateCirclingAirfield::onEnter plays
                    // VoiceLowFuel once while the exhausted craft begins to
                    // orbit its destroyed/unavailable producer. Retain that
                    // confirmed transition instead of polling the phase from
                    // presentation every frame.
                    outEvents.push_back(makeRuntimeEvent(
                        ObjectAirfieldEventKind::JetLowFuel,
                        candidate.object, rule.authoredOrder, "JetAIUpdate",
                        index, confirmedTick, runtime.state, runtime.phase));
                }
            };
            // JetPauseBeforeTakeoffState starts the source-owned afterburner
            // event after the jet has entered its final takeoff wait, and
            // JetTakeoffOrLandingState tears it down on leaving takeoff.  Do
            // not derive this from JETAFTERBURNER: the model condition is a
            // separate presentation projection and is allowed to be missing.
            const auto synchronizeAfterburnerAudio = [&]() {
                const bool enabled =
                    runtime.phase == ObjectJetAirfieldPhase::PauseBeforeTakeoff ||
                    runtime.phase == ObjectJetAirfieldPhase::TakingOff;
                if (enabled == runtime.afterburnerAudioActive) return;
                runtime.afterburnerAudioActive = enabled;
                outEvents.push_back(makeRuntimeEvent(
                    enabled ? ObjectAirfieldEventKind::AfterburnerLoopStarted
                            : ObjectAirfieldEventKind::AfterburnerLoopStopped,
                    candidate.object, rule.authoredOrder, "JetAIUpdate",
                    index, confirmedTick, runtime.state, runtime.phase));
            };
            const auto setAirborne = [&](bool airborne) {
                if (ObjectAirborneComponent* value =
                        ecs::try_get<ObjectAirborneComponent>(
                            registry, candidate.entity)) {
                    value->isAirborne = airborne;
                } else {
                    ecs::emplace<ObjectAirborneComponent>(
                        registry, candidate.entity,
                        ObjectAirborneComponent{airborne});
                }
            };
            const auto capturePendingOrder = [&](bool resumableOnly) {
                ObjectOrderQueueComponent* queue =
                    ecs::try_get<ObjectOrderQueueComponent>(
                        registry, candidate.entity);
                if (!queue) return false;
                if (runtime.pendingOrder && queue->orders.empty() &&
                    queue->externalRevision !=
                        runtime.pendingExternalRevision) {
                    // Stop is represented only by an external revision and
                    // an empty queue. It cancels the legacy pending command.
                    runtime.pendingOrder.reset();
                    runtime.pendingOrderTail.clear();
                    runtime.pendingQueueRevision = queue->revision;
                    runtime.pendingExternalRevision =
                        queue->externalRevision;
                    return true;
                }
                if (queue->orders.empty()) return false;
                if (runtime.pendingOrder &&
                    queue->revision == runtime.pendingQueueRevision) {
                    return false;
                }
                const ObjectOrderIntent& order = queue->orders.front();
                if (resumableOnly && !isResumableJetOrder(order))
                    return false;
                if (order.source == ObjectOrderSource::System &&
                    order.systemPurpose != ObjectOrderSystemPurpose::Generic)
                    return false;
                runtime.pendingOrder = std::move(queue->orders.front());
                runtime.pendingOrderTail.clear();
                if (queue->orders.size() > 1) {
                    runtime.pendingOrderTail.insert(
                        runtime.pendingOrderTail.end(),
                        std::make_move_iterator(queue->orders.begin() + 1),
                        std::make_move_iterator(queue->orders.end()));
                }
                queue->orders.clear();
                ++queue->revision;
                runtime.pendingQueueRevision = queue->revision;
                runtime.pendingExternalRevision =
                    queue->externalRevision;
                return true;
            };
            const auto restorePendingOrder = [&]() {
                if (!runtime.pendingOrder) return;
                ObjectOrderQueueComponent* queue =
                    ecs::try_get<ObjectOrderQueueComponent>(
                        registry, candidate.entity);
                if (!queue) {
                    queue = &ecs::emplace<ObjectOrderQueueComponent>(
                        registry, candidate.entity);
                }
                container::Vector<ObjectOrderIntent> restored;
                restored.reserve(std::min(
                    ObjectOrderQueueComponent::MaximumQueuedOrders,
                    size_t{1} + runtime.pendingOrderTail.size() +
                        queue->orders.size()));
                restored.push_back(std::move(*runtime.pendingOrder));
                for (ObjectOrderIntent& order : runtime.pendingOrderTail) {
                    if (restored.size() >=
                        ObjectOrderQueueComponent::MaximumQueuedOrders) break;
                    restored.push_back(std::move(order));
                }
                for (ObjectOrderIntent& order : queue->orders) {
                    if (restored.size() >=
                        ObjectOrderQueueComponent::MaximumQueuedOrders) break;
                    restored.push_back(std::move(order));
                }
                queue->orders = std::move(restored);
                ++queue->revision;
                runtime.pendingOrder.reset();
                runtime.pendingOrderTail.clear();
                runtime.pendingQueueRevision = queue->revision;
                runtime.pendingExternalRevision =
                    queue->externalRevision;
            };
            const auto parkingGeometry = [&]() {
                return resolveJetParkingGeometry(
                    registry, lifecycle, content,
                    runtime.parkingReservation, rule.parkingOffsetFixed);
            };
            const auto applyTakeoffLift = [&]() {
                ObjectLocomotionComponent* locomotion =
                    ecs::try_get<ObjectLocomotionComponent>(
                        registry, candidate.entity);
                const TransformComponent* transform =
                    ecs::try_get<TransformComponent>(
                        registry, candidate.entity);
                if (!locomotion || locomotion->profiles.empty() ||
                    !transform) {
                    return;
                }
                const game::FrozenLocomotorTemplate& profile =
                    locomotion->profiles.front();
                const JetParkingGeometry geometry = parkingGeometry();
                if (!geometry.valid) return;
                const LogicFixedVec3 runway{
                    .x = geometry.runwayEnd.x - geometry.runwayStart.x,
                    .y = geometry.runwayEnd.y - geometry.runwayStart.y,
                    .z = geometry.runwayEnd.z - geometry.runwayStart.z,
                };
                const math::q32_32 takeoffDistance =
                    math::q32_32::sqrt(
                        runway.x * runway.x + runway.y * runway.y +
                        runway.z * runway.z);
                math::q32_32 ratio{int32_t{1}};
                if (takeoffDistance > math::q32_32{}) {
                    const LogicFixedVec3 current =
                        readAuthoritativeObjectPosition(
                            registry, candidate.entity, *transform);
                    const LogicFixedVec3 remaining{
                        .x = geometry.runwayEnd.x - current.x,
                        .y = geometry.runwayEnd.y - current.y,
                        .z = geometry.runwayEnd.z - current.z,
                    };
                    const math::q32_32 remainingDistance =
                        math::q32_32::sqrt(
                            remaining.x * remaining.x +
                            remaining.y * remaining.y +
                            remaining.z * remaining.z);
                    ratio = math::q32_32::clamp(
                        math::q32_32{int32_t{1}} -
                            remainingDistance / takeoffDistance,
                        math::q32_32{},
                        math::q32_32{int32_t{1}});
                    ratio *= ratio;
                }
                locomotion->lift = profile.fixed.lift * ratio;
                locomotion->damagedLift =
                    profile.fixed.damagedLift * ratio;
            };
            const auto helipadApproachHeight = [&]() {
                const std::optional<ecs::entity> airfield =
                    lifecycle.entityFromId(runtime.reservedAirfield);
                const ObjectAirfieldComponent* destination = airfield
                    ? ecs::try_get<ObjectAirfieldComponent>(registry,
                                                             *airfield)
                    : nullptr;
                return destination && destination->plan &&
                        !destination->plan->parkingPlaces.empty()
                    ? math::q32_32::max(
                          math::q32_32{int32_t{1}},
                          destination->plan->parkingPlaces.front().
                              approachHeightFixed)
                    : math::q32_32{int32_t{30}};
            };
            const auto helipadGeometry = [&]() {
                return resolveHelicopterLandingGeometry(
                    registry, lifecycle, terrain, runtime.reservedAirfield,
                    candidate.object, helipadApproachHeight());
            };
            const auto fullyHealed = [&]() {
                const ObjectHealthComponent* health =
                    ecs::try_get<ObjectHealthComponent>(registry,
                                                         candidate.entity);
                return !health || health->maximumFixed <= math::q32_32{} ||
                    health->currentFixed >= health->maximumFixed;
            };
            const auto ensureParkingReservation = [&]() {
                const ObjectId activeAirfield = producedAtHelipad
                    ? runtime.reservedAirfield
                    : runtime.parkingReservation.airfield;
                if (activeAirfield && objectAlive(
                        registry, lifecycle, activeAirfield)) {
                    return true;
                }
                runtime.parkingReservation = {};
                runtime.runwayReservation = {};
                runtime.reservedAirfield = INVALID_OBJECT_ID;

                container::Vector<ObjectId> airfields;
                if (const ObjectProducerComponent* producer =
                        ecs::try_get<ObjectProducerComponent>(
                            registry, candidate.entity);
                    producer && objectAlive(registry, lifecycle,
                                            producer->producer)) {
                    airfields.push_back(producer->producer);
                }
                struct AirfieldDistance final {
                    ObjectId object = INVALID_OBJECT_ID;
                    math::q32_32 distanceSquared{};
                };
                container::Vector<AirfieldDistance> alternatives;
                const OwnerComponent* owner = ecs::try_get<OwnerComponent>(
                    registry, candidate.entity);
                const TransformComponent* transform =
                    ecs::try_get<TransformComponent>(registry,
                                                     candidate.entity);
                const auto airfieldView = ecs::view<
                    const ObjectIdentityComponent,
                    const ObjectAirfieldComponent>(registry);
                for (const ecs::entity airfieldEntity : airfieldView) {
                    const ObjectIdentityComponent& identity =
                        airfieldView.template get<
                            const ObjectIdentityComponent>(airfieldEntity);
                    if (!identity.id || identity.id == candidate.object ||
                        !objectAlive(registry, lifecycle, identity.id) ||
                        std::find(airfields.begin(), airfields.end(),
                                  identity.id) != airfields.end()) {
                        continue;
                    }
                    const ObjectAirfieldComponent& candidateAirfield =
                        airfieldView.template get<
                            const ObjectAirfieldComponent>(airfieldEntity);
                    if (!candidateAirfield.plan ||
                        (candidateAirfield.parkingPlaces.empty() &&
                         candidateAirfield.flightDecks.empty())) {
                        continue;
                    }
                    const ObjectStatusComponent* airfieldStatus =
                        ecs::try_get<ObjectStatusComponent>(
                            registry, airfieldEntity);
                    if (airfieldStatus && airfieldStatus->hasAny(
                            game::objectStatusBit(
                                game::ObjectStatusFlag::UnderConstruction) |
                            game::objectStatusBit(
                                game::ObjectStatusFlag::Sold))) {
                        continue;
                    }
                    const ObjectMapStatusComponent* aircraftMap =
                        ecs::try_get<ObjectMapStatusComponent>(
                            registry, candidate.entity);
                    const ObjectMapStatusComponent* airfieldMap =
                        ecs::try_get<ObjectMapStatusComponent>(
                            registry, airfieldEntity);
                    if ((aircraftMap && aircraftMap->offMap) !=
                        (airfieldMap && airfieldMap->offMap)) {
                        continue;
                    }
                    const OwnerComponent* airfieldOwner =
                        ecs::try_get<OwnerComponent>(registry,
                                                     airfieldEntity);
                    if (owner && airfieldOwner &&
                        owner->player != airfieldOwner->player) {
                        if (!players || players->relationship(
                                owner->player, airfieldOwner->player) !=
                                PlayerRelationship::Allies) {
                            continue;
                        }
                    }
                    math::q32_32 distance{};
                    if (transform) {
                        const TransformComponent* airfieldTransform =
                            ecs::try_get<TransformComponent>(
                                registry, airfieldEntity);
                        if (airfieldTransform) {
                            const LogicFixedVec3 sourcePosition =
                                readAuthoritativeObjectPosition(
                                    registry, candidate.entity, *transform);
                            const LogicFixedVec3 airfieldPosition =
                                readAuthoritativeObjectPosition(
                                    registry, airfieldEntity,
                                    *airfieldTransform);
                            const math::q32_32 dx =
                                airfieldPosition.x - sourcePosition.x;
                            const math::q32_32 dy =
                                airfieldPosition.y - sourcePosition.y;
                            distance = dx * dx + dy * dy;
                        }
                    }
                    alternatives.push_back({identity.id, distance});
                }
                std::sort(alternatives.begin(), alternatives.end(),
                          [](const AirfieldDistance& left,
                             const AirfieldDistance& right) {
                              if (left.distanceSquared != right.distanceSquared)
                                  return left.distanceSquared < right.distanceSquared;
                              return left.object < right.object;
                          });
                for (const AirfieldDistance& alternative : alternatives)
                    airfields.push_back(alternative.object);

                for (const ObjectId airfield : airfields) {
                    const std::optional<ecs::entity> candidateAirfieldEntity =
                        lifecycle.entityFromId(airfield);
                    if (!candidateAirfieldEntity) continue;
                    const ObjectStatusComponent* candidateStatus =
                        ecs::try_get<ObjectStatusComponent>(
                            registry, *candidateAirfieldEntity);
                    if (candidateStatus && candidateStatus->hasAny(
                            game::objectStatusBit(
                                game::ObjectStatusFlag::UnderConstruction) |
                            game::objectStatusBit(
                                game::ObjectStatusFlag::Sold))) {
                        continue;
                    }
                    const OwnerComponent* candidateOwner =
                        ecs::try_get<OwnerComponent>(
                            registry, *candidateAirfieldEntity);
                    if (owner && candidateOwner &&
                        owner->player != candidateOwner->player &&
                        (!players || players->relationship(
                            owner->player, candidateOwner->player) !=
                            PlayerRelationship::Allies)) {
                        continue;
                    }
                    if (!producedAtHelipad) {
                        ObjectAirfieldReservation reservation;
                        if (!reserveParkingSlot(
                                registry, lifecycle, airfield,
                                candidate.object, confirmedTick,
                                reservation, outEvents)) {
                            continue;
                        }
                    }
                    runtime.reservedAirfield = airfield;
                    if (ObjectProducerComponent* producer =
                            ecs::try_get<ObjectProducerComponent>(
                                registry, candidate.entity)) {
                        producer->producer = airfield;
                    } else {
                        ecs::emplace<ObjectProducerComponent>(
                            registry, candidate.entity,
                            ObjectProducerComponent{airfield});
                    }
                    const std::optional<ecs::entity> airfieldEntity =
                        lifecycle.entityFromId(airfield);
                    const TransformComponent* airfieldTransform =
                        airfieldEntity ? ecs::try_get<TransformComponent>(
                            registry, *airfieldEntity) : nullptr;
                    if (airfieldTransform) {
                        runtime.rememberedProducerPosition =
                            readAuthoritativeObjectPosition(
                                registry, *airfieldEntity,
                                *airfieldTransform);
                        runtime.producerPositionKnown = true;
                    }
                    return true;
                }
                return false;
            };

            if (!runtime.runtimeInitializedEventEmitted)
            {
                runtime.runtimeInitializedEventEmitted = true;
                outEvents.push_back(makeRuntimeEvent(ObjectAirfieldEventKind::AircraftRuntimeInitialized,
                                                     candidate.object,
                                                     component.plan->jetAi[index].authoredOrder,
                                                     "JetAIUpdate",
                                                     index,
                                                     confirmedTick,
                                                     runtime.state,
                                                     runtime.phase));
            }
            // RefCode JetAIUpdate::update (JetAIUpdate.cpp:2110-2114) sets
            // MODELCONDITION_JETEXHAUST whenever the physics velocity is
            // non-zero AND the ALLOW_AIR_LOCO flag is set, immediately before
            // the IS_ATTACKING attack-locomotor block below.
            //
            // ALLOW_AIR_LOCO is exactly "this jet is on the NORMAL locomotor
            // set rather than TAXIING": friend_setAllowAirLoco(true) happens in
            // JetTakeoffOrLandingState::onEnter (:727, :989) and it is cleared
            // by the taxi states (:263, :504), by that state's onExit once a
            // landing has touched down (:1136) and by onObjectCreated (:1919).
            // Phase is therefore the faithful projection.
            const bool allowsAirLocomotion = [phase = runtime.phase]() noexcept {
                switch (phase) {
                case ObjectJetAirfieldPhase::TakingOff:
                case ObjectJetAirfieldPhase::Airborne:
                case ObjectJetAirfieldPhase::ReturningToBase:
                case ObjectJetAirfieldPhase::AwaitLandingClearance:
                case ObjectJetAirfieldPhase::Landing:
                case ObjectJetAirfieldPhase::ReturningToDeadAirfield:
                case ObjectJetAirfieldPhase::CirclingDeadAirfield:
                    return true;
                case ObjectJetAirfieldPhase::Parked:
                case ObjectJetAirfieldPhase::AwaitTakeoffClearance:
                case ObjectJetAirfieldPhase::TaxiToTakeoff:
                case ObjectJetAirfieldPhase::PauseBeforeTakeoff:
                case ObjectJetAirfieldPhase::TaxiToParking:
                case ObjectJetAirfieldPhase::OrientForParking:
                case ObjectJetAirfieldPhase::Reloading:
                    return false;
                }
                return false;
            }();
            // PhysicsBehavior::getVelocityMagnitude() has no single owner here:
            // locomotion owns translation for a locomotor-driven object while
            // Physics integrates free bodies. Use the same precedence
            // ObjectSquishCollide already relies on.
            bool jetVelocityNonZero = false;
            if (const ObjectLocomotionComponent* jetLocomotion =
                    ecs::try_get<ObjectLocomotionComponent>(
                        registry, candidate.entity)) {
                jetVelocityNonZero =
                    jetLocomotion->forwardSpeed != math::q32_32{} ||
                    jetLocomotion->verticalSpeed != math::q32_32{};
            } else if (const ObjectPhysicsComponent* jetPhysics =
                           ecs::try_get<ObjectPhysicsComponent>(
                               registry, candidate.entity)) {
                jetVelocityNonZero =
                    jetPhysics->velocityUnitsPerSecond.x != math::q32_32{} ||
                    jetPhysics->velocityUnitsPerSecond.y != math::q32_32{} ||
                    jetPhysics->velocityUnitsPerSecond.z != math::q32_32{};
            }
            if (const bool exhaust = jetVelocityNonZero && allowsAirLocomotion;
                exhaust != runtime.jetExhaustPublished) {
                runtime.jetExhaustPublished = exhaust;
                const game::ModelConditionMask jetExhaust =
                    game::modelConditionMaskOf(
                        game::ModelConditionFlag::JetExhaust);
                publishObjectModelConditionContribution(
                    registry, candidate.entity,
                    ObjectModelConditionContributionSource::Airfield,
                    jetExhaust,
                    exhaust ? jetExhaust : game::ModelConditionMask{},
                    confirmedTick, rule.authoredOrder);
            }
            const ObjectStatusComponent* status =
                ecs::try_get<ObjectStatusComponent>(registry,
                                                     candidate.entity);
            const bool attacking = status && status->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::IsAttacking));
            if (attacking)
            {
                runtime.attackLocomotorExpiresTick = saturatingAdd(
                    confirmedTick, millisecondsToFrames(
                        rule.attackLocomotorPersistMilliseconds,
                        rules.logicFramesPerSecond));
                runtime.attackersMissExpiresTick =
                    rule.sneakyOffsetWhenAttackingFixed != math::q32_32{}
                    ? saturatingAdd(
                          confirmedTick, millisecondsToFrames(
                              rule.attackersMissPersistMilliseconds,
                              rules.logicFramesPerSecond))
                    : 0;
            }
            else
            {
                if (runtime.attackLocomotorExpiresTick != 0 &&
                    confirmedTick >= runtime.attackLocomotorExpiresTick)
                    runtime.attackLocomotorExpiresTick = 0;
                if (runtime.attackersMissExpiresTick != 0 &&
                    confirmedTick >= runtime.attackersMissExpiresTick)
                    runtime.attackersMissExpiresTick = 0;
            }

            if (!runtime.producerPositionKnown) {
                const ObjectProducerComponent* producer =
                    ecs::try_get<ObjectProducerComponent>(
                        registry, candidate.entity);
                const std::optional<ecs::entity> producerEntity = producer
                    ? lifecycle.entityFromId(producer->producer)
                    : std::optional<ecs::entity>{};
                const ecs::entity sourceEntity = producerEntity
                    ? *producerEntity : candidate.entity;
                const TransformComponent* source = producerEntity
                    ? ecs::try_get<TransformComponent>(registry,
                                                       *producerEntity)
                    : ecs::try_get<TransformComponent>(registry,
                                                       candidate.entity);
                if (source) {
                    runtime.rememberedProducerPosition =
                        readAuthoritativeObjectPosition(
                            registry, sourceEntity, *source);
                    runtime.producerPositionKnown = true;
                }
            }

            const bool outOfReturnAmmo = content &&
                objectIsOutOfReturnToBaseAmmo(
                    registry, candidate.entity, *content);
            ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(
                    registry, candidate.entity);
            const bool idle = !attacking &&
                (!queue || queue->orders.empty());

            if (runtime.phase == ObjectJetAirfieldPhase::Airborne) {
                setAirborne(true);
                bool shouldReturn = false;
                if (outOfReturnAmmo) {
                    shouldReturn = idle;
                    if (!shouldReturn && queue && !queue->orders.empty() &&
                        isResumableJetOrder(queue->orders.front())) {
                        shouldReturn = capturePendingOrder(true);
                    }
                }
                if (!shouldReturn && idle &&
                    rule.returnToBaseIdleMilliseconds != 0) {
                    if (runtime.returnToBaseIdleDueTick == 0) {
                        runtime.returnToBaseIdleDueTick = saturatingAdd(
                            confirmedTick, millisecondsToFrames(
                                rule.returnToBaseIdleMilliseconds,
                                rules.logicFramesPerSecond));
                    } else if (confirmedTick >=
                               runtime.returnToBaseIdleDueTick) {
                        shouldReturn = true;
                    }
                } else if (!idle) {
                    runtime.returnToBaseIdleDueTick = 0;
                }
                if (shouldReturn) {
                    runtime.returnToBaseIdleDueTick = 0;
                    setPhase(ObjectJetAirfieldPhase::ReturningToBase);
                }
            } else if (runtime.phase !=
                           ObjectJetAirfieldPhase::Parked) {
                static_cast<void>(capturePendingOrder(false));
            }

            if (runtime.phase == ObjectJetAirfieldPhase::Parked) {
                if (producedAtHelipad) {
                    setPhase(ObjectJetAirfieldPhase::AwaitTakeoffClearance);
                }
                setAirborne(false);
                if (!producedAtHelipad &&
                    !runtime.parkingReservation.airfield) {
                    const ObjectProducerComponent* producer =
                        ecs::try_get<ObjectProducerComponent>(
                            registry, candidate.entity);
                    if (!producer || !producer->producer ||
                        !ensureParkingReservation()) {
                        // A map-placed/script-spawned jet without a real
                        // parking assignment starts in the air, matching
                        // getProducerLocation's source fallback.
                        setPhase(ObjectJetAirfieldPhase::Airborne);
                    }
                }
                const ObjectProducedByComponent* producedBy =
                    ecs::try_get<ObjectProducedByComponent>(
                        registry, candidate.entity);
                if (runtime.phase == ObjectJetAirfieldPhase::Parked &&
                    !runtime.productionExitCompleted && producedBy &&
                    producedBy->producer ==
                        runtime.parkingReservation.airfield) {
                    const JetParkingGeometry geometry = parkingGeometry();
                    container::Vector<LogicFixedVec3> productionRoute;
                    if (geometry.valid) {
                        if (!geometry.creation.empty()) {
                            publishJetPosition(
                                registry, candidate.entity,
                                geometry.creation.front(),
                                geometry.creationOrientationRadians);
                            productionRoute.insert(
                                productionRoute.end(),
                                geometry.creation.begin() + 1,
                                geometry.creation.end());
                        }
                        productionRoute.push_back(geometry.parking);
                        runtime.parkingOrientationRadians =
                            geometry.parkingOrientationRadians;
                    }
                    runtime.productionExitCompleted = true;
                    setPhase(ObjectJetAirfieldPhase::TaxiToParking);
                    runtime.route = std::move(productionRoute);
                }
                if (runtime.phase == ObjectJetAirfieldPhase::Parked &&
                    queue && !queue->orders.empty() &&
                    capturePendingOrder(false) && !outOfReturnAmmo) {
                    setPhase(
                        ObjectJetAirfieldPhase::AwaitTakeoffClearance);
                }
            }

            if (runtime.phase == ObjectJetAirfieldPhase::ReturningToBase) {
                setAirborne(true);
                if (!ensureParkingReservation()) {
                    const ObjectProducerComponent* producer =
                        ecs::try_get<ObjectProducerComponent>(
                            registry, candidate.entity);
                    if (!outOfReturnAmmo &&
                        (!producer || !objectAlive(
                            registry, lifecycle, producer->producer))) {
                        setPhase(ObjectJetAirfieldPhase::Airborne);
                        restorePendingOrder();
                    } else {
                        setPhase(
                            ObjectJetAirfieldPhase::ReturningToDeadAirfield);
                    }
                } else {
                    if (runtime.route.empty()) {
                        if (producedAtHelipad) {
                            const HelicopterLandingGeometry geometry =
                                helipadGeometry();
                            if (geometry.valid) {
                                runtime.helipadLandingPosition =
                                    geometry.landing;
                                runtime.helipadLandingPositionValid = true;
                                runtime.route.push_back(geometry.approach);
                            }
                        } else {
                            const JetParkingGeometry geometry =
                                parkingGeometry();
                            if (geometry.valid) {
                                runtime.route.push_back(geometry.approach);
                                runtime.parkingOrientationRadians =
                                    geometry.parkingOrientationRadians;
                            } else {
                                const std::optional<ecs::entity> airfield =
                                    lifecycle.entityFromId(
                                        runtime.parkingReservation.airfield);
                                const TransformComponent* target = airfield
                                    ? ecs::try_get<TransformComponent>(
                                        registry, *airfield) : nullptr;
                                if (target) {
                                    LogicFixedVec3 targetPosition =
                                        readAuthoritativeObjectPosition(
                                            registry, *airfield, *target);
                                    targetPosition.z += math::q32_32::max(
                                        math::q32_32{}, rule.minHeightFixed);
                                    runtime.route.push_back(targetPosition);
                                }
                            }
                        }
                    }
                    if (advanceJetRoute(registry, candidate.entity, runtime,
                                        rules, false)) {
                        setPhase(ObjectJetAirfieldPhase::AwaitLandingClearance);
                    }
                }
            }

            if (runtime.phase ==
                    ObjectJetAirfieldPhase::AwaitLandingClearance) {
                if (producedAtHelipad) {
                    if (!runtime.helipadLandingPositionValid) {
                        const HelicopterLandingGeometry geometry =
                            helipadGeometry();
                        if (geometry.valid) {
                            runtime.helipadLandingPosition = geometry.landing;
                            runtime.helipadLandingPositionValid = true;
                        }
                    }
                    if (runtime.helipadLandingPositionValid) {
                        setPhase(ObjectJetAirfieldPhase::Landing);
                        runtime.route.push_back(
                            runtime.helipadLandingPosition);
                    }
                } else {
                ObjectAirfieldReservation runway;
                if (!rule.needsRunway || reserveRunway(
                        registry, lifecycle,
                        runtime.parkingReservation.airfield,
                        candidate.object, true, confirmedTick,
                        rules.logicFramesPerSecond, runway,
                        outEvents)) {
                    if (rule.needsRunway && !runway.active) {
                        // Landing reservations are never queued by either
                        // ParkingPlace or FlightDeck, but keep this invariant
                        // explicit for future reservation policies.
                        runtime.runwayReservation = runway;
                    } else {
                        if (rule.needsRunway)
                            runtime.runwayReservation = runway;
                        const JetParkingGeometry geometry = parkingGeometry();
                        if (geometry.valid) {
                            runtime.route = rule.needsRunway
                                ? container::Vector<LogicFixedVec3>{
                                      geometry.approach,
                                      geometry.landingStart,
                                      geometry.landingEnd}
                                : container::Vector<LogicFixedVec3>{
                                      geometry.approach,
                                      geometry.parking};
                            runtime.parkingOrientationRadians =
                                geometry.parkingOrientationRadians;
                        }
                        runtime.nextRoutePoint = 0;
                        setAirborne(true);
                        container::Vector<LogicFixedVec3> route =
                            std::move(runtime.route);
                        setPhase(ObjectJetAirfieldPhase::Landing);
                        runtime.route = std::move(route);
                    }
                }
                }
            }

            if (runtime.phase == ObjectJetAirfieldPhase::Landing &&
                advanceJetRoute(registry, candidate.entity, runtime, rules,
                                false)) {
                setAirborne(false);
                if (producedAtHelipad) {
                    runtime.helipadHealingRegistered =
                        setAirfieldHealee(
                            registry, lifecycle, runtime.reservedAirfield,
                            candidate.object, true) ||
                        runtime.helipadHealingRegistered;
                    runtime.reloadStartedTick = confirmedTick;
                    runtime.reloadCompleteTick = saturatingAdd(
                        confirmedTick,
                        content ? objectAirfieldReloadDurationFrames(
                            registry, candidate.entity, *content,
                            rules.logicFramesPerSecond) : 1u);
                    setPhase(ObjectJetAirfieldPhase::Reloading);
                } else {
                if (runtime.runwayReservation.airfield) {
                    static_cast<void>(releaseRunway(
                        registry, lifecycle,
                        runtime.runwayReservation.airfield,
                        candidate.object, confirmedTick, outEvents));
                }
                if (outServiceRequests &&
                    !runtime.countermeasuresReloadedForLanding) {
                    outServiceRequests->push_back({
                        .aircraft = candidate.object,
                        .reloadCountermeasures = true,
                    });
                    runtime.countermeasuresReloadedForLanding = true;
                }
                const JetParkingGeometry geometry = parkingGeometry();
                container::Vector<LogicFixedVec3> taxiRoute;
                if (geometry.valid) {
                    if (geometry.flightDeck) {
                        taxiRoute = geometry.taxi;
                    } else {
                        taxiRoute.push_back(geometry.runwayPrep);
                        if (geometry.hasIntermediate)
                            taxiRoute.push_back(geometry.intermediate);
                    }
                    taxiRoute.push_back(geometry.parking);
                    runtime.parkingOrientationRadians =
                        geometry.parkingOrientationRadians;
                }
                setPhase(ObjectJetAirfieldPhase::TaxiToParking);
                runtime.route = std::move(taxiRoute);
                }
            }

            if (runtime.phase == ObjectJetAirfieldPhase::TaxiToParking) {
                // RefCode asks ParkingPlaceBehavior for the best assignment
                // while taxiing and appends a newly promoted slot instead of
                // restarting the route. Reservation changes are already
                // authoritative here, so compare the live authored geometry
                // with the frozen route tail and append only on an actual
                // reassignment.
                const JetParkingGeometry liveGeometry = parkingGeometry();
                if (liveGeometry.valid &&
                    (runtime.route.empty() ||
                     runtime.route.back().x != liveGeometry.parking.x ||
                     runtime.route.back().y != liveGeometry.parking.y ||
                     runtime.route.back().z != liveGeometry.parking.z)) {
                    runtime.route.push_back(liveGeometry.parking);
                    runtime.parkingOrientationRadians =
                        liveGeometry.parkingOrientationRadians;
                }
                if (advanceJetRoute(
                        registry, candidate.entity, runtime, rules, true)) {
                    setPhase(ObjectJetAirfieldPhase::OrientForParking);
                }
            }

            if (runtime.phase == ObjectJetAirfieldPhase::OrientForParking) {
                const JetParkingGeometry geometry = parkingGeometry();
                if (geometry.valid) {
                    publishJetPosition(registry, candidate.entity,
                                       geometry.parking,
                                       geometry.parkingOrientationRadians);
                }
                runtime.reloadStartedTick = confirmedTick;
                runtime.reloadCompleteTick = saturatingAdd(
                    confirmedTick,
                    content ? objectAirfieldReloadDurationFrames(
                        registry, candidate.entity, *content,
                        rules.logicFramesPerSecond) : 1u);
                setPhase(ObjectJetAirfieldPhase::Reloading);
            }

            if (runtime.phase == ObjectJetAirfieldPhase::Reloading) {
                if (outServiceRequests) {
                    outServiceRequests->push_back({
                        .aircraft = candidate.object,
                        .reloadStartedTick = runtime.reloadStartedTick,
                        .reloadCompleteTick = runtime.reloadCompleteTick,
                        .reloadWeapons = true,
                    });
                }
                if (confirmedTick >= runtime.reloadCompleteTick &&
                    (!producedAtHelipad || fullyHealed())) {
                    runtime.countermeasuresReloadedForLanding = false;
                    if (producedAtHelipad) {
                        if (runtime.helipadHealingRegistered) {
                            static_cast<void>(setAirfieldHealee(
                                registry, lifecycle,
                                runtime.reservedAirfield,
                                candidate.object, false));
                            runtime.helipadHealingRegistered = false;
                        }
                        setPhase(ObjectJetAirfieldPhase::AwaitTakeoffClearance);
                    } else {
                        setPhase(ObjectJetAirfieldPhase::Parked);
                        if (runtime.pendingOrder)
                            setPhase(ObjectJetAirfieldPhase::AwaitTakeoffClearance);
                    }
                }
            }

            if (runtime.phase ==
                    ObjectJetAirfieldPhase::AwaitTakeoffClearance) {
                if (producedAtHelipad) {
                    if (!ensureParkingReservation()) {
                        setPhase(ObjectJetAirfieldPhase::Airborne);
                        restorePendingOrder();
                    } else {
                        const TransformComponent* currentTransform =
                            ecs::try_get<TransformComponent>(
                                registry, candidate.entity);
                        if (currentTransform) {
                            LogicFixedVec3 target =
                                readAuthoritativeObjectPosition(
                                    registry, candidate.entity,
                                    *currentTransform);
                            target.z += helipadApproachHeight();
                            setPhase(ObjectJetAirfieldPhase::TakingOff);
                            runtime.route.push_back(target);
                            setAirborne(true);
                        }
                    }
                } else {
                if (!ensureParkingReservation()) {
                    setPhase(ObjectJetAirfieldPhase::Airborne);
                    restorePendingOrder();
                } else {
                    ObjectAirfieldReservation runway;
                    if (!rule.needsRunway || reserveRunway(
                            registry, lifecycle,
                            runtime.parkingReservation.airfield,
                            candidate.object, false, confirmedTick,
                            rules.logicFramesPerSecond, runway,
                            outEvents)) {
                        if (!rule.needsRunway || runway.active) {
                            if (rule.needsRunway)
                                runtime.runwayReservation = runway;
                            const JetParkingGeometry geometry = parkingGeometry();
                            container::Vector<LogicFixedVec3> taxiRoute;
                            if (geometry.valid) {
                                if (geometry.flightDeck) {
                                    if (geometry.runwayStart.x != geometry.runwayPrep.x ||
                                        geometry.runwayStart.y != geometry.runwayPrep.y ||
                                        geometry.runwayStart.z != geometry.runwayPrep.z)
                                        taxiRoute.push_back(geometry.runwayStart);
                                } else {
                                    if (geometry.hasIntermediate)
                                        taxiRoute.push_back(
                                            geometry.intermediate);
                                    taxiRoute.push_back(geometry.runwayPrep);
                                    taxiRoute.push_back(geometry.runwayStart);
                                }
                            }
                            setPhase(ObjectJetAirfieldPhase::TaxiToTakeoff);
                            runtime.route = std::move(taxiRoute);
                        }
                    }
                }
                }
            }

            if (runtime.phase == ObjectJetAirfieldPhase::TaxiToTakeoff &&
                advanceJetRoute(registry, candidate.entity, runtime, rules,
                                true)) {
                runtime.takeoffPauseUntilTick = saturatingAdd(
                    confirmedTick, millisecondsToFrames(
                        rule.takeoffPauseMilliseconds,
                        rules.logicFramesPerSecond));
                setPhase(ObjectJetAirfieldPhase::PauseBeforeTakeoff);
            }

            if (runtime.phase == ObjectJetAirfieldPhase::PauseBeforeTakeoff &&
                confirmedTick >= runtime.takeoffPauseUntilTick) {
                const JetParkingGeometry geometry = parkingGeometry();
                container::Vector<LogicFixedVec3> takeoffRoute;
                if (geometry.valid) {
                    LogicFixedVec3 runwayEnd = geometry.runwayEnd;
                    runwayEnd.z = geometry.runwayExit.z;
                    takeoffRoute.push_back(runwayEnd);
                    takeoffRoute.push_back(geometry.runwayExit);
                }
                setPhase(ObjectJetAirfieldPhase::TakingOff);
                runtime.route = std::move(takeoffRoute);
                setAirborne(true);
            }

            if (runtime.phase == ObjectJetAirfieldPhase::TakingOff) {
                applyTakeoffLift();
                const bool takeoffComplete = advanceJetRoute(
                    registry, candidate.entity, runtime, rules, false);
                if (takeoffComplete) {
                    if (producedAtHelipad &&
                        runtime.helipadHealingRegistered) {
                        static_cast<void>(setAirfieldHealee(
                            registry, lifecycle, runtime.reservedAirfield,
                            candidate.object, false));
                        runtime.helipadHealingRegistered = false;
                    }
                    if (runtime.runwayReservation.airfield) {
                        static_cast<void>(releaseRunway(
                            registry, lifecycle,
                            runtime.runwayReservation.airfield,
                            candidate.object, confirmedTick, outEvents));
                    }
                    if (!rule.keepsParkingSpaceWhenAirborne &&
                        runtime.parkingReservation.airfield) {
                        static_cast<void>(releaseParkingSlot(
                            registry, lifecycle,
                            runtime.parkingReservation.airfield,
                            candidate.object, confirmedTick, outEvents));
                    }
                    setPhase(ObjectJetAirfieldPhase::Airborne);
                    restorePendingOrder();
                }
            }

            if (runtime.phase ==
                    ObjectJetAirfieldPhase::ReturningToDeadAirfield) {
                setAirborne(true);
                if (runtime.route.empty() &&
                    runtime.producerPositionKnown) {
                    runtime.route.push_back(
                        runtime.rememberedProducerPosition);
                }
                if (advanceJetRoute(registry, candidate.entity, runtime,
                                    rules, false)) {
                    runtime.nextAirfieldSearchTick = saturatingAdd(
                        confirmedTick,
                        std::max<uint32_t>(1u,
                                          rules.logicFramesPerSecond));
                    setPhase(ObjectJetAirfieldPhase::CirclingDeadAirfield);
                }
            }

            if (runtime.phase ==
                    ObjectJetAirfieldPhase::CirclingDeadAirfield) {
                setAirborne(true);
                const ObjectHealthComponent* health =
                    ecs::try_get<ObjectHealthComponent>(
                        registry, candidate.entity);
                if (health &&
                    rule.outOfAmmoDamagePerSecondPercentFixed >
                        math::q32_32{}) {
                    const math::q32_32 amount = health->maximumFixed *
                        rule.outOfAmmoDamagePerSecondPercentFixed /
                        math::q32_32{static_cast<int32_t>(
                            std::max<uint32_t>(1u,
                                rules.logicFramesPerSecond))};
                    if (amount > math::q32_32{}) {
                        outDamage.push_back({
                            .target = candidate.object,
                            .source = INVALID_OBJECT_ID,
                            .sourceSequence = rule.authoredOrder,
                            .amount = amount,
                            .damageType = game::DamageType::UNRESISTABLE,
                            .deathType = game::DeathType::NORMAL,
                            .confirmedTick = confirmedTick,
                        });
                    }
                }
                if (confirmedTick >= runtime.nextAirfieldSearchTick) {
                    runtime.nextAirfieldSearchTick = saturatingAdd(
                        confirmedTick,
                        std::max<uint32_t>(1u,
                                          rules.logicFramesPerSecond));
                    if (ensureParkingReservation())
                        setPhase(ObjectJetAirfieldPhase::ReturningToBase);
                }
            }
            // This runs after every JetAIUpdate transition, including a
            // forced state change made by a transaction in the preceding
            // confirmed stage.  The latch guarantees one control event per
            // source lifetime rather than one per logic tick.
            synchronizeAfterburnerAudio();
        }
        for (size_t index = 0; index < component.chinookAi.size() && index < component.plan->chinookAi.size(); ++index)
        {
            ObjectChinookAiRuntime& runtime = component.chinookAi[index];
            const game::ObjectChinookAiRule& rule =
                component.plan->chinookAi[index];
            const auto setAirborne = [&](bool airborne) {
                if (ObjectAirborneComponent* value =
                        ecs::try_get<ObjectAirborneComponent>(
                            registry, candidate.entity)) {
                    value->isAirborne = airborne;
                } else {
                    ecs::emplace<ObjectAirborneComponent>(
                        registry, candidate.entity,
                        ObjectAirborneComponent{airborne});
                }
            };
            const auto capturePendingOrder = [&]() {
                ObjectOrderQueueComponent* queue =
                    ecs::try_get<ObjectOrderQueueComponent>(
                        registry, candidate.entity);
                if (!queue) return false;
                if (runtime.pendingOrder && queue->orders.empty() &&
                    queue->externalRevision !=
                        runtime.pendingExternalRevision) {
                    runtime.pendingOrder.reset();
                    runtime.pendingOrderTail.clear();
                    runtime.pendingQueueRevision = queue->revision;
                    runtime.pendingExternalRevision =
                        queue->externalRevision;
                    return true;
                }
                if (queue->orders.empty() ||
                    (runtime.pendingOrder && queue->revision ==
                         runtime.pendingQueueRevision)) {
                    return false;
                }
                runtime.pendingOrder = std::move(queue->orders.front());
                runtime.pendingOrderTail.clear();
                if (queue->orders.size() > 1) {
                    runtime.pendingOrderTail.insert(
                        runtime.pendingOrderTail.end(),
                        std::make_move_iterator(queue->orders.begin() + 1),
                        std::make_move_iterator(queue->orders.end()));
                }
                queue->orders.clear();
                ++queue->revision;
                runtime.pendingQueueRevision = queue->revision;
                runtime.pendingExternalRevision = queue->externalRevision;
                return true;
            };
            const auto restorePendingOrder = [&]() {
                if (!runtime.pendingOrder) return;
                ObjectOrderQueueComponent* queue =
                    ecs::try_get<ObjectOrderQueueComponent>(
                        registry, candidate.entity);
                if (!queue) {
                    queue = &ecs::emplace<ObjectOrderQueueComponent>(
                        registry, candidate.entity);
                }
                container::Vector<ObjectOrderIntent> restored;
                restored.reserve(std::min(
                    ObjectOrderQueueComponent::MaximumQueuedOrders,
                    size_t{1} + runtime.pendingOrderTail.size() +
                        queue->orders.size()));
                restored.push_back(std::move(*runtime.pendingOrder));
                for (ObjectOrderIntent& order : runtime.pendingOrderTail) {
                    if (restored.size() >=
                        ObjectOrderQueueComponent::MaximumQueuedOrders) break;
                    restored.push_back(std::move(order));
                }
                for (ObjectOrderIntent& order : queue->orders) {
                    if (restored.size() >=
                        ObjectOrderQueueComponent::MaximumQueuedOrders) break;
                    restored.push_back(std::move(order));
                }
                queue->orders = std::move(restored);
                ++queue->revision;
                runtime.pendingOrder.reset();
                runtime.pendingOrderTail.clear();
                runtime.pendingQueueRevision = queue->revision;
                runtime.pendingExternalRevision = queue->externalRevision;
            };
            const auto approachHeight = [&]() {
                ObjectId airfield = runtime.healingAirfield;
                if (!airfield) {
                    const ObjectProducerComponent* producer =
                        ecs::try_get<ObjectProducerComponent>(
                            registry, candidate.entity);
                    if (producer) airfield = producer->producer;
                }
                const std::optional<ecs::entity> entity =
                    lifecycle.entityFromId(airfield);
                const ObjectAirfieldComponent* destination = entity
                    ? ecs::try_get<ObjectAirfieldComponent>(registry,
                                                             *entity)
                    : nullptr;
                return destination && destination->plan &&
                        !destination->plan->parkingPlaces.empty()
                    ? math::q32_32::max(
                          math::q32_32{int32_t{1}},
                          destination->plan->parkingPlaces.front().
                              approachHeightFixed)
                    : math::q32_32{int32_t{30}};
            };
            const auto fullyHealed = [&]() {
                const ObjectHealthComponent* health =
                    ecs::try_get<ObjectHealthComponent>(registry,
                                                         candidate.entity);
                return !health || health->maximumFixed <= math::q32_32{} ||
                    health->currentFixed >= health->maximumFixed;
            };

            if (runtime.flightPhase !=
                    ObjectHelicopterFlightPhase::Airborne) {
                static_cast<void>(capturePendingOrder());
            }
            if (runtime.flightPhase ==
                    ObjectHelicopterFlightPhase::ReturningForLanding) {
                setAirborne(true);
                if (!runtime.healingAirfield || !objectAlive(
                        registry, lifecycle, runtime.healingAirfield)) {
                    runtime.flightPhase =
                        ObjectHelicopterFlightPhase::Airborne;
                    runtime.flightRoute.clear();
                    runtime.nextFlightRoutePoint = 0;
                    restorePendingOrder();
                } else {
                    if (!runtime.landingPositionValid) {
                        const HelicopterLandingGeometry geometry =
                            resolveHelicopterLandingGeometry(
                                registry, lifecycle, terrain,
                                runtime.healingAirfield, candidate.object,
                                approachHeight());
                        if (geometry.valid) {
                            runtime.landingPosition = geometry.landing;
                            runtime.landingPositionValid = true;
                            runtime.flightRoute = {geometry.approach};
                            runtime.nextFlightRoutePoint = 0;
                        }
                    }
                    if (runtime.landingPositionValid &&
                        advanceChinookFlightRoute(
                            registry, candidate.entity, runtime, rules)) {
                        runtime.flightPhase =
                            ObjectHelicopterFlightPhase::Landing;
                        runtime.flightPhaseEnteredTick = confirmedTick;
                        runtime.flightRoute = {runtime.landingPosition};
                        runtime.nextFlightRoutePoint = 0;
                    }
                }
            }
            if (runtime.flightPhase ==
                    ObjectHelicopterFlightPhase::Landing &&
                advanceChinookFlightRoute(
                    registry, candidate.entity, runtime, rules)) {
                setAirborne(false);
                runtime.flightPhase =
                    ObjectHelicopterFlightPhase::Landed;
                runtime.flightPhaseEnteredTick = confirmedTick;
                runtime.healingRegistered = setAirfieldHealee(
                    registry, lifecycle, runtime.healingAirfield,
                    candidate.object, true) || runtime.healingRegistered;
            }
            if (runtime.flightPhase ==
                    ObjectHelicopterFlightPhase::Landed && fullyHealed()) {
                if (runtime.healingRegistered) {
                    static_cast<void>(setAirfieldHealee(
                        registry, lifecycle, runtime.healingAirfield,
                        candidate.object, false));
                    runtime.healingRegistered = false;
                }
                const TransformComponent* transform =
                    ecs::try_get<TransformComponent>(registry,
                                                     candidate.entity);
                if (transform) {
                    LogicFixedVec3 target = readAuthoritativeObjectPosition(
                        registry, candidate.entity, *transform);
                    target.z += approachHeight();
                    runtime.flightRoute = {target};
                    runtime.nextFlightRoutePoint = 0;
                    runtime.flightPhase =
                        ObjectHelicopterFlightPhase::TakingOff;
                    runtime.flightPhaseEnteredTick = confirmedTick;
                    setAirborne(true);
                }
            }
            if (runtime.flightPhase ==
                    ObjectHelicopterFlightPhase::TakingOff) {
                setAirborne(true);
                if (advanceChinookFlightRoute(
                        registry, candidate.entity, runtime, rules)) {
                    runtime.flightPhase =
                        ObjectHelicopterFlightPhase::Airborne;
                    runtime.flightPhaseEnteredTick = confirmedTick;
                    runtime.flightRoute.clear();
                    runtime.nextFlightRoutePoint = 0;
                    runtime.landingPositionValid = false;
                    restorePendingOrder();
                }
            }
            if (!runtime.runtimeInitializedEventEmitted)
            {
                runtime.runtimeInitializedEventEmitted = true;
                outEvents.push_back(makeRuntimeEvent(ObjectAirfieldEventKind::AircraftRuntimeInitialized,
                                                     candidate.object,
                                                     component.plan->chinookAi[index].authoredOrder,
                                                     "ChinookAIUpdate",
                                                     index,
                                                     confirmedTick,
                                                     ObjectAircraftRuntimeState::Airborne));
                outEvents.push_back({
                    .kind = ObjectAirfieldEventKind::ChinookRopeRuntimeInitialized,
                    .object = candidate.object,
                    .moduleIndex = index,
                    .authoredOrder = component.plan->chinookAi[index].authoredOrder,
                    .moduleClass = "ChinookAIUpdate",
                    .slotCount = static_cast<uint32_t>(runtime.ropeReadyTicks.size()),
                    .confirmedTick = confirmedTick,
                });
            }

            if (objectEffectivelyDead(registry, lifecycle, candidate.object) &&
                runtime.ropesDropping)
            {
                runtime.ropesDropping = false;
                const math::q32_32 gravityPerFrame =
                    ropeGravityPerFrame(rules);
                const uint64_t lifetime = static_cast<uint64_t>(
                    std::max<uint32_t>(1u, rules.logicFramesPerSecond)) * 5u;
                for (ObjectChinookAiRuntime::Rope& rope : runtime.ropes)
                {
                    rope.released = true;
                    rope.lastUpdateTick = confirmedTick;
                    rope.expirationTick = saturatingAdd(confirmedTick, lifetime);
                    rope.currentSpeedPerFrame = gravityPerFrame *
                        math::q32_32{static_cast<int32_t>(
                            std::max<uint32_t>(
                                1u, rules.logicFramesPerSecond))};
                    rope.maximumSpeedPerFrame = rope.dropSpeedPerFrame;
                    rope.accelerationPerFrame = gravityPerFrame;
                    if (outRopeEvents)
                        outRopeEvents->push_back(chinookRopeEvent(
                            ObjectChinookRopePresentationControl::Update,
                            candidate.object, rule, rope, confirmedTick));
                }
            }

            for (auto ropeIt = runtime.ropes.begin();
                 ropeIt != runtime.ropes.end();)
            {
                ObjectChinookAiRuntime::Rope& rope = *ropeIt;
                const uint64_t terminalTick = rope.released
                    ? std::min(confirmedTick, rope.expirationTick)
                    : confirmedTick;
                const uint64_t elapsed = terminalTick > rope.lastUpdateTick
                    ? terminalTick - rope.lastUpdateTick : 0u;
                const math::q32_32 gravityMagnitude =
                    math::q32_32::abs(ropeGravityPerFrame(rules));
                for (uint64_t frame = 0; frame < elapsed; ++frame)
                {
                    advanceRopeWobble(rope, rope.wobbleRatePerFrame);
                    if (rope.released)
                    {
                        rope.verticalOffset += rope.currentSpeedPerFrame;
                        rope.currentSpeedPerFrame +=
                            rope.accelerationPerFrame;
                        rope.currentSpeedPerFrame = math::q32_32::clamp(
                            rope.currentSpeedPerFrame,
                            -rope.maximumSpeedPerFrame,
                            rope.maximumSpeedPerFrame);
                        continue;
                    }
                    if (rope.simulatedLength < rope.targetLength)
                    {
                        rope.lengthSpeedPerFrame = math::q32_32::min(
                            rope.lengthSpeedPerFrame + gravityMagnitude,
                            rope.dropSpeedPerFrame);
                        rope.simulatedLength += rope.lengthSpeedPerFrame;
                        rope.presentedLength = rope.simulatedLength;
                        if (rule.waitForRopesToDrop)
                            rope.nextDropTick = saturatingAdd(
                                rope.nextDropTick, 1u);
                    }
                }
                rope.lastUpdateTick = terminalTick;
                if (rope.ropeIndex < runtime.ropeReadyTicks.size())
                    runtime.ropeReadyTicks[rope.ropeIndex] =
                        rope.nextDropTick;
                if (rope.released && confirmedTick >= rope.expirationTick)
                {
                    if (outRopeEvents)
                        outRopeEvents->push_back(chinookRopeEvent(
                            ObjectChinookRopePresentationControl::End,
                            candidate.object, rule, rope, confirmedTick));
                    ropeIt = runtime.ropes.erase(ropeIt);
                    continue;
                }
                if (outRopeEvents)
                    outRopeEvents->push_back(chinookRopeEvent(
                        ObjectChinookRopePresentationControl::Update,
                        candidate.object, rule, rope, confirmedTick));
                ++ropeIt;
            }
        }
        for (size_t index = 0;
             index < component.spectreGunships.size() && index < component.plan->spectreGunships.size();
             ++index)
        {
            ObjectSpectreGunshipRuntime& runtime = component.spectreGunships[index];
            const game::ObjectSpectreGunshipRule& rule =
                component.plan->spectreGunships[index];
            if (!runtime.runtimeInitializedEventEmitted)
            {
                runtime.runtimeInitializedEventEmitted = true;
                ObjectAirfieldEvent event = makeRuntimeEvent(ObjectAirfieldEventKind::SpectreRuntimeInitialized,
                                                             candidate.object,
                                                             rule.authoredOrder,
                                                             "SpectreGunshipUpdate",
                                                             index,
                                                             confirmedTick,
                                                             runtime.state);
                event.dueTick = runtime.orbitEndsTick;
                event.payloadTemplate = rule.specialPowerTemplate;
                outEvents.push_back(std::move(event));
            }
            const OwnerComponent* sourceOwner =
                ecs::try_get<OwnerComponent>(registry, candidate.entity);
            const PlayerState* sourcePlayer = sourceOwner && players
                ? players->get(sourceOwner->player) : nullptr;
            const LogicFixedVec3 currentPosition =
                spectrePosition(registry, candidate.entity);

            bool outsideMap = false;
            bool atDepartureBoundary = false;
            if (terrain && terrain->isLoaded()) {
                const game::terrain::TerrainExtentRaw extent =
                    terrain->map().extentIncludingBorderRaw();
                const math::q32_32 minimumX =
                    math::q32_32::from_raw(extent.minimumX);
                const math::q32_32 minimumY =
                    math::q32_32::from_raw(extent.minimumY);
                const math::q32_32 maximumX =
                    math::q32_32::from_raw(extent.maximumX);
                const math::q32_32 maximumY =
                    math::q32_32::from_raw(extent.maximumY);
                outsideMap = currentPosition.x < minimumX ||
                    currentPosition.x > maximumX ||
                    currentPosition.y < minimumY ||
                    currentPosition.y > maximumY;
                const math::q32_32 exitBoundaryTolerance{int32_t{2}};
                atDepartureBoundary =
                    currentPosition.x <= minimumX + exitBoundaryTolerance ||
                    currentPosition.x >= maximumX - exitBoundaryTolerance ||
                    currentPosition.y <= minimumY + exitBoundaryTolerance ||
                    currentPosition.y >= maximumY - exitBoundaryTolerance;
                ObjectMapStatusComponent* map =
                    ecs::try_get<ObjectMapStatusComponent>(
                        registry, candidate.entity);
                if (!map)
                    map = &ecs::emplace<ObjectMapStatusComponent>(
                        registry, candidate.entity);
                map->offMap = outsideMap;
            } else if (const ObjectMapStatusComponent* map =
                           ecs::try_get<ObjectMapStatusComponent>(
                               registry, candidate.entity)) {
                outsideMap = map->offMap;
            }

            const auto emitDestroy = [&](ObjectId object) {
                if (!object) return;
                ObjectAirfieldEvent event = makeRuntimeEvent(
                    ObjectAirfieldEventKind::SpectreObjectDestroyRequested,
                    object, rule.authoredOrder, "SpectreGunshipUpdate",
                    index, confirmedTick, runtime.state);
                event.spectrePhase = runtime.phase;
                appendAirfieldGameplay(
                    registry, lifecycle, event, outSlowDeathPhases,
                    outDestroyRequests, nextGameplaySubmissionOrdinal);
                outEvents.push_back(std::move(event));
            };
            const auto setGattlingParalyzed = [&](bool paralyzed) {
                const std::optional<ecs::entity> gun =
                    lifecycle.entityFromId(runtime.gattling);
                if (!gun) return;
                if (paralyzed) {
                    static_cast<void>(ObjectDisabledSystem::setUntil(
                        registry, *gun, ObjectDisabledReason::Paralyzed,
                        OBJECT_DISABLED_FOREVER_TICK, confirmedTick));
                } else {
                    static_cast<void>(ObjectDisabledSystem::clear(
                        registry, *gun, ObjectDisabledReason::Paralyzed,
                        confirmedTick));
                }
            };
            const auto setPhase = [&](ObjectSpectreGunshipPhase phase) {
                if (runtime.phase == phase && !runtime.phaseEventPending)
                    return;
                runtime.phase = phase;
                runtime.phaseEnteredTick = confirmedTick;
                runtime.phaseEventPending = false;
                runtime.state = phase == ObjectSpectreGunshipPhase::Orbiting
                    ? ObjectAircraftRuntimeState::Attacking
                    : phase == ObjectSpectreGunshipPhase::Departing
                        ? ObjectAircraftRuntimeState::ReturningToBase
                        : phase == ObjectSpectreGunshipPhase::Inserting
                            ? ObjectAircraftRuntimeState::Airborne
                            : ObjectAircraftRuntimeState::Idle;
                setGattlingParalyzed(
                    phase != ObjectSpectreGunshipPhase::Orbiting);
                publishSpectreModelState(
                    registry, candidate.entity, phase, confirmedTick,
                    rule.authoredOrder);
                ObjectAirfieldEvent event = makeRuntimeEvent(
                    ObjectAirfieldEventKind::SpectrePhaseChanged,
                    candidate.object, rule.authoredOrder,
                    "SpectreGunshipUpdate", index, confirmedTick,
                    runtime.state);
                event.spectrePhase = phase;
                event.dueTick = runtime.orbitEndsTick;
                outEvents.push_back(std::move(event));

                // SpectreGunshipUpdate enables its stored Afterburner audio
                // handle on insertion and departure, then removes that same
                // handle while orbiting or idle.  phaseEventPending covers
                // the direct Inserting assignment made by the activation
                // transaction before this update pass.
                const bool afterburnerEnabled =
                    phase == ObjectSpectreGunshipPhase::Inserting ||
                    phase == ObjectSpectreGunshipPhase::Departing;
                if (afterburnerEnabled != runtime.afterburnerAudioActive) {
                    runtime.afterburnerAudioActive = afterburnerEnabled;
                    outEvents.push_back(makeRuntimeEvent(
                        afterburnerEnabled
                            ? ObjectAirfieldEventKind::AfterburnerLoopStarted
                            : ObjectAirfieldEventKind::AfterburnerLoopStopped,
                        candidate.object, rule.authoredOrder,
                        "SpectreGunshipUpdate", index, confirmedTick,
                        runtime.state));
                }
            };
            const auto beginDeparture = [&]() {
                if (runtime.phase == ObjectSpectreGunshipPhase::Departing ||
                    runtime.phase == ObjectSpectreGunshipPhase::Idle) {
                    return;
                }
                if (outRadiusDecalEvents && runtime.targetingDecalsActive) {
                    static_cast<void>(endSpectreGunshipTargeting(
                        registry, lifecycle, rules, candidate.object, index,
                        confirmedTick, *outRadiusDecalEvents));
                }
                if (runtime.gattling) {
                    emitDestroy(runtime.gattling);
                    runtime.gattling = INVALID_OBJECT_ID;
                }
                math::q32_32 yaw{};
                if (const TransformComponent* transform =
                        ecs::try_get<TransformComponent>(
                            registry, candidate.entity)) {
                    yaw = readAuthoritativeObjectYaw(
                        registry, candidate.entity, *transform);
                }
                constexpr int32_t kDepartureDistance = 100000;
                runtime.departureTarget = {
                    currentPosition.x + math::fixed_cos(yaw) *
                        math::q32_32{kDepartureDistance},
                    currentPosition.y + math::fixed_sin(yaw) *
                        math::q32_32{kDepartureDistance},
                    currentPosition.z,
                };
                setPhase(ObjectSpectreGunshipPhase::Departing);
                if (sourceOwner) {
                    setSpectreMoveOrder(
                        registry, candidate.entity, sourceOwner->player,
                        rule, runtime.departureTarget, confirmedTick);
                }
            };

            const ObjectDisabledMask terminationDisabled =
                objectDisabledBit(ObjectDisabledReason::Subdued) |
                objectDisabledBit(ObjectDisabledReason::Underpowered) |
                objectDisabledBit(ObjectDisabledReason::Emp) |
                objectDisabledBit(ObjectDisabledReason::Hacked);
            const bool terminal = objectEffectivelyDead(
                registry, lifecycle, candidate.object);
            const bool disabled =
                (objectDisabledMask(registry, candidate.entity,
                                    confirmedTick) &
                 terminationDisabled) != 0;
            if (terminal) {
                if (outRadiusDecalEvents && runtime.targetingDecalsActive) {
                    static_cast<void>(endSpectreGunshipTargeting(
                        registry, lifecycle, rules, candidate.object, index,
                        confirmedTick, *outRadiusDecalEvents));
                }
                if (runtime.gattling) emitDestroy(runtime.gattling);
                runtime.gattling = INVALID_OBJECT_ID;
                setPhase(ObjectSpectreGunshipPhase::Idle);
                continue;
            }
            if (disabled) beginDeparture();

            if (runtime.phase == ObjectSpectreGunshipPhase::Idle)
                continue;
            if (runtime.phaseEventPending) setPhase(runtime.phase);

            if (runtime.phase == ObjectSpectreGunshipPhase::Departing) {
                if ((outsideMap || atDepartureBoundary) &&
                    confirmedTick > runtime.phaseEnteredTick) {
                    emitDestroy(candidate.object);
                    setPhase(ObjectSpectreGunshipPhase::Idle);
                    runtime.cleanupRequested = true;
                } else if (sourceOwner) {
                    setSpectreMoveOrder(
                        registry, candidate.entity, sourceOwner->player,
                        rule, runtime.departureTarget, confirmedTick);
                }
                continue;
            }

            if (outRadiusDecalEvents && runtime.targetingDecalsActive) {
                static_cast<void>(updateSpectreGunshipTargeting(
                    registry, lifecycle, rules, candidate.object, index,
                    runtime.overrideTargetDestination, confirmedTick,
                    *outRadiusDecalEvents));
            }

            const math::q32_32 dx =
                currentPosition.x - runtime.initialTargetPosition.x;
            const math::q32_32 dy =
                currentPosition.y - runtime.initialTargetPosition.y;
            const math::q32_32 distance =
                math::q32_32::sqrt(dx * dx + dy * dy);
            LogicFixedVec3 perigee{};
            if (distance > math::q32_32{}) {
                perigee.x = dx / distance;
                perigee.y = dy / distance;
            } else {
                perigee.x = math::q32_32{int32_t{1}};
            }
            const LogicFixedVec3 apogee{
                -perigee.y, perigee.x, math::q32_32{}};
            const math::q32_32 minimumInsertionSlope =
                math::q32_32::from_fraction(1, 2);
            const math::q32_32 maximumInsertionSlope =
                math::q32_32::from_fraction(4, 5);
            const math::q32_32 slope = math::q32_32::max(
                minimumInsertionSlope,
                math::q32_32::min(
                    maximumInsertionSlope,
                    rule.orbitInsertionSlopeFixed));
            const math::q32_32 inverseSlope =
                math::q32_32{int32_t{1}} - slope;
            const math::q32_32 orbitRadius = math::q32_32::max(
                math::q32_32{}, rule.gunshipOrbitRadiusFixed);
            runtime.satellitePosition = {
                runtime.initialTargetPosition.x +
                    (perigee.x * slope + apogee.x * inverseSlope) *
                        orbitRadius,
                runtime.initialTargetPosition.y +
                    (perigee.y * slope + apogee.y * inverseSlope) *
                        orbitRadius,
                currentPosition.z,
            };
            if (sourceOwner) {
                setSpectreMoveOrder(
                    registry, candidate.entity, sourceOwner->player,
                    rule, runtime.satellitePosition, confirmedTick);
            }

            if (runtime.phase == ObjectSpectreGunshipPhase::Inserting &&
                distance < orbitRadius) {
                setPhase(ObjectSpectreGunshipPhase::Orbiting);
                runtime.orbitEndsTick = saturatingAdd(
                    confirmedTick, millisecondsToFrames(
                        rule.orbitMilliseconds,
                        rules.logicFramesPerSecond));
                runtime.nextHowitzerFireTick = saturatingAdd(
                    confirmedTick, std::max<uint64_t>(
                        1u, millisecondsToFrames(
                            rule.howitzerFiringRateMilliseconds,
                            rules.logicFramesPerSecond)));
            }
            if (runtime.phase != ObjectSpectreGunshipPhase::Orbiting)
                continue;
            if (confirmedTick >= runtime.orbitEndsTick) {
                beginDeparture();
                continue;
            }

            struct Target final {
                ObjectId object = INVALID_OBJECT_ID;
                ecs::entity entity = ecs::null;
                LogicFixedVec3 position{};
                math::q32_32 distance{};
            };
            const auto selectTarget = [&](const LogicFixedVec3& center,
                                          math::q32_32 radiusFixed)
                -> std::optional<Target> {
                if (!players || !sourceOwner ||
                    radiusFixed <= math::q32_32{})
                    return std::nullopt;
                const math::q32_32 radiusSquared =
                    radiusFixed * radiusFixed;
                const math::q32_32 minimumShipDistance =
                    orbitRadius * math::q32_32::from_fraction(3, 4);
                const math::q32_32 minimumShipDistanceSquared =
                    minimumShipDistance * minimumShipDistance;
                std::optional<Target> best;
                const auto targets = ecs::view<
                    const ObjectIdentityComponent,
                    const OwnerComponent>(registry);
                for (const ecs::entity targetEntity : targets) {
                    const ObjectId targetObject = targets.template get<
                        const ObjectIdentityComponent>(targetEntity).id;
                    if (!targetObject || targetObject == candidate.object ||
                        !objectAlive(registry, lifecycle, targetObject)) {
                        continue;
                    }
                    const OwnerComponent& targetOwner = targets.template get<
                        const OwnerComponent>(targetEntity);
                    if (players->relationship(
                            sourceOwner->player, targetOwner.player) !=
                        PlayerRelationship::Enemies) {
                        continue;
                    }
                    const ObjectMapStatusComponent* targetMap =
                        ecs::try_get<ObjectMapStatusComponent>(
                            registry, targetEntity);
                    if (targetMap && targetMap->offMap) continue;
                    const ObjectStatusComponent* targetStatus =
                        ecs::try_get<ObjectStatusComponent>(
                            registry, targetEntity);
                    if (targetStatus && targetStatus->hasAny(
                            game::objectStatusBit(
                                game::ObjectStatusFlag::Stealthed)) &&
                        !targetStatus->hasAny(game::objectStatusBit(
                            game::ObjectStatusFlag::Detected)) &&
                        !targetStatus->hasAny(game::objectStatusBit(
                            game::ObjectStatusFlag::Disguised))) {
                        continue;
                    }
                    const LogicFixedVec3 targetPosition =
                        spectrePosition(registry, targetEntity);
                    const math::q32_32 distanceToCenter =
                        spectreDistanceSquared2D(targetPosition, center);
                    if (distanceToCenter > radiusSquared ||
                        spectreDistanceSquared2D(
                            targetPosition, currentPosition) <=
                            minimumShipDistanceSquared) {
                        continue;
                    }
                    if (visibility && visibility->renderingActive) {
                        const ObjectGeometryComponent* geometry =
                            ecs::try_get<ObjectGeometryComponent>(
                                registry, targetEntity);
                        const math::q32_32 targetRadius = geometry
                            ? math::q32_32::max(
                                  math::q32_32{},
                                  geometry->boundingCircleRadiusFixed)
                            : math::q32_32{};
                        if (!visibility->footprintHasClearCellRaw(
                                sourceOwner->player,
                                targetPosition.x.raw(),
                                targetPosition.y.raw(),
                                targetRadius.raw())) {
                            continue;
                        }
                    }
                    if (!best || distanceToCenter < best->distance ||
                        (distanceToCenter == best->distance &&
                         targetObject < best->object)) {
                        best = Target{targetObject, targetEntity,
                                      targetPosition, distanceToCenter};
                    }
                }
                return best;
            };

            const uint64_t fireInterval = std::max<uint64_t>(
                1u, millisecondsToFrames(
                    rule.howitzerFiringRateMilliseconds,
                    rules.logicFramesPerSecond));
            if (confirmedTick >= runtime.nextHowitzerFireTick) {
                runtime.nextHowitzerFireTick = saturatingAdd(
                    runtime.nextHowitzerFireTick, fireInterval);
                if (runtime.nextHowitzerFireTick <= confirmedTick)
                    runtime.nextHowitzerFireTick = saturatingAdd(
                        confirmedTick, fireInterval);
                runtime.positionToShootAt =
                    runtime.overrideTargetDestination;
                std::optional<Target> target = selectTarget(
                    runtime.overrideTargetDestination,
                    rule.targetingReticleRadiusFixed);
                if (!target && sourcePlayer &&
                    sourcePlayer->controller !=
                        PlayerControllerKind::Human) {
                    target = selectTarget(
                        runtime.initialTargetPosition,
                        rule.attackAreaRadiusFixed);
                }
                runtime.currentTarget = target
                    ? target->object : INVALID_OBJECT_ID;
                if (target) runtime.positionToShootAt = target->position;

                const uint64_t followLagTicks = millisecondsToFrames(
                    rule.howitzerFollowLagMilliseconds,
                    rules.logicFramesPerSecond);
                if (runtime.howitzerFollowTicks > followLagTicks &&
                    content && random && outWeaponCommands &&
                    lifecycle.entityFromId(runtime.gattling) &&
                    !rule.howitzerWeaponTemplate.empty()) {
                    LogicFixedVec3 impact =
                        runtime.gattlingTargetPosition;
                    const math::q32_32 offset = math::q32_32::max(
                        math::q32_32{},
                        rule.randomOffsetForHowitzerFixed);
                    impact.x += random->fixedInclusive(-offset, offset);
                    impact.y += random->fixedInclusive(-offset, offset);
                    const game::WeaponContentId weapon =
                        content->findWeaponId(
                            rule.howitzerWeaponTemplate);
                    if (weapon) {
                        const bool fired =
                            queueObjectTransientWeaponFireAtPosition(
                                weapon, registry, candidate.entity,
                                candidate.object, impact, *content, *random,
                                runtime.nextHowitzerShotSequence++,
                                rule.authoredOrder,
                                reserveWeaponEmissionSequence(), confirmedTick,
                                *outWeaponCommands);
                        // RefCode plays HowitzerFire only after
                        // createAndFireTempWeapon succeeded.  Mirror that
                        // boundary rather than using the firing timer or the
                        // model's strafe condition as a proxy.
                        if (fired) {
                            outEvents.push_back(makeRuntimeEvent(
                                ObjectAirfieldEventKind::SpectreHowitzerFired,
                                candidate.object, rule.authoredOrder,
                                "SpectreGunshipUpdate", index, confirmedTick,
                                runtime.state));
                        }
                    }
                }
            }

            const math::q32_32 strafeStep = math::q32_32::max(
                math::q32_32{}, rule.strafingIncrementFixed);
            const math::q32_32 strafeDistance =
                math::q32_32::sqrt(spectreDistanceSquared2D(
                    runtime.positionToShootAt,
                    runtime.gattlingTargetPosition));
            const std::optional<ecs::entity> gattling =
                lifecycle.entityFromId(runtime.gattling);
            if (!gattling) {
                runtime.howitzerFollowTicks = 0;
            } else if (strafeDistance <= strafeStep ||
                       strafeDistance <= math::q32_32{}) {
                runtime.gattlingTargetPosition = runtime.positionToShootAt;
                if (runtime.howitzerFollowTicks !=
                    std::numeric_limits<uint32_t>::max()) {
                    ++runtime.howitzerFollowTicks;
                }
            } else {
                runtime.howitzerFollowTicks = 0;
                const math::q32_32 ratio = strafeStep / strafeDistance;
                runtime.gattlingTargetPosition.x +=
                    (runtime.positionToShootAt.x -
                     runtime.gattlingTargetPosition.x) * ratio;
                runtime.gattlingTargetPosition.y +=
                    (runtime.positionToShootAt.y -
                     runtime.gattlingTargetPosition.y) * ratio;
                runtime.gattlingTargetPosition.z =
                    runtime.positionToShootAt.z;
            }

            bool gattlingFired = false;
            if (gattling && content && random && outWeaponCommands) {
                gattlingFired = tryQueueObjectSlotWeaponFireAtPosition(
                    registry, *gattling, runtime.gattling,
                    game::WeaponSlot::Primary,
                    runtime.gattlingTargetPosition, *content, *random,
                    rules.logicFramesPerSecond, rule.authoredOrder,
                    reserveWeaponEmissionSequence(), confirmedTick,
                    *outWeaponCommands);
            }
            if (gattlingFired &&
                !rule.gattlingStrafeFxParticleSystem.empty()) {
                ObjectAirfieldEvent event = makeRuntimeEvent(
                    ObjectAirfieldEventKind::SpectreStrafeFx,
                    candidate.object, rule.authoredOrder,
                    "SpectreGunshipUpdate", index, confirmedTick,
                    runtime.state);
                event.spectrePhase = runtime.phase;
                event.particleSystem =
                    rule.gattlingStrafeFxParticleSystem;
                event.worldPosition = runtime.gattlingTargetPosition;
                if (terrain && terrain->isLoaded()) {
                    event.worldPosition.z =
                        math::q32_32::from_raw(terrain->groundHeightRaw(
                            event.worldPosition.x.raw(),
                            event.worldPosition.y.raw()));
                }
                outEvents.push_back(std::move(event));
            }
        }
        for (size_t index = 0;
             index < component.spectreDeployments.size() && index < component.plan->spectreDeployments.size();
             ++index)
        {
            ObjectSpectreDeploymentRuntime& runtime = component.spectreDeployments[index];
            if (!runtime.runtimeInitializedEventEmitted)
            {
                runtime.runtimeInitializedEventEmitted = true;
                const game::ObjectSpectreDeploymentRule& rule = component.plan->spectreDeployments[index];
                ObjectAirfieldEvent event = makeRuntimeEvent(ObjectAirfieldEventKind::SpectreRuntimeInitialized,
                                                             candidate.object,
                                                             rule.authoredOrder,
                                                             "SpectreGunshipDeploymentUpdate",
                                                             index,
                                                             confirmedTick);
                event.payloadTemplate = rule.gunshipTemplateName;
                outEvents.push_back(std::move(event));
            }
        }
        const ObjectTerrainLayerComponent* terrainLayer =
            ecs::try_get<ObjectTerrainLayerComponent>(registry,
                                                       candidate.entity);
        const uint32_t sourcePathfindLayer = terrainLayer
            ? terrainLayer->pathfindLayer
            : game::terrain::kGroundPathfindLayer;
        for (size_t index = 0; index < component.slowDeaths.size() && index < component.plan->slowDeaths.size();
             ++index)
        {
            ObjectAircraftSlowDeathRuntime& runtime = component.slowDeaths[index];
            const game::ObjectAircraftSlowDeathRule& rule = component.plan->slowDeaths[index];
            // The wreck's fall, tumble and spiral run before the phase timers
            // below so a hit-ground transition published this tick is not
            // followed by another airborne motion step.
            advanceAircraftSlowDeathMotion(
                registry, candidate.entity, rule, runtime, rules,
                confirmedTick);
            if (runtime.phase == ObjectAircraftSlowDeathPhase::InitialDeath &&
                confirmedTick >= runtime.secondaryDueTick)
            {
                runtime.phase = ObjectAircraftSlowDeathPhase::Secondary;
                ObjectAirfieldEvent event = makeSlowDeathEvent(candidate.object,
                                                               sourcePathfindLayer,
                                                               rule,
                                                               ObjectAircraftSlowDeathPhase::Secondary,
                                                               runtime.secondaryDueTick,
                                                               confirmedTick,
                                                               rule.fxSecondary,
                                                               rule.oclSecondary);
                event.moduleIndex = index;
                appendAirfieldGameplay(
                    registry, lifecycle, event, outSlowDeathPhases,
                    outDestroyRequests, nextGameplaySubmissionOrdinal);
                outEvents.push_back(std::move(event));
            }
            if (runtime.phase == ObjectAircraftSlowDeathPhase::HitGround && confirmedTick >= runtime.finalBlowUpDueTick)
            {
                runtime.phase = ObjectAircraftSlowDeathPhase::FinalBlowUp;
                ObjectAirfieldEvent event = makeSlowDeathEvent(candidate.object,
                                                               sourcePathfindLayer,
                                                               rule,
                                                               ObjectAircraftSlowDeathPhase::FinalBlowUp,
                                                               runtime.finalBlowUpDueTick,
                                                               confirmedTick,
                                                               rule.fxFinalBlowUp,
                                                               rule.oclFinalBlowUp,
                                                               {},
                                                               rule.finalRubbleObject);
                event.moduleIndex = index;
                appendAirfieldGameplay(
                    registry, lifecycle, event, outSlowDeathPhases,
                    outDestroyRequests, nextGameplaySubmissionOrdinal);
                outEvents.push_back(std::move(event));
            }
            if (runtime.phase != ObjectAircraftSlowDeathPhase::Alive && !runtime.terminalDestroyEventEmitted &&
                confirmedTick >= runtime.destroyDueTick)
            {
                runtime.terminalDestroyEventEmitted = true;
                ObjectAirfieldEvent event = makeRuntimeEvent(ObjectAirfieldEventKind::AircraftTerminalDestroyRequested,
                                                             candidate.object,
                                                             rule.authoredOrder,
                                                             rule.moduleClass,
                                                             index,
                                                             confirmedTick);
                event.dueTick = runtime.destroyDueTick;
                appendAirfieldGameplay(
                    registry, lifecycle, event, outSlowDeathPhases,
                    outDestroyRequests, nextGameplaySubmissionOrdinal);
                outEvents.push_back(std::move(event));
            }
            if (runtime.phase != ObjectAircraftSlowDeathPhase::Alive &&
                runtime.phase != ObjectAircraftSlowDeathPhase::FinalBlowUp && runtime.bladeDetachDueTick != 0 &&
                confirmedTick >= runtime.bladeDetachDueTick && !runtime.bladeDetachEventEmitted &&
                (!rule.fxBlade.empty() || !rule.oclBlade.empty() || !rule.oclEjectPilot.empty()))
            {
                runtime.bladeDetachEventEmitted = true;
                ObjectAirfieldEvent event = makeSlowDeathEvent(candidate.object,
                                                               sourcePathfindLayer,
                                                               rule,
                                                               ObjectAircraftSlowDeathPhase::BladeDetached,
                                                               runtime.bladeDetachDueTick,
                                                               confirmedTick,
                                                               rule.fxBlade,
                                                               rule.oclBlade);
                event.moduleIndex = index;
                event.payloadTemplate = rule.bladeObjectName;
                event.boneName = rule.bladeBoneName;
                appendAirfieldGameplay(
                    registry, lifecycle, event, outSlowDeathPhases,
                    outDestroyRequests, nextGameplaySubmissionOrdinal);
                outEvents.push_back(std::move(event));
                if (!rule.oclEjectPilot.empty())
                {
                    ObjectAirfieldEvent eject = makeSlowDeathEvent(candidate.object,
                                                                   sourcePathfindLayer,
                                                                   rule,
                                                                   ObjectAircraftSlowDeathPhase::BladeDetached,
                                                                   runtime.bladeDetachDueTick,
                                                                   confirmedTick,
                                                                   {},
                                                                   rule.oclEjectPilot);
                    eject.moduleIndex = index;
                    appendAirfieldGameplay(
                        registry, lifecycle, eject, outSlowDeathPhases,
                        outDestroyRequests, nextGameplaySubmissionOrdinal);
                    outEvents.push_back(std::move(eject));
                }
            }
        }
        size_t parkingDoorOffset = 0;
        for (size_t index = 0; index < component.parkingPlaces.size() && index < component.plan->parkingPlaces.size();
             ++index)
        {
            ObjectAirfieldParkingRuntime& runtime = component.parkingPlaces[index];
            const bool purgedSpaces = purgeDeadSlots(registry, lifecycle, runtime.spaces);
            const bool purgedRunways = purgeDeadSlots(registry, lifecycle, runtime.runwayUsers);
            const bool purgedTakeoffQueue = purgeDeadSlots(registry, lifecycle, runtime.nextTakeoffUsers);
            const bool purgedHealees = purgeDeadSlots(
                registry, lifecycle, runtime.healees);
            if (purgedHealees) {
                std::erase(runtime.healees, INVALID_OBJECT_ID);
            }
            if (ObjectProductionComponent* production =
                    ecs::try_get<ObjectProductionComponent>(
                        registry, candidate.entity);
                production && production->exitPlan &&
                production->exitPlan->kind ==
                    game::ObjectProductionExitKind::AirfieldParking) {
                for (size_t slot = 0; slot < runtime.spaces.size(); ++slot) {
                    if (setProductionDoorHoldOpen(
                            *production, parkingDoorOffset + slot,
                            static_cast<bool>(runtime.spaces[slot]),
                            confirmedTick)) {
                        markObjectDirty(
                            registry, candidate.entity,
                            ObjectDirtyDomain::ModelCondition);
                    }
                }
            }
            for (size_t runway = 0; runway < runtime.runwayUsers.size() && runway < runtime.nextTakeoffUsers.size();
                 ++runway)
            {
                if (!runtime.runwayUsers[runway] && runtime.nextTakeoffUsers[runway])
                {
                    const ObjectId advanced = runtime.nextTakeoffUsers[runway];
                    runtime.nextTakeoffUsers[runway] = INVALID_OBJECT_ID;
                    runtime.runwayUsers[runway] = advanced;
                    outEvents.push_back(makeSlotEvent(ObjectAirfieldEventKind::RunwayReservationAdvanced,
                                                      candidate.object,
                                                      advanced,
                                                      ObjectAirfieldSlotKind::TakeoffRunway,
                                                      index,
                                                      runway,
                                                      component.plan->parkingPlaces[index].authoredOrder,
                                                      "ParkingPlaceBehavior",
                                                      static_cast<uint32_t>(runtime.spaces.size()),
                                                      static_cast<uint32_t>(runtime.runwayUsers.size()),
                                                      confirmedTick));
                    ObjectAirfieldReservation reservation{
                        .airfield = candidate.object,
                        .aircraft = advanced,
                        .slotKind = ObjectAirfieldSlotKind::TakeoffRunway,
                        .moduleIndex = index,
                        .slotIndex = runway,
                        .active = true,
                    };
                    rememberAircraftRunwayReservation(registry, lifecycle, advanced, reservation);
                }
            }
            if (purgedSpaces || purgedRunways || purgedTakeoffQueue ||
                purgedHealees)
            {
                outEvents.push_back({
                    .kind = ObjectAirfieldEventKind::ParkingPurged,
                    .object = candidate.object,
                    .authoredOrder = component.plan->parkingPlaces[index].authoredOrder,
                    .moduleClass = "ParkingPlaceBehavior",
                    .slotCount = static_cast<uint32_t>(runtime.spaces.size()),
                    .runwayCount = static_cast<uint32_t>(runtime.runwayUsers.size()),
                    .confirmedTick = confirmedTick,
                });
            }
            if (confirmedTick >= runtime.nextHealTick)
            {
                const game::ObjectParkingPlaceRule& rule = component.plan->parkingPlaces[index];
                const math::q32_32 amount =
                    rule.healAmountPerSecondFixed /
                    math::q32_32{int32_t{5}};
                if (amount > math::q32_32{})
                {
                    const auto heal = [&](ObjectId aircraft)
                    {
                        if (!aircraft ||
                            !objectAlive(registry, lifecycle, aircraft) ||
                            !parkedForAirfieldHealing(
                                registry, lifecycle, aircraft))
                        {
                            return;
                        }
                        outDamage.push_back({
                            .target = aircraft,
                            .source = candidate.object,
                            .sourceSequence = rule.authoredOrder,
                            .amount = amount,
                            .damageType = game::DamageType::HEALING,
                            .deathType = game::DeathType::NONE,
                            .confirmedTick = confirmedTick,
                        });
                    };
                    for (const ObjectId aircraft : runtime.spaces)
                    {
                        heal(aircraft);
                    }
                    for (const ObjectId aircraft : runtime.healees)
                    {
                        if (std::find(runtime.spaces.begin(),
                                      runtime.spaces.end(), aircraft) ==
                            runtime.spaces.end()) {
                            heal(aircraft);
                        }
                    }
                }
                runtime.nextHealTick = saturatingAdd(
                    confirmedTick, std::max<uint64_t>(1u, millisecondsToFrames(200u, rules.logicFramesPerSecond)));
            }
            parkingDoorOffset += runtime.spaces.size();
        }
        bool automaticProductionRequested = false;
        for (size_t index = 0; index < component.flightDecks.size() && index < component.plan->flightDecks.size();
             ++index)
        {
            ObjectAirfieldFlightDeckRuntime& runtime = component.flightDecks[index];
            const game::ObjectFlightDeckRule& rule = component.plan->flightDecks[index];
            const bool purgedSpaces = purgeDeadSlots(registry, lifecycle, runtime.spaces);
            const bool purgedTakeoff = purgeDeadSlots(registry, lifecycle, runtime.takeoffRunwayUsers);
            const bool purgedLanding = purgeDeadSlots(registry, lifecycle, runtime.landingRunwayUsers);
            const size_t runwayStateCount = std::min({
                runtime.rampRaised.size(), runtime.catapultDueTicks.size(),
                runtime.lowerRampTicks.size(),
                rule.runwayDefinitions.size()});
            for (size_t runway = 0; runway < runwayStateCount; ++runway) {
                if (runtime.catapultDueTicks[runway] <= confirmedTick) {
                    runtime.catapultDueTicks[runway] =
                        std::numeric_limits<uint64_t>::max();
                    const game::ObjectFlightDeckRunwayRule& authored =
                        rule.runwayDefinitions[runway];
                    if (!authored.catapultParticleSystem.empty()) {
                        ObjectAirfieldEvent event{
                            .kind = ObjectAirfieldEventKind::
                                FlightDeckCatapultFx,
                            .object = candidate.object,
                            .moduleIndex = index,
                            .slotIndex = runway,
                            .authoredOrder = rule.authoredOrder,
                            .moduleClass = "FlightDeckBehavior",
                            .particleSystem =
                                authored.catapultParticleSystem,
                            .boneName = authored.takeoffBones[0],
                            .confirmedTick = confirmedTick,
                        };
                        outEvents.push_back(std::move(event));
                    }
                }
                if (runtime.rampRaised[runway] &&
                    runtime.lowerRampTicks[runway] <= confirmedTick) {
                    runtime.rampRaised[runway] = 0u;
                    runtime.lowerRampTicks[runway] =
                        std::numeric_limits<uint64_t>::max();
                    publishObjectModelConditionDoor(
                        registry, candidate.entity,
                        ObjectModelConditionDoorSource::Airfield,
                        runway + 1u,
                        ObjectModelConditionDoorPhase::Closing,
                        confirmedTick, rule.authoredOrder);
                }
            }
            if (purgedSpaces || purgedTakeoff || purgedLanding)
            {
                outEvents.push_back({
                    .kind = ObjectAirfieldEventKind::ParkingPurged,
                    .object = candidate.object,
                    .authoredOrder = component.plan->flightDecks[index].authoredOrder,
                    .moduleClass = "FlightDeckBehavior",
                    .slotCount = static_cast<uint32_t>(runtime.spaces.size()),
                    .runwayCount = static_cast<uint32_t>(runtime.takeoffRunwayUsers.size()),
                    .confirmedTick = confirmedTick,
                });
            }
            if (rule.cleanupMilliseconds != 0 &&
                confirmedTick >= runtime.nextCleanupTick)
            {
                const auto jetRuntime = [&](ObjectId aircraft)
                    -> ObjectJetAiRuntime* {
                    const std::optional<ecs::entity> entity =
                        lifecycle.entityFromId(aircraft);
                    ObjectAirfieldComponent* aircraftAirfield = entity
                        ? ecs::try_get<ObjectAirfieldComponent>(registry,
                                                                *entity)
                        : nullptr;
                    if (!aircraftAirfield) return nullptr;
                    const auto found = std::find_if(
                        aircraftAirfield->jetAi.begin(),
                        aircraftAirfield->jetAi.end(),
                        [&](const ObjectJetAiRuntime& jet) {
                            return jet.parkingReservation.airfield ==
                                       candidate.object &&
                                jet.parkingReservation.slotKind ==
                                    ObjectAirfieldSlotKind::FlightDeck &&
                                jet.parkingReservation.moduleIndex == index;
                        });
                    return found != aircraftAirfield->jetAi.end()
                        ? &*found : nullptr;
                };
                const auto canGiveUpSpace = [&](ObjectId aircraft) {
                    if (!aircraft ||
                        !objectAlive(registry, lifecycle, aircraft)) {
                        return true;
                    }
                    const ObjectJetAiRuntime* jet = jetRuntime(aircraft);
                    if (!jet) return false;
                    if (jet->phase == ObjectJetAirfieldPhase::TaxiToParking ||
                        jet->phase ==
                            ObjectJetAirfieldPhase::OrientForParking) {
                        return false;
                    }
                    return jet->phase != ObjectJetAirfieldPhase::Parked &&
                        jet->phase != ObjectJetAirfieldPhase::Reloading;
                };
                const auto canMoveForward = [&](ObjectId aircraft) {
                    const ObjectJetAiRuntime* jet = jetRuntime(aircraft);
                    return jet &&
                        (jet->phase == ObjectJetAirfieldPhase::Parked ||
                         jet->phase == ObjectJetAirfieldPhase::Reloading);
                };

                bool promotedAny = false;
                size_t firstSlot = 0;
                for (size_t runway = 0;
                     runway < runtime.takeoffRunwayUsers.size() &&
                         firstSlot < runtime.spaces.size();
                     ++runway)
                {
                    const size_t authoredCount =
                        runway < rule.runwayDefinitions.size()
                        ? rule.runwayDefinitions[runway].spaceBones.size()
                        : 0u;
                    const size_t slotCount = authoredCount != 0
                        ? authoredCount
                        : static_cast<size_t>(
                              std::max(0, rule.spacesPerRunway));
                    const size_t endSlot = std::min(
                        runtime.spaces.size(), firstSlot + slotCount);
                    bool promotedRunway = false;
                    for (size_t front = firstSlot;
                         front < endSlot && !promotedRunway; ++front) {
                        const ObjectId displaced = runtime.spaces[front];
                        if (!canGiveUpSpace(displaced)) continue;
                        for (size_t rear = front + 1u; rear < endSlot;
                             ++rear) {
                            const ObjectId aircraft = runtime.spaces[rear];
                            if (!aircraft || !canMoveForward(aircraft))
                                continue;

                            ObjectJetAiRuntime* moving = jetRuntime(aircraft);
                            if (!moving) continue;
                            ObjectJetAiRuntime* displacedRuntime =
                                jetRuntime(displaced);
                            runtime.spaces[front] = aircraft;
                            runtime.spaces[rear] = displaced;
                            moving->parkingReservation.slotIndex = front;
                            if (displacedRuntime)
                                displacedRuntime->parkingReservation.slotIndex =
                                    rear;

                            const JetParkingGeometry geometry =
                                resolveJetParkingGeometry(
                                    registry, lifecycle, content,
                                    moving->parkingReservation,
                                    math::q32_32{});
                            moving->state =
                                ObjectAircraftRuntimeState::Taxiing;
                            moving->phase =
                                ObjectJetAirfieldPhase::TaxiToParking;
                            moving->phaseEnteredTick = confirmedTick;
                            moving->route.clear();
                            moving->nextRoutePoint = 0;
                            if (geometry.valid) {
                                moving->route.push_back(geometry.parking);
                                moving->parkingOrientationRadians =
                                    geometry.parkingOrientationRadians;
                            }

                            ObjectAirfieldEvent event = makeSlotEvent(
                                ObjectAirfieldEventKind::
                                    ParkingAssignmentChanged,
                                candidate.object, aircraft,
                                ObjectAirfieldSlotKind::FlightDeck, index,
                                front, rule.authoredOrder,
                                "FlightDeckBehavior",
                                static_cast<uint32_t>(runtime.spaces.size()),
                                static_cast<uint32_t>(
                                    runtime.takeoffRunwayUsers.size()),
                                confirmedTick);
                            event.previousSlotIndex = rear;
                            outEvents.push_back(std::move(event));
                            outEvents.push_back(makeRuntimeEvent(
                                ObjectAirfieldEventKind::
                                AircraftStateChanged,
                                aircraft, rule.authoredOrder,
                                "FlightDeckBehavior",
                                index, confirmedTick,
                                ObjectAircraftRuntimeState::Taxiing,
                                ObjectJetAirfieldPhase::TaxiToParking));
                            promotedRunway = true;
                            promotedAny = true;
                            break;
                        }
                    }
                    firstSlot = endSlot;
                }
                const uint32_t nextDelay = promotedAny &&
                        rule.humanFollowMilliseconds != 0
                    ? rule.humanFollowMilliseconds
                    : rule.cleanupMilliseconds;
                runtime.nextCleanupTick = saturatingAdd(
                    confirmedTick,
                    std::max<uint64_t>(
                        1u, millisecondsToFrames(
                                nextDelay,
                                rules.logicFramesPerSecond)));
            }
            if (confirmedTick >= runtime.nextHealTick)
            {
                const math::q32_32 amount =
                    rule.healAmountPerSecondFixed /
                    math::q32_32{int32_t{5}};
                if (amount > math::q32_32{})
                {
                    for (const ObjectId aircraft : runtime.spaces)
                    {
                        if (!aircraft ||
                            !objectAlive(registry, lifecycle, aircraft) ||
                            !parkedForAirfieldHealing(
                                registry, lifecycle, aircraft))
                        {
                            continue;
                        }
                        outDamage.push_back({
                            .target = aircraft,
                            .source = candidate.object,
                            .sourceSequence = rule.authoredOrder,
                            .amount = amount,
                            .damageType = game::DamageType::HEALING,
                            .deathType = game::DeathType::NONE,
                            .confirmedTick = confirmedTick,
                        });
                    }
                }
                runtime.nextHealTick = saturatingAdd(
                    confirmedTick, std::max<uint64_t>(1u, millisecondsToFrames(200u, rules.logicFramesPerSecond)));
            }
            const ObjectProductionComponent* production =
                ecs::try_get<ObjectProductionComponent>(registry,
                                                         candidate.entity);
            if (!automaticProductionRequested &&
                outAutomaticProductionRequests && production &&
                production->jobs.empty() && !rule.payloadTemplate.empty() &&
                confirmedTick >= runtime.nextAllowedProductionTick &&
                std::find(runtime.spaces.begin(), runtime.spaces.end(),
                          INVALID_OBJECT_ID) != runtime.spaces.end()) {
                outAutomaticProductionRequests->push_back({
                    .producer = candidate.object,
                    .moduleIndex = index,
                    .authoredOrder = rule.authoredOrder,
                    .payloadTemplate = rule.payloadTemplate,
                    .confirmedTick = confirmedTick,
                });
                automaticProductionRequested = true;
            }
        }
        if (!component.flightDecks.empty()) {
            const bool hasAircraft = std::any_of(
                component.flightDecks.begin(), component.flightDecks.end(),
                [&](const ObjectAirfieldFlightDeckRuntime& deck) {
                    return std::any_of(
                        deck.spaces.begin(), deck.spaces.end(),
                        [&](ObjectId aircraft) {
                            return aircraft &&
                                objectAlive(registry, lifecycle, aircraft);
                        });
                });
            static_cast<void>(ObjectStatusSystem::apply(
                registry, candidate.entity,
                hasAircraft
                    ? ObjectStatusMutation{
                          .clearMask = game::objectStatusBit(
                              game::ObjectStatusFlag::NoAttack),
                          .confirmedTick = confirmedTick,
                      }
                    : ObjectStatusMutation{
                          .setMask = game::objectStatusBit(
                              game::ObjectStatusFlag::NoAttack),
                          .confirmedTick = confirmedTick,
                      }));
        }
    }
}

bool ObjectAirfieldSystem::acknowledgeAutomaticProduction(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules,
    const ObjectAirfieldAutomaticProductionRequest& request) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromId(request.producer);
    ObjectAirfieldComponent* component = entity
        ? ecs::try_get<ObjectAirfieldComponent>(registry, *entity)
        : nullptr;
    if (!component || !component->plan ||
        request.moduleIndex >= component->flightDecks.size() ||
        request.moduleIndex >= component->plan->flightDecks.size()) {
        return false;
    }
    const game::ObjectFlightDeckRule& rule =
        component->plan->flightDecks[request.moduleIndex];
    if (rule.authoredOrder != request.authoredOrder ||
        rule.payloadTemplate != request.payloadTemplate) {
        return false;
    }
    ObjectAirfieldFlightDeckRuntime& runtime =
        component->flightDecks[request.moduleIndex];
    const uint64_t replacement = millisecondsToFrames(
        rule.replacementMilliseconds, rules.logicFramesPerSecond);
    const uint64_t dockAnimation = millisecondsToFrames(
        rule.dockAnimationMilliseconds, rules.logicFramesPerSecond);
    runtime.nextAllowedProductionTick = saturatingAdd(
        request.confirmedTick, saturatingAdd(replacement, dockAnimation));
    return true;
}

} // namespace engine
