#include "game/object/simulation/structure/ObjectParticleUplinkCannon.h"
#include "core/container/string_utils.h"

#include "presentation/fx/runtime/LegacyBeamTemplate.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/component/ObjectDirty.h"
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

namespace engine {
namespace {

using Fixed = math::q32_32;
constexpr uint32_t kInvalidWaypoint = UINT32_MAX;
const Fixed kOrbitalBeamHeight{int32_t{3500}};

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    const uint64_t numerator = static_cast<uint64_t>(milliseconds) * rate;
    return (numerator + 999u) / 1000u;
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t value,
                                     uint64_t increment) noexcept {
    return increment > std::numeric_limits<uint64_t>::max() - value
        ? std::numeric_limits<uint64_t>::max() : value + increment;
}

[[nodiscard]] uint64_t saturatingSubtract(uint64_t value,
                                          uint64_t decrement) noexcept {
    return value > decrement ? value - decrement : 0;
}

[[nodiscard]] uint64_t claimEmissionSequence(uint64_t& next) noexcept {
    const uint64_t result = next;
    if (next != std::numeric_limits<uint64_t>::max()) ++next;
    return result;
}

[[nodiscard]] uint64_t mix64(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] uint64_t beamIdentity(
    ObjectId object, uint32_t authoredOrder, uint64_t activationSequence,
    ObjectParticleUplinkBeamLane lane) noexcept {
    uint64_t value = object.value;
    value ^= static_cast<uint64_t>(authoredOrder) << 32u;
    value ^= activationSequence * 0x9e3779b97f4a7c15ull;
    value ^= static_cast<uint64_t>(lane) + 0xd1b54a32d192ed03ull;
    value = mix64(value);
    return value == 0 ? 1 : value;
}

[[nodiscard]] uint32_t ticksToFrames(uint64_t ticks) noexcept {
    return static_cast<uint32_t>(std::min<uint64_t>(ticks, UINT32_MAX));
}

[[nodiscard]] const ObjectSpecialPowerRuntime* findSpecialPowerRuntime(
    const ObjectSpecialPowerComponent& powers,
    SpecialPowerContentId content) noexcept {
    const auto found = std::find_if(
        powers.instances.begin(), powers.instances.end(),
        [content](const ObjectSpecialPowerRuntime& runtime) {
            return runtime.content == content;
        });
    return found == powers.instances.end() ? nullptr : &*found;
}

[[nodiscard]] LogicFixedVec3 add(const LogicFixedVec3& left,
                                 const LogicFixedVec3& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] LogicFixedVec3 subtract(const LogicFixedVec3& left,
                                      const LogicFixedVec3& right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] LogicFixedVec3 scale(const LogicFixedVec3& value,
                                   Fixed factor) noexcept {
    return {value.x * factor, value.y * factor, value.z * factor};
}

[[nodiscard]] Fixed length2D(const LogicFixedVec3& value) noexcept {
    return Fixed::sqrt(value.x * value.x + value.y * value.y);
}

[[nodiscard]] Fixed length3D(const LogicFixedVec3& value) noexcept {
    return Fixed::sqrt(
        value.x * value.x + value.y * value.y + value.z * value.z);
}

[[nodiscard]] LogicFixedVec3 positionOf(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, entity);
    return transform
        ? readAuthoritativeObjectPosition(registry, entity, *transform)
        : LogicFixedVec3{};
}

[[nodiscard]] uint64_t proportionalTick(
    uint64_t begin, uint64_t end, uint32_t completed,
    uint32_t total) noexcept {
    if (total == 0 || end <= begin) return end;
    const uint64_t duration = end - begin;
    const uint64_t quotient = duration / total;
    const uint64_t remainder = duration % total;
    return saturatingAdd(
        begin, quotient * completed +
        (remainder * static_cast<uint64_t>(completed)) / total);
}

[[nodiscard]] Fixed widthScaleAt(
    const ObjectParticleUplinkRuntime& runtime,
    uint64_t confirmedTick) noexcept {
    const Fixed zero{};
    const Fixed one{int32_t{1}};
    if (confirmedTick < runtime.orbitalBirthTick ||
        confirmedTick >= runtime.orbitalDeathTick) {
        return zero;
    }
    if (runtime.widthGrowTicks != 0 &&
        confirmedTick < saturatingAdd(runtime.orbitalBirthTick,
                                      runtime.widthGrowTicks)) {
        const uint64_t elapsed = confirmedTick - runtime.orbitalBirthTick;
        return Fixed::clamp(Fixed::from_fraction(
            static_cast<int64_t>(std::min<uint64_t>(elapsed, INT64_MAX)),
            static_cast<int64_t>(std::min<uint64_t>(
                runtime.widthGrowTicks, INT64_MAX))), zero, one);
    }
    if (runtime.widthGrowTicks != 0 &&
        confirmedTick >= runtime.orbitalDecayStartTick) {
        const uint64_t elapsed = confirmedTick - runtime.orbitalDecayStartTick;
        return one - Fixed::clamp(Fixed::from_fraction(
            static_cast<int64_t>(std::min<uint64_t>(elapsed, INT64_MAX)),
            static_cast<int64_t>(std::min<uint64_t>(
                runtime.widthGrowTicks, INT64_MAX))), zero, one);
    }
    return one;
}

[[nodiscard]] bool abortDisabled(
    const ecs::registry& registry, ecs::entity entity,
    uint64_t confirmedTick) noexcept {
    const ObjectDisabledMask abortMask =
        objectDisabledBit(ObjectDisabledReason::Underpowered) |
        objectDisabledBit(ObjectDisabledReason::Emp) |
        objectDisabledBit(ObjectDisabledReason::Subdued) |
        objectDisabledBit(ObjectDisabledReason::Hacked);
    return (objectDisabledMask(registry, entity, confirmedTick) & abortMask) != 0;
}

void stopAudio(
    ObjectId object, const game::ObjectParticleUplinkCannonRule& rule,
    uint64_t confirmedTick,
    container::Vector<ObjectFireAudioCommand>& outAudio) {
    const container::StringView names[] = {
        rule.poweringUpSoundLoop, rule.unpackToIdleSoundLoop,
        rule.firingToPackSoundLoop,
    };
    for (const container::StringView name : names) {
        if (name.empty()) continue;
        outAudio.push_back({
            .kind = ObjectFireAudioCommandKind::StopLoop,
            .object = object,
            .eventName = container::String{name},
            .authoredOrder = rule.authoredOrder,
            .confirmedTick = confirmedTick,
        });
    }
}

