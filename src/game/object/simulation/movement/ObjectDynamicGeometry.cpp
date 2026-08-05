#include "game/object/simulation/movement/ObjectDynamicGeometry.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace engine {
namespace {

using Fixed = math::q32_32;

struct Candidate final {
    ObjectId object = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

struct GeometryProjection final {
    Fixed height{};
    Fixed major{};
    Fixed minor{};
    Fixed boundingCircle{};
    Fixed boundingSphere{};
};

[[nodiscard]] uint64_t fixedMillisecondsToTicks(
    Fixed milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds <= Fixed{}) return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    const uint64_t raw = static_cast<uint64_t>(milliseconds.raw());
    const uint64_t wholeMilliseconds = raw >> 32u;
    const uint64_t fractionalMilliseconds = raw & 0xffffffffull;
    const uint64_t wholeProduct = wholeMilliseconds * rate;
    uint64_t ticks = wholeProduct / 1000u;
    const uint64_t wholeRemainder = wholeProduct % 1000u;
    const uint64_t fractionalProduct = fractionalMilliseconds * rate;
    const uint64_t fractionalWhole = fractionalProduct >> 32u;
    const uint64_t fractionalRemainder = fractionalProduct & 0xffffffffull;
    const uint64_t remainderWhole = wholeRemainder + fractionalWhole;
    ticks += remainderWhole / 1000u;
    const uint64_t finalNumerator =
        (remainderWhole % 1000u) * (uint64_t{1} << 32u) +
        fractionalRemainder;
    if (finalNumerator != 0) ++ticks;
    return ticks;
}

[[nodiscard]] uint64_t unsignedMillisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    const uint64_t wholeSeconds = milliseconds / 1000u;
    const uint64_t remainderMilliseconds = milliseconds % 1000u;
    const uint64_t whole = wholeSeconds * rate;
    const uint64_t remainderProduct = remainderMilliseconds * rate;
    return whole + remainderProduct / 1000u +
        (remainderProduct % 1000u != 0 ? 1u : 0u);
}

[[nodiscard]] Fixed ratioFor(uint64_t numerator,
                             uint64_t denominator) noexcept {
    denominator = std::max<uint64_t>(1, denominator);
    const uint64_t bounded = std::min(numerator, denominator);
    const uint64_t raw = (bounded << 32u) / denominator;
    return Fixed::from_raw(static_cast<int64_t>(raw));
}

[[nodiscard]] GeometryProjection projectGeometry(
    ObjectGeometryShape shape, Fixed height, Fixed major,
    Fixed minor) noexcept {
    GeometryProjection result{
        .height = height,
        .major = major,
        .minor = minor,
    };
    switch (shape) {
    case ObjectGeometryShape::Sphere:
        result.height = major;
        result.minor = major;
        result.boundingCircle = major;
        result.boundingSphere = major;
        break;
    case ObjectGeometryShape::Cylinder:
        result.minor = major;
        result.boundingCircle = major;
        result.boundingSphere = Fixed::max(height * Fixed::from_fraction(1, 2),
                                           major);
        break;
    case ObjectGeometryShape::Box: {
        const Fixed footprintSquared = major * major + minor * minor;
        result.boundingCircle = Fixed::sqrt(footprintSquared);
        const Fixed halfHeight = height * Fixed::from_fraction(1, 2);
        result.boundingSphere = Fixed::sqrt(
            footprintSquared + halfHeight * halfHeight);
        break;
    }
    }
    return result;
}

void publishGeometry(ObjectGeometryComponent& destination,
                     const GeometryProjection& source) noexcept {
    destination.heightFixed = source.height;
    destination.majorRadiusFixed = source.major;
    destination.minorRadiusFixed = source.minor;
    destination.boundingCircleRadiusFixed = source.boundingCircle;
    destination.boundingSphereRadiusFixed = source.boundingSphere;
}

[[nodiscard]] bool hasLiveSpatialPresence(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    if (ecs::try_get<ObjectContainedByComponent>(registry, entity)) {
        return false;
    }
    if (const ObjectMapStatusComponent* map =
            ecs::try_get<ObjectMapStatusComponent>(registry, entity);
        map && map->offMap) {
        return false;
    }
    if (const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        health && health->terminalDeathIssued) {
        return false;
    }
    return true;
}

