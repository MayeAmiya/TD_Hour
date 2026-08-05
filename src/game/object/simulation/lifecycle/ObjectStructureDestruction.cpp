#include "game/object/simulation/lifecycle/ObjectStructureDestruction.h"

#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/base/SimulationRandom.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace engine {
namespace {

using Scalar = math::q32_32;

constexpr Scalar kOne{int32_t{1}};
constexpr Scalar kTwo{int32_t{2}};
constexpr Scalar kHalf = Scalar::from_fraction(1, 2);
constexpr Scalar kToppleAcceleration = Scalar::from_fraction(1, 50);
constexpr Scalar kPi = Scalar::from_raw(13493037705ll);
constexpr Scalar kHalfPi = Scalar::from_raw(6746518852ll);
constexpr Scalar kPiOverSix = Scalar::from_raw(2248839618ll);
constexpr Scalar kPiOverEight = Scalar::from_raw(1686629713ll);
constexpr Scalar kTwoPi = Scalar::from_raw(26986075409ll);
constexpr Scalar kLineSpacing{int32_t{25}};

[[nodiscard]] uint64_t mix(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] uint64_t structureKey(
    uint64_t sessionSeed, ObjectId object, ObjectId source,
    uint32_t authoredOrder, uint64_t confirmedTick) noexcept {
    uint64_t value = sessionSeed ^ 0x5354525543545552ull; // "STRUCTUR"
    value ^= static_cast<uint64_t>(object.value) * 0x9e3779b97f4a7c15ull;
    value ^= static_cast<uint64_t>(source.value) * 0xbf58476d1ce4e5b9ull;
    value ^= (static_cast<uint64_t>(authoredOrder) + 1u) << 32u;
    value ^= confirmedTick * 0x94d049bb133111ebull;
    return mix(value);
}

[[nodiscard]] Scalar randomUnit(uint64_t key, uint64_t purpose) noexcept {
    return Scalar::from_raw(static_cast<int64_t>(
        static_cast<uint32_t>(mix(key ^ purpose))));
}

[[nodiscard]] Scalar randomSigned(uint64_t key, uint64_t purpose) noexcept {
    return randomUnit(key, purpose) * kTwo - kOne;
}

[[nodiscard]] uint64_t millisecondsToFrames(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0 || framesPerSecond == 0) return 0;
    const uint64_t product = static_cast<uint64_t>(milliseconds) *
        static_cast<uint64_t>(framesPerSecond);
    return (product + 999u) / 1000u;
}

[[nodiscard]] uint64_t randomDelayFrames(
    uint64_t key, uint64_t purpose, uint32_t minimumMilliseconds,
    uint32_t maximumMilliseconds, uint32_t framesPerSecond) noexcept {
    const uint64_t minimum = millisecondsToFrames(
        minimumMilliseconds, framesPerSecond);
    const uint64_t maximum = std::max(
        minimum, millisecondsToFrames(maximumMilliseconds, framesPerSecond));
    if (maximum == minimum) return minimum;
    return minimum + mix(key ^ purpose) % (maximum - minimum + 1u);
}

[[nodiscard]] uint64_t saturatingTickAdd(
    uint64_t tick, uint64_t delta) noexcept {
    return delta > std::numeric_limits<uint64_t>::max() - tick
        ? std::numeric_limits<uint64_t>::max() : tick + delta;
}

[[nodiscard]] LogicFixedVec3 positionOf(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    if (const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, entity)) {
        return readAuthoritativeObjectPosition(registry, entity, *transform);
    }
    return {};
}

[[nodiscard]] Scalar yawOf(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    if (const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity);
        physics && physics->ownsAttitude) {
        return physics->yaw;
    }
    if (const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, entity)) {
        return readAuthoritativeObjectYaw(registry, entity, *transform);
    }
    return {};
}

void normalizeDirection(Scalar& x, Scalar& y) noexcept {
    const Scalar lengthSquared = x * x + y * y;
    if (lengthSquared <= Scalar{}) {
        x = kOne;
        y = {};
        return;
    }
    const Scalar length = Scalar::sqrt(lengthSquared);
    if (length <= Scalar{}) {
        x = kOne;
        y = {};
        return;
    }
    x /= length;
    y /= length;
}