void startAudio(
    ObjectId object, container::StringView eventName,
    const game::ObjectParticleUplinkCannonRule& rule,
    uint64_t confirmedTick,
    container::Vector<ObjectFireAudioCommand>& outAudio,
    uint64_t emitterKeyOverride = 0) {
    if (eventName.empty()) return;
    outAudio.push_back({
        .kind = ObjectFireAudioCommandKind::StartLoop,
        .object = object,
        .eventName = container::String{eventName},
        .emitterKeyOverride = emitterKeyOverride,
        .authoredOrder = rule.authoredOrder,
        .confirmedTick = confirmedTick,
    });
}

void stopOneAudio(
    ObjectId object, container::StringView eventName,
    const game::ObjectParticleUplinkCannonRule& rule,
    uint64_t confirmedTick,
    container::Vector<ObjectFireAudioCommand>& outAudio) {
    if (eventName.empty()) return;
    outAudio.push_back({
        .kind = ObjectFireAudioCommandKind::StopLoop,
        .object = object,
        .eventName = container::String{eventName},
        .authoredOrder = rule.authoredOrder,
        .confirmedTick = confirmedTick,
    });
}

void appendPhaseEvent(
    ObjectId object, const game::ObjectParticleUplinkCannonRule& rule,
    const ObjectParticleUplinkRuntime& runtime,
    uint64_t confirmedTick,
    container::Vector<ObjectParticleUplinkPhaseEvent>& outPhases) {
    ObjectParticleUplinkPhaseEvent event{
        .object = object,
        .phase = runtime.phase,
        .outerBonePrefix = rule.outerEffectBoneName,
        .outerBoneCount = rule.outerEffectNumBones,
        .connectorBone = rule.connectorBoneName,
        .fireBone = rule.fireBoneName,
        .authoredOrder = rule.authoredOrder,
        .activationSequence = runtime.activationSequence,
        .confirmedTick = confirmedTick,
    };
    switch (runtime.phase) {
    case ObjectParticleUplinkPhase::Charging:
        event.outerParticleSystem =
            rule.outerNodesLightFlareParticleSystem;
        break;
    case ObjectParticleUplinkPhase::Preparing:
        event.outerParticleSystem =
            rule.outerNodesMediumFlareParticleSystem;
        break;
    case ObjectParticleUplinkPhase::AlmostReady:
        event.outerParticleSystem =
            rule.outerNodesMediumFlareParticleSystem;
        event.connectorLaser = rule.connectorMediumLaserName;
        event.connectorFlare = rule.connectorMediumFlare;
        break;
    case ObjectParticleUplinkPhase::ReadyToFire:
        event.outerParticleSystem =
            rule.outerNodesMediumFlareParticleSystem;
        event.connectorLaser = rule.connectorMediumLaserName;
        event.connectorFlare = rule.connectorMediumFlare;
        event.laserBaseParticleSystem =
            rule.laserBaseLightFlareParticleSystemName;
        break;
    case ObjectParticleUplinkPhase::Firing:
        event.outerParticleSystem =
            rule.outerNodesIntenseFlareParticleSystem;
        event.connectorLaser = rule.connectorIntenseLaserName;
        event.connectorFlare = rule.connectorIntenseFlare;
        event.laserBaseParticleSystem =
            rule.laserBaseIntenseFlareParticleSystemName;
        break;
    case ObjectParticleUplinkPhase::Postfire:
        event.outerParticleSystem =
            rule.outerNodesMediumFlareParticleSystem;
        event.connectorLaser = rule.connectorMediumLaserName;
        event.connectorFlare = rule.connectorMediumFlare;
        event.laserBaseParticleSystem =
            rule.laserBaseMediumFlareParticleSystemName;
        break;
    case ObjectParticleUplinkPhase::Idle:
    case ObjectParticleUplinkPhase::Prefire:
    case ObjectParticleUplinkPhase::Packing:
        break;
    }
    outPhases.push_back(std::move(event));
}