void appendFirestormDamage(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId sourceObject,
    const LogicFixedVec3& sourcePosition, Fixed sourceRadius,
    const game::ObjectDynamicGeometryRule& rule, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& output) {
    if (sourceRadius <= Fixed{}) return;

    container::Vector<Candidate> targets;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const TransformComponent,
                                const ObjectGeometryComponent>(registry);
    targets.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id) ||
            lifecycle.isPendingDestroy(identity.id) ||
            !hasLiveSpatialPresence(registry, entity)) {
            continue;
        }
        targets.push_back({.object = identity.id, .entity = entity});
    }
    std::sort(targets.begin(), targets.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });

    for (const Candidate& target : targets) {
        const TransformComponent& transform =
            ecs::get<const TransformComponent>(registry, target.entity);
        const LogicFixedVec3 targetPosition = readAuthoritativeObjectPosition(
            registry, target.entity, transform);
        if (targetPosition.z > sourcePosition.z +
                                   rule.firestorm.maximumHeightForDamage) {
            continue;
        }
        const ObjectGeometryComponent& geometry =
            ecs::get<const ObjectGeometryComponent>(registry, target.entity);
        const Fixed targetRadius = Fixed::max(
            Fixed{}, geometry.boundingSphereRadiusFixed);
        const Fixed combined = sourceRadius + targetRadius;
        const Fixed dx = targetPosition.x - sourcePosition.x;
        const Fixed dy = targetPosition.y - sourcePosition.y;
        if (dx * dx + dy * dy > combined * combined) continue;

        output.push_back({
            .target = target.object,
            .source = sourceObject,
            .sourceSequence = rule.authoredOrder,
            .causalGroup = sourceObject,
            .amount = rule.firestorm.damageAmount,
            .damageType = game::DamageType::FLAME,
            .deathType = game::DeathType::BURNED,
            .confirmedTick = confirmedTick,
        });
    }
}

[[nodiscard]] LogicFixedVec3 firestormParticlePosition(
    const game::terrain::TerrainLogic& terrain,
    const LogicFixedVec3& objectPosition,
    Fixed particleOffsetZ) noexcept {
    LogicFixedVec3 result = objectPosition;
    result.z = (terrain.isLoaded()
        ? Fixed::from_raw(terrain.groundHeightRaw(
              objectPosition.x.raw(), objectPosition.y.raw()))
        : objectPosition.z) + particleOffsetZ;
    return result;
}

} // namespace

void ObjectDynamicGeometrySystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const ObjectSimulationRules& rules) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype ||
        !type->archetype->dynamicGeometryPlan ||
        type->archetype->dynamicGeometryPlan->rules.empty()) {
        return;
    }

    ObjectDynamicGeometryComponent component{
        .plan = type->archetype->dynamicGeometryPlan,
    };
    const ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    const uint64_t createdAtTick = lifecycle ? lifecycle->createdAtTick : 0;
    component.instances.reserve(component.plan->rules.size());
    for (const game::ObjectDynamicGeometryRule& rule :
         component.plan->rules) {
        const uint64_t authoredDelay = unsignedMillisecondsToTicks(
            rule.initialDelayMilliseconds, rules.logicFramesPerSecond);
        const uint64_t transitionTicks = rule.hasAuthoredTransitionTime
            ? unsignedMillisecondsToTicks(
                  rule.transitionMilliseconds,
                  rules.logicFramesPerSecond)
            : 1u;
        const uint64_t effectiveDelay = std::max<uint64_t>(1, authoredDelay);
        const uint64_t earliestStartTick = effectiveDelay >
                std::numeric_limits<uint64_t>::max() - createdAtTick
            ? std::numeric_limits<uint64_t>::max()
            : createdAtTick + effectiveDelay;
        component.instances.push_back({
            .startingDelayCountdown = effectiveDelay,
            .earliestStartTick = earliestStartTick,
            .transitionTicks = std::min<uint64_t>(
                std::numeric_limits<uint32_t>::max(),
                std::max<uint64_t>(1, transitionTicks)),
            .damageIntervalTicks = fixedMillisecondsToTicks(
                rule.firestorm.damageIntervalMilliseconds,
                rules.logicFramesPerSecond),
            .initialHeight = rule.initialHeight,
            .initialMajorRadius = rule.initialMajorRadius,
            .initialMinorRadius = rule.initialMinorRadius,
            .finalHeight = rule.finalHeight,
            .finalMajorRadius = rule.finalMajorRadius,
            .finalMinorRadius = rule.finalMinorRadius,
            .reversePending = rule.reverseAtTransitionTime,
        });
    }
    if (ObjectDynamicGeometryComponent* existing =
            ecs::try_get<ObjectDynamicGeometryComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectDynamicGeometryComponent>(
            registry, entity, std::move(component));
    }
}

void ObjectDynamicGeometrySystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectDynamicGeometryGameplayEvent>& outGameplay,
    container::Vector<ObjectDynamicGeometryPresentationEvent>&
        outPresentation) const {
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectDynamicGeometryComponent,
                                ObjectGeometryComponent,
                                const TransformComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id) ||
            lifecycle.isPendingDestroy(identity.id)) {
            continue;
        }
        if (const ObjectHealthComponent* health =
                ecs::try_get<ObjectHealthComponent>(registry, entity);
            health && health->terminalDeathIssued) {
            continue;
        }
        candidates.push_back({.object = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });

    for (const Candidate& candidate : candidates) {
        ObjectDynamicGeometryComponent& component =
            ecs::get<ObjectDynamicGeometryComponent>(registry,
                                                      candidate.entity);
        ObjectGeometryComponent& geometry =
            ecs::get<ObjectGeometryComponent>(registry, candidate.entity);
        const TransformComponent& transform =
            ecs::get<const TransformComponent>(registry, candidate.entity);
        const LogicFixedVec3 objectPosition = readAuthoritativeObjectPosition(
            registry, candidate.entity, transform);
        if (!component.plan) continue;
        const size_t count = std::min(component.plan->rules.size(),
                                      component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectDynamicGeometryRule& rule =
                component.plan->rules[index];
            ObjectDynamicGeometryRuntime& runtime =
                component.instances[index];

            if (!runtime.started) {
                if (confirmedTick < runtime.earliestStartTick) {
                    runtime.startingDelayCountdown =
                        runtime.earliestStartTick - confirmedTick;
                    continue;
                }
                runtime.startingDelayCountdown = 0;
                runtime.started = true;
            }

            if (!runtime.finished) {
                const Fixed ratio = ratioFor(runtime.timeActive,
                                             runtime.transitionTicks);
                const GeometryProjection projected = projectGeometry(
                    geometry.shape,
                    Fixed::lerp(runtime.initialHeight,
                                runtime.finalHeight, ratio),
                    Fixed::lerp(runtime.initialMajorRadius,
                                runtime.finalMajorRadius, ratio),
                    Fixed::lerp(runtime.initialMinorRadius,
                                runtime.finalMinorRadius, ratio));
                publishGeometry(geometry, projected);
                markObjectDirty(
                    registry, candidate.entity,
                    objectDirtyBit(ObjectDirtyDomain::Spatial) |
                        objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
                runtime.currentBoundingCircleRadius =
                    projected.boundingCircle;

                ++runtime.timeActive;
                // Strictly greater is intentional: the frame whose ratio is
                // exactly one must publish the exact final geometry first.
                if (runtime.timeActive > runtime.transitionTicks) {
                    if (runtime.reversePending) {
                        runtime.switchedDirections = true;
                        runtime.timeActive = 0;
                        runtime.reversePending = false;
                        std::swap(runtime.initialHeight,
                                  runtime.finalHeight);
                        std::swap(runtime.initialMajorRadius,
                                  runtime.finalMajorRadius);
                        std::swap(runtime.initialMinorRadius,
                                  runtime.finalMinorRadius);
                    } else {
                        runtime.finished = true;
                    }
                }
            }

            if (rule.kind != game::ObjectDynamicGeometryKind::Firestorm) {
                continue;
            }
            runtime.currentBoundingCircleRadius =
                geometry.boundingCircleRadiusFixed;
            const LogicFixedVec3 particlePosition =
                firestormParticlePosition(
                    terrain, objectPosition,
                    rule.firestorm.particleOffsetZ);
            ObjectDynamicGeometryGameplayEvent gameplay{
                .object = candidate.object,
                .authoredOrder = rule.authoredOrder,
                .confirmedTick = confirmedTick,
            };
            if (!runtime.effectsStarted) {
                if (const OwnerComponent* owner =
                        ecs::try_get<OwnerComponent>(registry,
                                                     candidate.entity)) {
                    gameplay.owner = owner->player;
                    gameplay.firestormCreated = true;
                }
                outPresentation.push_back({
                    .kind = ObjectDynamicGeometryPresentationEventKind::Start,
                    .object = candidate.object,
                    .authoredOrder = rule.authoredOrder,
                    .confirmedTick = confirmedTick,
                    .objectPosition = objectPosition,
                    .particlePosition = particlePosition,
                    .majorRadius = geometry.majorRadiusFixed,
                    .scorchSize = rule.firestorm.scorchSize,
                    .particleSystems = rule.firestorm.particleSystems,
                    .fxList = rule.firestorm.fxList,
                });
                runtime.effectsStarted = true;
            }
            outPresentation.push_back({
                .kind =
                    ObjectDynamicGeometryPresentationEventKind::RadiusUpdate,
                .object = candidate.object,
                .authoredOrder = rule.authoredOrder,
                .confirmedTick = confirmedTick,
                .objectPosition = objectPosition,
                .particlePosition = particlePosition,
                .majorRadius = geometry.majorRadiusFixed,
            });
            if (runtime.switchedDirections && !runtime.scorchPlaced) {
                outPresentation.push_back({
                    .kind = ObjectDynamicGeometryPresentationEventKind::Scorch,
                    .object = candidate.object,
                    .authoredOrder = rule.authoredOrder,
                    .confirmedTick = confirmedTick,
                    .objectPosition = objectPosition,
                    .particlePosition = particlePosition,
                    .majorRadius = geometry.majorRadiusFixed,
                    .scorchSize = rule.firestorm.scorchSize,
                });
                runtime.scorchPlaced = true;
            }

            if (confirmedTick - runtime.lastDamageTick >=
                runtime.damageIntervalTicks) {
                appendFirestormDamage(
                    registry, lifecycle, candidate.object, objectPosition,
                    runtime.currentBoundingCircleRadius, rule,
                    confirmedTick, gameplay.damage);
                runtime.lastDamageTick = confirmedTick;
            }
            if (gameplay.firestormCreated || !gameplay.damage.empty()) {
                gameplay.submissionOrdinal =
                    nextGameplaySubmissionOrdinal++;
                if (nextGameplaySubmissionOrdinal == 0u) {
                    ++nextGameplaySubmissionOrdinal;
                }
                outGameplay.push_back(std::move(gameplay));
            }
        }
    }
}

