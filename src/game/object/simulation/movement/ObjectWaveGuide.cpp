#include "game/object/simulation/movement/ObjectWaveGuide.h"
#include "core/container/string_utils.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/combat/ObjectNeutronMissileSlowDeath.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectToppleTransaction.h"
#include "game/object/simulation/combat/ObjectTactical.h"
#include "game/object/runtime/ObjectStatus.h"
#include "presentation/render/WaterSurfaceVisualSettings.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>

namespace engine {
namespace {
using Fixed = math::q32_32;

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    const uint64_t numerator = static_cast<uint64_t>(milliseconds) * rate;
    return (numerator + 999u) / 1000u;
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t value,
                                     uint64_t increment) noexcept {
    return increment > std::numeric_limits<uint64_t>::max() - value
        ? std::numeric_limits<uint64_t>::max() : value + increment;
}

[[nodiscard]] uint64_t claimEmissionSequence(uint64_t& next) noexcept {
    const uint64_t result = next++;
    if (next == 0) next = 1;
    return result;
}

void advanceSourceSequence(uint32_t& sequence) noexcept {
    ++sequence;
    if (sequence == 0) ++sequence;
}

[[nodiscard]] uint64_t attachmentGroupFor(
    ObjectId object, uint32_t authoredOrder) noexcept {
    const uint64_t low = static_cast<uint64_t>(authoredOrder) + 1u;
    return (static_cast<uint64_t>(object.value) << 32u) |
        (low & 0xffffffffu);
}

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf expected) noexcept {
    return kinds && game::objectHasKind(kinds->mask, expected);
}

[[nodiscard]] bool hasToppleUpdate(const ecs::registry& registry,
                                   ecs::entity entity) noexcept {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    return type && type->archetype && type->archetype->tacticalPlan &&
           !type->archetype->tacticalPlan->topple.empty();
}

[[nodiscard]] Fixed length2D(const LogicFixedVec3& value) noexcept {
    return Fixed::sqrt(value.x * value.x + value.y * value.y);
}

[[nodiscard]] LogicFixedVec3 waypointPosition(
    const game::terrain::WaypointRecord& waypoint) noexcept {
    return {
        Fixed::from_raw(waypoint.positionRaw[0]),
        Fixed::from_raw(waypoint.positionRaw[1]),
        Fixed::from_raw(waypoint.positionRaw[2]),
    };
}

[[nodiscard]] const game::FrozenLocomotorTemplate* waveLocomotor(
    const game::ThingTemplate& objectTemplate,
    const GameContentSnapshot& content) noexcept {
    const auto pick = [&content](
        const container::Vector<container::String>& names)
        -> const game::FrozenLocomotorTemplate* {
        for (const container::String& name : names) {
            if (const game::FrozenLocomotorTemplate* value =
                    content.findLocomotor(name)) return value;
        }
        return nullptr;
    };
    for (const game::LocomotorSetDefinition& set :
         objectTemplate.locomotorSets) {
        if (set.slot != game::LocomotorSetSlot::Normal) continue;
        if (const game::FrozenLocomotorTemplate* value = pick(set.templates)) {
            return value;
        }
    }
    if (objectTemplate.locomotorSets.empty()) {
        if (const game::FrozenLocomotorTemplate* value =
                pick(objectTemplate.locomotors)) return value;
        if (!objectTemplate.locomotor.empty()) {
            return content.findLocomotor(objectTemplate.locomotor);
        }
    }
    return nullptr;
}

[[nodiscard]] bool buildPath(
    const game::terrain::TerrainLogic& terrain,
    container::Vector<LogicFixedVec3>& output) {
    const game::terrain::WaypointRecord* current =
        terrain.waypointByName("WaveGuide1");
    if (!current) return false;
    container::Vector<uint32_t> visited;
    while (current) {
        if (std::find(visited.begin(), visited.end(), current->id) !=
            visited.end()) return false;
        visited.push_back(current->id);
        output.push_back(waypointPosition(*current));
        if (current->links.empty()) break;
        if (current->links.size() != 1) return false;
        current = terrain.waypointById(current->links.front());
        if (!current) return false;
    }
    return output.size() >= 2;
}