void setPhase(
    ecs::registry& registry, ecs::entity entity, ObjectId object,
    const game::ObjectParticleUplinkCannonRule& rule,
    ObjectParticleUplinkRuntime& runtime,
    ObjectParticleUplinkPhase phase, uint64_t confirmedTick,
    container::Vector<ObjectParticleUplinkPhaseEvent>& outPhases,
    container::Vector<ObjectFireAudioCommand>& outAudio) {
    if (runtime.phase == phase) return;
    runtime.phase = phase;
    markObjectDirty(
        registry, entity,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
    switch (phase) {
    case ObjectParticleUplinkPhase::Idle:
        stopAudio(object, rule, confirmedTick, outAudio);
        break;
    case ObjectParticleUplinkPhase::Charging:
        stopOneAudio(object, rule.unpackToIdleSoundLoop, rule,
                     confirmedTick, outAudio);
        stopOneAudio(object, rule.firingToPackSoundLoop, rule,
                     confirmedTick, outAudio);
        startAudio(object, rule.poweringUpSoundLoop, rule,
                   confirmedTick, outAudio);
        break;
    case ObjectParticleUplinkPhase::Preparing:
        stopOneAudio(object, rule.firingToPackSoundLoop, rule,
                     confirmedTick, outAudio);
        startAudio(object, rule.unpackToIdleSoundLoop, rule,
                   confirmedTick, outAudio);
        break;
    case ObjectParticleUplinkPhase::ReadyToFire:
        stopOneAudio(object, rule.poweringUpSoundLoop, rule,
                     confirmedTick, outAudio);
        stopOneAudio(object, rule.firingToPackSoundLoop, rule,
                     confirmedTick, outAudio);
        break;
    case ObjectParticleUplinkPhase::Firing:
        stopOneAudio(object, rule.poweringUpSoundLoop, rule,
                     confirmedTick, outAudio);
        stopOneAudio(object, rule.unpackToIdleSoundLoop, rule,
                     confirmedTick, outAudio);
        startAudio(object, rule.firingToPackSoundLoop, rule,
                   confirmedTick, outAudio);
        runtime.nextLaunchFxTick = 0;
        break;
    case ObjectParticleUplinkPhase::AlmostReady:
    case ObjectParticleUplinkPhase::Prefire:
    case ObjectParticleUplinkPhase::Postfire:
    case ObjectParticleUplinkPhase::Packing:
        break;
    }
    appendPhaseEvent(object, rule, runtime, confirmedTick, outPhases);
}

void emitBeam(
    ObjectId object, const game::ObjectParticleUplinkCannonRule& rule,
    ObjectParticleUplinkBeamLane lane,
    ObjectParticleUplinkBeamControl control, uint64_t identity,
    const LogicFixedVec3& source, const LogicFixedVec3& target,
    uint64_t widthGrowTicks, uint64_t confirmedTick,
    container::Vector<ObjectParticleUplinkBeamEvent>& outBeams) {
    outBeams.push_back({
        .object = object,
        .lane = lane,
        .control = control,
        .identity = identity,
        .beamTemplate = rule.particleBeamLaserName,
        .sourceBone = lane == ObjectParticleUplinkBeamLane::GroundToOrbit
            ? rule.fireBoneName : container::String{},
        .sourcePosition = source,
        .targetPosition = target,
        .widthGrowFrames = ticksToFrames(widthGrowTicks),
        .authoredOrder = rule.authoredOrder,
        .confirmedTick = confirmedTick,
    });
}

void endAllBeams(
    ObjectId object, const game::ObjectParticleUplinkCannonRule& rule,
    ObjectParticleUplinkRuntime& runtime, uint64_t confirmedTick,
    container::Vector<ObjectParticleUplinkBeamEvent>& outBeams) {
    if (runtime.groundBeamAlive) {
        emitBeam(object, rule,
                 ObjectParticleUplinkBeamLane::GroundToOrbit,
                 ObjectParticleUplinkBeamControl::End,
                 runtime.groundBeamIdentity, {}, {}, 0,
                 confirmedTick, outBeams);
    }
    if (runtime.orbitalBeamAlive) {
        emitBeam(object, rule,
                 ObjectParticleUplinkBeamLane::OrbitToTarget,
                 ObjectParticleUplinkBeamControl::End,
                 runtime.orbitalBeamIdentity, {}, {}, 0,
                 confirmedTick, outBeams);
    }
    runtime.groundBeamAlive = false;
    runtime.orbitalBeamAlive = false;
    runtime.groundBeamDecaying = false;
    runtime.orbitalBeamDecaying = false;
}

[[nodiscard]] LogicFixedVec3 automaticTarget(
    const LogicFixedVec3& source,
    const game::ObjectParticleUplinkCannonRule& rule,
    const ObjectParticleUplinkRuntime& runtime,
    uint64_t confirmedTick) noexcept {
    if (runtime.orbitalDeathTick <= runtime.orbitalBirthTick)
        return runtime.initialTargetPosition;
    const Fixed factor = Fixed::clamp(Fixed::from_fraction(
        static_cast<int64_t>(std::min<uint64_t>(
            confirmedTick - runtime.orbitalBirthTick, INT64_MAX)),
        static_cast<int64_t>(std::min<uint64_t>(
            runtime.orbitalDeathTick - runtime.orbitalBirthTick,
            INT64_MAX))), Fixed{}, Fixed{int32_t{1}});
    const Fixed pi = Fixed::from_raw(13'493'037'705ll);
    const Fixed radians = factor * (pi + pi) - pi;
    const Fixed along = factor * rule.swathOfDeathDistance -
        rule.swathOfDeathDistance * Fixed::from_fraction(1, 2);
    const Fixed lateral = math::fixed_sin(radians) *
        rule.swathOfDeathAmplitude;
    LogicFixedVec3 direction = subtract(runtime.initialTargetPosition, source);
    const Fixed targetDistance = length3D(direction);
    direction.z = {};
    const Fixed directionLength = length2D(direction);
    if (directionLength <= Fixed{}) return runtime.initialTargetPosition;
    direction.x /= directionLength;
    direction.y /= directionLength;
    const LogicFixedVec3 perpendicular{-direction.y, direction.x, {}};
    LogicFixedVec3 result = add(source, add(
        scale(direction, targetDistance + along),
        scale(perpendicular, lateral)));
    result.z = runtime.initialTargetPosition.z;
    return result;
}

void advanceWaypoint(
    const game::terrain::TerrainLogic& terrain,
    ObjectParticleUplinkRuntime& runtime) {
    if (runtime.nextWaypointId == kInvalidWaypoint) return;
    const game::terrain::WaypointRecord* waypoint =
        terrain.waypointById(runtime.nextWaypointId);
    if (!waypoint || waypoint->links.empty()) {
        runtime.nextWaypointId = kInvalidWaypoint;
        return;
    }
    container::Vector<uint32_t> links = waypoint->links;
    std::sort(links.begin(), links.end());
    const uint64_t choiceSeed = mix64(
        runtime.orbitalBeamIdentity ^ runtime.nextWaypointId ^
        static_cast<uint64_t>(runtime.waypointAdvanceCount++));
    const uint32_t selected = links[choiceSeed % links.size()];
    const game::terrain::WaypointRecord* next = terrain.waypointById(selected);
    if (!next) {
        runtime.nextWaypointId = kInvalidWaypoint;
        return;
    }
    runtime.nextWaypointId = selected;
    runtime.overrideTargetDestination = LogicFixedVec3{
        math::q32_32::from_raw(next->positionRaw[0]),
        math::q32_32::from_raw(next->positionRaw[1]),
        math::q32_32::from_raw(next->positionRaw[2]),
    };
}

void moveManualTarget(
    const game::terrain::TerrainLogic& terrain,
    const game::ObjectParticleUplinkCannonRule& rule,
    ObjectParticleUplinkRuntime& runtime, uint32_t framesPerSecond) {
    LogicFixedVec3 difference = subtract(
        runtime.overrideTargetDestination, runtime.currentTargetPosition);
    const Fixed distance = length3D(difference);
    const bool doubleClicked = runtime.secondLastDrivingClickTick != 0 &&
        runtime.lastDrivingClickTick >= runtime.secondLastDrivingClickTick &&
        runtime.lastDrivingClickTick - runtime.secondLastDrivingClickTick <
            runtime.doubleClickTicks;
    const Fixed speed = runtime.targetMode ==
            ObjectParticleUplinkTargetMode::Waypoint || doubleClicked
        ? rule.manualFastDrivingSpeed : rule.manualDrivingSpeed;
    const Fixed step = speed / Fixed{static_cast<int32_t>(
        std::max<uint32_t>(1, framesPerSecond))};
    if (distance > Fixed{} && step > Fixed{}) {
        if (distance <= step) {
            runtime.currentTargetPosition =
                runtime.overrideTargetDestination;
            if (runtime.targetMode ==
                ObjectParticleUplinkTargetMode::Waypoint) {
                advanceWaypoint(terrain, runtime);
            }
        } else {
            runtime.currentTargetPosition = add(
                runtime.currentTargetPosition,
                scale(difference, step / distance));
        }
    }
    if (terrain.isLoaded()) {
        runtime.currentTargetPosition.z = Fixed::from_raw(
            terrain.groundHeightRaw(runtime.currentTargetPosition.x.raw(),
                                    runtime.currentTargetPosition.y.raw()));
    }
}

void appendDamagePulse(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId source, const LogicFixedVec3& center, Fixed radius,
    const game::ObjectParticleUplinkCannonRule& rule,
    const ObjectParticleUplinkRuntime& runtime,
    uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage) {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        LogicFixedVec3 position{};
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<
        const ObjectIdentityComponent, const TransformComponent,
        const ObjectHealthComponent>(registry);
    candidates.reserve(view.size_hint());
    const Fixed radiusSquared = radius * radius;
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const ObjectHealthComponent& health =
            view.template get<const ObjectHealthComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id) ||
            health.effectivelyDead || !health.acceptsDamage) {
            continue;
        }
        if (const ObjectMapStatusComponent* map =
                ecs::try_get<ObjectMapStatusComponent>(registry, entity);
            map && map->offMap) {
            continue;
        }
        const TransformComponent& transform =
            view.template get<const TransformComponent>(entity);
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            registry, entity, transform);
        const Fixed dx = position.x - center.x;
        const Fixed dy = position.y - center.y;
        if (dx * dx + dy * dy <= radiusSquared)
            candidates.push_back({identity.id, position});
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });
    for (const Candidate& candidate : candidates) {
        outDamage.push_back({
            .target = candidate.object,
            .source = source,
            .sourceSequence = runtime.damagePulsesMade,
            .amount = runtime.damagePerPulse,
            .damageType = rule.damageType,
            .deathType = rule.deathType,
            .confirmedTick = confirmedTick,
        });
    }
}

} // namespace