void ObjectDynamicGeometrySystem::onObjectReclaim(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick,
    container::Vector<ObjectDynamicGeometryPresentationEvent>&
        outPresentation) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return;
    ObjectDynamicGeometryComponent* component =
        ecs::try_get<ObjectDynamicGeometryComponent>(registry, *entity);
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, *entity);
    if (!component || !component->plan || !transform) return;
    const LogicFixedVec3 objectPosition = readAuthoritativeObjectPosition(
        registry, *entity, *transform);
    const size_t count = std::min(component->plan->rules.size(),
                                  component->instances.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectDynamicGeometryRule& rule =
            component->plan->rules[index];
        ObjectDynamicGeometryRuntime& runtime = component->instances[index];
        if (rule.kind != game::ObjectDynamicGeometryKind::Firestorm ||
            !runtime.effectsStarted || runtime.stopEmitted) {
            continue;
        }
        outPresentation.push_back({
            .kind = ObjectDynamicGeometryPresentationEventKind::Stop,
            .object = object,
            .authoredOrder = rule.authoredOrder,
            .confirmedTick = confirmedTick,
            .objectPosition = objectPosition,
            .majorRadius = runtime.currentBoundingCircleRadius,
        });
        runtime.stopEmitted = true;
    }
}

} // namespace engine