void synchronizePosition(ecs::registry& registry, ecs::entity entity,
                         const LogicFixedVec3& position,
                         Fixed yaw) {
    writeAuthoritativeObjectPosition(registry, entity, position);
    writeAuthoritativeObjectYaw(registry, entity, yaw);
    if (ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity)) {
        physics->yaw = yaw;
        physics->ownsAttitude = true;
    }
}

void emitEvent(container::Vector<ObjectWaveGuideEvent>& output,
               uint64_t& sequence, ObjectWaveGuideEventKind kind,
               ObjectId source, ObjectId target,
               container::StringView effect,
               const LogicFixedVec3& position, Fixed rotation,
               uint32_t authoredOrder, uint64_t confirmedTick,
               uint64_t attachmentGroup = 0) {
    output.push_back({
        .kind = kind,
        .source = source,
        .target = target,
        .effect = container::String{effect},
        .position = position,
        .rotationRadians = rotation,
        .attachmentGroup = attachmentGroup,
        .authoredOrder = authoredOrder,
        .emissionSequence = claimEmissionSequence(sequence),
        .confirmedTick = confirmedTick,
    });
}

[[nodiscard]] container::Vector<LogicFixedVec3> transformedShape(
    const game::ObjectWaveGuideRule& rule,
    const ObjectWaveGuideRuntime& runtime,
    const game::terrain::TerrainLogic& terrain) {
    container::Vector<LogicFixedVec3> output;
    output.reserve(rule.localShapePoints.size());
    const math::q32_32_sincos rotation = math::fixed_sincos(runtime.yawRadians);
    for (const LogicFixedVec3& local : rule.localShapePoints) {
        LogicFixedVec3 world{
            runtime.fixedPosition.x + local.x * rotation.cosine -
                local.y * rotation.sine,
            runtime.fixedPosition.y + local.x * rotation.sine +
                local.y * rotation.cosine,
            {},
        };
        world.z = Fixed::from_raw(
            terrain.groundHeightRaw(world.x.raw(), world.y.raw()));
        output.push_back(world);
    }
    return output;
}

void setModelCondition(RenderModelComponent* render,
                       game::ModelConditionFlag flag) {
    if (!render) return;
    render->modelConditionFlags.set(flag);
}

[[nodiscard]] bool pointBehindWave(
    const LogicFixedVec3& target,
    const LogicFixedVec3& sample,
    const math::q32_32_sincos& forward) noexcept {
    const Fixed dx = target.x - sample.x;
    const Fixed dy = target.y - sample.y;
    return dx * forward.cosine + dy * forward.sine <= Fixed{};
}

[[nodiscard]] bool intersectsAnySample(
    const LogicFixedVec3& target,
    container::Span<const LogicFixedVec3> samples,
    Fixed radius, const math::q32_32_sincos& forward,
    LogicFixedVec3& nearest) noexcept {
    const Fixed radiusSquared = radius * radius;
    bool found = false;
    Fixed best{};
    for (const LogicFixedVec3& sample : samples) {
        const Fixed dx = target.x - sample.x;
        const Fixed dy = target.y - sample.y;
        const Fixed distanceSquared = dx * dx + dy * dy;
        if (distanceSquared > radiusSquared ||
            !pointBehindWave(target, sample, forward)) continue;
        if (!found || distanceSquared < best) {
            found = true;
            best = distanceSquared;
            nearest = sample;
        }
    }
    return found;
}