void notifyParticleUplinkSpecialPowerActivated(
    ecs::registry& registry, ecs::entity entity,
    SpecialPowerContentId specialPower, ObjectOrderSource source,
    ObjectId targetObject, const LogicFixedVec3& targetPosition,
    uint64_t activationSequence, uint64_t confirmedTick) noexcept {
    static_cast<void>(targetObject);
    ObjectParticleUplinkComponent* component =
        ecs::try_get<ObjectParticleUplinkComponent>(registry, entity);
    const ObjectIdentityComponent* identity =
        ecs::try_get<ObjectIdentityComponent>(registry, entity);
    if (!component || !component->plan || !identity || !identity->id ||
        !specialPower) {
        return;
    }
    const size_t count = std::min(
        component->plan->rules.size(), component->instances.size());
    for (size_t index = 0; index < count; ++index) {
        ObjectParticleUplinkRuntime& runtime = component->instances[index];
        const game::ObjectParticleUplinkCannonRule& rule =
            component->plan->rules[index];
        if (runtime.specialPower != specialPower) continue;

        runtime.activationSequence = activationSequence != 0
            ? activationSequence : runtime.activationSequence + 1u;
        if (runtime.activationSequence == 0) ++runtime.activationSequence;
        runtime.initialTargetPosition = targetPosition;
        runtime.currentTargetPosition = targetPosition;
        runtime.overrideTargetDestination = targetPosition;
        runtime.targetMode = source == ObjectOrderSource::Player
            ? ObjectParticleUplinkTargetMode::Manual
            : ObjectParticleUplinkTargetMode::Automatic;
        runtime.nextWaypointId = kInvalidWaypoint;
        runtime.waypointAdvanceCount = 0;
        runtime.startAttackTick = confirmedTick;
        runtime.startDecayTick = saturatingAdd(
            confirmedTick, runtime.totalFiringTicks);
        runtime.orbitalBirthTick = saturatingAdd(
            confirmedTick, runtime.beamTravelTicks);
        runtime.orbitalDecayStartTick = saturatingAdd(
            runtime.startDecayTick, runtime.beamTravelTicks);
        runtime.orbitalDeathTick = saturatingAdd(
            runtime.orbitalDecayStartTick, runtime.widthGrowTicks);
        runtime.endGroundDecayTick = saturatingAdd(
            runtime.startDecayTick, runtime.widthGrowTicks);
        runtime.nextLaunchFxTick = confirmedTick;
        runtime.nextScorchTick = runtime.orbitalBirthTick;
        runtime.nextDamagePulseTick = runtime.orbitalBirthTick;
        runtime.scorchMarksMade = 0;
        runtime.damagePulsesMade = 0;
        runtime.laserPhase = ObjectParticleUplinkLaserPhase::None;
        if (runtime.groundBeamAlive)
            runtime.pendingEndGroundBeamIdentity =
                runtime.groundBeamIdentity;
        if (runtime.orbitalBeamAlive)
            runtime.pendingEndOrbitalBeamIdentity =
                runtime.orbitalBeamIdentity;
        runtime.groundBeamIdentity = beamIdentity(
            identity->id, rule.authoredOrder, runtime.activationSequence,
            ObjectParticleUplinkBeamLane::GroundToOrbit);
        runtime.orbitalBeamIdentity = beamIdentity(
            identity->id, rule.authoredOrder, runtime.activationSequence,
            ObjectParticleUplinkBeamLane::OrbitToTarget);
        runtime.groundBeamAlive = false;
        runtime.orbitalBeamAlive = false;
        runtime.groundBeamDecaying = false;
        runtime.orbitalBeamDecaying = false;
        runtime.attackActive = true;
    }
}

void ObjectParticleUplinkCannonSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const auto plan = type && type->archetype
        ? type->archetype->particleUplinkCannonPlan : nullptr;
    if (!plan || plan->rules.empty()) {
        ecs::remove<ObjectParticleUplinkComponent>(registry, entity);
        return;
    }
    ObjectParticleUplinkComponent component{.plan = plan};
    component.instances.reserve(plan->rules.size());
    for (const game::ObjectParticleUplinkCannonRule& rule : plan->rules) {
        ObjectParticleUplinkRuntime runtime;
        if (const SpecialPowerDefinition* definition =
                content.findSpecialPower(rule.specialPowerTemplate)) {
            runtime.specialPower = definition->id;
        }
        runtime.beginChargeTicks = millisecondsToTicks(
            rule.beginChargeMilliseconds, logicFramesPerSecond);
        runtime.raiseAntennaTicks = millisecondsToTicks(
            rule.raiseAntennaMilliseconds, logicFramesPerSecond);
        runtime.readyDelayTicks = millisecondsToTicks(
            rule.readyDelayMilliseconds, logicFramesPerSecond);
        runtime.widthGrowTicks = millisecondsToTicks(
            rule.widthGrowMilliseconds, logicFramesPerSecond);
        runtime.beamTravelTicks = millisecondsToTicks(
            rule.beamTravelMilliseconds, logicFramesPerSecond);
        runtime.totalFiringTicks = millisecondsToTicks(
            rule.totalFiringMilliseconds, logicFramesPerSecond);
        runtime.launchFxDelayTicks = millisecondsToTicks(
            rule.delayBetweenLaunchFxMilliseconds, logicFramesPerSecond);
        runtime.doubleClickTicks = millisecondsToTicks(
            rule.doubleClickToFastDriveDelayMilliseconds,
            logicFramesPerSecond);
        if (const fx::LegacyBeamTemplate* beam =
                content.findLegacyBeamTemplate(rule.particleBeamLaserName);
            beam && beam->kind == fx::LegacyBeamTemplateKind::Laser &&
            beam->laser.outerBeamWidthFixed > Fixed{}) {
            runtime.templateLaserRadius =
                beam->laser.outerBeamWidthFixed /
                Fixed{int32_t{2}};
        }
        if (rule.totalDamagePulses != 0) {
            runtime.damagePerPulse = rule.damagePerSecond *
                Fixed::from_fraction(
                    static_cast<int64_t>(std::min<uint64_t>(
                        runtime.totalFiringTicks, INT64_MAX)),
                    static_cast<int64_t>(std::max<uint32_t>(
                        1, logicFramesPerSecond))) /
                Fixed{static_cast<int32_t>(std::min<uint32_t>(
                    rule.totalDamagePulses,
                    static_cast<uint32_t>(INT32_MAX)))};
        }
        runtime.nextLaunchFxTick = confirmedTick;
        component.instances.push_back(runtime);
    }
    if (ObjectParticleUplinkComponent* existing =
            ecs::try_get<ObjectParticleUplinkComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectParticleUplinkComponent>(
            registry, entity, std::move(component));
    }
}

