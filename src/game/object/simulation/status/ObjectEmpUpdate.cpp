#include "core/container/string_utils.h"
#include "game/data/base/ContentBoolParsing.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/status/ObjectEmpUpdate.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <optional>
namespace engine {
namespace {

[[nodiscard]] uint64_t saturatingAdd(uint64_t value,
                                     uint64_t delta) noexcept {
    return delta > std::numeric_limits<uint64_t>::max() - value
        ? std::numeric_limits<uint64_t>::max() : value + delta;
}

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t fps = std::max<uint32_t>(1, framesPerSecond);
    const uint64_t product = static_cast<uint64_t>(milliseconds) * fps;
    return product / 1000u + (product % 1000u != 0 ? 1u : 0u);
}

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] bool isFactionStructure(
    const ObjectKindOfComponent* kinds) noexcept {
    static const game::ObjectKindOfMask mask = [] {
        game::ObjectKindOfMask value;
        for (const game::ObjectKindOf kind : {
                 game::ObjectKindOf::FsFactory,
                 game::ObjectKindOf::FsBaseDefense,
                 game::ObjectKindOf::FsTechnology,
                 game::ObjectKindOf::FsSupplyDropzone,
                 game::ObjectKindOf::FsSuperweapon,
                 game::ObjectKindOf::FsBlackMarket,
                 game::ObjectKindOf::FsSupplyCenter,
                 game::ObjectKindOf::FsStrategyCenter,
                 game::ObjectKindOf::FsFake,
                 game::ObjectKindOf::FsInternetCenter,
                 game::ObjectKindOf::FsAdvancedTech,
                 game::ObjectKindOf::FsBarracks,
                 game::ObjectKindOf::FsWarfactory,
                 game::ObjectKindOf::FsAirfield}) {
            game::setObjectKind(value, kind);
        }
        return value;
    }();
    return kinds && kinds->mask.test_for_any(mask);
}

[[nodiscard]] bool airborne(const ecs::registry& registry,
                            ecs::entity entity) noexcept {
    const ObjectAirborneComponent* state =
        ecs::try_get<ObjectAirborneComponent>(registry, entity);
    if (state && state->isAirborne) return true;
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    return status && status->hasAny(game::objectStatusBit(
                         game::ObjectStatusFlag::AirborneTarget));
}