[[nodiscard]] LogicFixedVec3 groundPosition(
    const game::terrain::TerrainLogic& terrain,
    Scalar x, Scalar y, Scalar fallbackZ) noexcept {
    return {
        .x = x,
        .y = y,
        .z = terrain.isLoaded()
            ? Scalar::from_raw(terrain.groundHeightRaw(x.raw(), y.raw()))
            : fallbackZ,
    };
}

void emitEffect(
    ObjectStructureEffectKind kind, ObjectStructureEffectAnchor anchor,
    ObjectId object, uint32_t sourcePathfindLayer,
    const LogicFixedVec3& position, Scalar orientation,
    container::StringView resource, uint32_t authoredOrder,
    uint64_t confirmedTick, uint64_t& nextSequence,
    container::Vector<ObjectStructureEffectEvent>& output) {
    if (resource.empty()) return;
    output.push_back({
        .kind = kind,
        .anchor = anchor,
        .object = object,
        .position = position,
        .orientationRadians = orientation,
        .sourcePathfindLayer = sourcePathfindLayer,
        .resource = container::String{resource},
        .authoredOrder = authoredOrder,
        .emissionSequence = nextSequence++,
        .confirmedTick = confirmedTick,
    });
    if (nextSequence == 0) ++nextSequence;
}

void emitSelected(
    ObjectStructureEffectKind kind, ObjectId object,
    uint32_t sourcePathfindLayer,
    const LogicFixedVec3& position, Scalar orientation,
    const container::Vector<container::String>& candidates,
    uint64_t randomKey, uint64_t purpose, uint32_t authoredOrder,
    uint64_t confirmedTick, uint64_t& nextSequence,
    container::Vector<ObjectStructureEffectEvent>& output) {
    if (candidates.empty()) return;
    const size_t index = static_cast<size_t>(
        mix(randomKey ^ purpose) % candidates.size());
    emitEffect(kind, ObjectStructureEffectAnchor::WorldPosition,
               object, sourcePathfindLayer, position, orientation,
               candidates[index],
               authoredOrder, confirmedTick, nextSequence, output);
}

void emitTopplePhase(
    const game::ObjectStructureToppleParameters& parameters,
    game::ObjectStructureTopplePhase phase, ObjectId object,
    uint32_t sourcePathfindLayer,
    const LogicFixedVec3& position, Scalar orientation,
    uint64_t randomKey, uint32_t& phaseSequence, uint32_t authoredOrder,
    uint64_t confirmedTick, uint64_t& nextSequence,
    container::Vector<ObjectStructureEffectEvent>& output) {
    const size_t index = static_cast<size_t>(phase);
    // StructureToppleUpdate passes INVALID_ANGLE to every phase OCL in the
    // reference implementation.  The modern OCL executor canonicalizes that
    // sentinel to zero; the building's yaw is still retained independently
    // for topple FX, crushing geometry and render attitude.
    static_cast<void>(orientation);
    emitSelected(
        ObjectStructureEffectKind::ObjectCreationList, object,
        sourcePathfindLayer, position, Scalar{}, parameters.ocls[index],
        randomKey,
        0x544f50434c000000ull ^
            (static_cast<uint64_t>(index) << 16u) ^ phaseSequence++,
        authoredOrder, confirmedTick, nextSequence, output);
}

void emitCollapsePhase(
    const game::ObjectStructureCollapseParameters& parameters,
    game::ObjectStructureCollapsePhase phase, ObjectId object,
    uint32_t sourcePathfindLayer,
    const LogicFixedVec3& position, Scalar orientation,
    uint64_t randomKey, uint32_t& phaseSequence, uint32_t authoredOrder,
    uint64_t confirmedTick, uint64_t& nextSequence,
    container::Vector<ObjectStructureEffectEvent>& output) {
    const size_t index = static_cast<size_t>(phase);
    const uint64_t ordinal = phaseSequence++;
    emitSelected(
        ObjectStructureEffectKind::FxList, object, sourcePathfindLayer,
        position, orientation,
        parameters.fx[index], randomKey,
        0x434f4c4658000000ull ^
            (static_cast<uint64_t>(index) << 16u) ^ ordinal,
        authoredOrder, confirmedTick, nextSequence, output);
    emitSelected(
        ObjectStructureEffectKind::ObjectCreationList, object,
        sourcePathfindLayer, position,
        orientation, parameters.ocls[index], randomKey,
        0x434f4c4f434c0000ull ^
            (static_cast<uint64_t>(index) << 16u) ^ ordinal,
        authoredOrder, confirmedTick, nextSequence, output);
}