void ObjectParticleUplinkCannonSystem::update(
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
    container::Vector<ObjectFireAudioCommand>& outAudio) const {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<
        const ObjectIdentityComponent, ObjectParticleUplinkComponent,
        const ObjectSpecialPowerComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (identity.id && lifecycle.entityFromId(identity.id))
            candidates.push_back({identity.id, entity});
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });

    for (const Candidate& candidate : candidates) {
        ObjectParticleUplinkComponent& component =
            ecs::get<ObjectParticleUplinkComponent>(
                registry, candidate.entity);
        const ObjectSpecialPowerComponent& powers =
            ecs::get<const ObjectSpecialPowerComponent>(
                registry, candidate.entity);
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, candidate.entity);
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, candidate.entity);
        const bool sold = status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Sold));
        const bool underConstruction = status && status->hasAny(
            game::objectStatusBit(
                game::ObjectStatusFlag::UnderConstruction));
        const size_t count = std::min(
            component.plan ? component.plan->rules.size() : 0u,
            component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectParticleUplinkCannonRule& rule =
                component.plan->rules[index];
            ObjectParticleUplinkRuntime& runtime =
                component.instances[index];
            if (runtime.pendingEndGroundBeamIdentity != 0) {
                emitBeam(candidate.object, rule,
                    ObjectParticleUplinkBeamLane::GroundToOrbit,
                    ObjectParticleUplinkBeamControl::End,
                    runtime.pendingEndGroundBeamIdentity, {}, {}, 0,
                    confirmedTick, outBeams);
                runtime.pendingEndGroundBeamIdentity = 0;
            }
            if (runtime.pendingEndOrbitalBeamIdentity != 0) {
                const uint64_t oldIdentity =
                    runtime.pendingEndOrbitalBeamIdentity;
                emitBeam(candidate.object, rule,
                    ObjectParticleUplinkBeamLane::OrbitToTarget,
                    ObjectParticleUplinkBeamControl::End,
                    runtime.pendingEndOrbitalBeamIdentity, {}, {}, 0,
                    confirmedTick, outBeams);
                runtime.pendingEndOrbitalBeamIdentity = 0;
                if (!rule.groundAnnihilationSoundLoop.empty()) {
                    outAudio.push_back({
                        .kind = ObjectFireAudioCommandKind::StopLoop,
                        .object = candidate.object,
                        .eventName =
                            rule.groundAnnihilationSoundLoop,
                        .emitterKeyOverride =
                            particleUplinkAudioEmitterKey(oldIdentity),
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                }
            }
            if (sold) {
                if (runtime.orbitalBeamAlive &&
                    !rule.groundAnnihilationSoundLoop.empty()) {
                    outAudio.push_back({
                        .kind = ObjectFireAudioCommandKind::StopLoop,
                        .object = candidate.object,
                        .eventName =
                            rule.groundAnnihilationSoundLoop,
                        .emitterKeyOverride =
                            particleUplinkAudioEmitterKey(
                                runtime.orbitalBeamIdentity),
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                }
                endAllBeams(candidate.object, rule, runtime,
                            confirmedTick, outBeams);
                runtime.startAttackTick = 0;
                runtime.attackActive = false;
                runtime.laserPhase = ObjectParticleUplinkLaserPhase::None;
                setPhase(registry, candidate.entity, candidate.object,
                         rule, runtime,
                         ObjectParticleUplinkPhase::Idle,
                         confirmedTick, outPhases, outAudio);
                continue;
            }
            if (underConstruction || (health && health->effectivelyDead))
                continue;

            const ObjectSpecialPowerRuntime* power =
                findSpecialPowerRuntime(powers, runtime.specialPower);
            if (!power) continue;
            const LogicFixedVec3 sourcePosition =
                positionOf(registry, candidate.entity);

            if (runtime.attackActive &&
                runtime.startAttackTick <= confirmedTick) {
                if (runtime.startDecayTick > confirmedTick &&
                    abortDisabled(registry, candidate.entity,
                                  confirmedTick)) {
                    runtime.startDecayTick = confirmedTick;
                    runtime.endGroundDecayTick = saturatingAdd(
                        confirmedTick, runtime.widthGrowTicks);
                    runtime.orbitalDecayStartTick = saturatingAdd(
                        confirmedTick, runtime.beamTravelTicks);
                    runtime.orbitalDeathTick = saturatingAdd(
                        runtime.orbitalDecayStartTick,
                        runtime.widthGrowTicks);
                }

                if (!runtime.groundBeamAlive) {
                    LogicFixedVec3 orbit = sourcePosition;
                    orbit.z += kOrbitalBeamHeight;
                    emitBeam(candidate.object, rule,
                        ObjectParticleUplinkBeamLane::GroundToOrbit,
                        ObjectParticleUplinkBeamControl::Begin,
                        runtime.groundBeamIdentity, sourcePosition, orbit,
                        runtime.widthGrowTicks, confirmedTick, outBeams);
                    runtime.groundBeamAlive = true;
                }
                if (!runtime.groundBeamDecaying &&
                    confirmedTick >= runtime.startDecayTick) {
                    LogicFixedVec3 orbit = sourcePosition;
                    orbit.z += kOrbitalBeamHeight;
                    emitBeam(candidate.object, rule,
                        ObjectParticleUplinkBeamLane::GroundToOrbit,
                        ObjectParticleUplinkBeamControl::BeginDecay,
                        runtime.groundBeamIdentity, sourcePosition, orbit,
                        runtime.widthGrowTicks, confirmedTick, outBeams);
                    runtime.groundBeamDecaying = true;
                }
                if (runtime.groundBeamAlive &&
                    confirmedTick >= runtime.endGroundDecayTick) {
                    emitBeam(candidate.object, rule,
                        ObjectParticleUplinkBeamLane::GroundToOrbit,
                        ObjectParticleUplinkBeamControl::End,
                        runtime.groundBeamIdentity, {}, {}, 0,
                        confirmedTick, outBeams);
                    runtime.groundBeamAlive = false;
                }

                if (!runtime.orbitalBeamAlive &&
                    confirmedTick >= runtime.orbitalBirthTick) {
                    runtime.laserPhase =
                        ObjectParticleUplinkLaserPhase::Born;
                    runtime.orbitalBeamAlive = true;
                    runtime.nextScorchTick = confirmedTick;
                    runtime.nextDamagePulseTick = confirmedTick;
                    LogicFixedVec3 orbit = runtime.currentTargetPosition;
                    orbit.z += kOrbitalBeamHeight;
                    emitBeam(candidate.object, rule,
                        ObjectParticleUplinkBeamLane::OrbitToTarget,
                        ObjectParticleUplinkBeamControl::Begin,
                        runtime.orbitalBeamIdentity, orbit,
                        runtime.currentTargetPosition,
                        runtime.widthGrowTicks, confirmedTick, outBeams);
                    startAudio(candidate.object,
                        rule.groundAnnihilationSoundLoop, rule,
                        confirmedTick, outAudio,
                        particleUplinkAudioEmitterKey(
                            runtime.orbitalBeamIdentity));
                }
                if (runtime.orbitalBeamAlive &&
                    !runtime.orbitalBeamDecaying &&
                    confirmedTick >= runtime.orbitalDecayStartTick) {
                    runtime.laserPhase =
                        ObjectParticleUplinkLaserPhase::Decaying;
                    runtime.orbitalBeamDecaying = true;
                    LogicFixedVec3 orbit = runtime.currentTargetPosition;
                    orbit.z += kOrbitalBeamHeight;
                    emitBeam(candidate.object, rule,
                        ObjectParticleUplinkBeamLane::OrbitToTarget,
                        ObjectParticleUplinkBeamControl::BeginDecay,
                        runtime.orbitalBeamIdentity, orbit,
                        runtime.currentTargetPosition,
                        runtime.widthGrowTicks, confirmedTick, outBeams);
                }

                const bool beamFiring =
                    confirmedTick >= runtime.orbitalBirthTick &&
                    confirmedTick < runtime.orbitalDeathTick;
                if (beamFiring) {
                    if (runtime.targetMode ==
                        ObjectParticleUplinkTargetMode::Automatic) {
                        runtime.currentTargetPosition = automaticTarget(
                            sourcePosition, rule, runtime, confirmedTick);
                        if (terrain.isLoaded()) {
                            runtime.currentTargetPosition.z =
                                Fixed::from_raw(terrain.groundHeightRaw(
                                    runtime.currentTargetPosition.x.raw(),
                                    runtime.currentTargetPosition.y.raw()));
                        }
                    } else {
                        moveManualTarget(terrain, rule, runtime,
                                         logicFramesPerSecond);
                    }

                    LogicFixedVec3 orbit = runtime.currentTargetPosition;
                    orbit.z += kOrbitalBeamHeight;
                    emitBeam(candidate.object, rule,
                        ObjectParticleUplinkBeamLane::OrbitToTarget,
                        ObjectParticleUplinkBeamControl::Update,
                        runtime.orbitalBeamIdentity, orbit,
                        runtime.currentTargetPosition,
                        runtime.widthGrowTicks, confirmedTick, outBeams);

                    const Fixed logicalRadius =
                        runtime.templateLaserRadius *
                        widthScaleAt(runtime, confirmedTick);
                    if (rule.totalScorchMarks != 0 &&
                        runtime.scorchMarksMade < rule.totalScorchMarks &&
                        confirmedTick >= runtime.nextScorchTick) {
                        ++runtime.scorchMarksMade;
                        const OwnerComponent* owner =
                            ecs::try_get<OwnerComponent>(
                                registry, candidate.entity);
                        outScorches.push_back({
                            .object = candidate.object,
                            .position = runtime.currentTargetPosition,
                            .radius = logicalRadius * rule.scorchMarkScalar,
                            .groundHitFx = rule.groundHitFx,
                            .authoredOrder = rule.authoredOrder,
                            .sequence = runtime.scorchMarksMade,
                            .confirmedTick = confirmedTick,
                        });
                        if (owner && owner->player &&
                            rule.revealRange > Fixed{}) {
                            outReveals.push_back({
                                .source = candidate.object,
                                .owner = owner->player,
                                .position = runtime.currentTargetPosition,
                                .revealRange = rule.revealRange,
                                .authoredOrder = rule.authoredOrder,
                                .scorchOrdinal = runtime.scorchMarksMade,
                                .emissionSequence = claimEmissionSequence(
                                    nextEmissionSequence),
                                .confirmedTick = confirmedTick,
                            });
                        }
                        runtime.nextScorchTick = proportionalTick(
                            runtime.orbitalBirthTick,
                            runtime.orbitalDeathTick,
                            runtime.scorchMarksMade,
                            rule.totalScorchMarks);
                    }
                    if (rule.totalDamagePulses != 0 &&
                        runtime.damagePulsesMade < rule.totalDamagePulses &&
                        confirmedTick >= runtime.nextDamagePulseTick) {
                        ++runtime.damagePulsesMade;
                        appendDamagePulse(
                            registry, lifecycle, candidate.object,
                            runtime.currentTargetPosition,
                            logicalRadius * rule.damageRadiusScalar,
                            rule, runtime, confirmedTick, outDamage);
                        const OwnerComponent* owner =
                            ecs::try_get<OwnerComponent>(
                                registry, candidate.entity);
                        const PrimaryTeamComponent* team =
                            ecs::try_get<PrimaryTeamComponent>(
                                registry, candidate.entity);
                        if (!rule.damagePulseRemnantObjectName.empty() &&
                            owner && team && team->team) {
                            outRemnants.push_back({
                                .source = candidate.object,
                                .owner = owner->player,
                                .primaryTeam = team->team,
                                .objectTemplate =
                                    rule.damagePulseRemnantObjectName,
                                .position = runtime.currentTargetPosition,
                                .authoredOrder = rule.authoredOrder,
                                .damagePulseOrdinal =
                                    runtime.damagePulsesMade,
                                .emissionSequence = claimEmissionSequence(
                                    nextEmissionSequence),
                                .confirmedTick = confirmedTick,
                            });
                        }
                        runtime.nextDamagePulseTick = proportionalTick(
                            runtime.orbitalBirthTick,
                            runtime.orbitalDeathTick,
                            runtime.damagePulsesMade,
                            rule.totalDamagePulses);
                    }
                }

                if (runtime.orbitalBeamAlive &&
                    confirmedTick >= runtime.orbitalDeathTick) {
                    emitBeam(candidate.object, rule,
                        ObjectParticleUplinkBeamLane::OrbitToTarget,
                        ObjectParticleUplinkBeamControl::End,
                        runtime.orbitalBeamIdentity, {}, {}, 0,
                        confirmedTick, outBeams);
                    runtime.orbitalBeamAlive = false;
                    runtime.laserPhase =
                        ObjectParticleUplinkLaserPhase::Dead;
                    if (!rule.groundAnnihilationSoundLoop.empty()) {
                        outAudio.push_back({
                            .kind = ObjectFireAudioCommandKind::StopLoop,
                            .object = candidate.object,
                            .eventName =
                                rule.groundAnnihilationSoundLoop,
                            .emitterKeyOverride =
                                particleUplinkAudioEmitterKey(
                                    runtime.orbitalBeamIdentity),
                            .authoredOrder = rule.authoredOrder,
                            .confirmedTick = confirmedTick,
                        });
                    }
                    runtime.startAttackTick = 0;
                    runtime.attackActive = false;
                    setPhase(registry, candidate.entity, candidate.object,
                        rule, runtime,
                        ObjectParticleUplinkPhase::Idle,
                        confirmedTick, outPhases, outAudio);
                    continue;
                }

                if (confirmedTick >= runtime.endGroundDecayTick) {
                    setPhase(registry, candidate.entity, candidate.object,
                        rule, runtime,
                        ObjectParticleUplinkPhase::Packing,
                        confirmedTick, outPhases, outAudio);
                } else if (confirmedTick >= runtime.startDecayTick) {
                    setPhase(registry, candidate.entity, candidate.object,
                        rule, runtime,
                        ObjectParticleUplinkPhase::Postfire,
                        confirmedTick, outPhases, outAudio);
                } else {
                    setPhase(registry, candidate.entity, candidate.object,
                        rule, runtime,
                        ObjectParticleUplinkPhase::Firing,
                        confirmedTick, outPhases, outAudio);
                    if (!rule.beamLaunchFx.empty() &&
                        confirmedTick >= runtime.nextLaunchFxTick) {
                        outFx.push_back({
                            .object = candidate.object,
                            .position = sourcePosition,
                            .fxList = rule.beamLaunchFx,
                            .boneName = rule.fireBoneName,
                            .authoredOrder = rule.authoredOrder,
                            .confirmedTick = confirmedTick,
                        });
                        runtime.nextLaunchFxTick = saturatingAdd(
                            confirmedTick,
                            std::max<uint64_t>(1,
                                runtime.launchFxDelayTicks));
                    }
                }
                continue;
            }

            const uint64_t readyTick = power->readyTick <= confirmedTick
                ? confirmedTick : power->readyTick;
            const uint64_t almostReadyTick = saturatingSubtract(
                readyTick, runtime.readyDelayTicks);
            const uint64_t preparingTick = saturatingSubtract(
                almostReadyTick, runtime.raiseAntennaTicks);
            const uint64_t chargingTick = saturatingSubtract(
                preparingTick, runtime.beginChargeTicks);
            ObjectParticleUplinkPhase desired =
                ObjectParticleUplinkPhase::Idle;
            if (confirmedTick >= readyTick)
                desired = ObjectParticleUplinkPhase::ReadyToFire;
            else if (confirmedTick >= almostReadyTick)
                desired = ObjectParticleUplinkPhase::AlmostReady;
            else if (confirmedTick >= preparingTick)
                desired = ObjectParticleUplinkPhase::Preparing;
            else if (confirmedTick >= chargingTick)
                desired = ObjectParticleUplinkPhase::Charging;
            else if (runtime.phase ==
                     ObjectParticleUplinkPhase::AlmostReady)
                desired = ObjectParticleUplinkPhase::AlmostReady;
            else if (runtime.phase ==
                     ObjectParticleUplinkPhase::ReadyToFire)
                desired = ObjectParticleUplinkPhase::Packing;
            else if (runtime.phase == ObjectParticleUplinkPhase::Packing)
                desired = ObjectParticleUplinkPhase::Packing;
            setPhase(registry, candidate.entity, candidate.object,
                     rule, runtime, desired,
                     confirmedTick, outPhases, outAudio);
        }
    }
}

bool ObjectParticleUplinkCannonSystem::setOverridableDestination(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, const LogicFixedVec3& destination,
    uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    ObjectParticleUplinkComponent* component = entity
        ? ecs::try_get<ObjectParticleUplinkComponent>(registry, *entity)
        : nullptr;
    if (!component ||
        isObjectDisabled(registry, *entity, confirmedTick)) {
        return false;
    }
    bool changed = false;
    for (ObjectParticleUplinkRuntime& runtime : component->instances) {
        if (!runtime.attackActive &&
            runtime.phase != ObjectParticleUplinkPhase::Prefire &&
            runtime.phase != ObjectParticleUplinkPhase::Firing &&
            runtime.phase != ObjectParticleUplinkPhase::Postfire) {
            continue;
        }
        runtime.secondLastDrivingClickTick = runtime.lastDrivingClickTick;
        runtime.lastDrivingClickTick = confirmedTick;
        runtime.overrideTargetDestination = destination;
        runtime.targetMode = ObjectParticleUplinkTargetMode::Manual;
        runtime.nextWaypointId = kInvalidWaypoint;
        changed = true;
    }
    return changed;
}

bool ObjectParticleUplinkCannonSystem::setWaypointDestination(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain, ObjectId object,
    SpecialPowerContentId specialPower, uint32_t waypointId,
    uint64_t confirmedTick) const {
    const game::terrain::WaypointRecord* waypoint =
        terrain.waypointById(waypointId);
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    ObjectParticleUplinkComponent* component = entity
        ? ecs::try_get<ObjectParticleUplinkComponent>(registry, *entity)
        : nullptr;
    if (!waypoint || !component ||
        isObjectDisabled(registry, *entity, confirmedTick)) {
        return false;
    }
    bool changed = false;
    for (ObjectParticleUplinkRuntime& runtime : component->instances) {
        if (specialPower && runtime.specialPower != specialPower) continue;
        if (!runtime.attackActive &&
            runtime.phase != ObjectParticleUplinkPhase::Prefire &&
            runtime.phase != ObjectParticleUplinkPhase::Firing &&
            runtime.phase != ObjectParticleUplinkPhase::Postfire) {
            continue;
        }
        runtime.targetMode = ObjectParticleUplinkTargetMode::Waypoint;
        runtime.nextWaypointId = waypointId;
        runtime.waypointAdvanceCount = 0;
        runtime.overrideTargetDestination = LogicFixedVec3{
            math::q32_32::from_raw(waypoint->positionRaw[0]),
            math::q32_32::from_raw(waypoint->positionRaw[1]),
            math::q32_32::from_raw(waypoint->positionRaw[2]),
        };
        // RefCode treats the supplied waypoint as the path start, not as a
        // destination to revisit. Select its first deterministic successor
        // immediately so a beam already centered on the start cannot stall.
        advanceWaypoint(terrain, runtime);
        changed = true;
    }
    return changed;
}

void ObjectParticleUplinkCannonSystem::onObjectReclaim(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick,
    container::Vector<ObjectParticleUplinkBeamEvent>& outBeams,
    container::Vector<ObjectParticleUplinkPhaseEvent>& outPhases,
    container::Vector<ObjectFireAudioCommand>& outAudio) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    ObjectParticleUplinkComponent* component = entity
        ? ecs::try_get<ObjectParticleUplinkComponent>(registry, *entity)
        : nullptr;
    if (!component || !component->plan) return;
    const size_t count = std::min(
        component->plan->rules.size(), component->instances.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectParticleUplinkCannonRule& rule =
            component->plan->rules[index];
        ObjectParticleUplinkRuntime& runtime = component->instances[index];
        if (runtime.pendingEndGroundBeamIdentity != 0) {
            emitBeam(object, rule,
                ObjectParticleUplinkBeamLane::GroundToOrbit,
                ObjectParticleUplinkBeamControl::End,
                runtime.pendingEndGroundBeamIdentity, {}, {}, 0,
                confirmedTick, outBeams);
            runtime.pendingEndGroundBeamIdentity = 0;
        }
        if (runtime.pendingEndOrbitalBeamIdentity != 0) {
            const uint64_t oldIdentity =
                runtime.pendingEndOrbitalBeamIdentity;
            emitBeam(object, rule,
                ObjectParticleUplinkBeamLane::OrbitToTarget,
                ObjectParticleUplinkBeamControl::End,
                runtime.pendingEndOrbitalBeamIdentity, {}, {}, 0,
                confirmedTick, outBeams);
            runtime.pendingEndOrbitalBeamIdentity = 0;
            if (!rule.groundAnnihilationSoundLoop.empty()) {
                outAudio.push_back({
                    .kind = ObjectFireAudioCommandKind::StopLoop,
                    .object = object,
                    .eventName = rule.groundAnnihilationSoundLoop,
                    .emitterKeyOverride =
                        particleUplinkAudioEmitterKey(oldIdentity),
                    .authoredOrder = rule.authoredOrder,
                    .confirmedTick = confirmedTick,
                });
            }
        }
        if (runtime.orbitalBeamAlive &&
            !rule.groundAnnihilationSoundLoop.empty()) {
            outAudio.push_back({
                .kind = ObjectFireAudioCommandKind::StopLoop,
                .object = object,
                .eventName = rule.groundAnnihilationSoundLoop,
                .emitterKeyOverride = particleUplinkAudioEmitterKey(
                    runtime.orbitalBeamIdentity),
                .authoredOrder = rule.authoredOrder,
                .confirmedTick = confirmedTick,
            });
        }
        endAllBeams(object, rule, runtime, confirmedTick, outBeams);
        runtime.startAttackTick = 0;
        runtime.attackActive = false;
        setPhase(registry, *entity, object, rule, runtime,
                 ObjectParticleUplinkPhase::Idle,
                 confirmedTick, outPhases, outAudio);
        stopAudio(object, rule, confirmedTick, outAudio);
    }
}

} // namespace engine