[[nodiscard]] math::q32_32 footprintArea(
    const ObjectGeometryComponent& geometry) noexcept {
    const math::q32_32 zero{};
    if (geometry.shape == ObjectGeometryShape::Box) {
        return math::q32_32{int32_t{4}} *
               std::max(zero, geometry.majorRadiusFixed) *
               std::max(zero, geometry.minorRadiusFixed);
    }
    const math::q32_32 radius =
        std::max(zero, geometry.boundingCircleRadiusFixed);
    const math::q32_32 pi =
        math::q32_32::from_raw(13'493'037'705ll);
    return pi * radius * radius;
}

[[nodiscard]] uint32_t ceilPositiveToUint32(
    math::q32_32 value) noexcept {
    if (value <= math::q32_32{}) return 0;
    const int64_t whole = value.raw() >> 32u;
    const uint64_t rounded = static_cast<uint64_t>(whole) +
        ((value.raw() & 0xffffffffll) != 0 ? 1u : 0u);
    return rounded >= std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(rounded);
}

void initializeRuntime(ObjectEmpRuntime& runtime,
                       const game::ObjectEmpParameters& rule,
                       SimulationRandom* random,
                       uint32_t logicFramesPerSecond,
                       uint64_t createdAtTick) {
    const uint64_t lifetime = std::max<uint64_t>(
        1, millisecondsToTicks(rule.lifetimeMilliseconds,
                               logicFramesPerSecond));
    const uint64_t authoredFade = millisecondsToTicks(
        rule.startFadeMilliseconds, logicFramesPerSecond);
    const uint64_t fade = std::min(authoredFade, lifetime - 1u);
    runtime.dieTick = saturatingAdd(createdAtTick, lifetime);
    runtime.fadeTick = saturatingAdd(createdAtTick, fade);
    runtime.currentScale = rule.startScale;
    if (random) {
        runtime.targetScale = random->fixedInclusive(
            rule.targetScaleMinimum, rule.targetScaleMaximum);
        runtime.randomized = true;
    } else {
        runtime.targetScale = rule.targetScaleMinimum;
    }
}

void ensureRandomized(ObjectEmpRuntime& runtime,
                      const game::ObjectEmpParameters& rule,
                      SimulationRandom& random) {
    if (runtime.randomized) return;
    runtime.targetScale = random.fixedInclusive(
        rule.targetScaleMinimum, rule.targetScaleMaximum);
    runtime.randomized = true;
}

[[nodiscard]] ObjectId intendedVictim(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity effectEntity) {
    const ObjectProducerComponent* producer =
        ecs::try_get<ObjectProducerComponent>(registry, effectEntity);
    if (!producer || !producer->producer) return INVALID_OBJECT_ID;
    const std::optional<ecs::entity> producerEntity =
        lifecycle.entityFromIdIncludingPending(producer->producer);
    if (!producerEntity) return INVALID_OBJECT_ID;
    if (const ObjectWeaponComponent* weapons =
            ecs::try_get<ObjectWeaponComponent>(registry, *producerEntity);
        weapons && weapons->target) {
        return weapons->target;
    }
    const ObjectOrderQueueComponent* orders =
        ecs::try_get<ObjectOrderQueueComponent>(registry, *producerEntity);
    return orders && !orders->orders.empty()
        ? orders->orders.front().targetObject : INVALID_OBJECT_ID;
}

[[nodiscard]] bool withinBoundingSphere(
    const LogicFixedVec3& source, LogicFixedVec3 target,
    const ObjectGeometryComponent* geometry,
    math::q32_32 radius) noexcept {
    const math::q32_32 combined = radius + (geometry
        ? math::q32_32::max(math::q32_32{},
              geometry->boundingSphereRadiusFixed)
        : math::q32_32{});
    if (geometry && geometry->shape != ObjectGeometryShape::Sphere) {
        target.z += math::q32_32::max(math::q32_32{},
            geometry->heightFixed) /
            math::q32_32{int32_t{2}};
    }
    const math::q32_32 dx = target.x - source.x;
    const math::q32_32 dy = target.y - source.y;
    const math::q32_32 dz = target.z - source.z;
    return dx * dx + dy * dy + dz * dz < combined * combined;
}

void updateVisual(ObjectEmpUpdateComponent& component,
                  const ObjectEmpRuntime& runtime,
                  uint64_t confirmedTick, uint32_t ruleIndex) {
    component.visualScale = runtime.currentScale;
    component.visualBlend = {};
    if (confirmedTick >= runtime.fadeTick &&
        runtime.dieTick > runtime.fadeTick) {
        const uint64_t elapsed = confirmedTick - runtime.fadeTick;
        const uint64_t duration = runtime.dieTick - runtime.fadeTick;
        component.visualBlend = math::q32_32::min(
            math::q32_32{int32_t{1}},
            math::q32_32::from_fraction(
                static_cast<int64_t>(std::min<uint64_t>(elapsed, INT64_MAX)),
                static_cast<int64_t>(std::min<uint64_t>(duration, INT64_MAX))));
    }
    component.visualRuleIndex = ruleIndex;
    component.visualActive = true;
}

} // namespace

void ObjectEmpUpdateSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity, SimulationRandom* random,
    uint32_t logicFramesPerSecond, uint64_t createdAtTick) const {
    const ThingTemplateComponent* objectTemplate =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const container::SharedPtr<const game::ObjectEmpPlan> plan =
        objectTemplate && objectTemplate->archetype
            ? objectTemplate->archetype->empPlan : nullptr;
    if (!plan || plan->rules.empty()) return;
    ObjectEmpUpdateComponent component;
    component.plan = plan;
    component.instances.resize(plan->rules.size());
    for (size_t index = 0; index < plan->rules.size(); ++index) {
        initializeRuntime(component.instances[index], plan->rules[index],
                          random, logicFramesPerSecond, createdAtTick);
    }
    if (random) {
        const math::q32_32 pi =
            math::q32_32::from_raw(13'493'037'705ll);
        writeAuthoritativeObjectYaw(
            registry, entity, random->fixedInclusive(-pi, pi));
    }
    ecs::emplace<ObjectEmpUpdateComponent>(registry, entity,
                                            std::move(component));
}

void ObjectEmpUpdateSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectEmpParticleEvent>& outParticles) {
    struct Candidate final { ObjectId object; ecs::entity entity; };
    container::Vector<Candidate> effects;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectEmpUpdateComponent,
                                const TransformComponent>(registry);
    effects.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (identity.id && !lifecycle.isPendingDestroy(identity.id)) {
            effects.push_back({identity.id, entity});
        }
    }
    std::sort(effects.begin(), effects.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });
    if (effects.empty()) return;
    m_currentPositionIndex.rebuild(registry, lifecycle);

    for (const Candidate& effect : effects) {
        ObjectEmpUpdateComponent& component =
            ecs::get<ObjectEmpUpdateComponent>(registry, effect.entity);
        if (!component.plan) continue;
        if (isObjectDisabled(registry, effect.entity, confirmedTick)) continue;
        if (component.lastUpdatedTick == confirmedTick) continue;
        const math::q32_32 previousVisualScale = component.visualScale;
        const math::q32_32 previousVisualBlend = component.visualBlend;
        const uint32_t previousVisualRuleIndex = component.visualRuleIndex;
        const bool previousVisualActive = component.visualActive;
        component.lastUpdatedTick = confirmedTick;
        component.visualActive = false;
        const TransformComponent& sourceTransform =
            ecs::get<const TransformComponent>(registry, effect.entity);
        const LogicFixedVec3 sourcePosition = readAuthoritativeObjectPosition(
            registry, effect.entity, sourceTransform);
        const OwnerComponent* sourceOwner =
            ecs::try_get<OwnerComponent>(registry, effect.entity);
        const ObjectId intended = intendedVictim(registry, lifecycle,
                                                  effect.entity);
        const std::optional<ecs::entity> intendedEntity = intended
            ? lifecycle.entityFromIdIncludingPending(intended) : std::nullopt;
        const bool onlyAirborne = intendedEntity &&
            airborne(registry, *intendedEntity);

        const size_t count = std::min(component.plan->rules.size(),
                                      component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectEmpParameters& rule =
                component.plan->rules[index];
            ObjectEmpRuntime& runtime = component.instances[index];
            ensureRandomized(runtime, rule, random);
            runtime.currentScale +=
                (runtime.targetScale - runtime.currentScale) *
                math::q32_32::from_fraction(1, 20);
            updateVisual(component, runtime, confirmedTick,
                         static_cast<uint32_t>(index));

            if (!runtime.effectApplied && confirmedTick >= runtime.fadeTick) {
                runtime.effectApplied = true;
                const math::q32_32 radius = math::q32_32::max(
                    math::q32_32{}, rule.effectRadius);
                const container::Vector<ObjectId> nearby =
                    radius > math::q32_32{}
                        ? m_currentPositionIndex.querySphereRadiusFixed(
                              sourcePosition, radius)
                        : container::Vector<ObjectId>{};
                bool intendedProcessed = false;
                for (const ObjectId targetId : nearby) {
                    if (!targetId || targetId == effect.object ||
                        lifecycle.isPendingDestroy(targetId)) continue;
                    const std::optional<ecs::entity> targetEntity =
                        lifecycle.entityFromId(targetId);
                    if (!targetEntity) continue;
                    const ObjectMapStatusComponent* mapStatus =
                        ecs::try_get<ObjectMapStatusComponent>(registry,
                                                               *targetEntity);
                    if (mapStatus && mapStatus->offMap) continue;
                    const TransformComponent* targetTransform =
                        ecs::try_get<TransformComponent>(registry,
                                                         *targetEntity);
                    const ObjectGeometryComponent* geometry =
                        ecs::try_get<ObjectGeometryComponent>(registry,
                                                              *targetEntity);
                    if (!targetTransform || !withinBoundingSphere(
                            sourcePosition,
                            readAuthoritativeObjectPosition(
                                registry, *targetEntity,
                                *targetTransform),
                            geometry, radius)) continue;
                    if (onlyAirborne && !airborne(registry, *targetEntity)) {
                        continue;
                    }
                    const ObjectKindOfComponent* kinds =
                        ecs::try_get<ObjectKindOfComponent>(registry,
                                                            *targetEntity);
                    const bool vehicle =
                        hasKind(kinds, game::ObjectKindOf::Vehicle);
                    const bool structure =
                        hasKind(kinds, game::ObjectKindOf::Structure);
                    const bool spawnsAreWeapons = hasKind(
                        kinds, game::ObjectKindOf::SpawnsAreTheWeapons);
                    if (!vehicle && !structure && !spawnsAreWeapons) continue;
                    const OwnerComponent* targetOwner =
                        ecs::try_get<OwnerComponent>(registry, *targetEntity);
                    if (rule.doesNotAffectOwnBuildings && structure &&
                        sourceOwner && targetOwner &&
                        sourceOwner->player == targetOwner->player) {
                        continue;
                    }
                    if (hasKind(kinds, game::ObjectKindOf::Aircraft) &&
                        airborne(registry, *targetEntity)) {
                        if (hasKind(kinds, game::ObjectKindOf::EmpHardened))
                            continue;
                        outDamage.push_back({
                            .target = targetId,
                            .sourceSequence = rule.authoredOrder,
                            .causalGroup = effect.object,
                            .damageType = game::DamageType::UNRESISTABLE,
                            .deathType = game::DeathType::NORMAL,
                            .forceKill = true,
                            .confirmedTick = confirmedTick,
                        });
                        continue;
                    }
                    if (structure) {
                        if (!isFactionStructure(kinds)) continue;
                    } else if (sourceOwner && targetOwner) {
                        // RefCode parses the complete WeaponAffects mask but
                        // this module's shipped logic consults only ALLIES.
                        if ((rule.doesNotAffect & game::weaponAffectsBit(
                                game::WeaponAffectsTarget::Allies)) != 0 &&
                            relationshipBetweenObjects(
                                registry, players, effect.entity,
                                *targetEntity) ==
                                PlayerRelationship::Allies) {
                            continue;
                        }
                    }

                    const uint64_t disabledTicks = millisecondsToTicks(
                        rule.disabledDurationMilliseconds,
                        logicFramesPerSecond);
                    static_cast<void>(ObjectDisabledSystem::setUntil(
                        registry, *targetEntity, ObjectDisabledReason::Emp,
                        saturatingAdd(confirmedTick, disabledTicks),
                        confirmedTick));
                    if (targetId == intended) intendedProcessed = true;

                    if (!rule.disableParticleSystem.empty() && geometry) {
                        const math::q32_32 height = std::max(
                            math::q32_32{}, geometry->heightFixed);
                        const math::q32_32 volume = footprintArea(*geometry) *
                            std::min(height, math::q32_32{int32_t{10}});
                        const math::q32_32 requested =
                            rule.sparksPerCubicFoot * volume;
                        const uint32_t emitters = std::max(
                            15u, ceilPositiveToUint32(requested));
                        outParticles.push_back({
                            .source = effect.object,
                            .target = targetId,
                            .particleSystem = rule.disableParticleSystem,
                            .emitterCount = emitters,
                            .systemLifetimeFrames = disabledTicks > 30u
                                ? static_cast<uint32_t>(std::min<uint64_t>(
                                      disabledTicks - 30u,
                                      std::numeric_limits<uint32_t>::max()))
                                : 0u,
                            .footprintMajorRadius =
                                geometry->majorRadiusFixed.to_float(),
                            .footprintMinorRadius =
                                geometry->minorRadiusFixed.to_float(),
                            .maximumHeight = height.to_float(),
                            .boxFootprint =
                                geometry->shape == ObjectGeometryShape::Box,
                            .authoredOrder = rule.authoredOrder,
                            .confirmedTick = confirmedTick,
                        });
                    }
                }

                // RefCode preserves this patched dimensional quirk: an
                // airborne intended target that missed the partition result
                // still receives EMP inside radius*2 or forty units.
                if (intendedEntity && !intendedProcessed &&
                    hasKind(ecs::try_get<ObjectKindOfComponent>(
                                registry, *intendedEntity),
                            game::ObjectKindOf::Aircraft) &&
                    !hasKind(ecs::try_get<ObjectKindOfComponent>(
                                 registry, *intendedEntity),
                             game::ObjectKindOf::EmpHardened)) {
                    const TransformComponent* targetTransform =
                        ecs::try_get<TransformComponent>(registry,
                                                         *intendedEntity);
                    if (targetTransform) {
                        const LogicFixedVec3 targetPosition =
                            readAuthoritativeObjectPosition(
                                registry, *intendedEntity,
                                *targetTransform);
                        const math::q32_32 dx =
                            targetPosition.x - sourcePosition.x;
                        const math::q32_32 dy =
                            targetPosition.y - sourcePosition.y;
                        const math::q32_32 dz =
                            targetPosition.z - sourcePosition.z;
                        const math::q32_32 distanceSquared =
                            dx * dx + dy * dy + dz * dz;
                        if (distanceSquared <= radius *
                                math::q32_32{int32_t{2}} ||
                            distanceSquared <=
                                math::q32_32{int32_t{1600}}) {
                            const uint64_t disabledTicks = millisecondsToTicks(
                                rule.disabledDurationMilliseconds,
                                logicFramesPerSecond);
                            static_cast<void>(ObjectDisabledSystem::setUntil(
                                registry, *intendedEntity,
                                ObjectDisabledReason::Emp,
                                saturatingAdd(confirmedTick, disabledTicks),
                                confirmedTick));
                        }
                    }
                }
            }

            if (!runtime.killRequested && confirmedTick >= runtime.dieTick) {
                runtime.killRequested = true;
                outDamage.push_back({
                    .target = effect.object,
                    .sourceSequence = rule.authoredOrder,
                    .causalGroup = effect.object,
                    .damageType = game::DamageType::UNRESISTABLE,
                    .deathType = game::DeathType::NORMAL,
                    .forceKill = true,
                    .confirmedTick = confirmedTick,
                });
            }
        }
        if (component.visualScale != previousVisualScale ||
            component.visualBlend != previousVisualBlend ||
            component.visualRuleIndex != previousVisualRuleIndex ||
            component.visualActive != previousVisualActive) {
            markObjectDirty(
                registry, effect.entity,
                ObjectDirtyDomain::RenderExtraction);
        }
    }
}

} // namespace engine