void setPostCollapseModelCondition(
    ecs::registry& registry, ecs::entity entity) {
    RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(registry, entity);
    if (!visual) return;
    const game::ModelConditionMask rubble =
        game::modelConditionMaskOf(game::ModelConditionFlag::Rubble);
    const game::ModelConditionMask post =
        game::modelConditionMaskOf(game::ModelConditionFlag::PostCollapse);
    visual->modelConditionFlags.clear(rubble);
    for (size_t index = 0;
         index < visual->modelConditionFlags.words.size(); ++index) {
        visual->modelConditionFlags.words[index] |= post.words[index];
    }
}

void requestBoneStop(
    ObjectId object, uint32_t authoredOrder, uint64_t confirmedTick,
    container::Vector<ObjectBoneFxStopRequest>& output) {
    output.push_back({
        .object = object,
        .callerAuthoredOrder = authoredOrder,
        .confirmedTick = confirmedTick,
    });
}

[[nodiscard]] bool damageFxAccepted(
    const game::ObjectStructureToppleParameters& parameters,
    game::DamageType damageType) noexcept {
    const uint8_t index = static_cast<uint8_t>(damageType);
    return index < 64 &&
        (parameters.damageFxTypes & (uint64_t{1} << index)) != 0;
}

[[nodiscard]] const game::ObjectDeathReactionRule* runtimeRule(
    const container::SharedPtr<const game::ObjectDeathReactionPlan>& plan,
    uint32_t ruleIndex) noexcept {
    return plan && ruleIndex < plan->rules.size()
        ? &plan->rules[ruleIndex] : nullptr;
}

} // namespace