[[nodiscard]] bool advanceAlongPath(
    ObjectWaveGuideRuntime& runtime, uint32_t framesPerSecond) {
    if (runtime.segmentIndex + 1 >= runtime.path.size()) return true;
    Fixed remaining = runtime.movementSpeedUnitsPerSecond /
        Fixed{static_cast<int32_t>(std::max<uint32_t>(1, framesPerSecond))};
    while (remaining > Fixed{} &&
           runtime.segmentIndex + 1 < runtime.path.size()) {
        const LogicFixedVec3 target = runtime.path[runtime.segmentIndex + 1];
        LogicFixedVec3 delta{
            target.x - runtime.fixedPosition.x,
            target.y - runtime.fixedPosition.y,
            {},
        };
        const Fixed distance = length2D(delta);
        if (distance <= Fixed{} || distance <= remaining) {
            runtime.fixedPosition = target;
            ++runtime.segmentIndex;
            remaining -= distance;
            continue;
        }
        runtime.yawRadians = math::fixed_atan2(delta.y, delta.x);
        runtime.fixedPosition.x += delta.x * remaining / distance;
        runtime.fixedPosition.y += delta.y * remaining / distance;
        remaining = {};
    }
    if (runtime.segmentIndex + 1 < runtime.path.size()) {
        const LogicFixedVec3& target = runtime.path[runtime.segmentIndex + 1];
        runtime.yawRadians = math::fixed_atan2(
            target.y - runtime.fixedPosition.y,
            target.x - runtime.fixedPosition.x);
    }
    if (runtime.segmentIndex + 1 >= runtime.path.size()) return true;
    const LogicFixedVec3& final = runtime.path.back();
    const LogicFixedVec3 toFinal{
        final.x - runtime.fixedPosition.x,
        final.y - runtime.fixedPosition.y,
        {},
    };
    return length2D(toFinal) <= Fixed{int32_t{100}};
}

} // namespace

void ObjectWaveGuideSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick) const {
    const ThingTemplateComponent* objectTemplate =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!objectTemplate || !objectTemplate->archetype ||
        !objectTemplate->archetype->waveGuidePlan) return;

    ObjectWaveGuideComponent component;
    component.plan = objectTemplate->archetype->waveGuidePlan;
    component.instances.resize(component.plan->rules.size());
    const game::FrozenLocomotorTemplate* locomotor = waveLocomotor(
        objectTemplate->archetype->templateData, content);
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, entity);
    for (size_t index = 0; index < component.instances.size(); ++index) {
        ObjectWaveGuideRuntime& runtime = component.instances[index];
        const game::ObjectWaveGuideRule& rule = component.plan->rules[index];
        runtime.delayTicks = millisecondsToTicks(
            rule.waveDelayMilliseconds, rules.logicFramesPerSecond);
        runtime.movementSpeedUnitsPerSecond = locomotor
            ? Fixed::max(Fixed{}, locomotor->fixed.maximumSpeed)
            : Fixed{};
        if (transform) {
            runtime.fixedPosition = readAuthoritativeObjectPosition(
                registry, entity, *transform);
            runtime.yawRadians = readAuthoritativeObjectYaw(
                registry, entity, *transform);
        }
        runtime.defaultDisableInstalled = true;
    }
    if (ObjectWaveGuideComponent* existing =
            ecs::try_get<ObjectWaveGuideComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectWaveGuideComponent>(registry, entity,
                                               std::move(component));
    }
    static_cast<void>(ObjectDisabledSystem::setUntil(
        registry, entity, ObjectDisabledReason::Default,
        OBJECT_DISABLED_FOREVER_TICK, confirmedTick));
}

void ObjectWaveGuideSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain,
    SimulationRandom& random, const ObjectSimulationRules& rules,
    uint64_t confirmedTick, uint64_t& nextEmissionSequence,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectWaveGuideBridgeImpact>& outBridgeImpacts,
    container::Vector<ObjectWaveGuideEvent>& outEvents,
    container::Vector<ObjectFireAudioCommand>& outAudio) const {
    struct Candidate final { ObjectId object; ecs::entity entity; };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<ObjectIdentityComponent,
                                ObjectWaveGuideComponent>(registry);
    candidates.reserve(view.size_hint());
    for (ecs::entity entity : view) {
        const ObjectId object =
            view.template get<ObjectIdentityComponent>(entity).id;
        if (object && lifecycle.entityFromId(object) &&
            !lifecycle.isPendingDestroy(object)) {
            candidates.push_back({object, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });

    for (const Candidate& candidate : candidates) {
        ObjectWaveGuideComponent& component =
            ecs::get<ObjectWaveGuideComponent>(registry, candidate.entity);
        if (!component.plan || isObjectDisabled(
                registry, candidate.entity, confirmedTick)) continue;
        const size_t count = std::min(component.plan->rules.size(),
                                      component.instances.size());
        for (size_t ruleIndex = 0; ruleIndex < count; ++ruleIndex) {
            const game::ObjectWaveGuideRule& rule =
                component.plan->rules[ruleIndex];
            ObjectWaveGuideRuntime& runtime = component.instances[ruleIndex];
            if (runtime.lastUpdateTick == confirmedTick) continue;
            runtime.lastUpdateTick = confirmedTick;
            if (!runtime.activationObserved) {
                runtime.activationObserved = true;
                runtime.activatedTick = confirmedTick;
            }
            if (confirmedTick < saturatingAdd(runtime.activatedTick,
                                              runtime.delayTicks)) continue;

            if (!runtime.initialized) {
                runtime.path.clear();
                if (runtime.movementSpeedUnitsPerSecond <= Fixed{} ||
                    !buildPath(terrain, runtime.path)) {
                    emitEvent(outEvents, nextEmissionSequence,
                        ObjectWaveGuideEventKind::InvalidPath,
                        candidate.object, INVALID_OBJECT_ID, {},
                        runtime.fixedPosition, runtime.yawRadians,
                        rule.authoredOrder, confirmedTick);
                    static_cast<void>(lifecycle.requestDestroy(
                        candidate.object, ObjectDestroyReason::System,
                        confirmedTick));
                    continue;
                }
                runtime.fixedPosition = runtime.path.front();
                const LogicFixedVec3 initialDelta{
                    runtime.path[1].x - runtime.path[0].x,
                    runtime.path[1].y - runtime.path[0].y,
                    {},
                };
                runtime.yawRadians = math::fixed_atan2(
                    initialDelta.y, initialDelta.x);
                synchronizePosition(registry, candidate.entity,
                                    runtime.fixedPosition,
                                    runtime.yawRadians);
                runtime.initialized = true;
                runtime.nextSplashTick = saturatingAdd(
                    confirmedTick,
                    static_cast<uint64_t>(rules.logicFramesPerSecond / 2) + 1u);
                if (!rule.loopingSound.empty()) {
                    outAudio.push_back({
                        .kind = ObjectFireAudioCommandKind::StartLoop,
                        .object = candidate.object,
                        .eventName = rule.loopingSound,
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                    runtime.loopingAudioActive = true;
                }
                emitEvent(outEvents, nextEmissionSequence,
                    ObjectWaveGuideEventKind::Started, candidate.object,
                    INVALID_OBJECT_ID, {}, runtime.fixedPosition,
                    runtime.yawRadians, rule.authoredOrder, confirmedTick);
                const uint64_t attachmentGroup = attachmentGroupFor(
                    candidate.object, rule.authoredOrder);
                for (size_t sampleIndex = 0;
                     sampleIndex < rule.localShapePoints.size();
                     ++sampleIndex) {
                    const LogicFixedVec3& local =
                        rule.localShapePoints[sampleIndex];
                    emitEvent(outEvents, nextEmissionSequence,
                        ObjectWaveGuideEventKind::FrontSpray,
                        candidate.object, INVALID_OBJECT_ID,
                        "WaveSpray01", local, {}, rule.authoredOrder,
                        confirmedTick, attachmentGroup);
                    emitEvent(outEvents, nextEmissionSequence,
                        ObjectWaveGuideEventKind::FrontSpray,
                        candidate.object, INVALID_OBJECT_ID,
                        "WaveSpray02", local, {}, rule.authoredOrder,
                        confirmedTick, attachmentGroup);
                    if (sampleIndex % 5u == 0u) {
                        emitEvent(outEvents, nextEmissionSequence,
                            ObjectWaveGuideEventKind::FrontSpray,
                            candidate.object, INVALID_OBJECT_ID,
                            "WaveSpray03", local, {}, rule.authoredOrder,
                            confirmedTick, attachmentGroup);
                    }
                }
            }

            const bool finished = advanceAlongPath(
                runtime, rules.logicFramesPerSecond);
            synchronizePosition(registry, candidate.entity,
                                runtime.fixedPosition,
                                runtime.yawRadians);
            if (finished) {
                emitEvent(outEvents, nextEmissionSequence,
                    ObjectWaveGuideEventKind::Finished, candidate.object,
                    INVALID_OBJECT_ID, "WaveSplash01",
                    runtime.fixedPosition, runtime.yawRadians,
                    rule.authoredOrder, confirmedTick,
                    attachmentGroupFor(candidate.object,
                                       rule.authoredOrder));
                if (runtime.loopingAudioActive) {
                    outAudio.push_back({
                        .kind = ObjectFireAudioCommandKind::StopLoop,
                        .object = candidate.object,
                        .eventName = rule.loopingSound,
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                    runtime.loopingAudioActive = false;
                }
                runtime.initialized = false;
                static_cast<void>(lifecycle.requestDestroy(
                    candidate.object, ObjectDestroyReason::System,
                    confirmedTick));
                continue;
            }

            const container::Vector<LogicFixedVec3> samples =
                transformedShape(rule, runtime, terrain);
            emitEvent(outEvents, nextEmissionSequence,
                ObjectWaveGuideEventKind::FrontAdvanced,
                candidate.object, INVALID_OBJECT_ID, {},
                runtime.fixedPosition, runtime.yawRadians,
                rule.authoredOrder, confirmedTick);
            if (!outEvents.empty()) {
                ObjectWaveGuideEvent& front = outEvents.back();
                front.ySize = rule.ySize;
                front.bendMagnitude = rule.waveBendMagnitude;
                front.damageRadius = rule.damageRadius;
                front.toppleForce = rule.toppleForce;
                front.preferredHeight = rule.preferredHeight;
            }
            for (const LogicFixedVec3& sample : samples) {
                terrain.addWaveWaterMotion(
                    sample.x.to_float(), sample.y.to_float(),
                    rule.waterVelocity.to_float(),
                    rule.preferredHeight.to_float(), confirmedTick);
            }

            if (confirmedTick >= runtime.nextSplashTick) {
                runtime.nextSplashTick = saturatingAdd(
                    confirmedTick,
                    static_cast<uint64_t>(rules.logicFramesPerSecond / 2) + 1u);
                // The comparison was inverted: a frequency of 100 ("always")
                // never emitted and 0 emitted every pulse.  Flipping it does not
                // perturb the RNG stream — the draw happens either way, only its
                // outcome is interpreted correctly now.
                if (!rule.randomSplashSound.empty() &&
                    random.integerInclusive(1, 100) <=
                        rule.randomSplashSoundFrequency) {
                    emitEvent(outEvents, nextEmissionSequence,
                        ObjectWaveGuideEventKind::RandomSplashSound,
                        candidate.object, INVALID_OBJECT_ID,
                        rule.randomSplashSound, runtime.fixedPosition,
                        runtime.yawRadians, rule.authoredOrder,
                        confirmedTick);
                }
            }

            if ((confirmedTick & 1u) == 0u && samples.size() > 1u) {
                const math::q32_32_sincos forward =
                    math::fixed_sincos(runtime.yawRadians);
                bool underWater = true;
                LogicFixedVec3 previousPoint{};
                for (size_t index = 0; index < rule.localShapePoints.size();
                     ++index) {
                    const LogicFixedVec3& local = rule.localShapePoints[index];
                    LogicFixedVec3 point{
                        runtime.fixedPosition.x +
                            (local.x - rule.shorelineEffectDistance) *
                                forward.cosine - local.y * forward.sine,
                        runtime.fixedPosition.y +
                            (local.x - rule.shorelineEffectDistance) *
                                forward.sine + local.y * forward.cosine,
                        {},
                    };
                    point.z = Fixed::from_raw(
                        terrain.groundHeightRaw(point.x.raw(), point.y.raw()));
                    const bool nextUnderWater =
                        point.z <= rule.preferredHeight;
                    if (index != 0 && underWater != nextUnderWater) {
                        const LogicFixedVec3& effectPosition =
                            nextUnderWater ? point : previousPoint;
                        emitEvent(outEvents, nextEmissionSequence,
                            nextUnderWater
                                ? ObjectWaveGuideEventKind::ShoreSplashLeft
                                : ObjectWaveGuideEventKind::ShoreSplashRight,
                            candidate.object, INVALID_OBJECT_ID,
                            nextUnderWater ? "WaveSplashLeft01"
                                           : "WaveSplashRight01",
                            effectPosition, runtime.yawRadians,
                            rule.authoredOrder, confirmedTick);
                    }
                    underWater = nextUnderWater;
                    previousPoint = point;
                }
            }

            struct Target final {
                ObjectId object;
                ecs::entity entity;
                LogicFixedVec3 position;
            };
            container::Vector<Target> targets;
            const auto targetView = ecs::view<ObjectIdentityComponent,
                                              TransformComponent>(registry);
            targets.reserve(targetView.size_hint());
            for (ecs::entity entity : targetView) {
                const ObjectId object =
                    targetView.template get<ObjectIdentityComponent>(entity).id;
                if (!object || object == candidate.object ||
                    !lifecycle.entityFromId(object) ||
                    lifecycle.isPendingDestroy(object)) continue;
                const ObjectMapStatusComponent* mapStatus =
                    ecs::try_get<ObjectMapStatusComponent>(registry, entity);
                if (mapStatus && mapStatus->offMap) continue;
                const TransformComponent& transform =
                    targetView.template get<TransformComponent>(entity);
                targets.push_back({
                    object, entity,
                    readAuthoritativeObjectPosition(
                        registry, entity, transform),
                });
            }
            std::sort(targets.begin(), targets.end(),
                [](const Target& left, const Target& right) {
                    return left.object < right.object;
                });
            const math::q32_32_sincos forward =
                math::fixed_sincos(runtime.yawRadians);
            for (const Target& target : targets) {
                const ObjectKindOfComponent* kinds =
                    ecs::try_get<ObjectKindOfComponent>(registry,
                                                        target.entity);
                if (hasKind(kinds, game::ObjectKindOf::Waveguide) ||
                    hasKind(kinds, game::ObjectKindOf::BridgeTower)) continue;
                const bool bridge =
                    hasKind(kinds, game::ObjectKindOf::Bridge);
                uint64_t bridgeSourceRecordIndex = UINT64_MAX;
                Fixed bridgeRotation = runtime.yawRadians;
                if (bridge) {
                    const MapObjectProvenanceComponent* provenance =
                        ecs::try_get<MapObjectProvenanceComponent>(
                            registry, target.entity);
                    if (provenance) {
                        bridgeSourceRecordIndex =
                            provenance->sourceRecordIndex;
                        const auto surfaces =
                            terrain.elevatedPathfindSurfaces();
                        const auto surface = std::find_if(
                            surfaces.begin(), surfaces.end(),
                            [bridgeSourceRecordIndex](
                                const game::terrain::
                                    TerrainElevatedPathfindSurface& value) {
                                return value.sourceRecordIndex ==
                                    bridgeSourceRecordIndex;
                            });
                        if (surface != surfaces.end()) {
                            bridgeRotation = math::fixed_atan2(
                                Fixed::from_raw(
                                    surface->toRaw[1] - surface->fromRaw[1]),
                                Fixed::from_raw(
                                    surface->toRaw[0] - surface->fromRaw[0]));
                        } else {
                            bridgeSourceRecordIndex = UINT64_MAX;
                        }
                    }
                }
                if (!bridge && target.position.z > rule.preferredHeight) {
                    continue;
                }
                const ObjectStatusComponent* oldStatus =
                    ecs::try_get<ObjectStatusComponent>(registry,
                                                        target.entity);
                if (oldStatus && oldStatus->hasAny(
                        game::objectStatusBit(
                            game::ObjectStatusFlag::Wet))) continue;
                LogicFixedVec3 nearest{};
                if (!intersectsAnySample(target.position, samples,
                                         rule.damageRadius, forward,
                                         nearest)) continue;

                static_cast<void>(ObjectStatusSystem::apply(
                    registry, target.entity,
                    {.setMask = game::objectStatusBit(
                         game::ObjectStatusFlag::Wet),
                     .confirmedTick = confirmedTick}));
                RenderModelComponent* render =
                    ecs::try_get<RenderModelComponent>(registry,
                                                       target.entity);
                setModelCondition(render, game::ModelConditionFlag::Flooded);
                ObjectShadowSuppressionComponent suppression{
                    .confirmedTick = confirmedTick};
                if (ObjectShadowSuppressionComponent* existing =
                        ecs::try_get<ObjectShadowSuppressionComponent>(
                            registry, target.entity)) {
                    *existing = suppression;
                } else {
                    ecs::emplace<ObjectShadowSuppressionComponent>(
                        registry, target.entity, suppression);
                }

                outDamage.push_back({
                    .target = target.object,
                    .source = candidate.object,
                    .sourceSequence = runtime.sourceSequence,
                    .submissionOrdinal =
                        claimEmissionSequence(nextEmissionSequence),
                    .amount = rule.damageAmount,
                    .damageType = game::DamageType::WATER,
                    .deathType = game::DeathType::FLOODED,
                    .confirmedTick = confirmedTick,
                });
                advanceSourceSequence(runtime.sourceSequence);

                LogicFixedVec3 force{
                    target.position.x - nearest.x,
                    target.position.y - nearest.y,
                    {},
                };
                const Fixed forceLength = length2D(force);
                if (forceLength > Fixed{} && rule.toppleForce > Fixed{} &&
                    hasToppleUpdate(registry, target.entity)) {
                    queueObjectToppleRequest(registry, {
                        .object = target.object,
                        .source = candidate.object,
                        .direction = force,
                        .speed = rule.toppleForce,
                        .sourceSequence = runtime.sourceSequence,
                        .confirmedTick = confirmedTick,
                        .noBounce = true,
                        .noFx = true,
                    });
                }
                LogicFixedVec3 hitPosition = bridge ? target.position : nearest;
                hitPosition.z = target.position.z;
                // ZH always emits the ordinary WaveHit01 for the struck
                // object. A bridge additionally requests its authored bridge
                // particle only after WaterWaveBridge is created by the
                // authoritative bridge transaction.
                emitEvent(outEvents, nextEmissionSequence,
                    ObjectWaveGuideEventKind::ObjectHit,
                    candidate.object, target.object,
                    "WaveHit01", hitPosition, runtime.yawRadians,
                    rule.authoredOrder, confirmedTick);
                if (bridge && bridgeSourceRecordIndex != UINT64_MAX) {
                    outBridgeImpacts.push_back({
                        .source = candidate.object,
                        .target = target.object,
                        .position = hitPosition,
                        .targetRotationRadians = bridgeRotation,
                        .terrainSourceRecordIndex = bridgeSourceRecordIndex,
                        .bridgeParticle = rule.bridgeParticle,
                        .bridgeParticleRotationRadians =
                            bridgeRotation +
                            rule.bridgeParticleAngleFudgeRadians,
                        .authoredOrder = rule.authoredOrder,
                        .emissionSequence = claimEmissionSequence(
                            nextEmissionSequence),
                        .confirmedTick = confirmedTick,
                    });
                }
            }
        }
    }
}

void ObjectWaveGuideSystem::onObjectReclaim(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick,
    uint64_t& nextEmissionSequence,
    container::Vector<ObjectWaveGuideEvent>& outEvents,
    container::Vector<ObjectFireAudioCommand>& outAudio) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    ObjectWaveGuideComponent* component = entity
        ? ecs::try_get<ObjectWaveGuideComponent>(registry, *entity) : nullptr;
    if (!component || !component->plan) return;
    const size_t count = std::min(component->plan->rules.size(),
                                  component->instances.size());
    for (size_t index = 0; index < count; ++index) {
        ObjectWaveGuideRuntime& runtime = component->instances[index];
        const game::ObjectWaveGuideRule& rule = component->plan->rules[index];
        if (runtime.initialized) {
            emitEvent(outEvents, nextEmissionSequence,
                ObjectWaveGuideEventKind::Finished, object,
                INVALID_OBJECT_ID, {}, runtime.fixedPosition,
                runtime.yawRadians, rule.authoredOrder, confirmedTick,
                attachmentGroupFor(object, rule.authoredOrder));
            runtime.initialized = false;
        }
        if (!runtime.loopingAudioActive || rule.loopingSound.empty()) continue;
        outAudio.push_back({
            .kind = ObjectFireAudioCommandKind::StopLoop,
            .object = object,
            .eventName = rule.loopingSound,
            .authoredOrder = rule.authoredOrder,
            .confirmedTick = confirmedTick,
        });
        runtime.loopingAudioActive = false;
    }
}
} // namespace engine