bool ObjectStructureDestructionSystem::begin(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity entity, ObjectId object, ObjectId damageSource,
    game::DamageType damageType,
    container::SharedPtr<const game::ObjectDeathReactionPlan> plan,
    uint32_t ruleIndex, const ObjectSimulationRules& rules,
    uint64_t sessionSeed, uint64_t confirmedTick,
    uint64_t& nextEffectSequence,
    container::Vector<ObjectStructureEffectEvent>& effects) const {
    const game::ObjectDeathReactionRule* rule = runtimeRule(plan, ruleIndex);
    if (!rule || entity == ecs::null || !registry.valid(entity) || !object) {
        return false;
    }
    ObjectStructureDestructionComponent* component =
        ecs::try_get<ObjectStructureDestructionComponent>(registry, entity);
    if (!component) {
        component = &ecs::emplace<ObjectStructureDestructionComponent>(
            registry, entity);
    }
    const LogicFixedVec3 origin = positionOf(registry, entity);
    const Scalar orientation = yawOf(registry, entity);
    const ObjectTerrainLayerComponent* terrainLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);
    const uint32_t sourcePathfindLayer = terrainLayer
        ? terrainLayer->pathfindLayer
        : game::terrain::kGroundPathfindLayer;
    const ObjectGeometryComponent* geometry =
        ecs::try_get<ObjectGeometryComponent>(registry, entity);
    const Scalar height = geometry
        ? Scalar::max(Scalar{}, geometry->heightFixed)
        : Scalar{int32_t{1}};
    const uint64_t randomKey = structureKey(
        sessionSeed, object, damageSource, rule->authoredOrder,
        confirmedTick);

    if (rule->kind == game::ObjectDeathReactionKind::StructureTopple &&
        rule->structureTopple) {
        const game::ObjectStructureToppleParameters& parameters =
            *rule->structureTopple;
        ObjectStructureToppleRuntime runtime;
        runtime.plan = std::move(plan);
        runtime.ruleIndex = ruleIndex;
        runtime.damageSource = damageSource;
        runtime.origin = origin;
        runtime.originalYaw = orientation;
        runtime.sourcePathfindLayer = sourcePathfindLayer;
        runtime.structuralIntegrity = parameters.structuralIntegrity;
        runtime.buildingHeight = height;
        runtime.majorRadius = geometry
            ? Scalar::max(Scalar{}, geometry->majorRadiusFixed) : kOne;
        runtime.minorRadius = geometry
            ? Scalar::max(Scalar{}, geometry->minorRadiusFixed) : kOne;
        runtime.randomKey = randomKey;
        runtime.startTick = saturatingTickAdd(
            confirmedTick, randomDelayFrames(
                randomKey, 0x544f505354415254ull,
                parameters.minToppleDelayMilliseconds,
                parameters.maxToppleDelayMilliseconds,
                rules.logicFramesPerSecond));
        runtime.nextBurstTick = saturatingTickAdd(
            confirmedTick, randomDelayFrames(
                randomKey, 0x544f504255525354ull,
                parameters.minBurstDelayMilliseconds,
                parameters.maxBurstDelayMilliseconds,
                rules.logicFramesPerSecond));
        runtime.damageFxAccepted = damageFxAccepted(parameters, damageType);

        bool hasSourceDirection = false;
        if (damageSource) {
            if (const std::optional<ecs::entity> source =
                    lifecycle.entityFromIdIncludingPending(damageSource)) {
                const LogicFixedVec3 sourcePosition =
                    positionOf(registry, *source);
                runtime.directionX = origin.x - sourcePosition.x;
                runtime.directionY = origin.y - sourcePosition.y;
                hasSourceDirection = runtime.directionX != Scalar{} ||
                    runtime.directionY != Scalar{};
            }
        }
        if (hasSourceDirection) {
            normalizeDirection(runtime.directionX, runtime.directionY);
            const Scalar perturbation = randomSigned(
                randomKey, 0x544f505045525455ull) * kPiOverEight;
            const math::q32_32_sincos rotation =
                math::fixed_sincos(perturbation);
            const Scalar x = runtime.directionX;
            const Scalar y = runtime.directionY;
            runtime.directionX = x * rotation.cosine - y * rotation.sine;
            runtime.directionY = x * rotation.sine + y * rotation.cosine;
        } else {
            const math::q32_32_sincos direction = math::fixed_sincos(
                randomUnit(randomKey, 0x544f504449524543ull) * kTwoPi);
            runtime.directionX = direction.cosine;
            runtime.directionY = direction.sine;
        }
        if (const ObjectScriptToppleDirectionComponent* overrideDirection =
                ecs::try_get<ObjectScriptToppleDirectionComponent>(registry,
                                                                    entity)) {
            runtime.directionX = overrideDirection->direction.x;
            runtime.directionY = overrideDirection->direction.y;
        }
        normalizeDirection(runtime.directionX, runtime.directionY);
        const Scalar burstDistance =
            (runtime.majorRadius + runtime.minorRadius) * kHalf *
            Scalar::from_fraction(9, 10);
        runtime.delayBurstPosition = {
            .x = origin.x + runtime.directionX * burstDistance,
            .y = origin.y + runtime.directionY * burstDistance,
            .z = origin.z,
        };
        component->topples.push_back(std::move(runtime));
        ObjectStructureToppleRuntime& stored = component->topples.back();
        if (stored.damageFxAccepted) {
            emitEffect(
                ObjectStructureEffectKind::FxList,
                ObjectStructureEffectAnchor::WorldPosition,
                object, stored.sourcePathfindLayer, origin, orientation,
                parameters.toppleStartFx,
                rule->authoredOrder, confirmedTick, nextEffectSequence,
                effects);
        }
        emitTopplePhase(
            parameters, game::ObjectStructureTopplePhase::Initial,
            object, stored.sourcePathfindLayer, origin, orientation,
            stored.randomKey,
            stored.phaseSequence, rule->authoredOrder, confirmedTick,
            nextEffectSequence, effects);
        return true;
    }

    if (rule->kind == game::ObjectDeathReactionKind::StructureCollapse &&
        rule->structureCollapse) {
        const game::ObjectStructureCollapseParameters& parameters =
            *rule->structureCollapse;
        ObjectStructureCollapseRuntime runtime;
        runtime.plan = std::move(plan);
        runtime.ruleIndex = ruleIndex;
        runtime.origin = origin;
        runtime.orientationRadians = orientation;
        runtime.sourcePathfindLayer = sourcePathfindLayer;
        runtime.buildingHeight = height;
        runtime.randomKey = randomKey;
        runtime.startTick = saturatingTickAdd(
            confirmedTick, randomDelayFrames(
                randomKey, 0x434f4c5354415254ull,
                parameters.minCollapseDelayMilliseconds,
                parameters.maxCollapseDelayMilliseconds,
                rules.logicFramesPerSecond));
        component->collapses.push_back(std::move(runtime));
        ObjectStructureCollapseRuntime& stored = component->collapses.back();
        emitCollapsePhase(
            parameters, game::ObjectStructureCollapsePhase::Initial,
            object, stored.sourcePathfindLayer, origin, orientation,
            stored.randomKey,
            stored.phaseSequence, rule->authoredOrder, confirmedTick,
            nextEffectSequence, effects);
        return true;
    }
    return false;
}

void ObjectStructureDestructionSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const GameContentSnapshot* content, SimulationRandom* random,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    uint64_t& nextEffectSequence, uint64_t& nextWeaponSequence,
    container::Vector<ObjectStructureEffectEvent>& effects,
    container::Vector<ObjectSystemWeaponFireCommand>& weaponCommands,
    container::Vector<ObjectBoneFxStopRequest>& boneStops) const {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectStructureDestructionComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectId object =
            view.template get<const ObjectIdentityComponent>(entity).id;
        if (object && lifecycle.entityFromId(object)) {
            candidates.push_back({object, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });

    const Scalar gravityPerFrame = rules.logicFramesPerSecond == 0
        ? Scalar{}
        : rules.gravityUnitsPerSecondSq /
            Scalar{static_cast<int32_t>(rules.logicFramesPerSecond)} /
            Scalar{static_cast<int32_t>(rules.logicFramesPerSecond)};

    for (const Candidate& candidate : candidates) {
        ObjectStructureDestructionComponent* component =
            ecs::try_get<ObjectStructureDestructionComponent>(
                registry, candidate.entity);
        if (!component) continue;
        bool presentationChanged = false;
        const ObjectTerrainLayerComponent* terrainLayer =
            ecs::try_get<ObjectTerrainLayerComponent>(registry,
                                                       candidate.entity);
        const uint32_t sourcePathfindLayer = terrainLayer
            ? terrainLayer->pathfindLayer
            : game::terrain::kGroundPathfindLayer;

        for (ObjectStructureToppleRuntime& runtime : component->topples) {
            runtime.sourcePathfindLayer = sourcePathfindLayer;
            if (runtime.state == ObjectStructureMotionState::Done) continue;
            presentationChanged = true;
            const game::ObjectDeathReactionRule* rule =
                runtimeRule(runtime.plan, runtime.ruleIndex);
            if (!rule || !rule->structureTopple) {
                runtime.state = ObjectStructureMotionState::Done;
                continue;
            }
            const game::ObjectStructureToppleParameters& parameters =
                *rule->structureTopple;
            runtime.delayBurstPosition = groundPosition(
                terrain, runtime.delayBurstPosition.x,
                runtime.delayBurstPosition.y, runtime.origin.z);

            const auto emitDelayBurst = [&]() {
                if (runtime.damageFxAccepted) {
                    emitEffect(
                        ObjectStructureEffectKind::FxList,
                        ObjectStructureEffectAnchor::WorldPosition,
                        candidate.object, runtime.sourcePathfindLayer,
                        runtime.delayBurstPosition,
                        runtime.originalYaw, parameters.toppleDelayFx,
                        rule->authoredOrder, confirmedTick,
                        nextEffectSequence, effects);
                }
                emitTopplePhase(
                    parameters, game::ObjectStructureTopplePhase::Delay,
                    candidate.object, runtime.sourcePathfindLayer,
                    runtime.delayBurstPosition,
                    runtime.originalYaw, runtime.randomKey,
                    runtime.phaseSequence, rule->authoredOrder,
                    confirmedTick, nextEffectSequence, effects);
                const uint64_t delay = randomDelayFrames(
                    runtime.randomKey,
                    0x544f504255525354ull ^ runtime.phaseSequence,
                    parameters.minBurstDelayMilliseconds,
                    parameters.maxBurstDelayMilliseconds,
                    rules.logicFramesPerSecond);
                runtime.nextBurstTick = saturatingTickAdd(
                    confirmedTick, std::max<uint64_t>(1u, delay));
            };

            if (runtime.state == ObjectStructureMotionState::Waiting) {
                if (confirmedTick >= runtime.nextBurstTick) emitDelayBurst();
                if (confirmedTick >= runtime.startTick) {
                    runtime.state = ObjectStructureMotionState::Moving;
                    runtime.structuralIntegrity = parameters.structuralIntegrity;
                }
            }
            if (runtime.state != ObjectStructureMotionState::Moving) continue;

            const Scalar oldAngle = runtime.accumulatedAngle;
            runtime.velocity += kToppleAcceleration *
                math::fixed_sin(oldAngle) *
                (kOne - runtime.structuralIntegrity);
            if (runtime.structuralIntegrity > Scalar{}) {
                runtime.structuralIntegrity *= parameters.structuralDecay;
                runtime.structuralIntegrity = Scalar::max(
                    Scalar{}, runtime.structuralIntegrity);
            }
            Scalar newAngle = oldAngle + runtime.velocity;
            for (const game::ObjectStructureAngleFx& angleFx :
                 parameters.angleFx) {
                if (runtime.damageFxAccepted &&
                    angleFx.angleRadians > oldAngle &&
                    angleFx.angleRadians <= newAngle) {
                    emitEffect(
                        ObjectStructureEffectKind::FxList,
                        ObjectStructureEffectAnchor::ObjectAttachment,
                        candidate.object, runtime.sourcePathfindLayer,
                        runtime.origin,
                        runtime.originalYaw, angleFx.fx,
                        rule->authoredOrder, confirmedTick,
                        nextEffectSequence, effects);
                }
            }
            runtime.accumulatedAngle = Scalar::min(newAngle, kHalfPi);

            const Scalar theta = kHalfPi - runtime.accumulatedAngle;
            if (theta <= kPiOverSix) {
                const math::q32_32_sincos yaw =
                    math::fixed_sincos(runtime.originalYaw);
                const Scalar cosineDifference =
                    yaw.cosine * runtime.directionX +
                    yaw.sine * runtime.directionY;
                const Scalar sineDifference =
                    yaw.sine * runtime.directionX -
                    yaw.cosine * runtime.directionY;
                const Scalar minor =
                    runtime.minorRadius * cosineDifference;
                const Scalar major =
                    runtime.majorRadius * sineDifference;
                const Scalar facingWidth = Scalar::sqrt(
                    minor * minor + major * major) * kHalf;
                const Scalar maximumDistance = runtime.buildingHeight *
                    (kOne - math::fixed_sin(theta));
                const game::WeaponContentId weapon = content
                    ? content->findWeaponId(parameters.crushingWeapon)
                    : game::WeaponContentId{};
                // applyCrushingDamage() returns immediately when the authored
                // weapon template is absent.  In particular, CrushingFX and
                // the per-line FINAL OCL do not survive that failed lookup.
                // The terminal origin FINAL phase below remains independent.
                if (!content || !random || !weapon) {
                    runtime.lastCrushedDistance = maximumDistance;
                } else {
                    size_t emittedPoints = 0;
                    const auto damageLine = [&](Scalar distance) {
                        const Scalar alongX = distance * runtime.directionX;
                        const Scalar alongY = distance * runtime.directionY;
                        const auto impact = [&](Scalar lateral) {
                            if (++emittedPoints > 4096) return;
                            // Sweep across the topple axis, not along its
                            // reflection: the perpendicular of (dx, dy) is
                            // (-dy, dx).  Using (dy, dx) collapsed the crush
                            // line onto the topple axis for any diagonal
                            // topple, so units standing beside the falling
                            // footprint took no damage while units on the axis
                            // could be hit repeatedly.
                            const LogicFixedVec3 target = groundPosition(
                                terrain,
                                runtime.origin.x + alongX -
                                    lateral * runtime.directionY,
                                runtime.origin.y + alongY +
                                    lateral * runtime.directionX,
                                runtime.origin.z);
                            static_cast<void>(
                                queueObjectTransientWeaponFireAtPosition(
                                    weapon, registry, candidate.entity,
                                    candidate.object, target, *content, *random,
                                    runtime.nextShotSequence++,
                                    rule->authoredOrder,
                                    nextWeaponSequence++, confirmedTick,
                                    weaponCommands));
                            if (nextWeaponSequence == 0) ++nextWeaponSequence;
                            if (runtime.damageFxAccepted) {
                                emitEffect(
                                    ObjectStructureEffectKind::FxList,
                                    ObjectStructureEffectAnchor::WorldPosition,
                                    candidate.object,
                                    runtime.sourcePathfindLayer, target,
                                    runtime.originalYaw,
                                    parameters.crushingFx,
                                    rule->authoredOrder, confirmedTick,
                                    nextEffectSequence, effects);
                            }
                        };
                        for (Scalar lateral = -facingWidth;
                             lateral < facingWidth && emittedPoints <= 4096;
                             lateral += kLineSpacing) {
                            impact(lateral);
                        }
                        impact(facingWidth);
                        const LogicFixedVec3 center = groundPosition(
                            terrain, runtime.origin.x + alongX,
                            runtime.origin.y + alongY, runtime.origin.z);
                        emitTopplePhase(
                            parameters,
                            game::ObjectStructureTopplePhase::Final,
                            candidate.object, runtime.sourcePathfindLayer,
                            center, runtime.originalYaw,
                            runtime.randomKey, runtime.phaseSequence,
                            rule->authoredOrder, confirmedTick,
                            nextEffectSequence, effects);
                    };

                    Scalar distance = runtime.lastCrushedDistance;
                    for (; distance < maximumDistance &&
                           emittedPoints <= 4096;
                         distance += kLineSpacing) {
                        damageLine(distance);
                    }
                    damageLine(maximumDistance);
                    runtime.lastCrushedDistance = distance;
                }
            }

            if (runtime.accumulatedAngle >= kHalfPi) {
                runtime.accumulatedAngle = kHalfPi;
                emitTopplePhase(
                    parameters, game::ObjectStructureTopplePhase::Final,
                    candidate.object, runtime.sourcePathfindLayer,
                    runtime.origin, runtime.originalYaw,
                    runtime.randomKey, runtime.phaseSequence,
                    rule->authoredOrder, confirmedTick,
                    nextEffectSequence, effects);
                if (runtime.damageFxAccepted) {
                    emitEffect(
                        ObjectStructureEffectKind::FxList,
                        ObjectStructureEffectAnchor::ObjectAttachment,
                        candidate.object, runtime.sourcePathfindLayer,
                        runtime.origin,
                        runtime.originalYaw, parameters.toppleDoneFx,
                        rule->authoredOrder, confirmedTick,
                        nextEffectSequence, effects);
                }
                setPostCollapseModelCondition(registry, candidate.entity);
                requestBoneStop(candidate.object, rule->authoredOrder,
                                confirmedTick, boneStops);
                runtime.state = ObjectStructureMotionState::Done;
            } else if (confirmedTick >= runtime.nextBurstTick) {
                emitDelayBurst();
            }
        }

        for (ObjectStructureCollapseRuntime& runtime : component->collapses) {
            runtime.sourcePathfindLayer = sourcePathfindLayer;
            if (runtime.state == ObjectStructureMotionState::Done) continue;
            presentationChanged = true;
            const game::ObjectDeathReactionRule* rule =
                runtimeRule(runtime.plan, runtime.ruleIndex);
            if (!rule || !rule->structureCollapse) {
                runtime.state = ObjectStructureMotionState::Done;
                continue;
            }
            const game::ObjectStructureCollapseParameters& parameters =
                *rule->structureCollapse;
            const uint64_t shudderPurpose =
                0x434f4c5348554400ull ^ confirmedTick ^
                (static_cast<uint64_t>(runtime.phaseSequence) << 32u);
            runtime.visualShudderX = randomSigned(
                runtime.randomKey, shudderPurpose) * parameters.maxShudder;
            runtime.visualShudderY = randomSigned(
                runtime.randomKey, shudderPurpose ^ 0x9e3779b97f4a7c15ull) *
                parameters.maxShudder;

            if (runtime.state == ObjectStructureMotionState::Waiting &&
                confirmedTick >= runtime.startTick) {
                runtime.state = ObjectStructureMotionState::Moving;
                emitCollapsePhase(
                    parameters, game::ObjectStructureCollapsePhase::Burst,
                    candidate.object, runtime.sourcePathfindLayer,
                    runtime.origin,
                    runtime.orientationRadians, runtime.randomKey,
                    runtime.phaseSequence, rule->authoredOrder,
                    confirmedTick, nextEffectSequence, effects);
                runtime.nextBurstTick = saturatingTickAdd(
                    confirmedTick, std::max<uint64_t>(1u, randomDelayFrames(
                        runtime.randomKey, 0x434f4c4255525354ull,
                        parameters.minBurstDelayMilliseconds,
                        parameters.maxBurstDelayMilliseconds,
                        rules.logicFramesPerSecond)));
            }
            if (runtime.state != ObjectStructureMotionState::Moving) continue;

            runtime.currentHeight -= runtime.velocity;
            runtime.velocity -= gravityPerFrame *
                (kOne - parameters.collapseDamping);

            if (confirmedTick >= runtime.nextBurstTick) {
                const bool bigBurst = parameters.bigBurstFrequency != 0 &&
                    mix(runtime.randomKey ^ confirmedTick ^
                        runtime.phaseSequence) %
                            parameters.bigBurstFrequency == 0;
                emitCollapsePhase(
                    parameters,
                    bigBurst ? game::ObjectStructureCollapsePhase::Burst
                             : game::ObjectStructureCollapsePhase::Delay,
                    candidate.object, runtime.sourcePathfindLayer,
                    runtime.origin,
                    runtime.orientationRadians, runtime.randomKey,
                    runtime.phaseSequence, rule->authoredOrder,
                    confirmedTick, nextEffectSequence, effects);
                runtime.nextBurstTick = saturatingTickAdd(
                    runtime.nextBurstTick,
                    std::max<uint64_t>(1u, randomDelayFrames(
                        runtime.randomKey,
                        0x434f4c4255525354ull ^ runtime.phaseSequence,
                        parameters.minBurstDelayMilliseconds,
                        parameters.maxBurstDelayMilliseconds,
                        rules.logicFramesPerSecond)));
            }

            if (runtime.currentHeight + runtime.buildingHeight <= Scalar{}) {
                emitCollapsePhase(
                    parameters, game::ObjectStructureCollapsePhase::Final,
                    candidate.object, runtime.sourcePathfindLayer,
                    runtime.origin,
                    runtime.orientationRadians, runtime.randomKey,
                    runtime.phaseSequence, rule->authoredOrder,
                    confirmedTick, nextEffectSequence, effects);
                runtime.currentHeight = {};
                runtime.visualShudderX = {};
                runtime.visualShudderY = {};
                setPostCollapseModelCondition(registry, candidate.entity);
                requestBoneStop(candidate.object, rule->authoredOrder,
                                confirmedTick, boneStops);
                runtime.state = ObjectStructureMotionState::Done;
            }
        }
        if (presentationChanged) {
            markObjectDirty(
                registry, candidate.entity,
                ObjectDirtyDomain::RenderExtraction);
        }
    }
}

} // namespace engine
